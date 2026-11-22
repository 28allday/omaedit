#include "audiotimeline.h"
#include "clipentry.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {
constexpr qint64 maxTicks = 9000000000000000LL, maxRate = 1000000000LL;
bool integer(const QVariant &value, qint64 *result) {
    if (value.metaType().id()==QMetaType::Bool || !value.isValid() || value.isNull()) return false;
    bool ok=false; const auto number=value.toLongLong(&ok);
    if (!ok || number < -maxTicks || number > maxTicks) return false;
    if ((value.metaType().id()==QMetaType::Double || value.metaType().id()==QMetaType::Float) && value.toDouble()!=double(number)) return false;
    if (value.metaType().id()==QMetaType::QString && value.toString()!=QString::number(number)) return false;
    *result=number; return true;
}
long double seconds(AudioTimeline::Time time) { return static_cast<long double>(time.ticks)/time.rate; }
AudioTimeline::Time reduced(AudioTimeline::Time time) {
    const auto divisor=std::gcd(time.ticks,time.rate);
    return {time.ticks/divisor,time.rate/divisor};
}
bool valid(AudioTimeline::Time time) { return time.rate>0 && time.rate<=maxRate && time.ticks>=-maxTicks && time.ticks<=maxTicks; }
}

bool AudioTimeline::containsPreciseCuts(const QVariant &value) {
    if (value.metaType().id()==QMetaType::QVariantList) {
        for(const auto &child:value.toList())if(containsPreciseCuts(child))return true;
    } else if(value.metaType().id()==QMetaType::QVariantMap) {
        const auto map=value.toMap();if(map.contains("audioTiming"))return true;
        for(auto it=map.cbegin();it!=map.cend();++it) {
            const auto child=it.key()=="compoundJson" ? QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant() : it.value();
            if(containsPreciseCuts(child))return true;
        }
    }
    return false;
}

bool AudioTimeline::uniqueIds(const QVariantList &entries) {
    QSet<QString> ids;
    for (const auto &value:entries) {
        const auto entry=value.toMap(); const auto id=entry.value(ClipKey::clipId).toString();
        if (id.isEmpty()) { if(entry.contains("audioTiming"))return false; continue; }
        if (ids.contains(id)) return false;
        ids.insert(id);
    }
    return true;
}
QVariantMap AudioTimeline::entryWithId(const QVariantList &entries, const QString &id) {
    if (id.isEmpty()) return {};
    QVariantMap found;
    for (const auto &value:entries) {
        const auto entry=value.toMap();
        if (entry.value(ClipKey::clipId).toString()!=id) continue;
        if (!found.isEmpty()) return {};
        found=entry;
    }
    return found;
}
QString AudioTimeline::editState(QVariantMap entry) {
    for (const char *key:{"track","lane","index","strip","wave","offline"}) entry.remove(key);
    return QString::fromUtf8(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact));
}
bool AudioTimeline::sameEditContext(const QVariantMap &captured, const QVariantMap &current, bool legacy) {
    if (captured.value("session").toString().isEmpty() || captured.value("sequenceId").toString().isEmpty()) return false;
    for (const char *key:{"session","generation","sequenceId","activation","fpsNum","fpsDen"})
        if (!captured.contains(key) || captured.value(key)!=current.value(key)) return false;
    return !legacy || (captured.contains("revision") && captured.value("revision")==current.value("revision"));
}

bool AudioTimeline::readTime(const QVariant &value, Time *time) {
    if (value.metaType().id()!=QMetaType::QVariantMap) return false;
    const auto map=value.toMap(); Time result;
    if (map.size()!=2 || !integer(map.value("ticks"),&result.ticks) || !integer(map.value("rate"),&result.rate) || !valid(result)) return false;
    *time=reduced(result); return true;
}
QVariantMap AudioTimeline::writeTime(Time time) {
    if (!valid(time)) return {};
    time=reduced(time);
    // Decimal strings keep physical sample identities intact across JSON/QML.
    return {{"ticks",QString::number(time.ticks)},{"rate",time.rate}};
}
bool AudioTimeline::readSpan(const QVariantMap &entry, Time *start, Time *duration) {
    const auto timing=entry.value("audioTiming").toMap();
    return readTime(timing.value("start"),start) && readTime(timing.value("duration"),duration)
        && start->ticks>=0 && duration->ticks>0 && add(*start,*duration).has_value();
}
bool AudioTimeline::contains(const QVariantMap &entry, Time position) {
    Time start,duration;
    if(!valid(position) || !readSpan(entry,&start,&duration))return false;
    const auto end=*add(start,duration);
    return __int128(start.ticks)*position.rate<=__int128(position.ticks)*start.rate
        && __int128(position.ticks)*end.rate<__int128(end.ticks)*position.rate;
}
bool AudioTimeline::startsAt(const QVariantMap &entry, Time position) {
    Time start,duration;
    return valid(position) && readSpan(entry,&start,&duration)
        && __int128(start.ticks)*position.rate==__int128(position.ticks)*start.rate;
}
std::optional<AudioTimeline::Time> AudioTimeline::add(Time a, Time b) {
    if (!valid(a) || !valid(b)) return {};
    const qint64 divisor=std::gcd(a.rate,b.rate);
    const __int128 numerator=__int128(a.ticks)*(b.rate/divisor)+__int128(b.ticks)*(a.rate/divisor);
    const __int128 denominator=__int128(a.rate/divisor)*b.rate;
    if (numerator<=std::numeric_limits<qint64>::min() || numerator>std::numeric_limits<qint64>::max()
        || denominator>std::numeric_limits<qint64>::max()) return {};
    const auto result=reduced({qint64(numerator),qint64(denominator)});
    return valid(result) ? std::optional<Time>(result) : std::nullopt;
}
QVariantMap AudioTimeline::timed(QVariantMap entry, Time start, Time in, Time duration, int numerator, int denominator) {
    if (numerator<=0 || denominator<=0 || !valid(start) || !valid(in) || !valid(duration)
        || start.ticks<0 || in.ticks<0 || duration.ticks<=0) return {};
    const auto end=add(start,duration), sourceEnd=add(in,duration);
    if (!end || !sourceEnd) return {};
    const long double fps=static_cast<long double>(numerator)/denominator;
    if (seconds(*end)*fps>100000000 || seconds(*sourceEnd)*fps>100000000) return {};
    const int first=int(std::floor(seconds(start)*fps+1e-12L));
    const int last=int(std::ceil(seconds(*end)*fps-1e-12L));
    const int sourceFirst=int(std::floor(seconds(in)*fps+1e-12L));
    const int sourceLast=int(std::ceil(seconds(*sourceEnd)*fps-1e-12L));
    if (entry.value(ClipKey::clipId).toString().isEmpty()) entry[ClipKey::clipId]=QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry["audioTiming"]=QVariantMap{{"start",writeTime(start)},{"in",writeTime(in)},{"duration",writeTime(duration)}};
    entry["start"]=first; entry["frames"]=qMax(1,last-first);
    entry["in"]=sourceFirst; entry["out"]=qMax(sourceFirst,sourceLast-1);
    entry["audioOnly"]=true; entry["videoOnly"]=false;
    return entry;
}
QVariantMap AudioTimeline::fromFrames(const QVariantMap &entry, int numerator, int denominator) {
    if (entry.contains("audioTiming")) return entry;
    qint64 start,in,out;
    if (!integer(entry.value("start"),&start) || !integer(entry.value("in"),&in) || !integer(entry.value("out"),&out)) return {};
    const auto count=out-in+1;
    if (start<0 || start>100000000 || in<0 || in>100000000 || count<1 || count>100000000 || denominator<=0) return {};
    return timed(entry,{start*denominator,numerator},{in*denominator,numerator},{count*denominator,numerator},numerator,denominator);
}
std::optional<QList<AudioTimeline::Cut>> AudioTimeline::prepare(const QVariantList &entries, int numerator, int denominator, QString *error) {
    if (error) error->clear();
    const auto fail=[&](const char *text)->std::optional<QList<Cut>> {
        if (error) *error=QCoreApplication::translate("AudioTimeline",text);
        return {};
    };
    QList<Cut> cuts; QSet<QString> ids;
    for (const auto &value:entries) {
        Cut cut; cut.entry=value.toMap(); const auto timing=cut.entry.value("audioTiming").toMap();
        const auto id=cut.entry.value(ClipKey::clipId).toString();
        if (value.metaType().id()!=QMetaType::QVariantMap || id.isEmpty() || ids.contains(id)
            || timing.size()!=3 || !readTime(timing.value("start"),&cut.start) || !readTime(timing.value("in"),&cut.in)
            || !readTime(timing.value("duration"),&cut.duration))
            return fail(QT_TRANSLATE_NOOP("AudioTimeline","The sound cut identity or precise timing is invalid."));
        const auto canonical=timed(cut.entry,cut.start,cut.in,cut.duration,numerator,denominator);
        if (canonical.isEmpty()) return fail(QT_TRANSLATE_NOOP("AudioTimeline","The sound cut exceeds the supported time range."));
        // Until an operation uses the rational fields, refuse stale frame edits
        // instead of silently ignoring them or rounding the selected samples.
        for (const char *key:{"start","in","out","frames"}) if (cut.entry.contains(key) && cut.entry.value(key)!=canonical.value(key))
            return fail(QT_TRANSLATE_NOOP("AudioTimeline","This edit changed video-frame bounds without updating the precise sound timing."));
        if (!cut.entry.value("audioOnly").toBool() || cut.entry.value("lane").toInt()<1 || cut.entry.value("path").toString().isEmpty())
            return fail(QT_TRANSLATE_NOOP("AudioTimeline","Precise sound cuts need source media on an audio track."));
        cut.entry=canonical; ids.insert(id); cuts.append(cut);
    }
    std::stable_sort(cuts.begin(),cuts.end(),[](const Cut &a,const Cut &b) {
        return __int128(a.start.ticks)*b.start.rate<__int128(b.start.ticks)*a.start.rate;
    });
    return cuts;
}
QVariantMap AudioTimeline::shifted(const QVariantMap &entry, Time delta, int numerator, int denominator) {
    const auto cuts=prepare({entry},numerator,denominator); if (!cuts || cuts->isEmpty()) return {};
    const auto &cut=cuts->first(); const auto start=add(cut.start,delta); if (!start) return {};
    return timed(entry,*start,cut.in,cut.duration,numerator,denominator);
}
QVariantList AudioTimeline::split(const QVariantMap &entry, Time position, int numerator, int denominator) {
    const auto cuts=prepare({entry},numerator,denominator); if (!cuts || cuts->isEmpty()) return {};
    const auto &cut=cuts->first(); const auto head=add(position,{-cut.start.ticks,cut.start.rate});
    if (!head || head->ticks<=0) return {};
    const auto tail=add(cut.duration,{-head->ticks,head->rate}), source=add(cut.in,*head);
    if (!tail || tail->ticks<=0 || !source) return {};
    auto first=timed(entry,cut.start,cut.in,*head,numerator,denominator);
    auto second=entry; second.remove(ClipKey::clipId); second=timed(second,position,*source,*tail,numerator,denominator);
    return first.isEmpty() || second.isEmpty() ? QVariantList{} : QVariantList{first,second};
}
