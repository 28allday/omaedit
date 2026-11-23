#include <atomic>
#include "timeline_p.h"

namespace {

QMutex g_pluginMutex;
QSet<QString> g_disabledProviders;
QVariantMap g_quarantine;
}

// The retime clock: the timeline's rate (in thousandths below 1000)
// divided by the speed, truncated to whole units.
static double timewarpRate(const edit::Profile &profile, double speed) {
    long long num = profile.frame_rate_num(), den = profile.frame_rate_den();
    if (num < 1000) { num *= 1000; den *= 1000; }
    const double scaled = double(num) / std::abs(speed);
    if (scaled > double(INT_MAX)) den = (long long)(double(den) * std::abs(speed));
    else num = (long long)int(scaled);
    return den > 0 ? double(num) / double(den) : 0;
}

// A structural edit changes cuts, not their underlying media. Keep references
// to ordinary file producers until the new playlists have taken fresh cuts of
// them. The pool lasts only for this edit; it is not a persistent media cache.
struct Timeline::RebuildMedia {
    std::vector<std::unique_ptr<edit::Producer>> producers;
};

namespace {

bool validRetimeEntries(const QVariantList &entries,int depth=0) {
    if(depth>8)return false;
    for(const auto &v:entries) {
        const auto e=v.toMap();
        if(!FrameSampling::valid(e.value(ClipKey::frameBlend).toInt()))return false;
        std::optional<double> origin;if(!SourceTiming::readOrigin(e,origin))return false;
        if(e.contains(ClipKey::retime)) {
            const auto definition=e.value(ClipKey::retime).toMap();RetimeMap map;
            if(e.value(ClipKey::retime).metaType().id()!=QMetaType::QVariantMap || !map.set(definition)
                || !qFuzzyCompare(e.value(ClipKey::speed,1.).toDouble(),1.) || e.value(ClipKey::gap).toBool()
                || !e.value(ClipKey::title).toString().isEmpty() || e.contains(ClipKey::compoundJson))return false;
            const int in=e.value(ClipKey::in,-1).toInt(),out=e.value(ClipKey::out,-1).toInt();
            if(in < -1 || (in>=0 && (out<in || out>=map.frames())))return false;
        }
        if(e.contains(ClipKey::compoundJson) && !validRetimeEntries(QJsonDocument::fromJson(e.value(ClipKey::compoundJson).toString().toUtf8()).toVariant().toList(),depth+1))return false;
    }
    return true;
}
}


Timeline::Timeline() = default;

Timeline::~Timeline() = default;
QString Timeline::providerOf(const QString &name) {
    if (name.startsWith(QStringLiteral("avfilter."))) return QStringLiteral("avfilter");
    if (name.startsWith(QStringLiteral("frei0r."))) return QStringLiteral("frei0r");
    if (name == QStringLiteral("vidstab")) return QStringLiteral("vidstab");
    if (name.startsWith(QStringLiteral("opencv."))) return QStringLiteral("opencv");
    return QStringLiteral("omaedit");
}
void Timeline::setDisabledProviders(const QSet<QString> &providers) { QMutexLocker lock(&g_pluginMutex); g_disabledProviders = providers; }
QSet<QString> Timeline::disabledProviders() { QMutexLocker lock(&g_pluginMutex); return g_disabledProviders; }
void Timeline::setQuarantinedServices(const QVariantMap &services) { QMutexLocker lock(&g_pluginMutex); g_quarantine = services; }
QVariantMap Timeline::quarantinedServices() { QMutexLocker lock(&g_pluginMutex); return g_quarantine; }
bool Timeline::serviceAvailable(const QString &type, const QString &name) {
    {
        QMutexLocker lock(&g_pluginMutex);
        if (g_disabledProviders.contains(providerOf(name)) || g_quarantine.contains(name)) return false;
    }
    return serviceListed(type, name);
}
bool Timeline::serviceListed(const QString &, const QString &) {
    // Every attribute is the editor's own now: none waits on a plug-in.
    return true;
}

bool Timeline::lockProfileTo(const QString &firstClip) {
    auto profile = std::make_unique<edit::Profile>();
    const bool missing = QFileInfo(firstClip).isAbsolute() && !QFileInfo::exists(firstClip);
    // Recovered nested documents carry the native snapshot without a
    // profile element. Their saved profile remains authoritative; probing the
    // thin XML graph would otherwise replace it with the default format.
    const auto nestedProfile=ProjectFile::isProject(firstClip)
        ?ProjectSafety::readDocument(firstClip).value("profile").toMap():QVariantMap{};
    if (!nestedProfile.isEmpty() || (missing && ((m_allowOffline && m_restoring) || playbackPath(firstClip) != firstClip))) {
        QVariantMap meta = nestedProfile.isEmpty()?m_fallbackProfile:nestedProfile;
        if(nestedProfile.isEmpty()) {
            const QVariantMap source = m_mediaSources.value(firstClip).toMap();
            for (auto it = source.constBegin(); it != source.constEnd(); ++it) meta.insert(it.key(), it.value());
        }
        if (meta.value(QStringLiteral("width")).toInt() > 0) profile->set_width(meta.value(QStringLiteral("width")).toInt());
        if (meta.value(QStringLiteral("height")).toInt() > 0) profile->set_height(meta.value(QStringLiteral("height")).toInt());
        if (meta.value(QStringLiteral("fpsNum")).toInt() > 0)
            profile->set_frame_rate(meta.value(QStringLiteral("fpsNum")).toInt(), qMax(1, meta.value(QStringLiteral("fpsDen")).toInt()));
        else if (meta.value(QStringLiteral("fps")).toDouble() > 0)
            profile->set_frame_rate(qRound(meta.value(QStringLiteral("fps")).toDouble()*1000), 1000);
        const QStringList sar = meta.value(QStringLiteral("pixelAspect")).toString().split(QLatin1Char(':'));
        const int sarNum = sar.size() == 2 ? sar.first().toInt() : meta.value(QStringLiteral("sarNum"), 1).toInt();
        const int sarDen = sar.size() == 2 ? sar.last().toInt() : meta.value(QStringLiteral("sarDen"), 1).toInt();
        profile->set_sample_aspect(qMax(1, sarNum), qMax(1, sarDen));
        profile->set_display_aspect(profile->width()*qMax(1,sarNum),profile->height()*qMax(1,sarDen));
        profile->set_progressive(meta.value("progressive",true).toBool()?1:0);
        if (meta.value(QStringLiteral("colorspace")).toInt() > 0) profile->set_colorspace(meta.value(QStringLiteral("colorspace")).toInt());
    } else if (QFileInfo(firstClip).isFile()) {
        // The first clip's own clock and frame shape.
        const media::Probe probe = media::probeCached(firstClip);
        if (!probe.valid) return false;
        profileFromProbe(*profile, probe);
    }
    applyProjectProfile(*profile);
    m_profile = std::move(profile);
    m_rebuildProfileValid = true;
    settleStartFrame();
    m_tracks.clear();
    Track v1;
    v1.kind = Track::Video;
    v1.playlist = std::make_unique<edit::Playlist>(*m_profile);
    m_tracks.push_back(std::move(v1));
    return true;
}

int Timeline::primaryFrameCount() const {
    return primary() ? primary()->get_playtime() : 0;
}


// V1 holds empty space somewhere (a blank entry) — the bare playlist would
// show the test card there, so playback and export go through the tractor.
bool Timeline::primaryHasBlanks() const {
    if (!primary())
        return false;
    for (int i = 0; i < primary()->count(); ++i)
        if (primary()->is_blank(i))
            return true;
    return false;
}

bool Timeline::primaryNeedsComposite() const {
    if (!primary())
        return false;
    for (int i = 0; i < primary()->count(); ++i) {
        if (primary()->is_blank(i) || primary()->is_mix(i))
            continue;
        std::unique_ptr<edit::ClipInfo> info(primary()->clip_info(i));
        if (!info || !info->cut)
            continue;
        for (int k = 0; k < info->cut->filter_count(); ++k) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(k));
            if (f && f->is_valid() && f->get_int("omavid:cropAlpha") && !f->get_int("disable"))
                return true;
        }
    }
    return false;
}

// A source as the edit graph holds it: its resource and its length on the
// timeline's clock at `effective` speed, as the timeline has always counted
// it (a file's duration on that clock, a still, colour or generator held,
// a document or nest its own length), with its own frames and rate for a
// conform. Invalid when it cannot be read.
edit::Producer Timeline::sourceProducer(const QString &resource, const QString &playback, double effective,
                                           int *nativeFrames, double *nativeRate) {
    int frames = MediaPlayer::kStillFrames, native = MediaPlayer::kStillFrames;
    double rate = m_profile->fps();
    bool audio = false, video = true;
    const QString file = playback.startsWith(QLatin1String("omaedit.project:")) ? playback.mid(16) : playback;
    const QFileInfo info(file);
    const bool document = info.isFile() && ProjectFile::isProject(file);
    if (document) {
        // A nest: its own timeline's length on its own clock.
        const QVariantMap doc = ProjectSafety::readDocument(file);
        if (doc.isEmpty()) return {};
        const QVariantMap profile = doc.value(QStringLiteral("profile")).toMap();
        const int num = profile.value(QStringLiteral("fpsNum")).toInt(), den = profile.value(QStringLiteral("fpsDen")).toInt();
        if (num > 0 && den > 0) rate = double(num) / den;
        native = 1;
        for (const QVariant &v : doc.value(QStringLiteral("entries")).toList()) {
            const QVariantMap e = v.toMap();
            native = std::max(native, e.value(ClipKey::start).toInt() + e.value(ClipKey::frames).toInt());
        }
        frames = qFuzzyCompare(rate, m_profile->fps()) ? native : int(std::lrint(native * m_profile->fps() / rate));
        audio = true;
    } else if (info.isFile()) {
        const media::Probe probe = media::probeCached(file);
        if (!probe.valid) return {};
        native = probe.frames;
        rate = probe.fps();
        audio = probe.hasAudio;
        video = probe.hasVideo || probe.still;
        frames = media::framesOn(probe, qFuzzyCompare(std::abs(effective), 1.) ? m_profile->fps() : timewarpRate(*m_profile, effective));
    } else if ((info.isAbsolute() || !file.contains(QLatin1Char(':'))) && generatorKindOf(file).isEmpty()) {
        return {};  // a file that is not there (generators name no file)
    } else if (file == noFootagePath()) {
        frames = native = kNoFootageFrames;  // the multicam clip's own frames, however long it is
    }
    edit::Producer producer(*m_profile, resource.toUtf8().constData(), std::max(1, frames));
    producer.set("audio_index", audio ? 1 : -1);
    producer.set("video_index", video ? 0 : -1);
    if (nativeFrames) *nativeFrames = native;
    if (nativeRate) *nativeRate = rate;
    return producer;
}

// As sourceProducer reads the file: the one that plays (a proxy stands in
// for its original), a still counted as a picture, a nest as one too.
bool Timeline::soundOnlySource(const QString &path) const {
    const QString playback = AudioEngine::projectResource(playbackPath(path));
    if (playback.startsWith(QLatin1String("omaedit.project:")))
        return false;
    return !ProjectFile::isProject(playback) && media::soundOnly(playback);
}

// `source` read along a speed map: its length the map's, the map kept on it.
// Refused when the map reads past the source.
edit::Producer Timeline::retimedProducer(const edit::Producer &source, int nativeFrames, double nativeRate,
                                            const QVariantMap &definition, bool explicitPictures, double sourceScale,
                                            QString *error) {
    RetimeMap map;
    if (!map.set(definition, error)) return {};
    const double length = explicitPictures ? nativeFrames * m_profile->fps() / nativeRate : nativeFrames;
    const double rounding = explicitPictures ? .5 * qMax(1., sourceScale) : 0.;
    if (map.sourceLength() * sourceScale > length + rounding + 1e-6) {
        if (error) *error = QCoreApplication::translate("SpeedRamp", "The timing map is longer than the available source.");
        return {};
    }
    edit::Producer result(*m_profile, source.get("resource"), map.frames());
    result.inherit(source);
    result.set("length", map.frames());
    result.set("in", 0);
    result.set("out", map.frames() - 1);
    result.set("omavid:retime", QJsonDocument::fromVariant(map.definition()).toJson(QJsonDocument::Compact).constData());
    return result;
}

edit::Producer Timeline::wrapRetime(edit::Producer &source, const QVariantMap &retime) {
    if (!source.is_valid()) return {};
    return retimedProducer(source, source.get_length(), m_profile->fps(), retime, false, 1., nullptr);
}

edit::Producer Timeline::makeSampledProducer(const QString &path, bool audioOnly,
    double speed, bool videoOnly, const QVariantMap &retime, int sampling) {
    const QString playback = AudioEngine::projectResource(playbackPath(path));
    const auto settings = m_mediaSources.value(path).toMap();
    int nativeFrames = 0;
    double nativeRate = 0;
    edit::Producer source = sourceProducer(AnimatedWebP::resource(playback), playback, 1., &nativeFrames, &nativeRate);
    if (!source.is_valid() || nativeRate <= 0 || nativeFrames < 1) return {};
    const double scale = interpretationFactor(settings);
    const double userSpeed = speed == 0 ? 1. : speed;
    auto definition = retime.isEmpty()
        ? RetimeMap::conform(nativeFrames,nativeRate,m_profile->fps(),userSpeed,scale,m_sourceOrigin) : retime;
    if (definition.isEmpty()) return {};
    edit::Producer result = retimedProducer(source, nativeFrames, nativeRate, definition, true, scale, nullptr);
    if (!result.is_valid()) return {};
    applyInputTransform(*m_profile, result, settings);
    if (audioOnly) result.set("video_index",-1);
    else if (videoOnly) result.set("audio_index",-1);
    result.set("omavid:path",path.toUtf8().constData());
    result.set("omavid:sampling",sampling);
    if(m_sourceOrigin)result.set("omavid:sourceOrigin",*m_sourceOrigin);
    result.set("_omaedit.sourceTiming",QJsonDocument::fromVariant(QVariantMap{
        {"duration",nativeFrames/nativeRate},{"rate",m_profile->fps()/scale}}).toJson(QJsonDocument::Compact).constData());
    if (!settings.isEmpty()) result.set("omavid:sourceMeta",QJsonDocument::fromVariant(settings).toJson(QJsonDocument::Compact).constData());
    if (audioOnly) result.set("omavid:audioOnly",1);
    else if (videoOnly) result.set("omavid:videoOnly",1);
    if (retime.isEmpty()) {
        result.clear("omavid:retime");
        result.set("omavid:conform",1);
        result.set("_omaedit.conformDefinition",QJsonDocument::fromVariant(definition).toJson(QJsonDocument::Compact).constData());
        if (!qFuzzyCompare(userSpeed,1.)) result.set("omavid:speed",userSpeed);
    }
    return result;
}

edit::Producer Timeline::makeClipProducer(const QString &path, int in,
                                            int out, double speed, bool gap,
                                            bool videoOnly, const QVariantMap &retime) {
    if ((FrameSampling::explicitPictures(m_restoreSampling) || m_sourceOrigin) && !gap
        && !path.startsWith("color:") && !MediaPlayer::isStillPath(path)
        && generatorKindOf(path).isEmpty() && !path.startsWith("tone:")
        && !(QFileInfo(path).isAbsolute() && !QFileInfo::exists(path) && m_allowOffline
             && !QFileInfo::exists(playbackPath(path))))
        return makeSampledProducer(path,false,speed,videoOnly,retime,m_restoreSampling);
    if (!retime.isEmpty()) {
        if (gap || !qFuzzyCompare(speed, 1.0)) return edit::Producer();
        auto source = makeClipProducer(path, -1, -1, 1.0, false, videoOnly);
        return wrapRetime(source, retime);
    }
    // Negative = reversed: timewarp plays the material backwards. A frame-rate
    // interpretation (Clip Attributes) composes with the user's retime; only
    // the user's speed is remembered on the producer.
    const QVariantMap settings = m_mediaSources.value(path).toMap();
    const bool userRetimed = speed != 0.0 && !qFuzzyCompare(speed, 1.0);
    const double effective = (speed == 0.0 ? 1.0 : speed) * interpretationFactor(settings);
    const bool retimed = !qFuzzyCompare(effective, 1.0);
    const QString playback = AudioEngine::projectResource(playbackPath(path));
    const bool offline = QFileInfo(playback).isAbsolute() && !QFileInfo::exists(playback) && m_allowOffline && m_restoring;
    const QString resource = offline ? QStringLiteral("color:0x181820") :
        retimed ? QStringLiteral("timewarp:%1:%2")
                      .arg(QString::number(effective), AnimatedWebP::resource(playback))
                : AnimatedWebP::resource(playback);
    edit::Producer producer = offline ? edit::Producer(*m_profile, resource.toUtf8().constData())
                                      : sourceProducer(resource, playback, retimed ? effective : 1.);
    if (!producer.is_valid())
        return producer;
    // The path it reopens from — snapshots restore from it.
    producer.set("omavid:path", path.toUtf8().constData());
    if(m_sourceOrigin)producer.set("omavid:sourceOrigin",*m_sourceOrigin);
    if (offline) {
        producer.set("omavid:offline", 1);
        // Snapshot bounds already use the project's clock. Library duration
        // can still be in recorded pictures, especially with mixed-rate media.
        const int sourceFrames = m_offlineLengths.value(path, settings.value(QStringLiteral("duration"), 100000000).toInt());
        const int length = qMax(1, qRound(sourceFrames / qMax(0.001, qAbs(speed))));
        producer.set("length", length);
        producer.set_in_and_out(0, length - 1);
    }
    applyInputTransform(*m_profile, producer, settings);
    if (!settings.isEmpty()) producer.set("omavid:sourceMeta", QJsonDocument::fromVariant(settings).toJson(QJsonDocument::Compact).constData());
    if (userRetimed)
        producer.set("omavid:speed", speed);
    if (retimed && !offline) {
        // Keep the pitch at the new rate (rbpitch under the hood).
        // Verified 7.40: honoured even set post-open.
        producer.set("warp_pitch", 1);
        holdRetimedTail(*m_profile, producer, effective);
    }
    if (videoOnly) {
        // Drop the audio stream entirely (the detached-video half of Detach
        // Audio). Verified 7.40: propagates through a timewarp wrapper too.
        producer.set("audio_index", -1);
        producer.set("omavid:videoOnly", 1);
        if(playback.contains("omaedit.project:"))attachRecord(producer,"omaedit.fader",{{"muted",true}},"omavid:nestVideoOnly");
    }
    if (gap) {
        producer.set("omavid:gap", 1);
        if (in >= 0) {
            producer.set("length", out + 1);
            producer.set_in_and_out(in, out);
        }
    }
    return producer;
}

bool Timeline::withSourceOrigin(const QVariantMap &source, const std::function<bool()> &place) {
    std::optional<double> origin;
    if(!SourceTiming::readOrigin(source,origin))return false;
    QScopedValueRollback<std::optional<double>> scope(m_sourceOrigin,origin);
    return place();
}

bool Timeline::append(const QString &path, int in, int out, double speed,
                         bool gap, bool videoOnly, const QVariantMap &retime) {
    if (trackLocked(0))
        return false;
    if (!primary() && !lockProfileTo(path))
        return false;

    edit::Producer producer =
        makeClipProducer(path, in, out, speed, gap, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;

    return (in >= 0 ? primary()->append(producer, in, out)
                    : primary()->append(producer)) == 0;
}

bool Timeline::insertGapAt(int position, int frames) {
    if (trackLocked(0))
        return false;
    if (!primary() || clipCount() == 0 || frames < 1)
        return false;
    // A gap is empty track space (A3) — a playlist blank, not a clip. The
    // timeline ends at its last clip, so there is no gap to insert AT the
    // end; move the clips instead.
    if (position < 0 || position >= primaryFrameCount())
        return false;
    if (!ensureBoundaryAt(position))
        return false;
    const int entry = primary()->get_clip_index_at(position);
    if (primary()->is_mix(entry)
        || (entry > 0 && primary()->is_mix(entry - 1)))
        return false; // never wedge a gap into a dissolve
    if (breakRippledLaneDissolves(0, position) < 0)
        return false;
    primary()->insert_blank(primary()->get_clip_index_at(position), frames - 1);
    tidyPrimary();
    rippleOthersFrom(0, position, frames);
    return true;
}

int Timeline::closeGapOn(int track, int position, QString *error, int *from) {
    auto refuse = [error](const QString &why) {
        if (error)
            *error = why;
        return 0;
    };
    if (track < 0 || track >= trackCount() || position < 0 || !primary())
        return -1;
    if (trackLocked(track))
        return refuse(QCoreApplication::translate("Timeline", "That track is locked."));
    struct Span { int track, start, end; QString link; };
    QList<Span> spans;
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        if (m.value(ClipKey::gap).toBool())
            continue;
        const int start = m.value(ClipKey::start).toInt();
        spans.append({m.value(ClipKey::track).toInt(), start,
                      start + m.value(ClipKey::frames).toInt(),
                      m.value(ClipKey::link).toString()});
    }
    // The gap: from the last clip ending at or before the point to the
    // first clip after it. Space after the last clip is not a gap.
    int gapStart = 0, gapEnd = -1;
    for (const Span &c : std::as_const(spans)) {
        if (c.track != track)
            continue;
        if (c.start <= position && position < c.end)
            return refuse(QCoreApplication::translate("Timeline", "There is no gap there."));
        if (c.end <= position)
            gapStart = qMax(gapStart, c.end);
        else if (gapEnd < 0 || c.start < gapEnd)
            gapEnd = c.start;
    }
    if (gapEnd <= gapStart)
        return refuse(QCoreApplication::translate("Timeline", "There is no gap there."));
    // Which clips ride, by the ripple rule rippleFrom applies: the source
    // track's own clips after the gap, and those of every unlocked,
    // auto-selected other track — sound linked to a V1 picture only when
    // that picture moves.
    auto rides = [this, track](int t) {
        return t == track || (!m_tracks[size_t(t)].locked && m_tracks[size_t(t)].autoSelect);
    };
    QHash<QString, int> pictures;
    for (const Span &c : std::as_const(spans))
        if (c.track == 0 && !c.link.isEmpty())
            pictures.insert(c.link, c.start);
    auto moves = [&](const Span &c) {
        if (c.start < gapEnd || !rides(c.track))
            return false;
        if (track != 0 && c.track != track && c.track != 0 && pictures.contains(c.link))
            return rides(0) && pictures.value(c.link) >= gapEnd;
        return true;
    };
    // As far as every moving clip has empty room before it. V1 closes only
    // empty space ending right at the gap's end (shiftPrimaryFrom).
    int frames = gapEnd - gapStart;
    for (const Span &m : std::as_const(spans)) {
        if (!moves(m))
            continue;
        frames = qMin(frames, m.start);
        for (const Span &s : std::as_const(spans))
            if (s.track == m.track && !moves(s) && s.start < m.start)
                frames = qMin(frames, m.track == 0 && track != 0 ? gapEnd - s.end : m.start - s.end);
    }
    if (frames < 1)
        return refuse(QCoreApplication::translate("Timeline", "Clips on other tracks are in the way, so the gap can't close. "
                                  "Turn off Auto Track Select on those tracks to close it on this track alone."));
    if (breakRippledLaneDissolves(track, gapEnd) < 0)
        return -1;
    if (track == 0) {
        if (primary()->remove_region(gapEnd - frames, frames) < 0)
            return -1;
        tidyPrimary();
        rippleOthersFrom(0, gapEnd, -frames);
    } else {
        rippleFrom(track, gapEnd, -frames, true);
    }
    tidyConnected();
    if (from)
        *from = gapEnd - frames;
    return frames;
}

int Timeline::closeCommonGaps(QList<QPair<int, int>> *closedSpans) {
    if (!primary())
        return -1;
    int closed = 0;
    // Last stretch first, so the earlier ones keep their place.
    for (int before = std::numeric_limits<int>::max();;) {
        QList<QPair<int, int>> spans; // every clip on every track
        QSet<int> lockedAfter;        // starts of locked tracks' clips
        for (const QVariant &v : entries()) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::gap).toBool())
                continue;
            const int start = m.value(ClipKey::start).toInt();
            spans.append({start, start + m.value(ClipKey::frames).toInt()});
            if (trackLocked(m.value(ClipKey::track).toInt()))
                lockedAfter.insert(start);
        }
        std::sort(spans.begin(), spans.end());
        // Walk the union of the spans; keep the last hole before `before`.
        int gapStart = -1, gapEnd = -1, reach = -1;
        for (const auto &span : std::as_const(spans)) {
            if (reach >= 0 && span.first > reach && span.first <= before) {
                bool locked = false;
                for (int start : std::as_const(lockedAfter))
                    locked = locked || start >= span.first;
                if (!locked) {
                    gapStart = reach;
                    gapEnd = span.first;
                }
            }
            reach = qMax(reach, span.second);
        }
        if (gapEnd < 0)
            return closed;
        const int frames = gapEnd - gapStart;
        if (gapEnd < primaryFrameCount()) {
            if (primary()->remove_region(gapStart, frames) < 0)
                return -1;
            tidyPrimary();
        }
        QList<int> wanted;
        for (const QVariant &v : entries()) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::lane).toInt() < 1)
                continue;
            const int start = m.value(ClipKey::start).toInt();
            wanted.append(start >= gapEnd ? start - frames : start);
        }
        if (connectedCount() > 0)
            replaceLaneStarts(wanted);
        tidyConnected();
        if (closedSpans)
            closedSpans->append({gapStart, frames});
        ++closed;
        before = gapStart;
    }
}

bool Timeline::setClipDisabled(int index, bool disabled) {
    return setDisabledAt(0, index, disabled);
}

bool Timeline::setClipColor(int index, double brightness, double contrast,
                               double saturation) {
    return setColorAt(0, index, brightness, contrast, saturation);
}

double Timeline::speedSourceLength(const QVariantMap &entry) {
    QScopedValueRollback<int> sampling(m_restoreSampling,entry.value(ClipKey::frameBlend).toInt());
    std::optional<double> origin;if(!SourceTiming::readOrigin(entry,origin))return 0;
    QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
    const auto path=entry.value(ClipKey::path).toString();
    if(!primary() && !lockProfileTo(path))return 0;
    auto source=makeLaneProducer(path,entry.value(ClipKey::audioOnly).toBool(),1.,entry.value(ClipKey::videoOnly).toBool());
    if(!source.is_valid())return 0;
    if(origin) {
        const auto conform=QJsonDocument::fromJson(QByteArray(source.get("_omaedit.conformDefinition"))).toVariant().toMap();
        if(!conform.isEmpty())return conform.value("sourceLength").toDouble();
    }
    return source.get_length();
}

QVariantMap Timeline::speedRampState(int track,int index) {
    const auto entry=entryMapOn(track,index);
    auto fail=[](const QString &error){return QVariantMap{{"valid",false},{"error",error}};};
    if(entry.isEmpty() || trackLocked(track))return fail(QCoreApplication::translate("SpeedRamp","The clip is unavailable or its track is locked."));
    if(entry.value(ClipKey::gap).toBool() || entry.contains(ClipKey::compoundJson) || !entry.value(ClipKey::title).toString().isEmpty())
        return fail(QCoreApplication::translate("SpeedRamp","Choose a media clip to edit its speed segments."));
    if(entry.value(ClipKey::offline).toBool())return fail(QCoreApplication::translate("SpeedRamp","Relink this clip before changing its speed segments."));
    const auto link=entry.value(ClipKey::link).toString();
    if(!link.isEmpty() && !entry.value(ClipKey::audioOnly).toBool())for(const auto &v:entries()) {
        const auto mate=v.toMap();if(mate.value(ClipKey::link).toString()!=link || !mate.value(ClipKey::audioOnly).toBool())continue;
        if(trackLocked(mate.value(ClipKey::track).toInt()))return fail(QCoreApplication::translate("SpeedRamp","Unlock the linked audio track before changing speed."));
        if(mate.value(ClipKey::path)!=entry.value(ClipKey::path))return fail(QCoreApplication::translate("SpeedRamp","Unlink the alternate-angle audio before editing speed segments."));
    }
    const double length=speedSourceLength(entry);
    if(length<=0)return fail(QCoreApplication::translate("SpeedRamp","The source could not be opened for speed editing."));
    if(entry.contains(ClipKey::sourceOrigin) && !entry.contains(ClipKey::retime)) {
        std::optional<double> origin;if(!SourceTiming::readOrigin(entry,origin))return fail(QCoreApplication::translate("SpeedRamp","The source could not be opened for speed editing."));
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
        QScopedValueRollback<int> sampling(m_restoreSampling,entry.value(ClipKey::frameBlend).toInt());
        auto source=makeLaneProducer(entry.value(ClipKey::path).toString(),entry.value(ClipKey::audioOnly).toBool(),
            entry.value(ClipKey::speed,1.).toDouble(),entry.value(ClipKey::videoOnly).toBool());
        if(!source.is_valid())return fail(QCoreApplication::translate("SpeedRamp","The source could not be opened for speed editing."));
        const auto conform=QJsonDocument::fromJson(QByteArray(source.get("_omaedit.conformDefinition"))).toVariant().toMap();
        if(conform.isEmpty())return fail(QCoreApplication::translate("SpeedRamp","The source could not be opened for speed editing."));
        auto preparedEntry=entry;preparedEntry[ClipKey::retime]=conform;
        return SpeedRampEdit::prepare(preparedEntry,length);
    }
    return SpeedRampEdit::prepare(entry,length);
}

bool Timeline::setSpeedRampOn(int track,int index,const QVariantMap &definition,QString *error,bool normal) {
    if(error)error->clear();
    const auto prepared=speedRampState(track,index);
    if(!prepared.value("valid").toBool()){if(error)*error=prepared.value("error").toString();return false;}
    const auto entry=entryMapOn(track,index);
    const double sourceRate=m_profile->fps()/interpretationFactor(m_mediaSources.value(entry.value(ClipKey::path).toString()).toMap());
    const auto result=SpeedRampEdit::splice(entry,prepared.value("parent").toMap(),definition,normal,sourceRate);
    if(!result.value("valid").toBool()){if(error)*error=result.value("error").toString();return false;}
    const auto draft=SpeedRampEdit::inspect(definition);
    if(normal) {
        if(!entry.contains(ClipKey::retime) && !entry.contains(ClipKey::freeze) && qFuzzyCompare(entry.value(ClipKey::speed,1.).toDouble(),1.)
            && entry.value(ClipKey::in)==result.value("in") && entry.value(ClipKey::out)==result.value("out"))return false;
    } else if(entry.contains(ClipKey::retime) && !entry.contains(ClipKey::freeze) && prepared.value("definition").toMap()==draft.value("definition").toMap())return false;
    const auto before=entries();auto clips=before;
    const int at=snapshotIndexOn(clips,track,index,nullptr);if(at<0)return false;
    const int oldEnd=entry.value(ClipKey::start).toInt()+entry.value(ClipKey::frames).toInt(),delta=result.value("delta").toInt();
    const int primaryBefore=primaryFrameCount();const auto link=entry.value(ClipKey::link).toString();
    auto retime=[&](QVariantMap old) {
        const double ratio=result.value("ratio").toDouble();const int oldIn=old.value(ClipKey::in).toInt();
        for(const auto &key:QStringList{"retime","in","out","frames","length"})old[key]=result.value(key);
        if(result.contains("sourceOrigin"))old[ClipKey::sourceOrigin]=result.value("sourceOrigin");
        if(normal)old.remove(ClipKey::retime);
        old[ClipKey::speed]=1.;old.remove(ClipKey::freeze);
        old[ClipKey::editKeys]=EditEngine::scaledKeys(old.value(ClipKey::editKeys).toMap(),ratio,result.value("frames").toInt());
        if(old.contains(ClipKey::videoEffects))old[ClipKey::videoEffects]=EditEngine::scaledEffects(old.value(ClipKey::videoEffects).toList(),ratio);
        old[ClipKey::opacityKf]=EditEngine::scaledCurve(old.value(ClipKey::opacityKf).toList(),ratio);
        old[ClipKey::volumeKf]=EditEngine::scaledCurve(old.value(ClipKey::volumeKf).toList(),ratio);
        auto markers=old.value(ClipKey::markers).toList();for(auto &v:markers){auto m=v.toMap();m["frame"]=result.value("in").toInt()+qRound((m.value("frame").toInt()-oldIn)*ratio);v=m;}old[ClipKey::markers]=markers;
        for(const auto &key:QStringList{"fadeIn","fadeOut"})if(old.contains(key))old[key]=qMin(result.value("frames").toInt(),qRound(old.value(key).toInt()*ratio));
        return old;
    };
    clips[at]=retime(entry);
    if(!entry.value(ClipKey::audioOnly).toBool() && !link.isEmpty())for(int i=0;i<clips.size();++i) {
        const auto mate=clips[i].toMap();if(mate.value(ClipKey::link).toString()==link && mate.value(ClipKey::audioOnly).toBool()) {
            auto next=retime(mate);next[ClipKey::start]=entry.value(ClipKey::start);clips[i]=next;
        }
    }
    if(track>=1)shiftSnapshotLane(&clips,track,oldEnd,delta);
    if(!rebuild(clips,before)){if(error)*error=QCoreApplication::translate("SpeedRamp","The speed edit could not be applied. The previous timing was preserved.");return false;}
    rippleOthersFrom(track,oldEnd,track==0?primaryFrameCount()-primaryBefore:delta);
    return true;
}

bool Timeline::setClipSpeed(int index, double speed) {
    return setClipSpeedOn(0, index, speed);
}

// A transition made of spare frames counts them in its clips' in and out
// points: retimed with them, the clip would come out cut at another of its
// frames. The transitions on the clip's two cuts are held aside while it is
// retimed as the clip drawn (withMixesHeld).
bool Timeline::setClipSpeedOn(int track, int index, double speed) {
    if (m_restoring || handleMixesOn(track).isEmpty())
        return setClipSpeedPlain(track, index, speed);
    const QVariantMap clip = entryMapOn(track, index);
    if (!std::isfinite(speed) || trackLocked(track) || !RetimeEdit::preview(clip, speed).value("valid").toBool()
        || qFuzzyCompare(clip.value(ClipKey::speed, 1.0).toDouble(), speed))
        return false; // refused before anything is taken apart
    return withMixesHeld(track, index, [&] { return setClipSpeedPlain(track, index, speed); });
}

bool Timeline::setClipSpeedPlain(int track, int index, double speed) {
    // Sign is direction; magnitude is the rate.
    if (!std::isfinite(speed) || qAbs(speed) < 0.05 || qAbs(speed) > 20.0 || trackLocked(track))
        return false;
    const QVariantList before = entries();
    const int primaryBefore = primaryFrameCount();
    QVariantList clips = before;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    const auto result = RetimeEdit::preview(map,speed);
    if (!result.value("valid").toBool()) return false;
    const double current = map.value(ClipKey::speed, 1.0).toDouble();
    if (qFuzzyCompare(current, speed)) return false;
    const double ca = qAbs(current), a = qAbs(speed);
    const int oldIn = map.value(ClipKey::in).toInt(), oldOut = map.value(ClipKey::out).toInt();
    const int newIn = result.value(ClipKey::in).toInt(), newOut = result.value(ClipKey::out).toInt();
    // Clip markers are timewarp frames like in/out: rescale them with the
    // bounds (and mirror them when the direction flips).
    QVariantList markers = map.value(ClipKey::markers).toList();
    if (!markers.isEmpty()) {
        const bool flip = (current < 0.0) != (speed < 0.0);
        const int newLen = result.value(ClipKey::length).toInt();
        for (QVariant &v : markers) {
            QVariantMap mk = v.toMap();
            int frame;
            if (map.contains(ClipKey::sourceOrigin)) {
                const int relative=qRound((mk.value("frame").toInt()-oldIn)*ca/a);
                frame=flip ? newOut-relative : newIn+relative;
            } else {
                frame=qRound(mk.value(QStringLiteral("frame")).toInt() * ca / a);
                if (flip)
                frame = qMax(0, newLen - 1 - frame);
            }
            mk.insert(QStringLiteral("frame"), frame);
            v = mk;
        }
        map.insert(ClipKey::markers, markers);
    }
    map.insert(ClipKey::in, newIn);
    map.insert(ClipKey::out, newOut);
    map.insert(ClipKey::speed, speed);
    if (result.contains(ClipKey::sourceOrigin)) map[ClipKey::sourceOrigin]=result.value(ClipKey::sourceOrigin);
    map[ClipKey::editKeys]=EditEngine::scaledKeys(map.value(ClipKey::editKeys).toMap(),ca/a,newOut-newIn+1);
    if(map.contains(ClipKey::videoEffects))map[ClipKey::videoEffects]=EditEngine::scaledEffects(map.value(ClipKey::videoEffects).toList(),ca/a);
    map[ClipKey::opacityKf] = EditEngine::scaledCurve(map.value(ClipKey::opacityKf).toList(), ca/a);
    map[ClipKey::volumeKf] = EditEngine::scaledCurve(map.value(ClipKey::volumeKf).toList(), ca/a); // clip frames too, as the ramp path scales them
    clips[at] = map;
    const int oldEnd = map.value(ClipKey::start).toInt()
                       + map.value(ClipKey::frames).toInt();
    const int delta = (newOut - newIn) - (oldOut - oldIn);
    // Lanes place by absolute start: make room for (or close up after) the
    // resized clip on its own track before the rebuild.
    if (track >= 1)
        shiftSnapshotLane(&clips, track, oldEnd, delta);
    if (!rebuild(clips, before))
        return false;
    // The clip's length changed — everything on the auto-selected other
    // tracks from the clip's OLD end onward rides the delta.
    rippleOthersFrom(track, oldEnd,
                     track == 0 ? primaryFrameCount() - primaryBefore : delta);
    return true;
}


void Timeline::resetForRestore() {
    if (!m_rebuildMedia) { clear(); return; }
    // Producers reference this profile. Retain it for this same-format edit;
    // loading projects and changing formats still take the ordinary clear path.
    m_tracks.clear();
    Track v1;
    v1.playlist = std::make_unique<edit::Playlist>(*m_profile);
    m_tracks.push_back(std::move(v1));
}

// A snapshot's track numbers are used as they stand to make its tracks. One
// from a damaged or hand-edited project file can name a track no project
// has: it is refused here, before the timeline in place is cleared and
// before a track is made for every number below it.
static bool tracksWithinReason(const QVariantList &clips) {
    for (const QVariant &v : clips) {
        const QVariantMap entry = v.toMap();
        for (const QString &key : {ClipKey::lane, ClipKey::track}) {
            const int track = entry.value(key).toInt();
            if (track < 0 || track > Timeline::kMaxTracks)
                return false;
        }
    }
    return true;
}

bool Timeline::restore(const QVariantList &clips) {
    if(!validRetimeEntries(clips) || !AudioTimeline::uniqueIds(clips) || !tracksWithinReason(clips))return false;
    // Header flags are not part of a snapshot — they survive the rebuild
    // (and are lifted while it runs, so a locked track still rebuilds).
    const std::vector<TrackFlags> flags = trackFlags();
    resetForRestore();
    bool ok = restoreClips(clips);
    applyTrackFlags(flags);
    return ok;
}

bool Timeline::restore(const QVariantList &clips,
                          const std::vector<TrackFlags> &flags) {
    if(!validRetimeEntries(clips) || !AudioTimeline::uniqueIds(clips) || !tracksWithinReason(clips))return false;
    resetForRestore();
    bool ok = restoreClips(clips);
    applyTrackFlags(flags);
    return ok;
}

// One lane entry of a snapshot, placed at `start` and dressed. `clipIndex` is
// the place it takes among its lane's clips. A whole restore places every
// clip in order, so the clip is then also the `last` one connected; one clip
// put back among others is not. `landed` says whether it was placed at all; a
// later step failing still returns false.
bool Timeline::restoreLaneEntry(const ClipEntry &e, int start, int clipIndex, bool last, bool *landed) {
    const int laneNumber = e.lane;
    const QString title = ClipEntry::orDefault(e.title);
    const QString mcam = ClipEntry::orDefault(e.mcam);
    const QString audioMap = ClipEntry::orDefault(e.audioMap);
    const QVariantMap grade = ClipEntry::orDefault(e.grade);
    const QVariantList gradeVersions = ClipEntry::orDefault(e.gradeVersions);
    const QVariantList markers = ClipEntry::orDefault(e.markers);
    const QVariantMap original = ClipEntry::orDefault(e.original);
    const QString category = ClipEntry::orDefault(e.category);
    const QString link = ClipEntry::orDefault(e.link);
    const int frameBlend = ClipEntry::orDefault(e.frameBlend);
    const bool cropped = e.cropL != 0.0 || e.cropR != 0.0 || e.cropT != 0.0 || e.cropB != 0.0;
    const bool coloured = e.colorB != 0.0 || e.colorC != 1.0 || e.colorS != 1.0;
    bool placed = false;
    if (!title.isEmpty()) {
        QVariantMap style;
        style.insert(QStringLiteral("place"), ClipEntry::orInvalid(e.titlePlace));
        style.insert(QStringLiteral("size"), ClipEntry::orInvalid(e.titleSize));
        style.insert(QStringLiteral("font"), ClipEntry::orInvalid(e.titleFont));
        style.insert(QStringLiteral("color"), ClipEntry::orInvalid(e.titleColor));
        style.insert(QStringLiteral("align"), ClipEntry::orInvalid(e.titleAlign));
        style.insert(QStringLiteral("weight"), ClipEntry::orInvalid(e.titleWeight));
        style.insert(QStringLiteral("italic"), ClipEntry::orInvalid(e.titleItalic));
        style.insert(QStringLiteral("bg"), ClipEntry::orInvalid(e.titleBg));
        style.insert(QStringLiteral("outline"), e.titleOutline ? QVariant(*e.titleOutline) : QVariant(true));
        style.insert(QStringLiteral("caption"), ClipEntry::orInvalid(e.caption));
        if (e.titleTracking) style[QStringLiteral("tracking")] = *e.titleTracking;
        if (e.titleLineSpacing) style[QStringLiteral("lineSpacing")] = *e.titleLineSpacing;
        if (e.titlePixelSize) style[QStringLiteral("pixelSize")] = *e.titlePixelSize;
        if (e.titleMargin) style[QStringLiteral("margin")] = *e.titleMargin;
        if (e.titleWidth) style[QStringLiteral("width")] = *e.titleWidth;
        if (e.titleLook)
            for (auto it = e.titleLook->begin(); it != e.titleLook->end(); ++it) style.insert(it.key(), it.value());
        if (e.speaker) style[QStringLiteral("speaker")] = *e.speaker;
        if (e.ownStyle) style[QStringLiteral("ownStyle")] = true;
        // (A title is made by its length, and a transition into it takes
        // its frames from the title's head: the entry counts them apart.)
        placed = connectTitleAt(title, start, e.frames + e.mixIn, style, laneNumber);
    } else {
        placed = connectAt(e.path, start, e.in, e.out, e.audioOnly, laneNumber, e.speed, e.videoOnly, e.retimeMap());
    }
    *landed = placed;
    if (!placed)
        return false;
    // Its place in the flat list of connected clips.
    const auto flat = [&] {
        if (last)
            return connectedCount() - 1;
        int before = 0;
        for (int t = 1; t < laneNumber; ++t)
            before += trackClipCount(t);
        return before + clipIndex;
    };
    // The clip just connected, looked up afresh for each setter.
    const auto onClip = [&](auto &&apply) {
        int t = -1, c = -1;
        if (connectedAddress(flat(), &t, &c)) apply(t, c);
    };
    if (e.fadeIn > 0 || e.fadeOut > 0)
        setFadesOn(laneNumber, clipIndex, e.fadeIn, e.fadeOut);
    if (e.disabled) // V1 re-applies this below; lanes lost it on every rebuild
        setDisabledAt(laneNumber, clipIndex, true);
    if (e.rectX != 0.0 || e.rectY != 0.0 || e.rectW != 1.0 || e.rectH != 1.0 || e.rotation != 0.0
        || e.anchorX != 0.5 || e.anchorY != 0.5)
        onClip([&](int t, int c) { setRectAt(t, c, e.rectX, e.rectY, e.rectW, e.rectH, e.rotation, e.anchorX, e.anchorY); });
    if (cropped)
        setConnectedCrop(flat(), e.cropL, e.cropR, e.cropT, e.cropB, e.cropFeather, e.cropInvert);
    if (frameBlend > 0)
        onClip([&](int t, int c) { placed = setFrameBlendAt(t, c, frameBlend) && placed; });
    if (e.adjust)
        onClip([&](int t, int c) { setAdjustAt(t, c, true); });
    if (e.volume != 0.0)
        setConnectedVolume(flat(), e.volume);
    if (!e.volumeKf.isEmpty())
        setConnectedVolumeKf(flat(), e.volumeKf);
    if (e.opacity < 1.0)
        setConnectedOpacity(flat(), e.opacity);
    if (!e.opacityKf.isEmpty())
        setConnectedOpacityKf(flat(), e.opacityKf);
    if (e.pan != 0.0)
        setConnectedPan(flat(), e.pan);
    if (!e.eq.isEmpty())
        setConnectedEq(flat(), e.eq);
    if (!e.label.isEmpty())
        onClip([&](int t, int c) { setLabelAt(t, c, e.label); });
    if (!category.isEmpty())
        onClip([&](int t, int c) { setCategoryAt(t, c, category); });
    if (!link.isEmpty())
        onClip([&](int t, int c) { setLinkAt(t, c, link); });
    if (!markers.isEmpty())
        onClip([&](int t, int c) { setClipMarkersAt(t, c, markers); });
    if (!original.isEmpty())
        onClip([&](int t, int c) { setOriginalAt(t, c, original); });
    onClip([&](int t, int c) { applyLooks(t, c, e); });
    if (!mcam.isEmpty())
        onClip([&](int t, int c) { setMulticamAt(t, c, mcam, ClipEntry::orDefault(e.mcamAngle)); });
    if (e.flipH || e.flipV || e.freeze)
        onClip([&](int t, int c) {
            setFlipAt(t, c, e.flipH, e.flipV);
            if (e.freeze)
                setFreezeAt(t, c, *e.freeze, true);
        });
    if (!audioMap.isEmpty() || e.normalize)
        onClip([&](int t, int c) {
            setAudioMapAt(t, c, audioMap);
            setNormalizeAt(t, c, e.normalize);
        });
    // Colour correction (the lane site missed this until day 17:
    // a lane clip's grade vanished on every snapshot rebuild).
    if (coloured)
        onClip([&](int t, int c) { setColorAt(t, c, e.colorB, e.colorC, e.colorS); });
    if (!grade.isEmpty() || !gradeVersions.isEmpty())
        onClip([&](int t, int c) {
            setGradeAt(t, c, grade);
            if (!gradeVersions.isEmpty())
                setGradeVersionsAt(t, c, gradeVersions, ClipEntry::orDefault(e.gradeVersion));
        });
    onClip([&](int t, int c) { applyEditExtras(t, c, e); });
    if (e.bypass) // last: the setters above create the filters
        onClip([&](int t, int c) {
            for (const QString &key : *e.bypass)
                setBypassAt(t, c, key, true);
        });
    return placed;
}

// What a main-track entry of a snapshot carries, put onto the clip at `index`
// once it is in place: a whole restore dresses the clip it has just appended,
// and one clip put back among others is dressed where it stands.
bool Timeline::dressPrimaryEntry(const ClipEntry &e, int index) {
    const QString mcam = ClipEntry::orDefault(e.mcam);
    const QString audioMap = ClipEntry::orDefault(e.audioMap);
    const QVariantMap grade = ClipEntry::orDefault(e.grade);
    const QVariantList gradeVersions = ClipEntry::orDefault(e.gradeVersions);
    const QVariantList markers = ClipEntry::orDefault(e.markers);
    const QVariantMap original = ClipEntry::orDefault(e.original);
    const QString category = ClipEntry::orDefault(e.category);
    const QString link = ClipEntry::orDefault(e.link);
    const int frameBlend = ClipEntry::orDefault(e.frameBlend);
    const bool cropped = e.cropL != 0.0 || e.cropR != 0.0 || e.cropT != 0.0 || e.cropB != 0.0;
    const bool coloured = e.colorB != 0.0 || e.colorC != 1.0 || e.colorS != 1.0;
    bool ok = true;
    if (e.fadeIn > 0 || e.fadeOut > 0)
        ok = setFades(index, e.fadeIn, e.fadeOut) && ok;
    if (e.volume != 0.0)
        ok = setClipVolume(index, e.volume) && ok;
    if (!e.volumeKf.isEmpty())
        ok = setClipVolumeKf(index, e.volumeKf) && ok;
    if (e.pan != 0.0)
        ok = setClipPan(index, e.pan) && ok;
    if (!e.eq.isEmpty())
        ok = setClipEq(index, e.eq) && ok;
    if (e.disabled)
        ok = setClipDisabled(index, true) && ok;
    if (!e.label.isEmpty())
        ok = setLabelAt(0, index, e.label) && ok;
    if (!category.isEmpty())
        ok = setCategoryAt(0, index, category) && ok;
    if (!link.isEmpty())
        ok = setLinkAt(0, index, link) && ok;
    if (!markers.isEmpty())
        ok = setClipMarkersAt(0, index, markers) && ok;
    if (!original.isEmpty())
        ok = setOriginalAt(0, index, original) && ok;
    applyLooks(0, index, e);
    if (!mcam.isEmpty())
        ok = setMulticamAt(0, index, mcam, ClipEntry::orDefault(e.mcamAngle)) && ok;
    if (e.flipH || e.flipV)
        ok = setFlipAt(0, index, e.flipH, e.flipV) && ok;
    if (e.freeze)
        ok = setFreezeAt(0, index, *e.freeze, true) && ok;
    if (!audioMap.isEmpty())
        ok = setAudioMapAt(0, index, audioMap) && ok;
    if (e.normalize)
        ok = setNormalizeAt(0, index, true) && ok;
    if (coloured)
        ok = setClipColor(index, e.colorB, e.colorC, e.colorS) && ok;
    if (!grade.isEmpty() || !gradeVersions.isEmpty()) {
        ok = setGradeAt(0, index, grade) && ok;
        if (!gradeVersions.isEmpty())
            setGradeVersionsAt(0, index, gradeVersions, ClipEntry::orDefault(e.gradeVersion));
    }
    if (cropped)
        ok = setClipCrop(index, e.cropL, e.cropR, e.cropT, e.cropB, e.cropFeather, e.cropInvert) && ok;
    if (frameBlend > 0)
        ok = setFrameBlendAt(0, index, frameBlend) && ok;
    if (e.adjust)
        ok = setAdjustAt(0, index, true) && ok;
    applyEditExtras(0, index, e);
    for (const QString &key : ClipEntry::orDefault(e.bypass))
        setBypassAt(0, index, key, true); // last: after every filter exists
    return ok;
}

bool Timeline::restoreClips(const QVariantList &clips) {
    QScopedValueRollback<bool> restoring(m_restoring, true);
    QScopedValueRollback<QVariantMap> sourceScope(m_mediaSources);
    QScopedValueRollback<QHash<QString, int>> lengthScope(m_offlineLengths);
    // Read once, typed; every pass below works from these.
    const QList<ClipEntry> entries = ClipEntry::readList(clips);
    QMap<int,QVariantList> preciseTracks;
    for (const ClipEntry &e : entries) if (e.audioTiming) preciseTracks[e.lane] = {};
    for (const ClipEntry &e : entries) {
        if (preciseTracks.contains(e.lane)) {
            if (!e.audioTiming || e.lane < 1) return false;
            preciseTracks[e.lane].append(e.raw);
        }
    }
    for (const ClipEntry &e : entries) {
        const int sourceLength = e.retime ? qCeil(e.retimeMap().value(QStringLiteral("sourceLength")).toDouble())
            : qRound(e.length * qAbs(e.speed));
        m_offlineLengths.insert(e.path, qMax(m_offlineLengths.value(e.path), sourceLength));
    }
    // Scratch timelines (export) recover input transforms from the original
    // entries; live timelines use their current source preferences.
    const auto inside = [](const ClipEntry &compound) {
        return ClipEntry::readList(QJsonDocument::fromJson(ClipEntry::orDefault(compound.compoundJson).toUtf8()).toVariant().toList());
    };
    std::function<void(const QList<ClipEntry> &)> remember = [&](const QList<ClipEntry> &list) {
        for (const ClipEntry &e : list) {
            if (!m_mediaSources.contains(e.path) && e.sourceMeta)
                m_mediaSources.insert(e.path, *e.sourceMeta);
            if (e.compoundJson) remember(inside(e));
        }
    };
    remember(entries);
    bool ok = true;
    // Re-lock the profile from the first REAL media entry, not blindly from
    // clip 0 — a gap ("color:black") carries no media metadata, and letting
    // it win the lock would silently re-profile the whole timeline to the
    // defaults (wrong fps/resolution on every later clip and export).
    for (const ClipEntry &e : entries) {
        if (e.lane >= 1)
            continue;
        QString path = e.path;
        // A compound carries no media path of its own — lock from the
        // first real clip INSIDE it instead.
        if (!ClipEntry::orDefault(e.compoundJson).isEmpty()) {
            path.clear();
            const QList<ClipEntry> subs = inside(e);
            for (const ClipEntry &sub : subs) {
                if (sub.gap || sub.path.startsWith(QStringLiteral("color:")))
                    continue;
                path = sub.path;
                break;
            }
            if (path.isEmpty())
                continue;
        } else if (e.gap || path.startsWith(QStringLiteral("color:"))) {
            continue;
        }
        if (!primary() && !lockProfileTo(path))
            ok = false; // the append below will fail and report it too
        break;
    }
    // Attributes must never land on the WRONG clip: when an append fails
    // (missing media), everything that would have ridden that clip is
    // skipped, and the dissolve pass below skips seams touching it.
    QList<bool> appended;
    // Lane dissolves: a playlist mix shrinks that lane by its overlap
    // downstream of the PAIR, so every lane clip two or more entries past
    // a dissolved clip is placed `overlap` later than its snapshot start
    // and slides back into place when the mix pass below re-applies it.
    struct LaneMix { int lane, clipIndex, overlap; QString style; QString align; int extA, extB; };
    QList<LaneMix> laneMixes;
    QHash<int, int> lanePlaced;       // lane → clips landed so far
    QHash<int, int> laneShift;        // lane → overlaps of pairs fully behind
    QHash<int, int> lanePrevMixOut;   // lane → previous entry's mixOut
    // A leading gap on V1 (a lifted first clip, a nudged-right first clip)
    // can arrive before the playlist exists — the profile lock above skips
    // synthetic clips — so it waits here and is inserted in front of the
    // first clip that lands.
    int leadingGap = 0;
    // Storyline entries come first in a snapshot, so the lane a connected
    // clip needs to connect to is always rebuilt before it.
    for (const ClipEntry &e : entries) {
        const int laneNumber = e.lane;
        if (preciseTracks.contains(laneNumber)) continue;
        QScopedValueRollback<int> sampling(m_restoreSampling, ClipEntry::orDefault(e.frameBlend));
        std::optional<double> origin;if(!SourceTiming::readOrigin(e.raw,origin))return false;
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
        if (laneNumber >= 1) {
            // shift(X) = Σ mixOut of entries k <= X-2 on this lane.
            const int shift = laneShift.value(laneNumber);
            laneShift[laneNumber] = shift + lanePrevMixOut.value(laneNumber);
            lanePrevMixOut[laneNumber] = e.mixOut;
            const int clipIndex = lanePlaced.value(laneNumber);
            bool landed = false;
            const bool placed = restoreLaneEntry(e, e.start + shift, clipIndex, true, &landed);
            if (landed) {
                lanePlaced[laneNumber] = clipIndex + 1;
                if (e.mixOut > 0)
                    laneMixes.append({laneNumber, clipIndex, e.mixOut, e.mixStyle, e.mixAlign,
                                      ClipEntry::orDefault(e.mixExtA), ClipEntry::orDefault(e.mixExtB)});
            }
            ok = placed && ok;
        } else {
            // Empty space first — from gapBefore, and from legacy gap CLIPS
            // (pre-A3 projects/snapshots), which come back as plain blanks.
            if (e.gapBefore > 0) {
                if (!primary() || primary()->count() == 0)
                    leadingGap += e.gapBefore;
                else
                    primary()->blank(e.gapBefore - 1);
            }
            if (e.gap) {
                if (e.frames > 0) {
                    if (!primary() || primary()->count() == 0)
                        leadingGap += e.frames;
                    else
                        primary()->blank(e.frames - 1);
                }
                appended.append(false); // no clip landed — nothing decorates it
                continue;
            }
            const QString compound = ClipEntry::orDefault(e.compoundJson);
            const bool added = !compound.isEmpty()
                ? appendCompound(compound, e.in, e.out)
                : append(e.path, e.in, e.out, e.speed, e.gap, e.videoOnly, e.retimeMap());
            appended.append(added);
            ok = added && ok;
            if (!added)
                continue; // never decorate whatever clip happens to be last
            if (leadingGap > 0 && primary() && primary()->count() == 1) {
                primary()->insert_blank(0, leadingGap - 1);
                leadingGap = 0;
            }
            // The clip just appended, by CLIP index: clipCount() counts V1's
            // blanks (a ripple gap) but clip indices skip them, so "count − 1"
            // aimed one clip too far past any gap — every attribute after the
            // gap missed and the project refused to open.
            const int last = lastClipIndex();
            if (last < 0) {
                ok = false;
                continue;
            }
            ok = dressPrimaryEntry(e, last) && ok;
        }
    }
    // Second pass: dissolves. mixAt takes clip indices (mix entries are
    // hidden from clip indexing), so earlier mixes never shift later ones.
    // `clip` counts ACTUAL playlist clips, so a failed append shifts the
    // targets correctly — and a seam whose either side is missing is
    // skipped rather than dissolved into the wrong pair.
    int clip = 0;
    int snapshotIndex = 0;
    for (const ClipEntry &e : entries) {
        if (e.lane >= 1)
            continue;
        const int i = snapshotIndex++;
        if (i >= appended.size() || !appended.at(i))
            continue; // this clip never landed — no index, no mix
        const bool nextLanded = i + 1 < appended.size() && appended.at(i + 1);
        if (e.mixOut > 0 && nextLanded)
            ok = mixAt(clip, e.mixOut, false, // lane starts already final
                       e.mixStyle, e.mixAlign, ClipEntry::orDefault(e.mixExtA), ClipEntry::orDefault(e.mixExtB))
                 && ok;
        ++clip;
    }
    // Lane dissolves, in order; a pair whose next clip never landed is
    // skipped rather than mixed into the wrong neighbour.
    for (const LaneMix &m : laneMixes)
        if (m.clipIndex + 1 < trackClipCount(m.lane))
            ok = mixOn(m.lane, m.clipIndex, m.overlap, false, m.style, m.align, m.extA, m.extB) && ok;
    for (auto it=preciseTracks.cbegin();it!=preciseTracks.cend();++it)
        ok=restoreAudioTrack(it.key(),it.value()) && ok;
    tidyPrimary();
    return ok;
}

// V1 housekeeping after any edit that can leave blanks touching: merge
// neighbouring blanks and drop trailing ones (the timeline ends at its
// last clip — a gap can't be the last thing on it).
void Timeline::tidyPrimary() {
    if (!primary())
        return;
    primary()->consolidate_blanks(0);
    while (primary()->count() > 0 && primary()->is_blank(primary()->count() - 1))
        primary()->remove(primary()->count() - 1);
}

// Snapshot-rebuild with a safety net: when the rebuild fails part-way
// (typically missing media), put the previous arrangement back instead of
// leaving a wiped or truncated engine behind a UI that still shows the old
// edit — the state a later Save would happily serialize.
bool Timeline::rebuild(const QVariantList &wanted,
                          const QVariantList &fallback) {
    // Most edits change a clip or two and ask for the timeline that results.
    // Where only those clips need to be put back to have it, only they are:
    // an edit then costs the same on a long timeline as on a short one.
    // OMAEDIT_NARROW_CONFORM=0 always rebuilds the whole timeline; =verify
    // does both and stops if the clip lists differ.
    const QByteArray mode = qgetenv("OMAEDIT_NARROW_CONFORM");
    const bool narrow = mode != "0" && rebuildNarrowly(wanted);
    noteSettle(narrow);
    if (narrow && mode != "verify")
        return true;
    const QVariantList narrowed = narrow ? entries() : QVariantList();
    const bool ok = rebuild(wanted, fallback, trackFlags());
    if (narrow && ok && entries() != narrowed)
        qFatal("the narrow rebuild differs from the whole one");
    return ok;
}

bool Timeline::rebuild(const QVariantList &wanted,
                          const QVariantList &fallback,
                          const std::vector<TrackFlags> &flags) {
    // A same-format edit keeps the timeline's profile (see resetForRestore).
    RebuildMedia media;
    QScopedValueRollback<RebuildMedia *> keep(m_rebuildMedia,
        m_profile && m_rebuildProfileValid && !wanted.isEmpty() ? &media : m_rebuildMedia);
    if (restore(wanted, flags))
        return true;
    restore(fallback);
    return false;
}

QVariantMap Timeline::snapshotDocument(const QStringList &library) const {
    const QVariantMap profile{{"width", m_profile ? m_profile->width() : 720}, {"height", m_profile ? m_profile->height() : 576},
        {"fpsNum", m_profile ? m_profile->frame_rate_num() : 25}, {"fpsDen", m_profile ? m_profile->frame_rate_den() : 1},
        {"sarNum", m_profile ? m_profile->sample_aspect_num() : 1}, {"sarDen", m_profile ? m_profile->sample_aspect_den() : 1},
        {"progressive",m_profile?bool(m_profile->progressive()):true},
        {"colorspace", m_profile ? m_profile->colorspace() : 709}};
    auto document=ProjectSafety::freezeNested({{"version", 1}, {"library", library}, {"project", m_project}, {"profile", profile},
        {"tracks", trackFlagsVariant()}, {"master", m_master}, {"adr", m_adrCues}, {"entries", entries()}, {"mediaSources", m_mediaSources}});
    if (!m_captionSettings.isEmpty()) document.insert(QStringLiteral("captions"), m_captionSettings);
    // An inactive sequence can require the newer reader even when the active
    // graph is entirely frame-based. Older builds must refuse, not round it.
    if(AudioTimeline::containsPreciseCuts(document) || AudioTimeline::containsPreciseCuts(ProjectSafety::libraryRecords(library)))document["version"]=2;
    return document;
}

bool Timeline::saveDocument(const QString &path, const QStringList &library) {
    // The project is its snapshot: the timeline, its tracks, master, library
    // and media settings as the editor holds them.
    return ProjectSafety::writeDocument(path, snapshotDocument(library));
}

bool Timeline::loadDocument(const QString &path, QStringList *library) {
    // Only the editor's own document (ProjectSafety::readDocument): files
    // from other programs are not read.
    QVariantMap document = ProjectSafety::readDocument(path);
    if (document.isEmpty()) return false;
    QString resolveError;
    document = ProjectSafety::resolveDocument(document, QFileInfo(path).absolutePath(), &resolveError);
    if (document.isEmpty()) return false;
    const QVariantList before = entries(); const auto beforeFlags = trackFlags();
    const QVariantMap beforeProject = m_project, beforeSources = m_mediaSources, beforeProfile = m_fallbackProfile, beforeMaster = m_master;
    const QVariantList beforeAdr = m_adrCues;
    const QVariantMap beforeCaptions = m_captionSettings;
    setProjectSettings(document.value(QStringLiteral("project")).toMap(), false);
    m_mediaSources = document.value(QStringLiteral("mediaSources")).toMap();
    m_fallbackProfile = document.value(QStringLiteral("profile")).toMap();
    m_master = document.value(QStringLiteral("master")).toMap(); m_adrCues = document.value(QStringLiteral("adr")).toList();
    m_captionSettings = document.value(QStringLiteral("captions")).toMap();
    if (!restore(document.value(QStringLiteral("entries")).toList(), trackFlagsFromVariant(document.value(QStringLiteral("tracks")).toList()))) {
        m_project = beforeProject; m_mediaSources = beforeSources; m_fallbackProfile = beforeProfile; m_master = beforeMaster; m_adrCues = beforeAdr;
        m_captionSettings = beforeCaptions;
        restore(before, beforeFlags); return false;
    }
    if (library) *library = document.value(QStringLiteral("library")).toStringList();
    return true;
}

void Timeline::clear() {
    m_tracks.clear();
    m_profile.reset();
    m_rebuildProfileValid = false;
}

int Timeline::clipCount() const {
    // Real clips only — a dissolve's mix entry is an implementation detail.
    if (!primary())
        return 0;
    int clips = 0;
    for (int i = 0; i < primary()->count(); ++i)
        if (!primary()->is_mix(i))
            ++clips;
    return clips;
}

int Timeline::entryIndexFor(int clipIndex) const {
    return entryIndexOn(0, clipIndex);
}

int Timeline::entryIndexOn(int track, int clipIndex) const {
    if (track < 0 || track >= trackCount() || clipIndex < 0
        || m_tracks[size_t(track)].locked || !m_tracks[size_t(track)].audioEntries.isEmpty())
        return -1;
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    int clip = 0;
    for (int i = 0; i < pl->count(); ++i) {
        if (pl->is_mix(i) || pl->is_blank(i))
            continue;
        if (clip == clipIndex)
            return i;
        ++clip;
    }
    return -1;
}

int Timeline::lastClipIndex() const {
    // The last REAL clip on V1 by clip index (blanks and mixes skipped),
    // -1 when V1 is empty or ends in a blank.
    if (!primary() || primary()->count() == 0)
        return -1;
    const int entry = primary()->count() - 1;
    if (primary()->is_blank(entry) || primary()->is_mix(entry))
        return -1;
    return clipIndexFromEntry(0, entry);
}

int Timeline::clipIndexFromEntry(int track, int entry) const {
    if (track < 0 || track >= trackCount())
        return -1;
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    int clip = 0;
    for (int i = 0; i < pl->count() && i < entry; ++i)
        if (!pl->is_mix(i) && !pl->is_blank(i))
            ++clip;
    return clip;
}

std::unique_ptr<edit::ClipInfo> Timeline::clipInfoAt(int track,
                                                       int index) const {
    const int entry = entryIndexOn(track, index);
    if (entry < 0)
        return nullptr;
    std::unique_ptr<edit::ClipInfo> info(
        m_tracks[size_t(track)].playlist->clip_info(entry));
    if (!info || !info->cut)
        return nullptr;
    return info;
}

bool Timeline::connectedAddress(int connectedIndex, int *track,
                                   int *index) const {
    int offset=0;
    for (int t=1;t<trackCount();++t) {
        const int count=trackClipCount(t);
        if (connectedIndex>=offset && connectedIndex<offset+count && !m_tracks[size_t(t)].audioEntries.isEmpty()) {
            *track=t; *index=connectedIndex-offset; return true;
        }
        offset+=count;
    }
    int laneIdx = -1, entry = -1;
    if (!connectedLocate(connectedIndex, &laneIdx, &entry))
        return false;
    *track = laneIdx + 1;
    *index = clipIndexFromEntry(laneIdx + 1, entry);
    return true;
}

int Timeline::frameCount() const {
    // A connected clip can extend the timeline past the storyline's end.
    int frames = primaryFrameCount();
    for (int L = 0; L < laneTotal(); ++L)
        frames = qMax(frames, laneAt(L)->get_playtime());
    return frames;
}

double Timeline::fps() const {
    if (m_profile)
        return m_profile->fps();
    // An empty timeline with custom settings still knows its rate.
    if (m_project.value(QStringLiteral("custom")).toBool()) {
        const int num = m_project.value(QStringLiteral("fpsNum")).toInt();
        const int den = m_project.value(QStringLiteral("fpsDen")).toInt();
        if (num > 0 && den > 0)
            return double(num) / den;
    }
    return 0.0;
}

int Timeline::width() const {
    if (m_profile)
        return m_profile->width();
    return m_project.value(QStringLiteral("custom")).toBool()
               ? m_project.value(QStringLiteral("width")).toInt() : 0;
}

int Timeline::height() const {
    if (m_profile)
        return m_profile->height();
    return m_project.value(QStringLiteral("custom")).toBool()
               ? m_project.value(QStringLiteral("height")).toInt() : 0;
}

// ── Project settings ─────────────────────────────────────────────────────
QVariantMap Timeline::defaultProjectSettings() {
    return QVariantMap{{QStringLiteral("title"), QString()},
                       {QStringLiteral("custom"), false},
                       // A matched source can have non-square pixels or interlaced frames.
                       {QStringLiteral("matchedProfile"), QVariantMap()},
                       {QStringLiteral("width"), 1920},
                       {QStringLiteral("height"), 1080},
                       {QStringLiteral("fpsNum"), 25},
                       {QStringLiteral("fpsDen"), 1},
                       {QStringLiteral("colorspace"), 709},
                       {QStringLiteral("sampleRate"), 48000},
                       {QStringLiteral("channels"), 2},
                       {QStringLiteral("startFrame"), 0},
                       // Whole frames a second startFrame was counted at; 0 is the timeline's own rate.
                       {QStringLiteral("startRate"), 0},
                       // How clips of another size or shape take the frame.
                       {QStringLiteral("scaling"), QStringLiteral("fit")},
                       // Per-project defaults: where Create Proxies goes and
                       // what Import Media proposes for input colour handling.
                       {QStringLiteral("proxyFolder"), QString()},
                       {QStringLiteral("importSpace"), 0},
                       {QStringLiteral("importLut"), QString()}};
}

// The profile a clip dictated, bent to the project: size and rate only when
// the format is custom, the colour space always. Square pixels — a custom
// frame is a delivery frame. A matched first clip retains its pixel aspect
// and scan mode so switching timelines cannot stretch or reinterpret it.
void Timeline::applyProjectProfile(edit::Profile &profile) const {
    const int cs = m_project.value(QStringLiteral("colorspace")).toInt();
    if (cs == 601 || cs == 709 || cs == 2020)
        profile.set_colorspace(cs);
    if (!m_project.value(QStringLiteral("custom")).toBool())
        return;
    const int w = m_project.value(QStringLiteral("width")).toInt();
    const int h = m_project.value(QStringLiteral("height")).toInt();
    if (w >= 16 && h >= 16 && w <= 8192 && h <= 8192) {
        profile.set_width(w);
        profile.set_height(h);
        const auto matched = m_project.value("matchedProfile").toMap();
        const int sarNum = qBound(1, matched.value("sarNum", 1).toInt(), 10000);
        const int sarDen = qBound(1, matched.value("sarDen", 1).toInt(), 10000);
        profile.set_sample_aspect(sarNum, sarDen);
        profile.set_display_aspect(w * sarNum, h * sarDen);
    }
    const int num = m_project.value(QStringLiteral("fpsNum")).toInt();
    const int den = m_project.value(QStringLiteral("fpsDen")).toInt();
    if (num > 0 && den > 0)
        profile.set_frame_rate(num, den);
    profile.set_progressive(m_project.value("matchedProfile").toMap().value("progressive", true).toBool() ? 1 : 0);
    profile.set_explicit(1); // a consumer must not re-profile from the producer
}

bool Timeline::setProjectSettings(const QVariantMap &settings, bool ownClock) {
    QVariantMap next = defaultProjectSettings();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it)
        if (next.contains(it.key()))
            next.insert(it.key(), it.value());
    next.insert(QStringLiteral("title"), next.value(QStringLiteral("title")).toString().trimmed().left(120));
    // Only sane values survive: an even frame in range, a real rate.
    const int w = next.value(QStringLiteral("width")).toInt();
    const int h = next.value(QStringLiteral("height")).toInt();
    if (w < 16 || h < 16 || w > 8192 || h > 8192 || (w % 2) || (h % 2)) {
        next.insert(QStringLiteral("width"), m_project.value(QStringLiteral("width")));
        next.insert(QStringLiteral("height"), m_project.value(QStringLiteral("height")));
    }
    if (next.value(QStringLiteral("fpsNum")).toInt() <= 0 || next.value(QStringLiteral("fpsDen")).toInt() <= 0) {
        next.insert(QStringLiteral("fpsNum"), m_project.value(QStringLiteral("fpsNum")));
        next.insert(QStringLiteral("fpsDen"), m_project.value(QStringLiteral("fpsDen")));
    }
    const int cs = next.value(QStringLiteral("colorspace")).toInt();
    if (cs != 601 && cs != 709 && cs != 2020)
        next.insert(QStringLiteral("colorspace"), 709);
    next.insert(QStringLiteral("startFrame"), qMax(0, next.value(QStringLiteral("startFrame")).toInt()));
    next.insert(QStringLiteral("startRate"), next.value(QStringLiteral("startFrame")).toInt() > 0
                    ? qBound(0, next.value(QStringLiteral("startRate")).toInt(), 1000) : 0);
    {
        // Timecode has no twenty-fourth hour: a start read from a damaged or
        // hand-made file at a day or more starts again after a day, as its
        // readout does, and leaves room to count frames on from it. The day
        // is at the rate the start was counted at, else at the settings' own,
        // else at the highest rate a start is counted at.
        const int counted = next.value(QStringLiteral("startRate")).toInt();
        const int num = next.value(QStringLiteral("fpsNum")).toInt(), den = next.value(QStringLiteral("fpsDen")).toInt();
        const int own = next.value(QStringLiteral("custom")).toBool() && num > 0 && den > 0 ? qRound(double(num) / den) : 0;
        const qint64 day = 86400LL * (counted > 0 ? counted : own > 0 ? own : 1000);
        next.insert(QStringLiteral("startFrame"), int(next.value(QStringLiteral("startFrame")).toInt() % day));
    }
    if (!render::isScalingKey(next.value(QStringLiteral("scaling")).toString()))
        next.insert(QStringLiteral("scaling"), QStringLiteral("fit"));
    next.insert(QStringLiteral("proxyFolder"), next.value(QStringLiteral("proxyFolder")).toString().trimmed());
    next.insert(QStringLiteral("importLut"), next.value(QStringLiteral("importLut")).toString().trimmed());
    const int importSpace = next.value(QStringLiteral("importSpace")).toInt();
    if (importSpace != 601 && importSpace != 709 && importSpace != 2020)
        next.insert(QStringLiteral("importSpace"), 0);
    const auto matched = next.value("matchedProfile").toMap();
    if (!next.value("custom").toBool() || matched.value("width") != next.value("width") || matched.value("height") != next.value("height"))
        next["matchedProfile"] = QVariantMap(); // a newly chosen custom size uses square pixels
    auto shape = [](const QVariantMap &m) {
        return QVariantList{m.value(QStringLiteral("matchedProfile")), m.value(QStringLiteral("custom")), m.value(QStringLiteral("width")),
                            m.value(QStringLiteral("height")), m.value(QStringLiteral("fpsNum")),
                            m.value(QStringLiteral("fpsDen")), m.value(QStringLiteral("colorspace"))};
    };
    const bool shapeChanged = shape(next) != shape(m_project);
    // A timeline switch can change settings before restoreSnapshot() rebuilds.
    // Those producers still reference the old profile and must be reopened.
    if (shapeChanged) m_rebuildProfileValid = false;
    m_project = next;
    // The start is counted at the rate these settings run at. A custom rate is
    // their own (every profile lock gives it). Otherwise it is the live clock,
    // but only while that clock is this timeline's: counted at the clock of a
    // timeline being replaced, a frame number that clock has no room for was
    // lost for good (10:00:00:30 at 50 came back as :24 over a 25 timeline).
    // Such a start waits as it was saved until its own clock is locked.
    const int num = next.value(QStringLiteral("fpsNum")).toInt(), den = next.value(QStringLiteral("fpsDen")).toInt();
    if (next.value(QStringLiteral("custom")).toBool() && num > 0 && den > 0)
        settleStartFrame(qRound(double(num) / den));
    else if (ownClock)
        settleStartFrame();
    return shapeChanged;
}

int Timeline::startFrameAtRate(int frame, int fromRate, int toRate) {
    if (frame <= 0 || fromRate <= 0 || toRate <= 0 || fromRate == toRate)
        return frame;
    // The frame number stays as typed; one the new rate has no room for
    // becomes the second's last frame.
    const qint64 counted = qint64(frame / fromRate) * toRate + qMin(frame % fromRate, toRate - 1);
    return int(qMin<qint64>(counted, std::numeric_limits<int>::max()));
}

void Timeline::settleStartFrame(int rate) {
    if (rate <= 0)
        rate = qRound(fps());
    const int start = m_project.value(QStringLiteral("startFrame")).toInt();
    const int counted = m_project.value(QStringLiteral("startRate")).toInt();
    // No rate yet (an empty timeline that matches its first clip): the start
    // waits as it was typed. A start with no rate written is at the
    // timeline's own, whatever that is, and is left exactly as it is.
    if (rate <= 0 || start <= 0 || counted <= 0 || counted == rate)
        return;
    m_project.insert(QStringLiteral("startFrame"), startFrameAtRate(start, counted, rate));
    m_project.insert(QStringLiteral("startRate"), rate);
}

namespace { std::atomic<quint64> soundEntryWalkCount{0}; }
// The entries as the mixer reads them: each source at the path it plays
// from (a proxy, or a nest's playback copy, as the source preferences choose).
QVariantList Timeline::soundEntries() const {
    // The mixer is handed this list for every frame it plays and compares it
    // with the one before: while nothing has changed it must be the same list
    // (shared, so the comparison is one look), not an equal copy.
    QVariantList out = entries();
    // Asked for every frame: while the clip list is the very list it was made
    // from (shared, so one look) and the source preferences are the ones it
    // was made with, the answer is the one before. Walking every clip, with a
    // look at the disk for each proxy, and then comparing two long lists, was
    // work per frame that grew with the timeline. A proxy that comes or goes
    // on disk is noticed within a second. (OMAEDIT_VERIFY_CLIP_LIST walks
    // every time, as before.)
    static const bool verify = qEnvironmentVariableIsSet("OMAEDIT_VERIFY_CLIP_LIST");
    const SoundEntriesFrom &from = m_soundEntriesFrom;
    if (!verify && from.checked.isValid() && from.checked.elapsed() < 1000 && from.clips.constData() == out.constData()
        && from.clips.size() == out.size() && from.sources.isSharedWith(m_mediaSources) && from.mode == m_proxyMode)
        return m_soundEntries;
    // (The kept `clips` holds the list's storage, so no other list can be at its address.)
    m_soundEntriesFrom = {out, m_mediaSources, m_proxyMode, {}};
    m_soundEntriesFrom.checked.start();
    ++soundEntryWalkCount;
    for (qsizetype i = 0; i < out.size(); ++i) {
        const QVariantMap e = std::as_const(out)[i].toMap();
        const QString path = e.value(ClipKey::path).toString();
        if (path.isEmpty() || (path.contains(QLatin1Char(':')) && !QFileInfo(path).isAbsolute())) continue;  // generators
        const QString played = playbackPath(path);
        if (played != path) { QVariantMap changed = e; changed[ClipKey::path] = played; out[i] = changed; }
    }
    if (out == m_soundEntries) return m_soundEntries;
    m_soundEntries = out;
    return out;
}
quint64 Timeline::soundEntryWalks() { return soundEntryWalkCount.load(); }

QString Timeline::playbackPath(const QString &source) const {
    // A nested sequence needs a playback XML whose resources follow the
    // current source preferences. Its original cache remains exportable.
    if (ProjectFile::isProject(source) && m_proxyMode != QStringLiteral("original") && QFileInfo::exists(source)) {
        static thread_local QSet<QString> preparing;
        bool anyProxy = false;
        for (auto it = m_mediaSources.constBegin(); it != m_mediaSources.constEnd(); ++it) {
            const QVariantMap settings = it.value().toMap();
            const QString proxy = settings.value(QStringLiteral("proxyPath")).toString();
            if (!proxy.isEmpty() && QFileInfo::exists(proxy)
                && (m_proxyMode == QStringLiteral("proxy") || settings.value(QStringLiteral("preferProxy"), true).toBool() || !QFileInfo::exists(it.key()))) anyProxy = true;
        }
        if (anyProxy && !preparing.contains(source) && preparing.size() < 16) {
            const QByteArray identity = source.toUtf8() + QByteArray::number(QFileInfo(source).lastModified().toMSecsSinceEpoch())
                + m_proxyMode.toUtf8() + QJsonDocument::fromVariant(m_mediaSources).toJson(QJsonDocument::Compact);
            const QDir directory(StoragePreferences::cacheRoot() + QStringLiteral("/omaedit-nest-playback"));
            QDir().mkpath(directory.absolutePath());
            const QString cached = directory.filePath(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()) + ProjectFile::extension());
            if (QFileInfo::exists(cached)) return cached;
            preparing.insert(source);
            Timeline nested;
            nested.setAllowOffline(true);
            QStringList ignored;
            bool ok = nested.loadDocument(source, &ignored);
            if (ok) {
                nested.setMediaSources(m_mediaSources, m_proxyMode);
                ok = nested.restore(nested.entries());
            }
            if (ok) {
                QTemporaryFile stage(directory.filePath(QStringLiteral(".nest-XXXXXX") + ProjectFile::extension()));
                if (stage.open()) { stage.close(); ok = nested.saveDocument(stage.fileName(), {});
                    if (ok && QFile::rename(stage.fileName(), cached)) stage.setAutoRemove(false); }
            }
            preparing.remove(source);
            if (ok && QFileInfo::exists(cached)) return cached;
        }
    }
    const QVariantMap settings = m_mediaSources.value(source).toMap();
    const QString proxy = settings.value(QStringLiteral("proxyPath")).toString();
    const bool wanted = m_proxyMode == QStringLiteral("proxy") || (m_proxyMode == QStringLiteral("preferred")
        && (settings.value(QStringLiteral("preferProxy"), true).toBool() || !QFileInfo::exists(source)));
    return wanted && !proxy.isEmpty() && QFileInfo::exists(proxy) ? proxy : source;
}
double Timeline::interpretationFactor(const QVariantMap &settings) {
    const double forced = settings.value(QStringLiteral("forceFps")).toDouble();
    const double source = settings.value(QStringLiteral("sourceFps"), settings.value(QStringLiteral("fps"))).toDouble();
    if (forced <= 0.0 || source <= 0.0 || qFuzzyCompare(forced, source))
        return 1.0;
    return forced / source;
}

void Timeline::applyInputTransform(edit::Profile &profile, edit::Producer &producer, const QVariantMap &settings) {
    for (const auto *key : {"rawExposure", "rawRed", "rawBlue"})
        producer.set(qPrintable(QString("omaedit.%1").arg(key)), settings.value(key, key == QString("rawExposure") ? 0. : 1.).toDouble());
    const int space = settings.value(QStringLiteral("inputSpace")).toInt();
    if (space == 601 || space == 709 || space == 2020) producer.set("force_colorspace", space);
    const QString lut = settings.value(QStringLiteral("inputLut")).toString();
    if (!lut.isEmpty() && QFileInfo::exists(lut)) { // looked up at 16 bits for a deep delivery
        edit::Filter filter(profile, "omaedit.lut3d");
        if (filter.is_valid()) { filter.set("resource", lut.toUtf8().constData()); filter.set("omavid:inputLut", 1); producer.attach(filter); }
    }
}
