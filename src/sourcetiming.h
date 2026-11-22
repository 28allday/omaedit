#pragma once
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <cmath>
#include <limits>
#include <optional>
#include "multicamtiming.h"

// Browser marks name recorded pictures. This conversion is shared by placement
// and its drag preview; the original library entry remains in source units.
namespace SourceTiming {
inline bool readOrigin(const QVariantMap &entry, std::optional<double> &origin) {
    origin.reset();
    if (!entry.contains("sourceOrigin")) return true;
    const auto value=entry.value("sourceOrigin");const int type=value.metaType().id();
    if (type!=QMetaType::Double && type!=QMetaType::Float && type!=QMetaType::Int
        && type!=QMetaType::UInt && type!=QMetaType::LongLong && type!=QMetaType::ULongLong) return false;
    const double seconds=value.toDouble();
    if (!std::isfinite(seconds) || seconds<0) return false;
    origin=seconds;return true;
}
inline QVariantMap resolve(const QVariantMap &source, double destination) {
    const double recorded = source.value("sourceFps", source.value("fps")).toDouble();
    const double forced = source.value("forceFps").toDouble();
    const double effective = forced > 0.0 ? forced : recorded;
    const double scale = effective > 0.0 && destination > 0.0 ? destination / effective : 1.0;
    const int nativeFrames = source.value("sourceDuration", source.value("duration")).toInt();
    if (!std::isfinite(scale) || scale <= 0.0 || nativeFrames < 1
        || nativeFrames * scale > std::numeric_limits<int>::max()) return {};
    const int markIn = source.value("rangeIn", -1).toInt();
    const int markOut = source.value("rangeOut", -1).toInt();
    const int sourceIn = qMax(0, markIn);
    const int sourceOut = markOut < 0 ? nativeFrames - 1 : qMin(markOut, nativeFrames - 1);
    if (sourceIn > sourceOut) return {};
    const bool marked = markIn >= 0 || markOut >= 0;
    // Unmarked media keeps its established rounded full-source duration. A
    // marked range carries a physical origin independently of integer cuts;
    // flooring the cut alone would lose the chosen recorded picture.
    const bool exact = marked && recorded > 0. && effective > 0. && destination > 0.;
    const double selected = sourceIn*scale;
    const double aligned = std::abs(selected-std::round(selected)) < 1e-9 ? std::round(selected) : selected;
    const qint64 first = marked ? (exact ? qint64(std::floor(aligned)) : qRound64(selected)) : 0;
    const double phase = exact ? qMax(0.,aligned-first) : 0.;
    const double origin = exact ? phase*(effective/recorded)/destination : 0.;
    if (!std::isfinite(origin)) return {};
    // Keep a final partial interval available to a mark at the last picture.
    // The selected duration rounds once, independently of its absolute start.
    const qint64 duration = qMax<qint64>(1, exact ? qint64(std::ceil(nativeFrames*scale-phase-1e-9))
                                                               : std::llrint(nativeFrames*scale));
    const qint64 start = qMin(first,duration-1);
    const qint64 selectedFrames = qMax<qint64>(1,std::llrint((qint64(sourceOut)-sourceIn+1)*scale));
    const qint64 end = !marked ? duration : exact ? qMin(duration,start+selectedFrames)
        : qBound(start+1,qRound64((qint64(sourceOut)+1)*scale),duration);
    QVariantMap result{{"in", int(start)}, {"out", int(end - 1)}, {"frames", int(end - start)},
            {"duration", int(duration)}, {"marked", marked}, {"scale", scale},
            {"sourceIn", sourceIn}, {"sourceOut", sourceOut}, {"sourceFrames", nativeFrames},
            {"sourceFps", recorded}, {"effectiveFps", effective}, {"projectFps", destination}};
    if (exact) result["sourceOrigin"] = origin; // physical seconds; zero is meaningful
    return result;
}

// The snapshot keeps recorded-picture coordinates while a placement waits for
// a destination format. Bounds and metadata must use the same resolved clock.
inline QVariantMap placement(const QVariantMap &snapshot, double destination) {
    auto source=snapshot.value("source").toMap();
    const auto timing=resolve(source,destination);
    if(timing.isEmpty())return {};
    const double scale=timing.value("scale").toDouble();
    const int duration=timing.value("duration").toInt();
    if(timing.value("marked").toBool()) {
        source["rangeIn"]=timing.value("in");source["rangeOut"]=timing.value("out");
    }
    source["duration"]=duration;
    const double phase=timing.value("sourceOrigin").toDouble()*timing.value("sourceFps").toDouble()*scale;
    auto markers=snapshot.value("markers").toList();
    for(auto &value:markers) {
        auto marker=value.toMap();
        marker["frame"]=int(qBound(qint64(0),qRound64(marker.value("frame").toInt()*scale-phase),qint64(duration)-1));
        value=marker;
    }
    QVariantMap tag{{"markers",markers}};
    if(timing.contains("sourceOrigin")) {
        tag["sourceOrigin"]=timing.value("sourceOrigin");
        tag["sourceSpan"]=(timing.value("sourceOut").toInt()-timing.value("sourceIn").toInt()+1)*scale;
    }
    if(!snapshot.value("mcam").toString().isEmpty()) {
        auto camera=QJsonDocument::fromJson(snapshot.value("mcam").toString().toUtf8()).object();
        if(camera.value("version").toInt()>2)return {};
        if(camera.value("version").toInt()==2) {
            const auto resolved=MulticamTiming::atRate(camera.toVariantMap(),destination);
            if(resolved.isEmpty())return {};
            camera=QJsonObject::fromVariantMap(resolved);
        } else {
            auto angles=camera.value("angles").toArray();
            for(int i=0;i<angles.size();++i) {
                auto item=angles[i].toObject();
                item["offset"]=qRound(item.value("offset").toInt()*scale);
                item["frames"]=qMax(1,qRound(item.value("frames").toInt()*scale));angles[i]=item;
            }
            camera["angles"]=angles;
        }
        tag["mcam"]=QString::fromUtf8(QJsonDocument(camera).toJson(QJsonDocument::Compact));
        tag["angle"]=snapshot.value("angle").toInt();
    }
    source["placementTag"]=tag;
    return source;
}
}
