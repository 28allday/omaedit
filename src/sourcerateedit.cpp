#include "sourcerateedit.h"
#include "projectfile.h"
#include "timelinerate.h"
#include "retimemap.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
double factor(const QVariantMap &metadata) {
    const double recorded=metadata.value("sourceFps",metadata.value("fps")).toDouble();
    const double forced=metadata.value("forceFps").toDouble();
    return forced>0 && recorded>0 ? forced/recorded : 1.;
}
bool fractionalMapSegments(const QVariantMap &entry,double scale) {
    const auto definition=entry.value("retime").toMap();
    if(definition.isEmpty())return false;
    RetimeMap map;if(!map.set(definition))return true;
    const int first=entry.value("in").toInt(),count=entry.value("out").toInt()-first+1;
    const auto selected=map.window(first,count).value("segments").toList();
    for(const auto &v:selected) {
        const double frames=v.toMap().value("frames").toInt()*scale;
        if(std::abs(frames-qRound64(frames))>1e-7)return true;
    }
    return false;
}
bool changedInside(const QVariant &value,const QVariantMap &previous,const QVariantMap &sources) {
    if(value.metaType().id()==QMetaType::QVariantList) {
        for(const auto &v:value.toList())if(changedInside(v,previous,sources))return true;
    } else if(value.metaType().id()==QMetaType::QVariantMap) {
        const auto entry=value.toMap();const auto path=entry.value("path").toString();
        if(sources.contains(path) && !qFuzzyCompare(factor(previous.value(path,entry.value("sourceMeta")).toMap()),factor(sources.value(path).toMap())))return true;
        for(auto it=entry.begin();it!=entry.end();++it) {
            if(it.key()=="sourceMeta")continue;
            const auto child=it.key()=="compoundJson"?QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant():it.value();
            if(changedInside(child,previous,sources))return true;
        }
    }
    return false;
}
bool hasExternalSequence(const QVariant &value) {
    if(value.metaType().id()==QMetaType::QVariantList) {
        for(const auto &v:value.toList())if(hasExternalSequence(v))return true;
    } else if(value.metaType().id()==QMetaType::QVariantMap) {
        const auto entry=value.toMap();
        if(ProjectFile::isProject(entry.value("path").toString()))return true;
        for(auto it=entry.begin();it!=entry.end();++it) {
            if(it.key()=="sourceMeta")continue;
            const auto child=it.key()=="compoundJson"?QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant():it.value();
            if(hasExternalSequence(child))return true;
        }
    }
    return false;
}
}

QVariantMap SourceRateEdit::prepare(const QVariantList &input,const QVariantMap &previous,
                                  const QVariantMap &sources,double fps,const QVariantList &trackState) {
    auto fail=[](const char *message) {
        return QVariantMap{{"valid",false},{"error",QCoreApplication::translate("SourceRateEdit",message)}};
    };
    if(!std::isfinite(fps) || fps<=0)
        return fail(QT_TRANSLATE_NOOP("SourceRateEdit","The timeline frame rate is unavailable."));
    bool rateChanged=changedInside(input,previous,sources);
    for(auto it=sources.begin();it!=sources.end();++it)
        rateChanged |= !qFuzzyCompare(factor(previous.value(it.key()).toMap()),factor(it.value().toMap()));
    if(rateChanged && std::any_of(input.cbegin(),input.cend(),[](const QVariant &v) { return v.toMap().contains("audioTiming"); }))
        return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Source-rate changes with precise sound cuts need sample-based timing conversion, which is not supported yet."));
    // This pure layer cannot inspect or rewrite an external sequence. Until a
    // project transaction prepares its dependencies, refuse even if the changed
    // source is not directly visible in this sequence's entries.
    if(rateChanged && hasExternalSequence(input))
        return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Keeping selected footage during source-rate changes is not yet supported for nested sequences."));
    QVariantList entries=input,tracks=trackState;
    int sentinel=-1;
    if(!entries.isEmpty() && entries.last().toMap().contains("omavid:markers")) {
        sentinel=entries.size()-1;
        if(entries.last().toMap().contains("omavid:tracks"))tracks=entries.last().toMap().value("omavid:tracks").toList();
    }
    QMap<int,QList<int>> lanes;
    QMap<QString,QList<int>> links;
    QMap<QString,int> primaryLinks;
    QMap<QString,int> linkAnchors;
    QMap<int,double> durationScales;
    QSet<int> affected;
    QVariantList events;
    bool changed=false;
    for(int at=0;at<entries.size();++at) {
        if(at==sentinel)continue;
        auto old=entries[at].toMap();
        const int track=old.value("track",old.value("lane",0)).toInt();
        const int start=old.value("start").toInt(),frames=old.value("frames",old.value("out").toInt()-old.value("in").toInt()+1).toInt();
        if(track<0 || start<0 || frames<1 || double(start)+frames>100000000)
            return fail(QT_TRANSLATE_NOOP("SourceRateEdit","An existing edit has invalid timeline bounds."));
        lanes[track].append(at);
        const auto link=old.value("link").toString();
        if(!link.isEmpty()) {
            links[link].append(at);
            if(track==0)primaryLinks[link]=primaryLinks.contains(link)?-1:at;
            const int anchor=linkAnchors.value(link,-1);
            const auto other=input.value(anchor).toMap();
            const int otherTrack=other.value("track",other.value("lane",0)).toInt();
            // Visit the lowest track first, then its earliest member. This also
            // gives linked sound-only groups a stable synchronization anchor.
            if(anchor<0 || track<otherTrack || (track==otherTrack && start<other.value("start").toInt()))
                linkAnchors[link]=at;
        }
        const auto path=old.value("path").toString();
        const auto before=previous.value(path,old.value("sourceMeta")).toMap();
        const auto after=sources.value(path,before).toMap();
        durationScales[at]=factor(before)/factor(after);
        if(!qFuzzyCompare(factor(before),factor(after))) {
            if(tracks.value(track).toMap().value("locked").toBool())
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Unlock tracks using this source before changing their playback duration."));
            if(!old.value("compoundJson").toString().isEmpty() || !old.value("mcam").toString().isEmpty()
                || !old.value("original").toMap().isEmpty())
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Keeping selected footage during source-rate changes is not yet supported for nested, multicam or alternate-source clips."));
            old["sourceMeta"]=before;
            QString error;
            auto next=TimelineRate::reinterpretSource(old,after,fps,&error);
            if(next.isEmpty())return {{"valid",false},{"error",error}};
            entries[at]=next;changed=true;affected.insert(at);
            if(track==0 && next.value("frames").toInt()!=frames)
                events<<QVariantMap{{"start",start},{"end",start+frames},{"delta",next.value("frames").toInt()-frames}};
        } else {
            if(changedInside(old,previous,sources))
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Keeping selected footage during source-rate changes is not yet supported for nested, multicam or alternate-source clips."));
            if(sources.contains(path)) {old["sourceMeta"]=after;entries[at]=old;}
        }
    }
    if(!changed)return {{"valid",true},{"error",""},{"changed",false},{"entries",entries},{"events",events}};
    // Independent trims retain their own source selection. Synchronization can
    // be preserved when every member changes its playback duration by the same
    // factor, including sources that already have different interpretations.
    for(auto it=links.begin();it!=links.end();++it) {
        if(std::none_of(it->begin(),it->end(),[&](int at){return affected.contains(at);}))continue;
        if(primaryLinks.value(it.key(),0)<0)
            return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Separate linked groups spanning multiple primary-track clips before changing their source playback duration."));
        const double scale=durationScales.value(linkAnchors.value(it.key()));
        for(int at:*it)if(!qFuzzyCompare(scale,durationScales.value(at)))
            return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Linked clips need the same playback-duration change to retain synchronisation. Change their source rates together or unlink them first."));
        const int anchorStart=input[linkAnchors.value(it.key())].toMap().value("start").toInt();
        for(int at:*it) {
            const double offset=(input[at].toMap().value("start").toInt()-anchorStart)*scale;
            // This timeline stores clip starts in whole frames. Keeping each
            // member's source selection rules out rounding its offset silently.
            if(std::abs(offset-qRound64(offset))>1e-7)
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","The linked offsets cannot stay in sync at this rate. Choose a source-rate change that keeps whole-frame offsets, or unlink the clips to adjust synchronisation separately."));
        }
        const auto anchor=input[linkAnchors.value(it.key())].toMap();
        bool sameTiming=true;
        for(int at:*it)for(const auto &key:QStringList{"start","in","out","retime"})
            sameTiming &= anchor.value(key)==input[at].toMap().value(key);
        if(!sameTiming)for(int at:*it)if(fractionalMapSegments(input[at].toMap(),scale))
            return fail(QT_TRANSLATE_NOOP("SourceRateEdit","This rate cannot preserve synchronisation between these trimmed speed maps. Choose a rate that keeps their speed-segment durations on whole frames."));
    }
    std::sort(events.begin(),events.end(),[](const QVariant &a,const QVariant &b){return a.toMap().value("end").toInt()<b.toMap().value("end").toInt();});
    QList<QPair<int,qint64>> ripples;
    ripples.reserve(events.size());
    qint64 total=0;
    for(const auto &v:events) {
        const auto event=v.toMap();total+=event.value("delta").toInt();
        ripples.append({event.value("end").toInt(),total});
    }
    auto primaryShift=[&](int frame) {
        const auto end=std::upper_bound(ripples.cbegin(),ripples.cend(),frame,
            [](int position,const auto &event){return position<event.first;});
        return end==ripples.cbegin()?qint64(0):(end-1)->second;
    };
    for(auto it=lanes.begin();it!=lanes.end();++it) {
        const int track=it.key();
        auto &indexes=it.value();
        std::sort(indexes.begin(),indexes.end(),[&](int a,int b){return input[a].toMap().value("start").toInt()<input[b].toMap().value("start").toInt();});
        const auto flags=tracks.value(track).toMap();
        qint64 localShift=0,previousEnd=0;
        for(int at:indexes) {
            const auto old=input[at].toMap();auto next=entries[at].toMap();
            const int start=old.value("start").toInt(),count=old.value("frames",old.value("out").toInt()-old.value("in").toInt()+1).toInt();
            const bool follow=track==0 || (!flags.value("locked").toBool() &&
                (flags.value("autoSelect",true).toBool() || primaryLinks.contains(old.value("link").toString())));
            const qint64 inheritedStart=follow?primaryShift(start):0,inheritedEnd=follow?primaryShift(start+count):0;
            qint64 position=start+inheritedStart+localShift;
            const int anchor=linkAnchors.value(old.value("link").toString(),-1);
            if(anchor>=0 && anchor!=at) {
                const int anchorStart=input[anchor].toMap().value("start").toInt();
                position=entries[anchor].toMap().value("start").toLongLong()
                    +qRound64((start-anchorStart)*durationScales.value(anchor));
                // Earlier local changes are absorbed by the gap before this
                // linked member; its independent trim is never conformed back.
                localShift=position-start-inheritedStart;
            }
            const int length=next.value("frames",count).toInt();
            if(position<previousEnd || position+length>100000000)
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","The source-rate change would overlap clips or exceed the timeline limit."));
            if(flags.value("locked").toBool() && position!=start)
                return fail(QT_TRANSLATE_NOOP("SourceRateEdit","Unlock tracks using this source before changing their playback duration."));
            next["start"]=int(position);next["gapBefore"]=int(position-previousEnd);
            entries[at]=next;previousEnd=position+length;
            // Primary changes already appear in the shared ripple events.
            // Changed clips account for their own duration delta, less the
            // primary ripple already crossing that interval. Unchanged
            // overlays must not cancel a ripple for later clips.
            if(track>0 && affected.contains(at))localShift+=length-count-(inheritedEnd-inheritedStart);
        }
    }
    if(sentinel>=0) {
        auto state=entries[sentinel].toMap();auto markers=state.value("omavid:markers").toList();
        for(auto &v:markers){auto marker=v.toMap();const int frame=marker.value("frame").toInt();marker["frame"]=qMax<qint64>(0,frame+primaryShift(frame));v=marker;}
        state["omavid:markers"]=markers;entries[sentinel]=state;
    }
    return {{"valid",true},{"error",""},{"changed",changed},{"entries",entries},{"events",events}};
}
