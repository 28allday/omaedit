#include "retimeedit.h"
#include "sourcetiming.h"
#include <QCoreApplication>
#include <QtMath>
#include <cmath>
#include <limits>

namespace RetimeEdit {
namespace {
QVariantMap fail(const char *message) {
    return {{"valid",false},{"error",QCoreApplication::translate("SpeedDuration",message)}};
}
bool fits(double frames) {
    return std::isfinite(frames) && frames >= 0 && frames <= std::numeric_limits<int>::max()-1.0;
}
}
QVariantMap preview(const QVariantMap &entry, double speed) {
    if (!std::isfinite(speed) || qAbs(speed) < 0.05 || qAbs(speed) > 20.0)
        return fail("Speed must be between 5% and 2000%.");
    if (entry.isEmpty() || entry.value("gap").toBool() || entry.contains("compoundJson") || !entry.value("title").toString().isEmpty())
        return fail("Choose a media clip to change its speed or duration.");
    if (!entry.value("retime").toMap().isEmpty())
        return fail(QT_TRANSLATE_NOOP("SpeedDuration","Edit this clip's speed segments to change its timing."));
    const double current = entry.value("speed",1.0).toDouble(), ca = qAbs(current), rate = qAbs(speed);
    const qint64 oldIn = entry.value("in",-1).toLongLong(), oldOut = entry.value("out",-1).toLongLong();
    const qint64 length = entry.value("length").toLongLong();
    if (!std::isfinite(current) || ca < 0.05 || ca > 20 || oldIn < 0 || oldOut < oldIn || length < 1
        || oldOut >= length || !fits(oldIn*ca) || !fits((oldOut+1)*ca) || !fits(length*ca))
        return fail("The clip has invalid source bounds for retiming.");
    if (entry.contains("sourceOrigin")) {
        std::optional<double> origin;
        const auto timing=entry.value("sourceTiming").toMap();
        const double clock=timing.value("rate").toDouble(), duration=timing.value("duration").toDouble();
        if (!SourceTiming::readOrigin(entry,origin) || !origin || !std::isfinite(clock) || clock<=0
            || !std::isfinite(duration) || duration<=*origin || !fits(duration*clock))
            return fail("The clip has invalid source bounds for retiming.");
        const double full=duration*clock, offset=*origin*clock;
        // The origin is a distance along playback: from the beginning for
        // forward clips and from the exclusive end for reverse clips.
        const double first=offset+oldIn*ca, end=qMin(full,offset+(oldOut+1)*ca);
        if (first>=full || end<=first) return fail("The clip has invalid source bounds for retiming.");
        const double sourceIn=current<0 ? full-end : first;
        const double sourceEnd=current<0 ? full-first : end;
        const double distance=speed<0 ? full-sourceEnd : sourceIn;
        if (!fits(distance/rate) || !fits(full/rate))
            return fail("The retimed source exceeds the supported frame range.");
        int in=int(std::floor(distance/rate+1e-9));
        double phase=qMax(0.,distance-in*rate);
        int newLength=qMax(1,int(std::ceil((full-phase)/rate-1e-9)));
        int frames=qMin(newLength-in,qMax(1,int(std::llrint((sourceEnd-sourceIn)/rate))));
        if (qFuzzyCompare(current,speed)) {
            in=int(oldIn);frames=int(oldOut-oldIn+1);newLength=int(length);phase=offset;
        }
        return {{"valid",true},{"error",""},{"speed",speed},{"in",in},{"out",in+frames-1},
            {"length",newLength},{"frames",frames},{"delta",frames-(oldOut-oldIn+1)},
            {"sourceIn",sourceIn},{"sourceEnd",sourceEnd},{"sourceOrigin",phase/clock},
            {"flip",(current<0)!=(speed<0)}};
    }
    const int sourceIn = qRound(oldIn*ca), sourceEnd = qRound((oldOut+1)*ca), sourceLength = qRound(length*ca);
    if (!fits(sourceIn/rate) || !fits(sourceEnd/rate) || !fits(sourceLength/rate))
        return fail("The retimed source exceeds the supported frame range.");
    int in = int(sourceIn/rate), out = qMax(in,int(sourceEnd/rate)-1);
    int newLength = qMax(1,int(sourceLength/rate));
    const bool flip = (current < 0) != (speed < 0);
    if (flip) {
        const int mirroredIn = qMax(0,newLength-1-out);
        out = qMax(mirroredIn,newLength-1-in);
        in = mirroredIn;
    }
    if (qFuzzyCompare(current,speed)) {
        in = int(oldIn); out = int(oldOut); newLength = int(length);
    }
    return {{"valid",true},{"error",""},{"speed",speed},{"in",in},{"out",out},
            {"length",newLength},{"frames",out-in+1},{"delta",out-in-(oldOut-oldIn)},
            {"sourceIn",sourceIn},{"sourceEnd",sourceEnd},{"flip",flip}};
}
QVariantMap forDuration(const QVariantMap &entry, int frames, bool reverse) {
    if (frames < 1) return fail("Duration must be at least one frame.");
    const double current = entry.value("speed",1.0).toDouble();
    const auto original = preview(entry,current);
    if (!original.value("valid").toBool()) return original;
    const auto sameRate = preview(entry,reverse ? -qAbs(current) : qAbs(current));
    if (sameRate.value("valid").toBool() && sameRate.value("frames").toInt() == frames)
        return sameRate;
    const double span = original.value("sourceEnd").toDouble()-original.value("sourceIn").toDouble();
    if (span <= 0 || (!entry.contains("sourceOrigin") && span < 1))
        return fail("This trim is shorter than one source frame. Enter a speed instead.");
    const double rate = double(span)/frames;
    if (rate < 0.05 || rate > 20.0) return fail("This duration needs a speed outside 5% to 2000%.");
    // A rounded source/rate division can fall one frame below an exact integer.
    // Try adjacent representable rates instead of silently changing duration.
    double lower = rate, upper = rate;
    for (int i=0;i<16;++i) {
        for (const double candidate : {lower,upper}) {
            auto result = preview(entry,reverse ? -candidate : candidate);
            if (result.value("valid").toBool() && result.value("frames").toInt() == frames) return result;
        }
        lower = std::nextafter(lower,0.0);
        upper = std::nextafter(upper,std::numeric_limits<double>::infinity());
    }
    return fail("This duration cannot be represented at a supported speed. Choose another duration.");
}
}
