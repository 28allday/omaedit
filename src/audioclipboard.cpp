#include "audioclipboard.h"
#include "clipentry.h"
#include <QCoreApplication>
#include <QUuid>
#include <algorithm>

namespace {
using Time = AudioTimeline::Time;
int compare(Time a,Time b) {
    const auto d=__int128(a.ticks)*b.rate-__int128(b.ticks)*a.rate;
    return d<0?-1:d>0?1:0;
}
std::optional<Time> subtract(Time a,Time b) { return AudioTimeline::add(a,{-b.ticks,b.rate}); }
struct Interval { Time start,end; };
QVariantMap fragment(const AudioTimeline::Cut &cut,Time start,Time end,
                     bool newIdentity,int num,int den) {
    const auto offset=subtract(start,cut.start),duration=subtract(end,start);
    const auto in=offset?AudioTimeline::add(cut.in,*offset):std::nullopt;
    if(!in || !duration)return {};
    auto entry=cut.entry;
    if(newIdentity)entry.remove(ClipKey::clipId);
    return AudioTimeline::timed(entry,start,*in,*duration,num,den);
}
}

std::optional<AudioClipboard::Placement> AudioClipboard::prepare(
    const QVariantList &copies,const QMap<int,QVariantList> &destinations,
    int sourceNum,int sourceDen,int num,int den,Time position,bool insert,
    QString *error,std::optional<Span> group) {
    if(error)error->clear();
    const auto fail=[&](const char *text)->std::optional<Placement> {
        if(error)*error=QCoreApplication::translate("AudioClipboard",text);
        return {};
    };
    const auto invalid=[&] {return fail(QT_TRANSLATE_NOOP("AudioClipboard","The copied sound or destination timing is invalid."));};
    if(copies.isEmpty() || destinations.isEmpty() || sourceNum<1 || sourceDen<1 || num<1 || den<1
        || position.ticks<0 || AudioTimeline::writeTime(position).isEmpty())return invalid();
    QVariantList sources;
    for(const auto &value:copies) {
        const auto entry=value.toMap();
        if(value.metaType().id()!=QMetaType::QVariantMap || !entry.value("audioOnly").toBool()
            || entry.value("gap").toBool())return invalid();
        const auto converted=AudioTimeline::fromFrames(entry,sourceNum,sourceDen);
        if(converted.isEmpty())return invalid();
        sources.append(converted);
    }
    const auto cuts=AudioTimeline::prepare(sources,sourceNum,sourceDen,error);
    if(!cuts || cuts->isEmpty())return {};
    Time origin=cuts->first().start,end=*AudioTimeline::add(origin,cuts->first().duration);
    for(const auto &cut:*cuts) {
        const auto tail=*AudioTimeline::add(cut.start,cut.duration);
        if(compare(tail,end)>0)end=tail;
    }
    if(group) {
        const auto tail=AudioTimeline::add(group->start,group->duration);
        if(group->start.ticks<0 || group->duration.ticks<=0 || !tail
            || compare(group->start,origin)>0 || compare(*tail,end)<0)return invalid();
        origin=group->start;end=*tail;
    }
    const auto duration=subtract(end,origin),delta=subtract(position,origin);
    if(!duration || !delta || !AudioTimeline::add(position,*duration))return invalid();
    Placement result;result.duration=*duration;
    auto &links=result.links;
    QMap<int,QList<Interval>> occupied;
    for(const auto &cut:*cuts) {
        const int track=cut.entry.value("track",cut.entry.value("lane")).toInt();
        if(track<1 || !destinations.contains(track) || cut.entry.value("lane").toInt()!=track)return invalid();
        const auto start=AudioTimeline::add(cut.start,*delta);
        if(!start)return invalid();
        auto entry=cut.entry;
        // Keep source handles in their original rational clock. Repeated copies
        // must not grow them by rounding to each intermediate project's frames.
        Time available;
        if(entry.contains("audioSourceDuration")) {
            if(!AudioTimeline::readTime(entry.value("audioSourceDuration"),&available) || available.ticks<=0)return invalid();
        } else if(entry.value("length").toLongLong()>0) {
            bool ok=false;const auto length=entry.value("length").toLongLong(&ok);
            if(!ok || length>100000000)return invalid();
            available={length*sourceDen,sourceNum};
            const auto exact=AudioTimeline::writeTime(available);
            if(exact.isEmpty() || !AudioTimeline::readTime(exact,&available))return invalid();
            entry["audioSourceDuration"]=exact;
        }
        if(available.ticks>0) {
            const auto n=__int128(available.ticks)*num,d=__int128(available.rate)*den;
            const auto frames=(n+d-1)/d;
            if(frames>100000000)return invalid();
            entry["length"]=int(frames);
        }
        const auto link=entry.value("link").toString();
        if(!link.isEmpty()) {
            if(!links.contains(link))links[link]=QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry["link"]=links[link];
        }
        for(const char *key:{"clipId","strip","wave","offline","gapBefore"})entry.remove(key);
        entry["timelineFps"]=double(num)/den;
        entry["track"]=track;entry["lane"]=track;
        entry=AudioTimeline::timed(entry,*start,cut.in,cut.duration,num,den);
        if(entry.isEmpty())return invalid();
        result.added.append(entry);
        occupied[track].append({*start,*AudioTimeline::add(*start,cut.duration)});
    }
    for(auto it=destinations.cbegin();it!=destinations.cend();++it) {
        if(it.key()<1)return invalid();
        QVariantList current;
        for(const auto &value:it.value()) {
            const auto entry=AudioTimeline::fromFrames(value.toMap(),num,den);
            if(entry.isEmpty() || !entry.value("audioOnly").toBool()
                || entry.value("lane").toInt()!=it.key()
                || entry.value("track",it.key()).toInt()!=it.key())return invalid();
            current.append(entry);
        }
        const auto before=AudioTimeline::prepare(current,num,den,error);
        if(!before)return {};
        auto intervals=occupied.value(it.key());
        std::sort(intervals.begin(),intervals.end(),[](const Interval &a,const Interval &b){return compare(a.start,b.start)<0;});
        QList<Interval> merged;
        for(const auto &interval:intervals) {
            if(merged.isEmpty() || compare(merged.last().end,interval.start)<0)merged.append(interval);
            else if(compare(interval.end,merged.last().end)>0)merged.last().end=interval.end;
        }
        auto &after=result.tracks[it.key()];
        for(const auto &cut:*before) {
            const auto tail=*AudioTimeline::add(cut.start,cut.duration);
            if(insert) {
                if(compare(tail,position)<=0)after.append(cut.entry);
                else if(compare(cut.start,position)>=0) {
                    const auto moved=AudioTimeline::shifted(cut.entry,*duration,num,den);
                    if(moved.isEmpty())return invalid();
                    after.append(moved);
                } else {
                    const auto halves=AudioTimeline::split(cut.entry,position,num,den);
                    if(halves.size()!=2)return invalid();
                    const auto moved=AudioTimeline::shifted(halves.last().toMap(),*duration,num,den);
                    if(moved.isEmpty())return invalid();
                    after.append(halves.first());after.append(moved);
                }
                continue;
            }
            Time cursor=cut.start;bool retained=false;
            for(const auto &interval:merged) {
                if(compare(interval.end,cursor)<=0)continue;
                if(compare(interval.start,tail)>=0)break;
                if(compare(cursor,interval.start)<0) {
                    const auto part=fragment(cut,cursor,interval.start,retained,num,den);
                    if(part.isEmpty())return invalid();
                    after.append(part);retained=true;
                }
                cursor=compare(interval.end,tail)<0?interval.end:tail;
                if(compare(cursor,tail)>=0)break;
            }
            if(compare(cursor,tail)<0) {
                const auto part=fragment(cut,cursor,tail,retained,num,den);
                if(part.isEmpty())return invalid();
                after.append(part);
            }
        }
    }
    for(const auto &entry:result.added)result.tracks[entry.toMap().value("track").toInt()].append(entry);
    QVariantList all;
    for(auto it=result.tracks.begin();it!=result.tracks.end();++it) {
        const auto final=AudioTimeline::prepare(it.value(),num,den,error);
        if(!final)return {};
        it.value().clear();
        for(const auto &cut:*final)it.value().append(cut.entry);
        all.append(it.value());
    }
    if(!AudioTimeline::uniqueIds(all))return invalid();
    return result;
}
