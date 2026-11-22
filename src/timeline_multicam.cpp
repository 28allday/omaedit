#include "timeline_p.h"

bool Timeline::setMulticamAt(int track, int index, const QString &json,
                                int angle) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    if(!json.isEmpty()) {
        const auto camera=QJsonDocument::fromJson(json.toUtf8()).toVariant().toMap();
        const int version=camera.value("version").toInt();
        if(version>2 || (version==2 && (MulticamTiming::atRate(camera,fps()).isEmpty()
            || angle<0 || angle>=camera.value("angles").toList().size())))return false;
    }
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:mcam"))
            cut->detach(*f);
    }
    if (!json.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:mcam", json.toUtf8().constData());
        tag.set("omavid:mcamAngle", qMax(0, angle));
        cut->attach(tag);
    }
    return true;
}

QList<Timeline::Angle> Timeline::anglesOf(const QString &json) {
    QList<Angle> angles;
    const QJsonArray list =
        QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("angles")).toArray();
    for (const auto &v : list) {
        const QJsonObject o = v.toObject();
        Angle a;
        a.path = o.value(QStringLiteral("path")).toString();
        a.name = o.value(QStringLiteral("name")).toString();
        a.offset = o.value(QStringLiteral("offset")).toInt();
        a.frames = o.value(QStringLiteral("frames")).toInt();
        angles.append(a);
    }
    return angles;
}

QList<int> Timeline::multicamNativeFrames(const QVariantMap &entry,int relativeFrame,double timelineFps) {
    const auto camera=multicamDefinition(entry);const auto angles=camera.value("angles").toList();
    QList<int> frames(angles.size(),-1);
    if(relativeFrame<0 || relativeFrame>=entry.value(ClipKey::frames).toInt())return frames;
    const int shown=entry.value(ClipKey::mcamAngle,-1).toInt();
    if(shown<0 || shown>=angles.size())return frames;
    if(camera.value("version").toInt()!=2) {
        const double speed=qAbs(entry.value(ClipKey::speed,1.).toDouble());
        const int source=entry.value(ClipKey::in).toInt()+qRound(relativeFrame*(speed>0?speed:1.));
        // (A stretch without footage counts the multicam clip's own frames.)
        const int virtualFrame=source+(angleWithoutFootage(entry)?0:angles[shown].toMap().value("offset").toInt());
        for(int i=0;i<angles.size();++i) {
            const auto angle=angles[i].toMap();const int frame=virtualFrame-angle.value("offset").toInt();
            if(frame>=0 && (angle.value("frames").toInt()<=0 || frame<angle.value("frames").toInt()))frames[i]=frame;
        }
        return frames;
    }
    const auto point=multicamPoint(entry,camera,relativeFrame,timelineFps);if(!point.valid)return frames;
    int mode=entry.value(ClipKey::frameBlend,FrameSampling::Original).toInt();
    if(!FrameSampling::valid(mode))return frames;
    if(mode==FrameSampling::Original && entry.contains(ClipKey::sourceOrigin) && entry.value(ClipKey::retime).toMap().isEmpty())mode=FrameSampling::Nearest;
    for(int i=0;i<angles.size();++i) {
        auto angle=angles[i].toMap();
        if(i==shown && entry.contains(ClipKey::sourceMeta))angle=MulticamTiming::withMetadata(angle,entry.value(ClipKey::sourceMeta).toMap());
        const double rate=MulticamTiming::effectiveRate(angle);const int count=MulticamTiming::nativeFrames(angle);
        // Match retarget's coordinate translation before rate conversion. A
        // seconds roundtrip loses exact half-picture positions such as 3.5.
        const double local=point.source+(point.offsetSeconds-MulticamTiming::offsetSeconds(angle))*timelineFps;
        double position=local*rate/timelineFps;
        if(!std::isfinite(position) || !std::isfinite(rate) || rate<=0 || count<1
            || position< -1e-7 || position>count+1e-7
            || (!point.backwards && position>=count-1e-7) || (point.backwards && position<=0.))continue;
        // Saved maps without a native origin retain their two-stage sampling:
        // choose an interpreted producer frame, then its recorded picture.
        const double native=MulticamTiming::nativeRate(angle);
        const bool legacyInterpreted=entry.contains(ClipKey::retime) && !entry.contains(ClipKey::sourceOrigin)
            && mode<FrameSampling::Floor && !qFuzzyCompare(rate,native);
        if(legacyInterpreted)position=local;
        if(mode==FrameSampling::Original)position=std::floor(position+(point.backwards?-1e-7:1e-7));
        else {
            if(point.backwards)position-=1.;
            if(std::abs(position-std::round(position))<1e-7)position=std::round(position);
            position=mode==FrameSampling::Nearest?std::floor(position+.5):std::floor(position);
        }
        if(legacyInterpreted)position=qRound(position*rate/timelineFps);
        frames[i]=qBound(0,int(position),count-1);
    }
    return frames;
}

// Source frames of the shown angle → the multicam's virtual timeline →
// source frames of the new angle, on a snapshot entry. Dissolved edges
// share frames with a neighbour; the switched clip keeps them (in/out
// carry the overlap). False when the angle does not cover the span.
bool Timeline::switchAngleInEntry(QVariantMap &map, int angle) {
    auto camera=multicamDefinition(map);
    if(camera.value("version").toInt()==2) {
        auto angles=camera.value("angles").toList();const int from=map.value(ClipKey::mcamAngle,-1).toInt();
        if(angle<0 || angle>=angles.size() || from<0 || from>=angles.size() || angle==from)return false;
        for(auto &value:angles) {
            auto item=value.toMap();const auto path=item.value(ClipKey::path).toString();
            if(m_mediaSources.contains(path))item=MulticamTiming::withMetadata(item,m_mediaSources.value(path).toMap());
            value=item;
        }
        camera["angles"]=angles;camera=MulticamTiming::atRate(camera,fps());if(camera.isEmpty())return false;
        const auto target=angles[angle].toMap();const auto path=target.value(ClipKey::path).toString();
        const double native=MulticamTiming::nativeRate(target),effective=MulticamTiming::effectiveRate(target);
        const int count=MulticamTiming::nativeFrames(target);
        if(path.isEmpty())return false;
        const int oldIn=map.value(ClipKey::in,-1).toInt();
        const qint64 span=map.value(ClipKey::out).toLongLong()-oldIn+1;
        if(oldIn<0 || span<1 || span>100000000)return false;
        if(map.contains(ClipKey::retime) || map.contains(ClipKey::freeze)) {
            RetimeMap parent;
            if(map.contains(ClipKey::freeze)) {
                const auto held=heldMulticamTiming(map,camera,fps(),map.value(ClipKey::freeze).toInt(),map.value(ClipKey::length).toInt());
                if(!parent.set(held))return false;
            } else if(!multicamParentTiming(map,camera,fps(),&parent))return false;
            const double shift=(MulticamTiming::offsetSeconds(angles[from].toMap())-MulticamTiming::offsetSeconds(target))*fps();
            int windowFirst=0;
            const auto translated=parent.retarget(shift,count*fps()/effective,oldIn,int(span),&windowFirst);
            RetimeMap retargeted;if(!retargeted.set(translated))return false;
            const bool constantNearest=map.contains(ClipKey::freeze) && !map.contains(ClipKey::retime)
                && map.contains(ClipKey::sourceOrigin) && map.value(ClipKey::frameBlend).toInt()==FrameSampling::Original;
            map[ClipKey::path]=path;map[ClipKey::retime]=retargeted.definition();map[ClipKey::speed]=1.;map.remove(ClipKey::freeze);
            map[ClipKey::in]=oldIn-windowFirst;map[ClipKey::out]=oldIn-windowFirst+int(span)-1;map[ClipKey::length]=retargeted.frames();
            // A saved map already owns the absolute phase. Preserve whether it
            // used native sampling, without importing the old angle's origin.
            if(map.contains(ClipKey::sourceOrigin))map[ClipKey::sourceOrigin]=0.;
            if(constantNearest)map[ClipKey::frameBlend]=FrameSampling::Nearest;
            map[ClipKey::mcamAngle]=angle;
            map[ClipKey::mcam]=QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
            const auto metadata=target.value(ClipKey::sourceMeta).toMap();
            if(metadata.isEmpty())map.remove(ClipKey::sourceMeta);else map[ClipKey::sourceMeta]=metadata;
            map.remove(ClipKey::sourceTiming);map.remove(ClipKey::name);
            if(map.contains(ClipKey::markers)) {
                QVariantList kept;
                for(const auto &value:map.value(ClipKey::markers).toList()) {
                    auto marker=value.toMap();const int frame=marker.value("frame").toInt()-windowFirst;
                    if(frame>=0 && frame<retargeted.frames()){marker["frame"]=frame;kept<<marker;}
                }
                map[ClipKey::markers]=kept;
            }
            return true;
        }
        const auto point=multicamPoint(map,camera,0,fps());if(!point.valid)return false;
        const double speed=map.value(ClipKey::speed,1.).toDouble(),rate=std::abs(speed),factor=effective/native;
        const double local=point.seconds-MulticamTiming::offsetSeconds(target),coverage=count/effective;
        if(path.isEmpty() || !std::isfinite(speed) || rate<.05 || rate>20. || !std::isfinite(local)
            || local< -1e-9 || local>coverage+1e-9)return false;
        const double distance=qMax(0.,speed<0?coverage-local:local),position=distance*fps()/rate;
        if(!std::isfinite(position) || position>100000000)return false;
        const double aligned=std::abs(position-std::round(position))<1e-9?std::round(position):position;
        const int in=int(std::floor(aligned));const double origin=qMax(0.,distance-in*rate/fps())*factor;
        RetimeMap timing;if(!timing.set(RetimeMap::conform(count,native,fps(),speed,factor,origin)))return false;
        if(span<1 || span>timing.frames()-in)return false;
        map[ClipKey::path]=path;map[ClipKey::in]=in;map[ClipKey::out]=in+int(span)-1;map[ClipKey::length]=timing.frames();
        map[ClipKey::sourceOrigin]=origin;map[ClipKey::mcamAngle]=angle;
        map[ClipKey::mcam]=QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
        const auto metadata=target.value(ClipKey::sourceMeta).toMap();
        if(metadata.isEmpty())map.remove(ClipKey::sourceMeta);else map[ClipKey::sourceMeta]=metadata;
        map.remove(ClipKey::sourceTiming);map.remove(ClipKey::name);
        map.remove(ClipKey::generator);map.remove(ClipKey::generatorColor); // of a stretch without footage
        if(map.contains(ClipKey::markers)) {
            auto markers=map.value(ClipKey::markers).toList();
            for(auto &value:markers){auto marker=value.toMap();marker["frame"]=qMax(0,marker.value("frame").toInt()+in-oldIn);value=marker;}
            map[ClipKey::markers]=markers;
        }
        return true;
    }
    if(camera.value("version").toInt()>2)return false;
    const QList<Angle> angles = anglesOf(map.value(ClipKey::mcam).toString());
    const int from = map.value(ClipKey::mcamAngle).toInt();
    if (angles.isEmpty() || angle < 0 || angle >= angles.size() || from < 0
        || from >= angles.size() || angle == from)
        return false;
    const int in = map.value(ClipKey::in).toInt();
    const int out = map.value(ClipKey::out).toInt();
    const int shift = (angleWithoutFootage(map) ? 0 : angles.at(from).offset) - angles.at(angle).offset;
    const int newIn = in + shift;
    const int newOut = out + shift;
    const int frames = angles.at(angle).frames;
    if (newIn < 0 || (frames > 0 && newOut >= frames))
        return false;
    map.insert(ClipKey::path, angles.at(angle).path);
    map.insert(ClipKey::in, newIn);
    map.insert(ClipKey::out, newOut);
    map.insert(ClipKey::mcamAngle, angle);
    map.remove(ClipKey::name); // derives from the path again
    map.remove(ClipKey::generator); map.remove(ClipKey::generatorColor); // of a stretch without footage
    return true;
}

bool Timeline::switchCoveredAngleOn(int track, int index, int angle) {
    if (trackLocked(track) || angleSwitchAudioLocked(track,index))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    if (!switchAngleInEntry(map, angle))
        return false;
    clips[at] = map;
    if(!rebuild(clips,pristine))return false;
    if(multicamDefinition(map).value("version").toInt()==2) {
        // The native API has the same linked-sound semantics as worker edits.
        // The worker's later settle is idempotent against the committed pair.
        settleLinkedAudio(pristine);
        // Producer metadata passes through JSON during a rebuild (including
        // absent library fields represented as JSON null). Verify the actual
        // picture/sound pair, not a raw proposal with different QVariant types.
        const auto committedPicture=entryMapOn(track,index);
        const auto link=map.value(ClipKey::link).toString();
        if(m_audioFollowsVideo && !link.isEmpty() && fixedSoundOf(map).isEmpty()) {
            bool needed=false,matched=false;
            for(const auto &value:pristine) {
                const auto entry=value.toMap();
                if(entry.value(ClipKey::audioOnly).toBool() && entry.value(ClipKey::link).toString()==link)needed=true;
            }
            for(const auto &value:entries()) {
                const auto entry=value.toMap();
                if(!entry.value(ClipKey::audioOnly).toBool() || entry.value(ClipKey::link).toString()!=link)continue;
                matched=true;
                for(const char *key:{"path","in","out","speed","retime","sourceOrigin","sourceMeta"})
                    if(entry.value(key)!=committedPicture.value(key))matched=false;
                if(matched)break;
            }
            if(needed && !matched){restore(pristine);return false;}
        }
    }
    return true;
}

bool Timeline::angleSwitchAudioLocked(int track,int index) const {
    if(!m_audioFollowsVideo)return false;
    const auto picture=entryMapOn(track,index);
    // (A sound source of its own is not changed by an angle change.)
    if(!fixedSoundOf(picture).isEmpty())return false;
    if(multicamDefinition(picture).value("version").toInt()!=2 || picture.value(ClipKey::audioOnly).toBool())return false;
    const auto link=picture.value(ClipKey::link).toString();if(link.isEmpty())return false;
    for(const auto &value:entries()) {
        const auto entry=value.toMap();
        if(entry.value(ClipKey::audioOnly).toBool() && entry.value(ClipKey::link).toString()==link && trackLocked(entry.value(ClipKey::track).toInt()))return true;
    }
    return false;
}

// One snapshot rebuild: the entry becomes a head (untouched) and a tail
// (the new angle) — a playlist split would drop the cut's marker filters,
// and a rebuild re-applies every attribute to both halves for free.
bool Timeline::cutToCoveredAngleOn(int track, int index, int frame, int angle) {
    if (trackLocked(track) || angleSwitchAudioLocked(track,index))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap head = clips.at(at).toMap();
    const int start = head.value(ClipKey::start).toInt();
    const int frames = head.value(ClipKey::frames).toInt();
    const int rel = frame - start;
    if (rel < 0 || rel >= frames)
        return false;
    if (rel == 0)
        return switchCoveredAngleOn(track, index, angle);
    if (head.value(ClipKey::mixOut).toInt() > 0
        || head.value(ClipKey::mixIn).toInt() > 0)
        return false; // never blade inside a dissolve (the blade rule)
    if(multicamDefinition(head).value("version").toInt()==2) {
        // Validate target coverage before making a blade, then use the shared
        // split path so linked sound and keyframes get the same fresh tails.
        auto proposed=head;proposed[ClipKey::in]=head.value(ClipKey::in).toInt()+rel;proposed[ClipKey::frames]=frames-rel;
        if(!switchAngleInEntry(proposed,angle))return false;
        if(!splitOn(track,frame)){restore(pristine);return false;}
        const int right=clipIndexContaining(track,frame);
        if(right<0 || !switchCoveredAngleOn(track,right,angle)){restore(pristine);return false;}
        return true;
    }
    const int in = head.value(ClipKey::in).toInt();
    const int out = head.value(ClipKey::out).toInt();
    const int headSource = qMax(1, rel); // in/out are timewarp frames: 1:1 with the timeline
    QVariantMap tail = head;
    head.insert(ClipKey::frames, rel);
    head.insert(ClipKey::out, qMin(out, in + headSource - 1));
    head.insert(ClipKey::fadeOut, 0);
    tail.insert(ClipKey::start, frame);
    tail.insert(ClipKey::frames, frames - rel);
    tail.insert(ClipKey::in, qMin(out, in + headSource));
    tail.insert(ClipKey::fadeIn, 0);
    tail.remove(ClipKey::gapBefore);
    rebaseEntryKeys(tail, tail.value(ClipKey::in).toInt() - in);
    if (!switchAngleInEntry(tail, angle))
        return false;
    clips[at] = head;
    clips.insert(at + 1, tail);
    return rebuild(clips, pristine);
}

// ── Where an angle has no footage ────────────────────────────────────────
// Cameras never start and stop together, so an angle seldom covers a whole
// segment. The stretch it has nothing for is a piece of its own: still the
// multicam clip on that angle (its tag and angle), its source an opaque
// black solid, the way the timeline already draws a colour (the engine and
// its CPU twin, thumbnails, export and the project file all know one), with
// no sound. Its frames count the multicam clip's own time, so it can be
// trimmed, rolled and switched to another angle like any segment; the cut
// beside it is where the angle's footage begins or ends.
bool Timeline::angleWithoutFootage(const QVariantMap &entry) {
    return entry.value(ClipKey::path).toString() == noFootagePath()
           && !entry.value(ClipKey::mcam).toString().isEmpty();
}

namespace {
// A segment the pieces below are made of: played forwards at its own speed,
// with no transition on an edge. (A retimed, held or reversed segment is
// still changed only where the angle covers all of it.)
bool plainSegment(const QVariantMap &e) {
    return !e.value(ClipKey::mcam).toString().isEmpty() && !e.value(ClipKey::audioOnly).toBool()
           && !e.contains(ClipKey::retime) && !e.contains(ClipKey::freeze)
           && qFuzzyCompare(e.value(ClipKey::speed, 1.).toDouble(), 1.)
           && e.value(ClipKey::mixIn).toInt() == 0 && e.value(ClipKey::mixOut).toInt() == 0;
}
// The multicam clip's own time, in seconds, at an entry's first frame.
bool momentOf(const QVariantMap &e, const QVariantMap &camera, double fps, double *seconds) {
    if (!(fps > 0)) return false;
    if (camera.value("version").toInt() == 2) {
        const auto point = multicamPoint(e, camera, 0, fps);
        if (!point.valid) return false;
        *seconds = point.seconds;
        return true;
    }
    const auto angles = camera.value("angles").toList();
    const int from = e.value(ClipKey::mcamAngle, -1).toInt();
    if (camera.value("version").toInt() > 2 || from < 0 || from >= angles.size()) return false;
    *seconds = (e.value(ClipKey::in).toInt()
                + (Timeline::angleWithoutFootage(e) ? 0 : angles[from].toMap().value("offset").toInt())) / fps;
    return true;
}
// One multicam clip, whichever piece carries its description.
bool sameCamera(const QVariantMap &a, const QVariantMap &b) {
    const auto id = a.value("id").toString();
    return id.isEmpty() ? a.value("name") == b.value("name") && a.value("angles").toList().size() == b.value("angles").toList().size()
                        : id == b.value("id").toString();
}
// `e` from its frame `from` on, `count` frames of it (a plain segment's
// frames are its source's, one for one).
QVariantMap partOf(QVariantMap e, int from, int count) {
    e[ClipKey::start] = e.value(ClipKey::start).toInt() + from;
    e[ClipKey::in] = e.value(ClipKey::in).toInt() + from;
    e[ClipKey::out] = e.value(ClipKey::in).toInt() + count - 1;
    e[ClipKey::frames] = count;
    return e;
}
}

// ── A sound source of its own ────────────────────────────────────────────
QVariantMap Timeline::fixedSoundOf(const QVariantMap &entry) {
    const QString json = entry.value(ClipKey::mcam).toString();
    if (!json.contains(QLatin1String("\"sound\""))) return {};
    const auto camera = QJsonDocument::fromJson(json.toUtf8()).toVariant().toMap();
    const auto sound = camera.value("sound").toMap();
    return camera.value("version").toInt() == 2 && !sound.value("path").toString().isEmpty() ? sound : QVariantMap();
}

// The source is lined up with the angles as one of them is, so where it lies
// under a picture is worked out as an angle's footage is: the picture, put
// on the source as though it were one more angle.
bool Timeline::fixedSoundFor(const QVariantMap &picture, QVariantMap &sound) {
    const QVariantMap fixed = fixedSoundOf(picture);
    if (fixed.isEmpty() || picture.value(ClipKey::audioOnly).toBool() || picture.contains(ClipKey::freeze)) return false;
    auto camera = multicamDefinition(picture);
    auto angles = camera.value("angles").toList();
    const int source = int(angles.size());
    angles.append(fixed);
    camera["angles"] = angles;
    QVariantMap as = picture;
    as[ClipKey::mcam] = QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
    as.remove(ClipKey::markers);
    const int count = picture.value(ClipKey::frames).toInt();
    // A picture in a transition shares frames with its neighbour; its sound
    // does not (the two sounds are one source, in step): the sound is cut
    // plainly where the picture's own frames begin and end, with no fade
    // across it, and plays unbroken through the transition.
    if (picture.value(ClipKey::mixIn).toInt() > 0 || picture.value(ClipKey::mixOut).toInt() > 0) {
        const int shared = as.value(ClipKey::out).toInt() - as.value(ClipKey::in).toInt() + 1 - count;
        if (shared < 0) return false;
        if (picture.value(ClipKey::mixIn).toInt() > 0) as[ClipKey::in] = as.value(ClipKey::in).toInt() + shared;
        else as[ClipKey::out] = as.value(ClipKey::out).toInt() - shared;
        for (const QString &key : {ClipKey::mixIn, ClipKey::mixOut, ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB, ClipKey::mixStyle})
            as.remove(key);
    }
    int lead = 0, covered = count;
    QVariantMap part = as;
    if (!switchAngleInEntry(part, source)) {
        // Not all of it: the frames the source has (a plain segment's only;
        // a retimed or held one, or one in a transition, plays the source
        // only where it has all of it).
        if (!angleCoverage(as, source, &lead, &covered) || covered < 1) return false;
        part = partOf(as, lead, covered);
        if (!switchAngleInEntry(part, source)) return false;
    }
    int in = part.value(ClipKey::in).toInt();
    const QString path = part.value(ClipKey::path).toString();
    // Pieces bladed from one sound count their frames from one origin. The
    // same frames worked out afresh may differ in the origin's last bit, or
    // name a frame's edge from its other side: said as the sound already
    // says it, the pieces stay one unbroken run of samples and join again.
    const bool plain = !part.contains(ClipKey::retime) && qFuzzyCompare(part.value(ClipKey::speed, 1.).toDouble(), 1.)
                       && !sound.contains(ClipKey::retime) && qFuzzyCompare(sound.value(ClipKey::speed, 1.).toDouble(), 1.);
    if (plain && sound.value(ClipKey::path).toString() == path && sound.contains(ClipKey::start)) {
        const int same = sound.value(ClipKey::in).toInt() + picture.value(ClipKey::start).toInt() + lead - sound.value(ClipKey::start).toInt();
        const double apart = in - same + (part.value(ClipKey::sourceOrigin).toDouble() - sound.value(ClipKey::sourceOrigin).toDouble()) * fps();
        if (same >= 0 && std::abs(apart) < 1e-6) {
            in = same;
            if (sound.contains(ClipKey::sourceOrigin)) part[ClipKey::sourceOrigin] = sound.value(ClipKey::sourceOrigin);
            else part.remove(ClipKey::sourceOrigin);
        }
    }
    const int span = part.value(ClipKey::out).toInt() - part.value(ClipKey::in).toInt();
    sound[ClipKey::start] = picture.value(ClipKey::start).toInt() + lead;
    sound[ClipKey::path] = path;
    sound[ClipKey::in] = in;
    sound[ClipKey::out] = in + span;
    sound[ClipKey::length] = part.value(ClipKey::length);
    sound[ClipKey::frames] = covered == count ? count : covered;
    sound[ClipKey::speed] = part.value(ClipKey::speed, 1.);
    for (const QString &key : {ClipKey::sourceOrigin, ClipKey::retime, ClipKey::sourceMeta}) {
        if (part.contains(key)) sound[key] = part.value(key);
        else sound.remove(key);
    }
    return true;
}

// ── The sound source, chosen after the clip is made ──────────────────────
namespace {
QVariantList recordingsIn(const QVariantMap &camera) {
    QVariantList list = camera.value("recordings").toList();
    const QVariantMap sound = camera.value("sound").toMap();
    if (list.isEmpty() && !sound.value("path").toString().isEmpty() && !camera.contains("soundAngle"))
        list.append(sound);
    return list;
}
}

QVariantList Timeline::soundSourcesIn(const QVariantMap &camera) {
    QVariantList sources;
    const auto add = [&](const QVariant &value, bool recording) {
        const QVariantMap item = value.toMap();
        sources.append(QVariantMap{{"source", int(sources.size())}, {"name", item.value("name")}, {"path", item.value("path")}, {"recording", recording}});
    };
    for (const QVariant &angle : camera.value("angles").toList()) add(angle, false);
    for (const QVariant &recording : recordingsIn(camera)) add(recording, true);
    return sources;
}

int Timeline::soundSourceIn(const QVariantMap &camera) {
    const QVariantMap sound = camera.value("sound").toMap();
    if (sound.value("path").toString().isEmpty()) return -1;
    const int angles = int(camera.value("angles").toList().size());
    if (camera.contains("soundAngle")) return qBound(0, camera.value("soundAngle").toInt(), qMax(0, angles - 1));
    const QVariantList recordings = recordingsIn(camera);
    for (int i = 0; i < recordings.size(); ++i)
        if (recordings.at(i).toMap().value("path") == sound.value("path")) return angles + i;
    return angles;
}

bool Timeline::chooseSoundSourceIn(QVariantMap &camera, int source) {
    const QVariantList angles = camera.value("angles").toList(), recordings = recordingsIn(camera);
    if (source < -1 || source >= angles.size() + recordings.size()) return false;
    if (!recordings.isEmpty()) camera["recordings"] = recordings;
    camera.remove("sound");
    camera.remove("soundAngle");
    if (source >= angles.size()) {
        camera["sound"] = recordings.at(source - angles.size());
    } else if (source >= 0) {
        camera["sound"] = angles.at(source);
        camera["soundAngle"] = source;
    }
    return true;
}

QList<int> Timeline::placedPiecesOf(int track, int index) const {
    struct Piece { int index, start, end; bool plain; double origin; bool in; };
    QList<Piece> pieces;
    const QVariantMap here = entryMapOn(track, index);
    const QVariantMap camera = multicamDefinition(here);
    if (here.value(ClipKey::mcam).toString().isEmpty() || here.value(ClipKey::audioOnly).toBool()) return {};
    for (const QVariant &v : entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() != track || e.value(ClipKey::audioOnly).toBool() || e.value(ClipKey::mcam).toString().isEmpty())
            continue;
        const QVariantMap own = multicamDefinition(e);
        if (!sameCamera(camera, own)) continue;
        Piece p{e.value(ClipKey::index).toInt(), e.value(ClipKey::start).toInt(), 0, false, 0., false};
        p.end = p.start + e.value(ClipKey::frames).toInt();
        // Where the multicam clip's own first moment would lie on the
        // timeline, by this piece: the same for every piece still in step.
        double seconds = 0;
        p.plain = plainSegment(e) && momentOf(e, own, fps(), &seconds);
        p.origin = p.start - seconds * fps();
        p.in = p.index == index;
        pieces.append(p);
    }
    for (bool again = true; again;) {
        again = false;
        for (Piece &p : pieces) {
            if (p.in) continue;
            for (const Piece &q : std::as_const(pieces)) {
                if (!q.in) continue;
                const bool touches = (p.index == q.index + 1 && p.start == q.end) || (q.index == p.index + 1 && q.start == p.end);
                if (touches || (p.plain && q.plain && std::abs(p.origin - q.origin) < .5)) { p.in = again = true; break; }
            }
        }
    }
    QList<int> found;
    for (const Piece &p : std::as_const(pieces))
        if (p.in) found.append(p.index);
    std::sort(found.begin(), found.end());
    return found;
}

bool Timeline::setSoundSourceOn(int track, int index, int source, SoundChange *done) {
    SoundChange local;
    SoundChange &result = done ? *done : local;
    result = {};
    const auto refuse = [&](const QString &why) { result.refused = why; return false; };
    const QList<int> pieces = placedPiecesOf(track, index);
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    if (pieces.isEmpty() || multicamDefinition(entryMapOn(track, index)).value("version").toInt() != 2)
        return refuse(QCoreApplication::translate("Timeline", "This multicam clip was made by an older version and has one sound only; make it again to choose its sound."));
    if (trackLocked(track))
        return refuse(QCoreApplication::translate("Timeline", "%1 is locked; the sound was not changed.").arg(trackName(track)));
    const auto soundOf = [&](const QString &link) {
        for (int i = 0; !link.isEmpty() && i < clips.size(); ++i)
            if (clips.at(i).toMap().value(ClipKey::audioOnly).toBool() && clips.at(i).toMap().value(ClipKey::link).toString() == link) return i;
        return -1;
    };
    const auto retimed = [](const QVariantMap &m) {
        return m.contains(ClipKey::retime) || m.contains(ClipKey::freeze) || !qFuzzyCompare(m.value(ClipKey::speed, 1.).toDouble(), 1.);
    };
    QVariantList asNew = pristine; // the timeline before, without the pieces that change: each is conformed as a new picture is
    QList<int> places, own, silent, fresh; // the changed pieces in `clips`; those to be given their own angle's sound, the silence of no footage, or (retimed) the source's
    QSet<QString> sounding;        // the links of changed pieces that had a sound
    for (const int piece : pieces) {
        const int at = snapshotIndexOn(clips, track, piece, nullptr);
        if (at < 0) return refuse(QCoreApplication::translate("Timeline", "The clip could not be found; the sound was not changed."));
        QVariantMap picture = clips.at(at).toMap();
        QVariantMap camera = multicamDefinition(picture);
        ++result.pieces;
        if (!chooseSoundSourceIn(camera, source))
            return refuse(QCoreApplication::translate("Timeline", "This multicam clip has no such sound source; the sound was not changed."));
        if (soundSourceIn(multicamDefinition(picture)) == source) continue;
        const QString link = picture.value(ClipKey::link).toString();
        const int sound = soundOf(link);
        if (sound >= 0 && trackLocked(clips.at(sound).toMap().value(ClipKey::track).toInt()))
            return refuse(QCoreApplication::translate("Timeline", "Unlock the linked audio track before changing the sound."));
        const QVariantMap was = picture;
        picture[ClipKey::mcam] = QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
        if (sound >= 0) {
            sounding.insert(link);
        } else if (!link.isEmpty()) {
            ++result.removed; // linked to a sound that is gone: the editor removed it
        } else {
            // No link. Its sound was unlinked by the editor (it had one to
            // unlink): left so, and said by a link of its own to no sound, as
            // a removed sound is. Otherwise it never had one (its angle has
            // no sound, or the source it had lay nowhere under it): it is
            // given what the new choice gives it.
            QVariantMap unused;
            const bool hadOne = fixedSoundOf(was).isEmpty()
                                    ? was.value(ClipKey::videoOnly).toBool() && pathHasAudio(was.value(ClipKey::path).toString())
                                    : fixedSoundFor(was, unused);
            const QString path = picture.value(ClipKey::path).toString();
            if (hadOne && !retimed(was)) {
                picture[ClipKey::link] = newLinkId();
                ++result.removed;
            } else if (source < 0 && picture.value(ClipKey::videoOnly).toBool() && pathHasAudio(path)
                       && picture.value(ClipKey::mixIn).toInt() == 0 && picture.value(ClipKey::mixOut).toInt() == 0
                       && !picture.contains(ClipKey::compoundJson) && !picture.contains(ClipKey::freeze)) {
                picture[ClipKey::videoOnly] = false; // its own sound again, put beside it below
                own.append(at);
            } else if (source < 0 && angleWithoutFootage(picture)) {
                silent.append(at); // a stretch without footage has its sound that plays nothing beside it
            } else if (source >= 0 && retimed(picture) && !picture.contains(ClipKey::freeze)) {
                fresh.append(at); // (a plain piece is given its sound when the change settles)
            }
        }
        clips[at] = picture;
        places.append(at);
        asNew[at] = QVariantMap();
    }
    if (places.isEmpty()) return true;
    asNew.removeAll(QVariant(QVariantMap()));
    if (!rebuild(clips, pristine))
        return refuse(QCoreApplication::translate("Timeline", "The timeline could not be rebuilt; the sound was not changed."));
    for (const int at : std::as_const(own)) {
        const QVariantMap picture = clips.at(at).toMap();
        if (!detachAudioOn(picture.value(ClipKey::track).toInt(), picture.value(ClipKey::index).toInt())) {
            restore(pristine);
            return refuse(QCoreApplication::translate("Timeline", "A piece could not be given its own sound; the sound was not changed."));
        }
    }
    for (const int at : std::as_const(silent)) {
        const QVariantMap picture = clips.at(at).toMap();
        if (withSourceOrigin(picture, [&] {
                return connectAt(noFootagePath(), picture.value(ClipKey::start).toInt(), picture.value(ClipKey::in).toInt(), picture.value(ClipKey::out).toInt(), true);
            }))
            linkDetached(picture.value(ClipKey::track).toInt(), picture.value(ClipKey::index).toInt(), QString());
    }
    for (const int at : std::as_const(fresh)) {
        // A retimed piece that had no sound: the source's, at the piece's
        // own speed, where the source covers all of it.
        const QVariantMap picture = clips.at(at).toMap();
        QVariantMap sound;
        if (!fixedSoundFor(picture, sound)) continue;
        const QString path = sound.value(ClipKey::path).toString(), link = newLinkId();
        const int start = sound.value(ClipKey::start).toInt();
        QScopedValueRollback<int> sampling(m_restoreSampling, picture.value(ClipKey::frameBlend).toInt());
        int t = -1, c = -1;
        if (withSourceOrigin(sound, [&] {
                return connectAt(path, start, sound.value(ClipKey::in).toInt(), sound.value(ClipKey::out).toInt(), true, -1,
                                 sound.value(ClipKey::speed, 1.).toDouble(), false, sound.value(ClipKey::retime).toMap());
            }) && placedAudioAt(path, start, &t, &c)) {
            setLinkAt(t, c, link);
            setLinkAt(picture.value(ClipKey::track).toInt(), picture.value(ClipKey::index).toInt(), link);
        }
    }
    {
        // Each changed piece is settled as a picture new to the timeline:
        // the source where it lies under it, or its own angle's sound (the
        // Audio Follows Video switch is about what a later cut does, not
        // about this choice).
        QScopedValueRollback<bool> follows(m_audioFollowsVideo, true);
        conformLinkedSound(asNew);
    }
    result.changed = int(places.size());
    // Each changed piece's sound is now its source's, or the change is not
    // made at all (a sound that could not be laid where it belongs, another
    // clip being in its way, would leave the piece saying one thing and
    // sounding another).
    QSet<QString> now;
    QHash<QString, QString> soundPath;
    const QVariantList settled = entries();
    for (const QVariant &v : settled)
        if (v.toMap().value(ClipKey::audioOnly).toBool()) {
            now.insert(v.toMap().value(ClipKey::link).toString());
            soundPath.insert(v.toMap().value(ClipKey::link).toString(), v.toMap().value(ClipKey::path).toString());
        }
    QSet<int> changedPieces;
    for (const int at : std::as_const(places)) changedPieces.insert(pristine.at(at).toMap().value(ClipKey::index).toInt());
    for (const QVariant &v : settled) {
        const QVariantMap m = v.toMap();
        const QString link = m.value(ClipKey::link).toString();
        if (m.value(ClipKey::track).toInt() != track || m.value(ClipKey::audioOnly).toBool() || !changedPieces.contains(m.value(ClipKey::index).toInt())
            || link.isEmpty() || !soundPath.contains(link))
            continue;
        const QVariantMap fixed = fixedSoundOf(m);
        if (soundPath.value(link) != (fixed.isEmpty() ? m.value(ClipKey::path).toString() : fixed.value("path").toString())) {
            restore(pristine);
            return refuse(QCoreApplication::translate("Timeline", "Another clip is in the way of the new sound on its track; the sound was not changed."));
        }
    }
    for (const int at : std::as_const(places)) {
        const QVariantMap was = pristine.at(at).toMap();
        if (retimed(was) && sounding.contains(was.value(ClipKey::link).toString()) && !now.contains(was.value(ClipKey::link).toString()))
            ++result.retimed;
    }
    return true;
}

// ── Collapse Multicam ────────────────────────────────────────────────────
namespace {
QVariantList compoundClips(const QVariantMap &entry) {
    const QString json = entry.value(ClipKey::compoundJson).toString();
    return json.isEmpty() ? QVariantList() : QJsonDocument::fromJson(json.toUtf8()).toVariant().toList();
}
// The sound is what a link can hold: its picture's own, settled as any
// linked sound is already (see Timeline::Collapse).
bool ownSoundOf(const QVariantMap &picture, const QVariantMap &sound) {
    return sound.value(ClipKey::path) == picture.value(ClipKey::path) && Timeline::fixedSoundOf(picture).isEmpty();
}
}

int Timeline::multicamPiecesIn(const QVariantList &entries) {
    int count = 0;
    for (const QVariant &v : entries) {
        const QVariantMap e = v.toMap();
        count += !e.value(ClipKey::mcam).toString().isEmpty() && !e.value(ClipKey::audioOnly).toBool();
        count += multicamPiecesIn(compoundClips(e));
    }
    return count;
}

Timeline::Collapse Timeline::multicamCollapsed(const QVariantList &entries, const QSet<int> &lockedTracks) {
    Collapse done;
    done.entries = entries;
    QHash<QString, int> soundAt; // link → the sound that carries it
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap e = entries.at(i).toMap();
        const QString link = e.value(ClipKey::link).toString();
        if (e.value(ClipKey::audioOnly).toBool() && !link.isEmpty() && !soundAt.contains(link)) soundAt.insert(link, i);
    }
    const auto refuse = [&](int track) { if (done.lockedTrack < 0) done.lockedTrack = track; };
    QSet<QString> parted; // the links of pairs that are not a picture and its own sound
    // The clips of a list, each without its tag. `inside`: the track of the
    // compound clip they are in (-1 for the timeline's own clips).
    const std::function<bool(QVariantList &, int)> collapse = [&](QVariantList &clips, int inside) {
        bool changed = false;
        for (QVariant &v : clips) {
            QVariantMap e = v.toMap();
            const int track = inside < 0 ? e.value(ClipKey::track).toInt() : inside;
            bool mine = false;
            if (QVariantList inner = compoundClips(e); !inner.isEmpty() && collapse(inner, track)) {
                e[ClipKey::compoundJson] = QString::fromUtf8(QJsonDocument::fromVariant(inner).toJson(QJsonDocument::Compact));
                mine = true;
            }
            if (!e.value(ClipKey::mcam).toString().isEmpty()) {
                if (!e.value(ClipKey::audioOnly).toBool()) {
                    ++done.pieces;
                    const QString link = e.value(ClipKey::link).toString();
                    const auto sound = soundAt.constFind(link);
                    if (sound != soundAt.constEnd() && !ownSoundOf(e, entries.at(*sound).toMap())) {
                        parted.insert(link);
                        if (lockedTracks.contains(entries.at(*sound).toMap().value(ClipKey::track).toInt()))
                            refuse(entries.at(*sound).toMap().value(ClipKey::track).toInt());
                    }
                }
                e.remove(ClipKey::mcam);
                e.remove(ClipKey::mcamAngle);
                mine = true;
            }
            if (!mine) continue;
            if (lockedTracks.contains(track)) refuse(track);
            v = e;
            changed = true;
        }
        return changed;
    };
    collapse(done.entries, -1);
    // Parted as Unlink Clips parts a pair: neither keeps the link.
    const std::function<bool(QVariantList &)> part = [&](QVariantList &clips) {
        bool changed = false;
        for (QVariant &v : clips) {
            QVariantMap e = v.toMap();
            bool mine = false;
            if (QVariantList inner = compoundClips(e); !inner.isEmpty() && part(inner)) {
                e[ClipKey::compoundJson] = QString::fromUtf8(QJsonDocument::fromVariant(inner).toJson(QJsonDocument::Compact));
                mine = true;
            }
            if (parted.contains(e.value(ClipKey::link).toString())) {
                e.remove(ClipKey::link);
                mine = true;
            }
            if (mine) { v = e; changed = true; }
        }
        return changed;
    };
    if (!parted.isEmpty()) part(done.entries);
    done.unlinked = int(parted.size());
    return done;
}

bool Timeline::collapseMulticam(Collapse *done) {
    const QVariantList pristine = entries();
    QSet<int> locked;
    for (int track = 0; track < trackCount(); ++track)
        if (trackLocked(track)) locked.insert(track);
    Collapse result = multicamCollapsed(pristine, locked);
    const bool ok = result.pieces > 0 && result.lockedTrack < 0 && rebuild(result.entries, pristine);
    if (done) *done = std::move(result);
    return ok;
}

// How much of a plain segment `angle` has footage for: `lead` frames with
// none, then `covered` frames with, then none again.
bool Timeline::angleCoverage(const QVariantMap &entry, int angle, int *lead, int *covered) {
    const auto camera = multicamDefinition(entry);
    const auto angles = camera.value("angles").toList();
    const int count = entry.value(ClipKey::frames).toInt();
    double moment = 0;
    if (!plainSegment(entry) || angle < 0 || angle >= angles.size() || count < 1 || !momentOf(entry, camera, fps(), &moment))
        return false;
    const auto target = angles[angle].toMap();
    *lead = count;
    *covered = 0;
    if (camera.value("version").toInt() != 2) {
        const int first = qRound(moment * fps()) - target.value("offset").toInt(); // the angle's own frame at the segment's first
        const int has = target.value("frames").toInt();
        const int from = qMax(0, -first);
        if (from >= count || (has > 0 && first + from >= has)) return true;
        *lead = from;
        *covered = has > 0 ? qMin(count - from, has - (first + from)) : count - from;
        return true;
    }
    const double position = (moment - MulticamTiming::offsetSeconds(target)) * fps();
    if (!std::isfinite(position) || std::abs(position) > 100000000) return false;
    const int from = qMax(0, int(std::ceil(-position - 1e-7)));
    if (from >= count) return true;
    // Its first frame with footage, as the angle would show it: where that
    // lies in the angle, and how many frames the angle has.
    QVariantMap one = partOf(entry, from, 1);
    if (!switchAngleInEntry(one, angle)) return true; // the angle ended before it
    *lead = from;
    *covered = qBound(1, one.value(ClipKey::length).toInt() - one.value(ClipKey::in).toInt(), count - from);
    return true;
}

// The stretch as `angle` shows it where it has no footage (see above).
bool Timeline::showNoFootageIn(QVariantMap &map, int angle) {
    const auto camera = multicamDefinition(map);
    double moment = 0;
    if (!momentOf(map, camera, fps(), &moment)) return false;
    const double position = moment * fps();
    const double aligned = std::abs(position - std::round(position)) < 1e-6 ? std::round(position) : position;
    const int count = map.value(ClipKey::frames).toInt();
    if (!std::isfinite(aligned) || aligned < 0 || aligned + count >= kNoFootageFrames || count < 1) return false;
    const int in = int(std::floor(aligned));
    map[ClipKey::path] = noFootagePath();
    map[ClipKey::in] = in;
    map[ClipKey::out] = in + count - 1;
    map[ClipKey::length] = kNoFootageFrames;
    map[ClipKey::mcamAngle] = angle;
    // Part of a frame, where the multicam clip's clock is not the timeline's.
    if (aligned - in > 1e-6) map[ClipKey::sourceOrigin] = (aligned - in) / fps();
    else map.remove(ClipKey::sourceOrigin);
    for (const QString &key : {ClipKey::sourceMeta, ClipKey::sourceTiming, ClipKey::name, ClipKey::frameBlend, ClipKey::markers,
                               ClipKey::generator, ClipKey::generatorColor})
        map.remove(key);
    return true;
}

// The pieces of the segment clip `index` is part of: its neighbours that are
// the same multicam clip on the same angle and follow on in its time across
// the edge of the angle's footage (a stretch without footage on one side).
void Timeline::segmentPieces(int track, int index, int *first, int *last) const {
    *first = *last = index;
    const auto follows = [&](const QVariantMap &a, const QVariantMap &b) {
        if (!plainSegment(a) || !plainSegment(b) || angleWithoutFootage(a) == angleWithoutFootage(b)
            || a.value(ClipKey::mcamAngle) != b.value(ClipKey::mcamAngle)
            || a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt() != b.value(ClipKey::start).toInt())
            return false;
        const auto camera = multicamDefinition(a);
        double end = 0, begin = 0;
        return sameCamera(camera, multicamDefinition(b)) && momentOf(a, camera, fps(), &end) && momentOf(b, multicamDefinition(b), fps(), &begin)
               && std::abs(end + a.value(ClipKey::frames).toInt() / fps() - begin) < .5 / fps();
    };
    const int count = trackClipCount(track);
    QVariantMap here = entryMapOn(track, index);
    for (QVariantMap at = here; *first > 0;) {
        const QVariantMap before = entryMapOn(track, *first - 1);
        if (!follows(before, at)) break;
        --*first;
        at = before;
    }
    for (QVariantMap at = here; *last + 1 < count;) {
        const QVariantMap after = entryMapOn(track, *last + 1);
        if (!follows(at, after)) break;
        ++*last;
        at = after;
    }
}

// The segment, from `frame` on, shows `angle`: a cut at `frame` when it lies
// inside a piece, footage where the angle has it and black where it has not.
bool Timeline::angleFromOn(int track, int index, int frame, int angle) {
    if (trackLocked(track)) return false;
    int first = index, last = index;
    segmentPieces(track, index, &first, &last);
    const QVariantMap here = entryMapOn(track, index), final = entryMapOn(track, last);
    const int start = here.value(ClipKey::start).toInt(), end = final.value(ClipKey::start).toInt() + final.value(ClipKey::frames).toInt();
    const int angles = multicamDefinition(here).value("angles").toList().size();
    if (frame < start || frame >= start + here.value(ClipKey::frames).toInt() || angle < 0 || angle >= angles
        || angle == here.value(ClipKey::mcamAngle, -1).toInt())
        return false;
    for (int i = index; i <= last; ++i)
        if (!plainSegment(entryMapOn(track, i)) || angleSwitchAudioLocked(track, i)) return false;
    const QVariantList pristine = entries();
    const auto fail = [&] { restore(pristine); return false; };
    if (frame > start && !splitOn(track, frame)) return fail();
    // Where the angle's footage begins and ends inside the pieces: a cut each.
    QList<int> edges;
    for (int at = frame; at < end;) {
        const QVariantMap e = entryMapOn(track, clipIndexContaining(track, at));
        const int from = e.value(ClipKey::start).toInt(), count = e.value(ClipKey::frames).toInt();
        int lead = 0, covered = 0;
        if (from != at || count < 1 || !angleCoverage(e, angle, &lead, &covered)) return fail();
        if (lead > 0 && lead < count) edges << from + lead;
        if (covered > 0 && lead + covered < count) edges << from + lead + covered;
        at = from + count;
    }
    for (const int edge : std::as_const(edges))
        if (!splitOn(track, edge)) return fail();
    QVariantList clips = entries();
    const QVariantList split = clips;
    for (int at = frame; at < end;) {
        const int place = snapshotIndexOn(clips, track, clipIndexContaining(track, at), nullptr);
        if (place < 0) return fail();
        QVariantMap map = clips.at(place).toMap();
        const int count = map.value(ClipKey::frames).toInt();
        int lead = 0, covered = 0;
        if (map.value(ClipKey::start).toInt() != at || !angleCoverage(map, angle, &lead, &covered)) return fail();
        if (lead == 0 && covered == count ? !switchAngleInEntry(map, angle) : covered > 0 || !showNoFootageIn(map, angle)) return fail();
        clips[place] = map;
        at += count;
    }
    if (!rebuild(clips, pristine)) return false;
    settleLinkedAudio(split);
    return joinSegmentPieces(track, frame, end) || fail();
}

// Pieces between `from` and `end` that now follow on from one another (an
// edge of the old angle's footage that the new angle plays through) are one
// piece again, their linked sound with them where it follows on too.
bool Timeline::joinSegmentPieces(int track, int from, int end) {
    const QVariantList before = entries();
    QVariantList clips = before;
    const auto followsOn = [](const QVariantMap &a, const QVariantMap &b) {
        return a.value(ClipKey::path) == b.value(ClipKey::path) && a.value(ClipKey::track) == b.value(ClipKey::track)
               && a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt() == b.value(ClipKey::start).toInt()
               && b.value(ClipKey::in).toInt() == a.value(ClipKey::out).toInt() + 1
               && sameSourceOrigin(a, b)
               && qFuzzyCompare(a.value(ClipKey::speed, 1.).toDouble(), b.value(ClipKey::speed, 1.).toDouble())
               && !a.contains(ClipKey::retime) && !b.contains(ClipKey::retime)
               && a.value(ClipKey::mixOut).toInt() == 0 && b.value(ClipKey::mixIn).toInt() == 0;
    };
    const auto soundOf = [&](const QVariantMap &picture) {
        const QString link = picture.value(ClipKey::link).toString();
        for (int i = 0; !link.isEmpty() && i < clips.size(); ++i) {
            const QVariantMap m = clips.at(i).toMap();
            if (m.value(ClipKey::audioOnly).toBool() && m.value(ClipKey::link).toString() == link) return i;
        }
        return -1;
    };
    const auto join = [&](int a, int b) {
        QVariantMap left = clips.at(a).toMap();
        const QVariantMap right = clips.at(b).toMap();
        left[ClipKey::out] = right.value(ClipKey::out);
        left[ClipKey::frames] = left.value(ClipKey::frames).toInt() + right.value(ClipKey::frames).toInt();
        left[ClipKey::fadeOut] = right.value(ClipKey::fadeOut);
        clips[a] = left;
    };
    bool joined = false;
    for (bool again = true; again;) {
        again = false;
        int previous = -1;
        for (int i = 0; i < clips.size() && !again; ++i) {
            const QVariantMap b = clips.at(i).toMap();
            if (b.value(ClipKey::lane).toInt() != track) continue;
            const int a = previous;
            previous = i;
            if (a < 0) continue;
            const QVariantMap left = clips.at(a).toMap();
            const int at = b.value(ClipKey::start).toInt();
            if (at <= from || at >= end || !followsOn(left, b) || left.value(ClipKey::mcamAngle) != b.value(ClipKey::mcamAngle)
                || !plainSegment(left) || !plainSegment(b))
                continue;
            int soundA = soundOf(left), soundB = soundOf(b);
            if (!fixedSoundOf(left).isEmpty()) {
                // A sound source of its own: the joined picture has the one
                // sound, the source where it lies under the whole of it,
                // whatever each part had (a part the source began after, or
                // ended before, had less of it or none).
                if (soundA >= 0 && soundB >= 0 && clips.at(soundA).toMap().value(ClipKey::track) != clips.at(soundB).toMap().value(ClipKey::track))
                    continue;
                join(a, i);
                QVariantMap picture = clips.at(a).toMap();
                if (soundA < 0 && soundB >= 0) {
                    QVariantMap sound = clips.at(soundB).toMap();
                    if (picture.value(ClipKey::link).toString().isEmpty()) picture[ClipKey::link] = sound.value(ClipKey::link);
                    else sound[ClipKey::link] = picture.value(ClipKey::link);
                    clips[soundB] = sound;
                    soundA = std::exchange(soundB, -1);
                }
                QList<int> gone{i};
                if (soundB >= 0) gone.append(soundB);
                if (soundA >= 0) {
                    QVariantMap sound = clips.at(soundA).toMap();
                    if (fixedSoundFor(picture, sound)) {
                        if (sound.value(ClipKey::start) != picture.value(ClipKey::start)) sound.remove(ClipKey::fadeIn);
                        if (soundB >= 0) sound[ClipKey::fadeOut] = clips.at(soundB).toMap().value(ClipKey::fadeOut);
                        clips[soundA] = sound;
                    } else {
                        picture.remove(ClipKey::link);
                        gone.append(soundA);
                    }
                }
                clips[a] = picture;
                // (The later place first: taking one out moves those after it.)
                std::sort(gone.begin(), gone.end(), std::greater<int>());
                for (const int place : std::as_const(gone)) clips.removeAt(place);
                joined = again = true;
                continue;
            }
            if ((soundA < 0) != (soundB < 0)) continue;
            if (soundA >= 0 && !followsOn(clips.at(soundA).toMap(), clips.at(soundB).toMap())) continue;
            join(a, i);
            if (soundA >= 0) join(soundA, soundB);
            // (The later place first: taking one out moves those after it.)
            clips.removeAt(qMax(i, soundB));
            if (soundB >= 0) clips.removeAt(qMin(i, soundB));
            joined = again = true;
        }
    }
    return !joined || rebuild(clips, before);
}

bool Timeline::angleCutOn(int track, int index) const {
    if (index < 0 || index + 1 >= trackClipCount(track)) return false;
    const QVariantMap a = entryMapOn(track, index), b = entryMapOn(track, index + 1);
    const auto camera = multicamDefinition(a);
    double end = 0, begin = 0;
    return plainSegment(a) && plainSegment(b) && a.value(ClipKey::mcamAngle) != b.value(ClipKey::mcamAngle)
           && a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt() == b.value(ClipKey::start).toInt()
           && sameCamera(camera, multicamDefinition(b)) && momentOf(a, camera, fps(), &end) && momentOf(b, multicamDefinition(b), fps(), &begin)
           && std::abs(end + a.value(ClipKey::frames).toInt() / fps() - begin) < .5 / fps();
}

bool Timeline::switchAngleOn(int track, int index, int angle) {
    int first = index, last = index;
    segmentPieces(track, index, &first, &last);
    if (first == last && !angleWithoutFootage(entryMapOn(track, index)) && switchCoveredAngleOn(track, index, angle))
        return true;
    return angleFromOn(track, first, entryMapOn(track, first).value(ClipKey::start).toInt(), angle);
}

bool Timeline::cutToAngleOn(int track, int index, int frame, int angle) {
    int first = index, last = index;
    segmentPieces(track, index, &first, &last);
    if (first == last && !angleWithoutFootage(entryMapOn(track, index)) && cutToCoveredAngleOn(track, index, frame, angle))
        return true;
    return angleFromOn(track, index, frame, angle);
}
