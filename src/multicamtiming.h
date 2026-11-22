#pragma once
#include <QVariantMap>
#include <QtMath>
#include <cmath>
#include <limits>

// Version 2 keeps synchronization in seconds and source marks in recorded
// pictures. Integer offsets/lengths are display values in the declared clock.
// Legacy definitions without a version retain their original frame semantics.
namespace MulticamTiming {
inline double nativeRate(const QVariantMap &angle) {
    const auto meta=angle.value("sourceMeta").toMap();
    double rate=angle.value("sourceFps",meta.value("sourceFps")).toDouble();
    if(rate<=0)rate=angle.value("fps",meta.value("fps")).toDouble();
    return rate;
}
inline double effectiveRate(const QVariantMap &angle) {
    const double forced=angle.value("forceFps",angle.value("sourceMeta").toMap().value("forceFps")).toDouble();
    return forced>0?forced:nativeRate(angle);
}
inline int nativeFrames(const QVariantMap &angle) {
    const auto meta=angle.value("sourceMeta").toMap();
    int frames=angle.value("sourceDuration",meta.value("sourceDuration")).toInt();
    if(frames<1)frames=angle.value("duration",meta.value("duration")).toInt();
    return frames;
}
inline double offsetSeconds(const QVariantMap &angle) {return angle.value("offsetSeconds").toDouble();}
inline QVariantMap withMetadata(QVariantMap angle,const QVariantMap &metadata) {
    const double recorded=nativeRate(angle);const int count=nativeFrames(angle);
    angle["sourceMeta"]=metadata;
    double rate=metadata.value("sourceFps").toDouble();
    if(rate<=0)rate=metadata.value("fps").toDouble();
    if(rate<=0)rate=recorded;
    int frames=metadata.value("sourceDuration").toInt();
    if(frames<1)frames=metadata.value("duration").toInt();
    if(frames<1)frames=count;
    angle["sourceFps"]=rate;angle["sourceDuration"]=frames;
    angle["forceFps"]=metadata.value("forceFps").toDouble();
    return angle;
}
inline QVariantMap atRate(QVariantMap camera,double destination) {
    if(camera.value("version").toInt()!=2 || !std::isfinite(destination) || destination<=0)return {};
    auto angles=camera.value("angles").toList();
    if(angles.isEmpty())return {};
    for(auto &value:angles) {
        auto angle=value.toMap();const double native=nativeRate(angle),effective=effectiveRate(angle),offset=offsetSeconds(angle);
        const int count=nativeFrames(angle);
        const double frames=count*destination/effective,ticks=offset*destination;
        if(!std::isfinite(native) || native<=0 || !std::isfinite(effective) || effective<=0 || count<1
            || !std::isfinite(ticks) || ticks<0 || ticks>100000000 || !std::isfinite(frames) || frames>100000000)return {};
        angle["offset"]=qRound(ticks);angle["frames"]=qMax(1,qRound(frames));value=angle;
    }
    camera["angles"]=angles;camera["timelineFps"]=destination;return camera;
}
inline QVariantMap nativeSource(const QVariantMap &entry,int index) {
    const auto angles=entry.value("angles").toList();
    if(entry.value("multicamVersion").toInt()!=2 || angles.isEmpty())return {};
    const auto angle=angles[qBound(0,index,int(angles.size())-1)].toMap();
    const double rate=nativeRate(angle),effective=effectiveRate(angle);const int count=nativeFrames(angle);
    if(!std::isfinite(rate) || rate<=0 || !std::isfinite(effective) || effective<=0 || count<1)return {};
    const double duration=count*rate/effective;
    if(!std::isfinite(duration) || duration>std::numeric_limits<int>::max())return {};
    auto source=entry;source.remove("multicam");source["path"]=angle.value("path");
    source["sourceFps"]=rate;source["sourceDuration"]=count;
    source["forceFps"]=angle.value("forceFps",angle.value("sourceMeta").toMap().value("forceFps")).toDouble();
    source["fps"]=effective;source["duration"]=qMax(1,qRound(duration));
    return source;
}
}
