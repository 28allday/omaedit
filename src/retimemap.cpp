#include "retimemap.h"
#include "framesampling.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool number(const QVariant &v, double *out) {
    const auto type = v.metaType().id();
    if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong
        && type != QMetaType::ULongLong && type != QMetaType::Double && type != QMetaType::Float) return false;
    *out = v.toDouble();
    return std::isfinite(*out);
}
bool only(const QVariantMap &map, const QStringList &keys) {
    for (auto it = map.begin(); it != map.end(); ++it) if (!keys.contains(it.key())) return false;
    return true;
}
QString message(const char *text) { return QCoreApplication::translate("SpeedRamp", text); }
}

double RetimeMap::Segment::value(double u) const {
    if (smooth) u = u * u * (3 - 2 * u);
    return speed + (endSpeed - speed) * u;
}
double RetimeMap::Segment::position(double offset) const {
    const double a=curveStart,d=(curveEnd-curveStart)*offset/frames;
    // Integrate over the local phase span without subtracting two almost
    // equal antiderivatives. A narrowly bladed ramp keeps sub-frame precision.
    const double average=smooth ? a*a*(3-2*a)+d*(3*a*(1-a)+d*(1-2*a-.5*d)) : a+.5*d;
    return source+offset*(speed+(endSpeed-speed)*average);
}
double RetimeMap::Segment::rate(double offset) const {
    return value(curveStart + (curveEnd - curveStart) * offset / frames);
}

QVariantMap RetimeMap::conform(int nativeFrames, double nativeFps, double timelineFps,
                             double speed, double interpretation, std::optional<double> originSeconds) {
    if (nativeFrames < 1 || !std::isfinite(nativeFps) || nativeFps <= 0
        || !std::isfinite(timelineFps) || timelineFps <= 0
        || !std::isfinite(interpretation) || interpretation <= 0
        || !std::isfinite(speed) || speed == 0 || std::abs(speed) > 20) return {};
    const double sourceLength = nativeFrames * timelineFps / nativeFps / interpretation;
    if (originSeconds && (!std::isfinite(*originSeconds) || *originSeconds < 0
        || *originSeconds >= nativeFrames / nativeFps)) return {};
    const double origin = originSeconds.value_or(0.) * timelineFps / interpretation;
    const double span = sourceLength-origin;
    const double length = span / std::abs(speed);
    if (!std::isfinite(length) || length > 100000000 || sourceLength > 100000000) return {};
    const int frames = qMax(1, int(originSeconds ? std::ceil(length-1e-9) : std::llrint(length)));
    const int whole = qMin(frames, int(std::floor(length + 1e-9)));
    QVariantList segments;
    if (whole > 0) segments << QVariantMap{{"frames", whole}, {"speed", speed}};
    if (whole < frames) segments << QVariantMap{{"frames", frames-whole},
        {"speed", std::copysign(qMax(0., span-whole*std::abs(speed)), speed)}};
    return {{"sourceIn", speed < 0 ? sourceLength-origin : origin}, {"sourceLength", sourceLength}, {"segments", segments}};
}

bool RetimeMap::set(const QVariantMap &definition, QString *error) {
    if (error) error->clear();
    auto fail = [&](const char *text) { if (error) *error = message(text); return false; };
    // Validate into a temporary value so a rejected edit leaves the old map intact.
    RetimeMap candidate;
    double version = 0, source = 0;
    if (!only(definition, {"version","sourceIn","sourceLength","segments"})
        || !number(definition.value("version",1), &version) || version != 1)
        return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Unsupported speed ramp definition."));
    if (!number(definition.value("sourceLength"), &candidate.m_sourceLength)
        || candidate.m_sourceLength <= 0 || candidate.m_sourceLength > 100000000
        || !number(definition.value("sourceIn"), &source) || source < 0 || source > candidate.m_sourceLength)
        return fail(QT_TRANSLATE_NOOP("SpeedRamp", "The speed ramp has invalid source bounds."));
    if (definition.value("segments").metaType().id() != QMetaType::QVariantList)
        return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Add at least one speed segment."));
    const auto segments = definition.value("segments").toList();
    if (segments.isEmpty() || segments.size() > 4096)
        return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Use between 1 and 4096 speed segments."));
    QVariantList normalized;
    for (const auto &v : segments) {
        const auto map = v.toMap();
        if (!only(map, {"frames","speed","endSpeed","easing","source","curveStart","curveEnd","holdBefore"}))
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Unknown speed segment option."));
        Segment segment;
        segment.start = candidate.m_frames;
        double count = 0;
        if (!number(map.value("frames"), &count) || count < 1 || count != std::round(count)
            || count > 100000000 - candidate.m_frames)
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Speed segments need a whole number of frames within the supported duration."));
        segment.frames = int(count);
        if (!number(map.value("speed"), &segment.speed)
            || !number(map.value("endSpeed",segment.speed), &segment.endSpeed)
            || std::abs(segment.speed) > 20 || std::abs(segment.endSpeed) > 20)
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Speed must be between reverse 2000% and forward 2000%; zero holds the picture."));
        if (!number(map.value("curveStart",0.), &segment.curveStart)
            || !number(map.value("curveEnd",1.), &segment.curveEnd)
            || segment.curveStart < 0 || segment.curveEnd > 1 || segment.curveStart >= segment.curveEnd)
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "The speed ramp has invalid curve handles."));
        const auto easing = map.value("easing","linear").toString();
        if (easing != "linear" && easing != "smooth")
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "Choose Linear or Smooth speed changes."));
        segment.smooth = easing == "smooth";
        if(map.contains("holdBefore") && (map.value("holdBefore").metaType().id()!=QMetaType::Bool
            || segment.speed!=0 || segment.endSpeed!=0))
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "The held-picture boundary is invalid."));
        segment.holdBefore=map.value("holdBefore").toBool();
        if (map.contains("source") && !number(map.value("source"), &source))
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "The segment has an invalid source position."));
        segment.source = source;
        // A reversal inside a ramp can exceed the source even when both ends fit.
        double low = std::min(source, segment.position(count)), high = std::max(source, segment.position(count));
        if (segment.rate(0) * segment.rate(count) < 0) {
            double a = segment.curveStart, b = segment.curveEnd;
            for (int i = 0; i < 52; ++i) {
                const double middle = (a+b)/2;
                if ((segment.value(middle) < 0) == (segment.value(a) < 0)) a = middle;
                else b = middle;
            }
            const double offset = count * ((a+b)/2-segment.curveStart) / (segment.curveEnd-segment.curveStart);
            const double turning = segment.position(offset);
            low = std::min(low,turning); high = std::max(high,turning);
        }
        if (low < -1e-6 || high > candidate.m_sourceLength + 1e-6)
            return fail(QT_TRANSLATE_NOOP("SpeedRamp", "This speed ramp runs beyond the available source. Shorten a segment or reduce its speed."));
        candidate.m_sourceFirst=candidate.m_segments.isEmpty()?low:std::min(candidate.m_sourceFirst,low);
        candidate.m_sourceEnd=candidate.m_segments.isEmpty()?high:std::max(candidate.m_sourceEnd,high);
        QVariantMap stored{{"frames",segment.frames},{"speed",segment.speed},{"endSpeed",segment.endSpeed},
                           {"easing",easing},{"curveStart",segment.curveStart},{"curveEnd",segment.curveEnd}};
        if (map.contains("source")) stored["source"] = source;
        if(segment.holdBefore)stored["holdBefore"]=true;
        normalized << stored;
        candidate.m_segments << segment;
        candidate.m_frames += segment.frames;
        source = segment.position(count);
    }
    candidate.m_definition = {{"version",1},{"sourceIn",definition.value("sourceIn").toDouble()},
                              {"sourceLength",candidate.m_sourceLength},{"segments",normalized}};
    *this = std::move(candidate);
    return true;
}

double RetimeMap::sourceAt(double frame, double *rate) const {
    if (m_segments.isEmpty() || !std::isfinite(frame)) { if (rate) *rate = 0; return 0; }
    frame = std::clamp(frame, 0., double(m_frames));
    // The frame's own segment: an output frame's end belongs to the segment
    // it started in, as in sample().
    const int whole = std::min(int(std::floor(frame)), m_frames - 1);
    const auto after = std::upper_bound(m_segments.begin(),m_segments.end(),whole,
                                       [](int f,const Segment &s) { return f < s.start; });
    const auto &segment = *(after-1);
    const double offset = frame - segment.start;
    if (rate) *rate = segment.rate(std::min(offset, double(segment.frames)));
    return std::clamp(segment.position(offset),0.,m_sourceLength);
}

bool RetimeMap::hasRamp() const {
    return std::any_of(m_segments.cbegin(),m_segments.cend(),[](const Segment &s) {
        return s.speed != s.endSpeed;
    });
}

RetimeMap::Sample RetimeMap::sample(int frame) const {
    if (frame < 0 || frame >= m_frames || m_segments.isEmpty()) return {};
    const auto after = std::upper_bound(m_segments.begin(),m_segments.end(),frame,
                                       [](int f,const Segment &s) { return f < s.start; });
    const auto &segment = *(after-1);
    const int offset = frame-segment.start;
    // Use this segment's exclusive end, not the next segment's optional jump.
    return {true,std::clamp(segment.position(offset),0.,m_sourceLength),
            std::clamp(segment.position(offset+1),0.,m_sourceLength),segment.rate(offset),int(after-m_segments.begin()-1),segment.holdBefore};
}

int RetimeMap::sourceFrame(int frame, double timelineFps, double sourceFps) const {
    const auto point = sample(frame);
    if (!point.valid || !std::isfinite(timelineFps) || !std::isfinite(sourceFps) || timelineFps <= 0 || sourceFps <= 0) return -1;
    const double scale = sourceFps/timelineFps;
    if (!std::isfinite(scale) || m_sourceLength*scale > std::numeric_limits<int>::max()-1.) return -1;
    const double position = point.source*scale + (point.nextSource < point.source || point.holdBefore ? -1e-7 : 1e-7);
    return int(std::clamp(std::floor(position),0.,std::max(0.,std::ceil(m_sourceLength*scale)-1)));
}

RetimeMap::ImageSample RetimeMap::imageSample(int frame, double timelineFps, double sourceFps) const {
    const auto point = sample(frame);
    if (!point.valid || !std::isfinite(timelineFps) || !std::isfinite(sourceFps)
        || timelineFps <= 0 || sourceFps <= 0) return {};
    const double scale = sourceFps / timelineFps;
    if (!std::isfinite(scale) || m_sourceLength * scale > std::numeric_limits<int>::max()-1.) return {};
    const int last = int(std::max(0., std::ceil(m_sourceLength * scale)-1));
    double position = std::clamp(point.source * scale
        - (point.nextSource < point.source || point.holdBefore ? 1. : 0.), 0., double(last));
    if (std::abs(position - std::round(position)) < 1e-7) position = std::round(position);
    const int first = int(std::floor(position));
    return {first, std::min(first+1, last), position-first};
}

RetimeMap::ImageSample RetimeMap::pictureSample(int frame, double timelineFps, double sourceFps, int mode) const {
    if (!FrameSampling::valid(mode)) return {};
    if (mode == FrameSampling::Original) {
        const int picture = sourceFrame(frame, timelineFps, sourceFps);
        return {picture, picture, 0};
    }
    const auto pair = imageSample(frame, timelineFps, sourceFps);
    if (FrameSampling::interpolates(mode) || pair.first < 0) return pair;
    const int picture = mode == FrameSampling::Nearest && pair.mix >= .5 ? pair.second : pair.first;
    return {picture, picture, 0};
}

QVariantMap RetimeMap::blade(int frame, QString *error) const {
    if (error) error->clear();
    const auto point = sample(frame);
    if (!point.valid || frame == 0) {
        if (error) *error = message(QT_TRANSLATE_NOOP("SpeedRamp", "Place the playhead inside a speed segment to split it."));
        return {};
    }
    const auto &segment = m_segments[point.segment];
    if (frame == segment.start) return m_definition;
    if (m_segments.size() >= 4096) {
        if (error) *error = message(QT_TRANSLATE_NOOP("SpeedRamp", "Use between 1 and 4096 speed segments."));
        return {};
    }
    auto result = m_definition;
    auto list = result.value("segments").toList();
    auto left = list[point.segment].toMap(), right = left;
    const int offset = frame-segment.start;
    const double u = segment.curveStart + (segment.curveEnd-segment.curveStart)*offset/segment.frames;
    left["frames"] = offset; left["curveEnd"] = u;
    right["frames"] = segment.frames-offset; right["curveStart"] = u; right.remove("source");
    list[point.segment] = left; list.insert(point.segment+1,right); result["segments"] = list;
    return result;
}

QVariantMap RetimeMap::window(int first,int count,QString *error) const {
    if(error)error->clear();
    if(first<0 || count<1 || first>m_frames-count) {
        if(error)*error=message(QT_TRANSLATE_NOOP("SpeedRamp","The speed segment range is outside the clip."));
        return {};
    }
    QVariantList list;
    for(int i=0;i<m_segments.size();++i) {
        const auto &s=m_segments[i];const int from=std::max(first,s.start),to=std::min(first+count,s.start+s.frames);
        if(to<=from)continue;
        auto v=m_definition.value("segments").toList()[i].toMap();
        v["frames"]=to-from;
        v["curveStart"]=s.curveStart+(s.curveEnd-s.curveStart)*double(from-s.start)/s.frames;
        v["curveEnd"]=s.curveStart+(s.curveEnd-s.curveStart)*double(to-s.start)/s.frames;
        if(list.isEmpty())v.remove("source");
        list<<v;
    }
    return {{"version",1},{"sourceIn",sample(first).source},{"sourceLength",m_sourceLength},{"segments",list}};
}

QVariantMap RetimeMap::retarget(double sourceOffset,double sourceLength,int first,int count,
                              int *windowFirst,QString *error) const {
    if(error)error->clear();
    if(!std::isfinite(sourceOffset) || !std::isfinite(sourceLength)
        || sourceLength<=0 || sourceLength>100000000) {
        if(error)*error=message(QT_TRANSLATE_NOOP("SpeedRamp","The speed ramp has invalid source bounds."));
        return {};
    }
    if(first<0 || count<1 || first>m_frames-count) {
        if(error)*error=message(QT_TRANSLATE_NOOP("SpeedRamp","The speed segment range is outside the clip."));
        return {};
    }
    auto translated=[&](int begin,int end,QString *failure=nullptr) {
        auto result=window(begin,end-begin,failure);
        if(result.isEmpty())return QVariantMap{};
        auto position=[&](double value) {
            value+=sourceOffset;
            // Equal sync boundaries may differ by floating-point roundoff.
            // Never clamp real missing footage into a repeated edge picture.
            if(std::abs(value)<1e-9)return 0.;
            if(std::abs(value-sourceLength)<1e-9)return sourceLength;
            return value;
        };
        result["sourceIn"]=position(result.value("sourceIn").toDouble());
        result["sourceLength"]=sourceLength;
        auto segments=result.value("segments").toList();
        for(auto &value:segments) {
            auto segment=value.toMap();
            if(segment.contains("source"))segment["source"]=position(segment.value("source").toDouble());
            value=segment;
        }
        result["segments"]=segments;
        RetimeMap candidate;
        if(!candidate.set(result,failure))return QVariantMap{};
        int frame=0;
        for(const auto &value:candidate.definition().value("segments").toList()) {
            const auto segment=value.toMap();
            if(segment.value("speed").toDouble()==0. && segment.value("endSpeed").toDouble()==0.) {
                const auto point=candidate.sample(frame);
                // A stationary boundary must name a picture in this source.
                // Reverse holds name the preceding picture; forward holds at
                // EOF and reverse holds at zero have no corresponding footage.
                if((point.holdBefore && point.source<=1e-9)
                    || (!point.holdBefore && point.source>=sourceLength-1e-9)) {
                    if(failure)*failure=message(QT_TRANSLATE_NOOP("SpeedRamp","This speed ramp runs beyond the available source. Shorten a segment or reduce its speed."));
                    return QVariantMap{};
                }
            }
            frame+=segment.value("frames").toInt();
        }
        return candidate.definition();
    };
    const int requiredEnd=first+count;
    if(translated(first,requiredEnd,error).isEmpty())return {};
    // Coverage is monotonic as a window expands: once an unavailable interval
    // is included, a larger window cannot make it valid. Search output frames,
    // not source positions, to preserve reversals, jumps and curve phases.
    int low=0,high=first;
    while(low<high) {
        const int middle=low+(high-low)/2;
        if(translated(middle,requiredEnd).isEmpty())low=middle+1;
        else high=middle;
    }
    const int begin=low;
    low=requiredEnd;high=m_frames;
    while(low<high) {
        const int middle=low+(high-low+1)/2;
        if(translated(begin,middle).isEmpty())high=middle-1;
        else low=middle;
    }
    auto result=translated(begin,low,error);
    if(!result.isEmpty() && windowFirst)*windowFirst=begin;
    return result;
}
