#include "audiotimingedit.h"
#include "audiotimeline.h"
#include <QCoreApplication>
#include <QRegularExpression>

namespace {
using Time = AudioTimeline::Time;
using Wide = __int128;
Wide gcd(Wide a, Wide b) {
    if (a < 0) a = -a;
    while (b) { const Wide r = a % b; a = b; b = r; }
    return a;
}
QString number(Wide value) {
    const bool negative = value < 0;
    if (negative) value = -value;
    QString text;
    do { text.prepend(QChar('0' + int(value % 10))); value /= 10; } while (value);
    return negative ? "-" + text : text;
}
Wide digits(const QString &text) {
    Wide value = 0;
    for (const QChar c : text) value = value * 10 + c.digitValue();
    return value;
}
std::optional<Time> parse(QString text, int scale) {
    text = text.trimmed();
    static const QRegularExpression decimal(QStringLiteral("^[+-]?(?:[0-9]{1,24}(?:\\.[0-9]{0,9})?|\\.[0-9]{1,9})$"));
    static const QRegularExpression fraction(QStringLiteral("^([+-]?[0-9]{1,24})/([0-9]{1,12})$"));
    Wide numerator = 0, denominator = 1;
    const auto match = fraction.match(text);
    const bool isDecimal = decimal.match(text).hasMatch();
    const bool negative = text.startsWith('-');
    if (text.startsWith('-') || text.startsWith('+')) text.remove(0, 1);
    if (match.hasMatch()) {
        const auto parts = text.split('/');
        numerator = digits(parts[0]); denominator = digits(parts[1]);
        if (!denominator) return {};
    } else if (isDecimal) {
        const int point = text.indexOf('.');
        if (point >= 0) {
            for (int i = point + 1; i < text.size(); ++i) denominator *= 10;
            text.remove(point, 1);
        }
        numerator = digits(text);
    } else return {};
    if (negative) numerator = -numerator;
    denominator *= scale;
    const auto divisor = gcd(numerator, denominator);
    numerator /= divisor; denominator /= divisor;
    if (numerator < -9000000000000000LL || numerator > 9000000000000000LL
        || denominator > 1000000000LL) return {};
    return Time{qint64(numerator), qint64(denominator)};
}
QString format(Time time, int scale) {
    Wide numerator = Wide(time.ticks) * scale, denominator = time.rate;
    const auto divisor = gcd(numerator, denominator);
    numerator /= divisor; denominator /= divisor;
    if (denominator == 1) return number(numerator);
    const bool negative = numerator < 0;
    const Wide absolute = negative ? -numerator : numerator;
    QString result = (negative ? "-" : "") + number(absolute / denominator);
    Wide remainder = absolute % denominator;
    result += '.';
    for (int i = 0; i < 9 && remainder; ++i) {
        remainder *= 10;
        result += QChar('0' + int(remainder / denominator));
        remainder %= denominator;
    }
    // Recurring values retain their exact fraction instead of silently rounding
    // another field when a dialog is applied or its display unit is changed.
    return remainder ? number(numerator) + '/' + number(denominator) : result;
}
}

QVariantMap AudioTimingEdit::parsePosition(const QString &text, const QString &unit, int sampleRate) {
    if ((unit != "samples" && unit != "milliseconds") || sampleRate < 1 || sampleRate > 768000) return {};
    const auto time = parse(text, unit == "samples" ? sampleRate : 1000);
    return time ? AudioTimeline::writeTime(*time) : QVariantMap{};
}

QString AudioTimingEdit::formatPosition(const QVariantMap &value, const QString &unit, int sampleRate) {
    Time time;
    if ((unit != "samples" && unit != "milliseconds") || sampleRate < 1 || sampleRate > 768000
        || !AudioTimeline::readTime(value, &time)) return {};
    return format(time, unit == "samples" ? sampleRate : 1000);
}

QVariantMap AudioTimingEdit::gesture(const QVariantMap &entry, int num, int den,
                                     const QString &action, const QVariantMap &changes, int sampleRate) {
    const auto fail=[](const char *text) { return QVariantMap{{"valid",false},{"error",QCoreApplication::translate("AudioTimingEdit",text)}}; };
    const auto source=AudioTimeline::fromFrames(entry,num,den);
    const auto timing=source.value("audioTiming").toMap();
    Time start,in,duration,delta;
    if (num<1 || den<1 || sampleRate<1 || sampleRate>768000 || source.isEmpty()
        || !AudioTimeline::readTime(timing.value("start"),&start)
        || !AudioTimeline::readTime(timing.value("in"),&in)
        || !AudioTimeline::readTime(timing.value("duration"),&duration))
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The selected sound cut has invalid timing."));
    const auto compare=[](Time a,Time b) { const auto d=Wide(a.ticks)*b.rate-Wide(b.ticks)*a.rate; return d<0?-1:d>0?1:0; };
    const auto subtract=[](Time a,Time b) { return AudioTimeline::add(a,{-b.ticks,b.rate}); };
    const auto end=AudioTimeline::add(start,duration);
    if (!end) return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound cut exceeds the supported time range."));
    if (action=="split") {
        Time at;
        if (!AudioTimeline::readTime(changes.value("position"),&at)
            || compare(at,start)<=0 || compare(at,*end)>=0)
            return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","Choose a position inside the sound cut."));
        const auto halves=AudioTimeline::split(source,at,num,den);
        if (halves.size()!=2) return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound cut exceeds the supported time range."));
        return {{"valid",true},{"changed",true},{"position",AudioTimeline::writeTime(at)},
                {"entry",source},{"end",AudioTimeline::writeTime(*end)}};
    }
    if ((action!="move" && action!="trimIn" && action!="trimOut" && action!="slip")
        || !AudioTimeline::readTime(changes.value("delta"),&delta))
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound gesture is invalid."));
    const bool ripple=changes.value("ripple").toBool();
    if (ripple && action!="trimIn" && action!="trimOut")
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","Only a trim can ripple the audio track."));
    if (changes.contains("snap")) {
        Time snap;
        const auto edge=changes.value("edge").toString();
        if (action=="slip" || !AudioTimeline::readTime(changes.value("snap"),&snap)
            || (edge!="start" && edge!="end"))
            return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound snap position is invalid."));
        const auto snapped=subtract(snap,edge=="end"?*end:start);
        if (!snapped) return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound cut exceeds the supported time range."));
        delta=*snapped;
    }
    Time nextStart=start,nextIn=in,nextDuration=duration;
    auto assign=[](Time &to,const std::optional<Time> &value) { if(!value)return false;to=*value;return true; };
    bool ok=true;
    if (action=="move") ok=assign(nextStart,AudioTimeline::add(start,delta));
    if (action=="trimIn") {
        ok=assign(nextIn,AudioTimeline::add(in,delta)) && assign(nextDuration,subtract(duration,delta));
        if (!ripple) ok=ok && assign(nextStart,AudioTimeline::add(start,delta));
    }
    if (action=="trimOut") ok=assign(nextDuration,AudioTimeline::add(duration,delta));
    if (action=="slip") ok=assign(nextIn,subtract(in,delta));
    if (!ok) return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound cut exceeds the supported time range."));
    if (nextStart.ticks<0 || nextIn.ticks<0 || nextDuration.ticks<=0)
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound edit goes beyond its available handles."));
    const auto sourceEnd=AudioTimeline::add(nextIn,nextDuration);
    Time available;
    if(entry.contains("audioSourceDuration")) {
        if(!AudioTimeline::readTime(entry.value("audioSourceDuration"),&available) || available.ticks<=0)
            return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The selected sound cut has invalid timing."));
    } else if(entry.value("length").toLongLong()>0)
        available={qMin<qint64>(entry.value("length").toLongLong(),100000000)*den,num};
    if (!sourceEnd || (available.ticks>0 && compare(*sourceEnd,available)>0))
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound edit goes beyond its available handles."));
    const auto result=AudioTimeline::timed(source,nextStart,nextIn,nextDuration,num,den);
    const auto resultEnd=AudioTimeline::add(nextStart,nextDuration),rippleDelta=subtract(nextDuration,duration);
    if (result.isEmpty() || !resultEnd || !rippleDelta)
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit","The sound cut exceeds the supported time range."));
    return {{"valid",true},{"changed",result.value("audioTiming")!=source.value("audioTiming")},
        {"timing",result.value("audioTiming")},{"entry",result},{"end",AudioTimeline::writeTime(*resultEnd)},
        {"ripplePivot",AudioTimeline::writeTime(*end)},
        {"rippleDelta",AudioTimeline::writeTime(ripple?*rippleDelta:Time{0,1})}};
}

QVariantMap AudioTimingEdit::fields(const QVariantMap &timing, const QString &unit, int sampleRate) {
    if ((unit != "samples" && unit != "milliseconds") || sampleRate < 1 || sampleRate > 768000) return {};
    QVariantMap result;
    for (const char *key : {"start", "in", "duration"}) {
        Time value;
        if (!AudioTimeline::readTime(timing.value(key), &value)) return {};
        result[key] = format(value, unit == "samples" ? sampleRate : 1000);
    }
    return result;
}

QVariantMap AudioTimingEdit::preview(const QVariantMap &entry, int fpsNum, int fpsDen,
                                    const QVariantMap &fields, const QString &unit, int sampleRate) {
    const auto fail = [](const char *error) { return QVariantMap{{"valid", false}, {"error", QCoreApplication::translate("AudioTimingEdit", error)}}; };
    if ((unit != "samples" && unit != "milliseconds") || sampleRate < 1 || sampleRate > 768000
        || fpsNum < 1 || fpsDen < 1)
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "The sound timing unit or clock is invalid."));
    const auto source = AudioTimeline::fromFrames(entry, fpsNum, fpsDen);
    const auto timing = source.value("audioTiming").toMap();
    Time start, in, duration;
    if (!AudioTimeline::readTime(timing.value("start"), &start)
        || !AudioTimeline::readTime(timing.value("in"), &in)
        || !AudioTimeline::readTime(timing.value("duration"), &duration)
        || AudioTimeline::timed(source, start, in, duration, fpsNum, fpsDen).isEmpty())
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "The selected sound cut has invalid timing."));
    const int scale = unit == "samples" ? sampleRate : 1000;
    auto result = timing;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (it.key() != "start" && it.key() != "in" && it.key() != "duration")
            return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "The sound timing field is invalid."));
        const auto value = it.value().metaType().id() == QMetaType::QString ? parse(it.value().toString(), scale) : std::nullopt;
        if (!value)
            return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "Enter a whole number, decimal or fraction within the supported time range."));
        result[it.key()] = AudioTimeline::writeTime(*value);
    }
    Time nextStart, nextIn, nextDuration;
    AudioTimeline::readTime(result.value("start"), &nextStart);
    AudioTimeline::readTime(result.value("in"), &nextIn);
    AudioTimeline::readTime(result.value("duration"), &nextDuration);
    if (nextStart.ticks < 0 || nextIn.ticks < 0 || nextDuration.ticks <= 0)
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "Position and source in must be non-negative; duration must be greater than zero."));
    if (AudioTimeline::timed(source, nextStart, nextIn, nextDuration, fpsNum, fpsDen).isEmpty())
        return fail(QT_TRANSLATE_NOOP("AudioTimingEdit", "The sound cut exceeds the supported time range."));
    const bool changed = AudioTimeline::writeTime(start) != AudioTimeline::writeTime(nextStart)
        || AudioTimeline::writeTime(in) != AudioTimeline::writeTime(nextIn)
        || AudioTimeline::writeTime(duration) != AudioTimeline::writeTime(nextDuration);
    return {{"valid", true}, {"changed", changed}, {"timing", result},
        {"fields", AudioTimingEdit::fields(result, unit, sampleRate)}};
}
