#include "timelinerate.h"
#include "clipentry.h"
#include "retimemap.h"
#include "sourcetiming.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cmath>
#include <algorithm>

namespace {
double interpretation(const QVariantMap &entry) {
    const auto meta=entry.value("sourceMeta").toMap();
    const double recorded=meta.value("sourceFps",meta.value("fps")).toDouble();
    const double forced=meta.value("forceFps").toDouble();
    return recorded>0 && forced>0 ? forced/recorded : 1.;
}
QVariant scaleKeys(const QVariant &value,double ratio) {
    if(value.metaType().id()==QMetaType::QVariantList) {
        const auto input=value.toList();
        const bool curve=!input.isEmpty() && std::all_of(input.begin(),input.end(),[](const QVariant &v){return v.toMap().contains("frame");});
        QVariantList list;QMap<int,QVariant> unique;
        for(const auto &v:input) {
            const auto scaled=scaleKeys(v,ratio);
            if(curve)unique[scaled.toMap().value("frame").toInt()]=scaled;
            else list<<scaled;
        }
        if(curve)for(const auto &v:unique)list<<v;
        return list;
    }
    if(value.metaType().id()!=QMetaType::QVariantMap)return value;
    auto map=value.toMap();
    for(auto it=map.begin();it!=map.end();++it)
        it.value()=it.key()=="frame" ? QVariant(qRound64(it.value().toDouble()*ratio)) : scaleKeys(it.value(),ratio);
    return map;
}
bool rescaleMap(const QVariantMap &definition,double sourceScale,int frames,QVariantList *result) {
    RetimeMap map;if(!map.set(definition) || frames<1)return false;
    const auto segments=map.definition().value("segments").toList();
    if(segments.size()>frames)return false;
    const double ratio=double(frames)/map.frames();int before=0,after=0,index=0;
    for(const auto &v:segments) {
        auto segment=v.toMap();const int count=segment.value("frames").toInt();
        ++index;
        const int end=qBound(after+1,qRound((before+count)*ratio),frames-int(segments.size())+index);
        const int length=end-after;
        const auto point=map.sample(before);
        segment["source"]=point.source*sourceScale;
        segment["frames"]=length;
        segment["speed"]=segment.value("speed").toDouble()*count*sourceScale/length;
        segment["endSpeed"]=segment.value("endSpeed").toDouble()*count*sourceScale/length;
        result->append(segment);before+=count;after=end;
    }
    return true;
}
}

double TimelineRate::sourceRate(const QVariantMap &entry) {
    if(entry.contains(ClipKey::timelineFps))return entry.value(ClipKey::timelineFps).toDouble();
    return entry.value(ClipKey::sourceTiming).toMap().value("rate").toDouble()*interpretation(entry);
}

QVariantMap TimelineRate::reinterpretSource(const QVariantMap &entry,const QVariantMap &metadata,
                                           double fps,QString *error) {
    auto timed=entry;
    timed[ClipKey::timelineFps]=fps;
    const double previous=interpretation(entry);
    const double next=interpretation({{"sourceMeta",metadata}});
    // A virtual destination clock converts the output duration without changing
    // the actual project clock. Source coordinates then use fps/next, while
    // user speed remains independent of the source interpretation.
    auto result=convert(timed,fps*previous/next,entry.value(ClipKey::start).toDouble()/fps,error,previous);
    if(result.isEmpty())return {};
    result[ClipKey::timelineFps]=fps;
    result[ClipKey::sourceMeta]=metadata;
    result[ClipKey::start]=entry.value(ClipKey::start,0);
    return result;
}

QVariantMap TimelineRate::convert(const QVariantMap &entry,double destination,double originSeconds,
                                 QString *error,double destinationInterpretation) {
    if(error)error->clear();
    auto fail=[&](const char *message) {
        if(error)*error=QCoreApplication::translate("TimelineRate",message);
        return QVariantMap{};
    };
    if(entry.contains(ClipKey::audioTiming))
        return fail(QT_TRANSLATE_NOOP("TimelineRate","Precise sound cuts cannot be copied or reinterpreted through video-frame timing yet."));
    const double oldRate=sourceRate(entry);
    // Older/external clipboard entries without a timebase retain the existing
    // destination-frame contract. Live snapshots always publish their clock.
    if(oldRate==0 && !entry.contains(ClipKey::timelineFps))return entry;
    const double oldFactor=interpretation(entry),factor=destinationInterpretation>0?destinationInterpretation:oldFactor;
    if(!std::isfinite(oldRate) || oldRate<=0 || !std::isfinite(destination) || destination<=0
        || !std::isfinite(originSeconds) || !std::isfinite(oldFactor) || oldFactor<=0
        || !std::isfinite(factor) || factor<=0)
        return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
    const double ratio=destination/oldRate,sourceScale=ratio*oldFactor/factor;
    const double oldStart=entry.value(ClipKey::start).toDouble();
    const int oldIn=entry.value(ClipKey::in).toInt(),oldOut=entry.value(ClipKey::out).toInt();
    const int visible=entry.value(ClipKey::frames,oldOut-oldIn+1).toInt();
    const double start=(oldStart/oldRate-originSeconds)*destination;
    const double end=start+visible*ratio;
    if(oldIn<0 || oldOut<oldIn || visible<1 || !std::isfinite(end) || std::abs(start)>100000000 || std::abs(end)>100000000
        || oldOut*ratio>100000000 || entry.value(ClipKey::length).toDouble()*ratio>100000000)
        return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
    auto result=entry;result[ClipKey::timelineFps]=destination;
    const int first=int(std::llround(start)),frames=qMax(1,int(std::llround(end))-first);
    result[ClipKey::start]=first;result[ClipKey::frames]=frames;
    if(qFuzzyCompare(ratio,1.) && qFuzzyCompare(oldFactor,factor))return result;
    if(!entry.value(ClipKey::mcam).toString().isEmpty() || entry.value(ClipKey::mixIn).toInt()>0 || entry.value(ClipKey::mixOut).toInt()>0)
        return fail(QT_TRANSLATE_NOOP("TimelineRate","Changing frame rates for multicam clips or transitions is not supported yet."));
    if(int(std::llround(end))-first<1)
        return fail(QT_TRANSLATE_NOOP("TimelineRate","This change would make a clip shorter than one timeline frame."));
    auto timing=entry.value(ClipKey::sourceTiming).toMap();
    if(!timing.isEmpty()){timing["rate"]=destination/factor;result[ClipKey::sourceTiming]=timing;}
    for(const auto &key:QStringList{"gapBefore","mixIn","mixOut","mixExtA","mixExtB","fadeIn","fadeOut"})
        if(entry.contains(key))result[key]=qRound(entry.value(key).toInt()*ratio);
    const int span=frames+result.value(ClipKey::mixIn).toInt();
    int in=0,length=0;
    if(!entry.value(ClipKey::retime).toMap().isEmpty()) {
        RetimeMap parent;if(!parent.set(entry.value(ClipKey::retime).toMap(),error))return {};
        if(oldOut>=parent.frames())return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
        QVariantList parts;
        in=qRound(oldIn*ratio);
        if((in>0 && !rescaleMap(parent.window(0,oldIn),sourceScale,in,&parts))
            || !rescaleMap(parent.window(oldIn,oldOut-oldIn+1),sourceScale,span,&parts))
            return fail(QT_TRANSLATE_NOOP("TimelineRate","The new timing cannot retain every speed segment. Join short segments before changing the frame rate."));
        const int tail=parent.frames()-oldOut-1,tailFrames=qRound(tail*ratio);
        if(tail>0 && tailFrames>0 && !rescaleMap(parent.window(oldOut+1,tail),sourceScale,tailFrames,&parts))
            return fail(QT_TRANSLATE_NOOP("TimelineRate","The new timing cannot retain every speed segment. Join short segments before changing the frame rate."));
        RetimeMap mapped;
        if(!mapped.set({{"sourceIn",parts.first().toMap().value("source")},{"sourceLength",parent.sourceLength()*sourceScale},{"segments",parts}},error))return {};
        result[ClipKey::retime]=mapped.definition();length=mapped.frames();
    } else {
        const double speed=entry.value(ClipKey::speed,1.).toDouble(),effective=std::abs(speed)*oldFactor;
        std::optional<double> origin;
        if(!SourceTiming::readOrigin(entry,origin) || !std::isfinite(speed) || effective<=0)
            return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
        const bool media=!entry.value(ClipKey::gap).toBool() && entry.value(ClipKey::title).toString().isEmpty()
            && !entry.value(ClipKey::still).toBool()
            && entry.value(ClipKey::generator).toString().isEmpty() && !entry.contains(ClipKey::compoundJson)
            && !entry.value(ClipKey::path).toString().startsWith("color:");
        if(media) {
            const double position=(origin.value_or(0.)+oldIn*effective/oldRate)*destination/effective;
            if(!std::isfinite(position) || position>100000000)
                return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
            in=int(std::floor(position+1e-9));
            result[ClipKey::sourceOrigin]=qMax(0.,position-in)*effective/destination;
            result[ClipKey::speed]=speed*oldFactor/factor;
            const double duration=timing.value("duration").toDouble();
            if(!std::isfinite(duration) || duration*destination/effective>100000000)
                return fail(QT_TRANSLATE_NOOP("TimelineRate","The clip has invalid timing for this frame-rate change."));
            length=duration>0 ? qMax(1,int(std::ceil((duration-result.value(ClipKey::sourceOrigin).toDouble())*destination/effective-1e-9)))
                : qMax(in+span,qRound(entry.value(ClipKey::length).toInt()*ratio));
        } else {in=qRound(oldIn*ratio);length=qMax(in+span,qRound(entry.value(ClipKey::length).toInt()*ratio));}
    }
    result[ClipKey::in]=in;result[ClipKey::out]=in+span-1;result[ClipKey::length]=qMax(length,in+span);
    if(entry.contains(ClipKey::freeze))result[ClipKey::freeze]=in+qRound((entry.value(ClipKey::freeze).toInt()-oldIn)*ratio);
    auto markers=entry.value(ClipKey::markers).toList();
    for(auto &v:markers){auto m=v.toMap();m["frame"]=in+qRound((m.value("frame").toInt()-oldIn)*ratio);v=m;}
    if(entry.contains(ClipKey::markers))result[ClipKey::markers]=markers;
    for(const auto &key:QStringList{"editKeys","opacityKf","volumeKf","grade","gradeVersions","stab","videoEffects"})
        if(entry.contains(key))result[key]=scaleKeys(entry.value(key),ratio);
    if(entry.contains(ClipKey::compoundJson)) {
        QVariantList children;
        for(const auto &v:QJsonDocument::fromJson(entry.value(ClipKey::compoundJson).toString().toUtf8()).toVariant().toList()) {
            auto child=v.toMap();if(!child.contains(ClipKey::timelineFps))child[ClipKey::timelineFps]=oldRate;
            const auto converted=convert(child,destination,0.,error);
            if(converted.isEmpty())return {};
            children<<converted;
        }
        result[ClipKey::compoundJson]=QString::fromUtf8(QJsonDocument::fromVariant(children).toJson(QJsonDocument::Compact));
    }
    if(entry.contains(ClipKey::original)) {
        auto original=entry.value(ClipKey::original).toMap();if(!original.contains(ClipKey::timelineFps))original[ClipKey::timelineFps]=oldRate;
        const auto converted=convert(original,destination,originSeconds,error);
        if(converted.isEmpty())return {};
        result[ClipKey::original]=converted;
    }
    return result;
}
