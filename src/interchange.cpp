#include "framesampling.h"
#include "interchange.h"
#include "retimemap.h"
#include "sourcetiming.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace Interchange {

// ── time helpers ──────────────────────────────────────────────────────────
QString timecode(qint64 frames, double fps, bool dropFrame) {
    if (fps <= 0) fps = 25.0;
    const int nominal = qCeil(fps - 0.001);
    qint64 f = qMax<qint64>(0, frames);
    if (dropFrame && (nominal == 30 || nominal == 60)) {
        // Drop-frame: skip the first two (or four) frame numbers of every
        // minute that is not a multiple of ten.
        const qint64 drop = nominal / 30 * 2;
        const qint64 perMinute = nominal * 60 - drop, perTen = perMinute * 10 + drop;
        const qint64 tens = f / perTen, rest = f % perTen;
        f += drop * (tens * 9) + (rest >= drop ? drop * ((rest - drop) / perMinute) : 0);
    }
    // Timecode has no twenty-fourth hour: it starts again after a day.
    const qint64 ff = f % nominal, s = f / nominal;
    return QStringLiteral("%1:%2:%3%4%5").arg((s / 3600) % 24, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0')).arg(dropFrame ? QChar(';') : QChar(':')).arg(ff, 2, 10, QChar('0'));
}
QString durationText(qint64 frames, double fps) {
    if (fps <= 0) fps = 25.0;
    const qint64 hundredths = qRound64(double(qMax<qint64>(0, frames)) / fps * 100.0);
    const qint64 total = hundredths / 100, part = hundredths % 100;
    const QString fraction = QStringLiteral(".%1").arg(part, 2, 10, QChar('0'));
    if (total < 60) return QStringLiteral("%1%2 s").arg(total).arg(fraction);
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3%4").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(fraction)
                 : QStringLiteral("%1:%2%3").arg(m).arg(s, 2, 10, QChar('0')).arg(fraction);
}
qint64 timecodeFrames(const QString &tc, double fps, bool *ok, bool forceDrop) {
    static const QRegularExpression form(QStringLiteral("^(\\d{1,2})[:;.](\\d{1,2})[:;.](\\d{1,2})([:;.,])(\\d{1,3})$"));
    const auto m = form.match(tc.trimmed());
    if (ok) *ok = m.hasMatch();
    if (!m.hasMatch()) return 0;
    if (fps <= 0) fps = 25.0;
    const int nominal = qCeil(fps - 0.001);
    const bool drop = (forceDrop || m.captured(4) == QStringLiteral(";")) && (nominal == 30 || nominal == 60);
    const qint64 h = m.captured(1).toInt(), mi = m.captured(2).toInt(), s = m.captured(3).toInt(), f = m.captured(5).toInt();
    qint64 frames = ((h * 3600 + mi * 60 + s) * nominal) + f;
    if (drop) { const qint64 d = nominal / 30 * 2, minutes = h * 60 + mi; frames -= d * (minutes - minutes / 10); }
    return frames;
}

// ── editor → document ─────────────────────────────────────────────────────
namespace {
QVariantMap libraryEntry(const QVariantList &library, const QString &path) {
    for (const QVariant &v : library) if (v.toMap().value(QStringLiteral("path")).toString() == path) return v.toMap();
    return {};
}
qint64 libraryTimecode(const QVariantMap &entry) {
    if (entry.contains(QStringLiteral("startTimecodeFrames"))) return entry.value(QStringLiteral("startTimecodeFrames")).toLongLong();
    return entry.value(QStringLiteral("timecodeFrames"), -1).toLongLong();
}
double libraryTimecodeSeconds(const QVariantMap &entry, double fallbackRate) {
    const double rate = entry.value(QStringLiteral("fps"), fallbackRate).toDouble();
    if (rate <= 0) return -1.;
    if (!entry.contains(QStringLiteral("startTimecodeFrames"))) {
        // The original clock string retains drop/non-drop frame numbering.
        // Older probes cached rounded seconds * actual fps, which loses that
        // distinction at fractional rates. Explicit user overrides take priority.
        bool ok = false;
        const qint64 clock = timecodeFrames(entry.value(QStringLiteral("timecode")).toString(), rate, &ok);
        if (ok) return double(clock) / rate;
    }
    const qint64 clock = libraryTimecode(entry);
    return clock >= 0 ? double(clock) / rate : -1.;
}
QList<Marker> clipMarkers(const QVariantMap &entry) {
    QList<Marker> out;
    for (const QVariant &v : entry.value(QStringLiteral("markers")).toList()) {
        const auto m = v.toMap();
        out.append({m.value(QStringLiteral("frame")).toInt(), m.value(QStringLiteral("text")).toString(), m.value(QStringLiteral("type")).toString()});
    }
    return out;
}
const QStringList kCarried = {"title", "titlePlace", "titleSize", "titleFont", "titleColor", "titleAlign", "titleWeight", "titleItalic",
                              "titleBg", "titleOutline", "caption", "generatorColor", "volume", "pan", "opacity", "label", "link",
                              "fadeIn", "fadeOut", "audioMap", "normalize", "rectX", "rectY", "rectW", "rectH", "rotation", "flipH", "flipV"};
}

Document fromTimeline(const QVariantList &storyline, const QVariantList &connected, const QVariantList &tracks,
                      const QVariantList &markers, const QVariantMap &project, const QVariantList &library) {
    Document doc;
    doc.name = project.value(QStringLiteral("name")).toString();
    doc.fpsNum = project.value(QStringLiteral("fpsNum"), 25).toInt();
    doc.fpsDen = qMax(1, project.value(QStringLiteral("fpsDen"), 1).toInt());
    doc.width = project.value(QStringLiteral("width"), 1920).toInt();
    doc.height = project.value(QStringLiteral("height"), 1080).toInt();
    doc.startTimecodeFrames = project.value(QStringLiteral("startFrame"), 0).toLongLong();
    for (const QVariant &v : markers) {
        const auto m = v.toMap();
        doc.markers.append({m.value(QStringLiteral("frame")).toInt(), m.value(QStringLiteral("text")).toString(), m.value(QStringLiteral("type")).toString()});
    }
    QVariantList entries = storyline + connected;
    int lanes = 1;
    for (const QVariant &v : entries) lanes = qMax(lanes, v.toMap().value(QStringLiteral("lane")).toInt() + 1);
    QList<Track> video, audio;
    for (int lane = 0; lane < lanes; ++lane) {
        const auto flags = lane < tracks.size() ? tracks.at(lane).toMap() : QVariantMap();
        // Backend::tracks() names a sound track by kind; saved track flags
        // carry an audio bool. Missing both, the lane was read as video and
        // a sound track went out as an extra, empty video track.
        const bool audioTrack = flags.value(QStringLiteral("kind")).toString() == QStringLiteral("audio")
                                || flags.value(QStringLiteral("audio")).toBool();
        Track v, a;
        v.kind = Track::Video; a.kind = Track::Audio;
        v.name = flags.value(QStringLiteral("name")).toString();
        if (v.name.isEmpty()) v.name = audioTrack ? QStringLiteral("A%1").arg(lane + 1) : QStringLiteral("V%1").arg(lane + 1);
        a.name = audioTrack ? v.name : QStringLiteral("%1 audio").arg(v.name);
        v.locked = a.locked = flags.value(QStringLiteral("locked")).toBool();
        v.muted = a.muted = flags.value(QStringLiteral("muted")).toBool();
        v.disabled = a.disabled = flags.value(QStringLiteral("disabled")).toBool();
        QList<QVariantMap> ordered;
        for (const QVariant &e : entries) if (e.toMap().value(QStringLiteral("lane")).toInt() == lane) ordered.append(e.toMap());
        std::sort(ordered.begin(), ordered.end(), [](const QVariantMap &x, const QVariantMap &y) {
            return x.value(QStringLiteral("start")).toInt() < y.value(QStringLiteral("start")).toInt(); });
        for (const QVariantMap &e : ordered) {
            Item item;
            item.path = e.value(QStringLiteral("path")).toString();
            item.name = e.value(QStringLiteral("name")).toString();
            item.start = e.value(QStringLiteral("start")).toInt();
            item.duration = e.value(QStringLiteral("frames")).toInt();
            item.transitionOut = e.value(QStringLiteral("mixOut")).toInt();
            // The mix region belongs to the outgoing side: the incoming
            // entry's visible range starts after its head handle.
            item.sourceIn = e.value(QStringLiteral("in")).toInt() + e.value(QStringLiteral("mixIn")).toInt();
            item.speed = e.value(QStringLiteral("speed"), 1.0).toDouble();
            if (item.speed == 0.0) item.speed = 1.0;
            item.retime = e.value(QStringLiteral("retime")).toMap();
            if(e.contains("audioTiming"))item.extras["audioTiming"]=e.value("audioTiming");
            item.sampling = e.value(QStringLiteral("frameBlend")).toInt();
            item.enabled = !e.value(QStringLiteral("disabled")).toBool();
            item.audioOnly = e.value(QStringLiteral("audioOnly")).toBool();
            item.videoOnly = e.value(QStringLiteral("videoOnly")).toBool();
            item.markers = clipMarkers(e);
            for (const QString &key : kCarried) if (e.contains(key)) item.extras.insert(key, e.value(key));
            if(e.contains("sourceOrigin"))item.extras["sourceOrigin"]=e.value("sourceOrigin");
            // NOLINTNEXTLINE(bugprone-branch-clone): a gap and a solid colour both leave as a gap; the order of the tests matters
            if (e.value(QStringLiteral("gap")).toBool()) { item.kind = Item::Gap; item.path.clear(); }
            else if (!e.value(QStringLiteral("title")).toString().isEmpty()) { item.generator = QStringLiteral("title"); item.path.clear(); }
            else if (!e.value(QStringLiteral("generator")).toString().isEmpty()) { item.generator = e.value(QStringLiteral("generator")).toString(); item.path.clear(); }
            else if (e.value(QStringLiteral("compound")).toBool() || e.value(QStringLiteral("nest")).toBool() || item.path.startsWith(QStringLiteral("omaedit.project:"))) {
                item.generator = QStringLiteral("nest"); item.path.clear();
                doc.notes.append(QStringLiteral("%1: nested clips export as a placeholder").arg(item.name));
            } else if (item.path.startsWith(QStringLiteral("color:"))) { item.kind = Item::Gap; item.path.clear(); }
            if (!item.path.isEmpty()) {
                const auto lib = libraryEntry(library, item.path);
                if(e.contains("sourceOrigin")) {
                    auto source=lib;const auto metadata=e.value("sourceMeta").toMap();
                    for(auto it=metadata.begin();it!=metadata.end();++it)source[it.key()]=it.value();
                    source.remove("rangeIn");source.remove("rangeOut");
                    const auto full=SourceTiming::resolve(source,doc.fps());
                    if(!full.isEmpty()) {
                        const double available=full.value("sourceFrames").toDouble()*full.value("scale").toDouble()/std::abs(item.speed);
                        // Zero phase is compatible with ordinary writers except
                        // when the cut needs a newly retained partial EOF frame.
                        if(e.value("out").toInt()>=std::llrint(available))item.extras["exactSourceTail"]=true;
                    }
                }
                if (item.name.isEmpty()) item.name = lib.value(QStringLiteral("name"), QFileInfo(item.path).fileName()).toString();
                item.sourceLength = lib.value(QStringLiteral("duration"), e.value(QStringLiteral("length")).toInt()).toInt();
                if (!item.retime.isEmpty()) item.sourceLength = qCeil(item.retime.value(QStringLiteral("sourceLength")).toDouble());
                item.sourceTimecodeFrames = libraryTimecode(lib);
                if (!item.retime.isEmpty()) {
                    item.sourceTimecodeSeconds = libraryTimecodeSeconds(lib, doc.fps());
                    if (item.sourceTimecodeSeconds >= 0) {
                        item.sourceTimecodeFrames = qRound64(item.sourceTimecodeSeconds * doc.fps());
                    }
                }
                if (!lib.isEmpty() && !lib.value(QStringLiteral("hasAudio"), true).toBool()) item.videoOnly = true;
                if (!lib.isEmpty() && !lib.value(QStringLiteral("hasVideo"), true).toBool()) item.audioOnly = true;
            }
            if (item.duration <= 0) continue;
            if (!item.audioOnly) v.items.append(item);
            if (!item.videoOnly && item.kind == Item::Clip && item.generator.isEmpty()) a.items.append(item);
        }
        if (!v.items.isEmpty() || !audioTrack) video.append(v);
        if (!a.items.isEmpty() || audioTrack) audio.append(a);
    }
    doc.tracks = video + audio;
    return doc;
}

// ── document → entries ────────────────────────────────────────────────────
Placement toEntries(const Document &doc, double projectFps, const QVariantList &library) {
    Placement out;
    const double targetRate = projectFps > 0 ? projectFps : doc.fps();
    const double scale = doc.fps() > 0 && projectFps > 0 ? projectFps / doc.fps() : 1.0;
    auto frames = [&](qint64 f) { return int(qRound64(f * scale)); };
    QList<Track> video, audio;
    for (const Track &t : doc.tracks) (t.kind == Track::Video ? video : audio).append(t);
    // Audio items that mirror a video item (same file, start and length)
    // ride that video entry; the rest become audio-only entries on lanes of
    // their own, after the video lanes.
    auto mirrored = [&](const Item &a) {
        for (const Track &v : video) for (const Item &i : v.items)
            if (i.kind == Item::Clip && i.path == a.path && i.start == a.start && i.duration == a.duration
                && i.sourceIn == a.sourceIn && i.speed == a.speed && i.retime == a.retime
                && i.extras.value("sourceOrigin")==a.extras.value("sourceOrigin") && !i.videoOnly) return true;
        return false;
    };
    QList<Track> lanes = video;
    for (const Track &a : audio) {
        Track own = a; own.items.clear();
        for (const Item &i : a.items) if (i.kind == Item::Clip && !mirrored(i)) { Item copy = i; copy.audioOnly = true; own.items.append(copy); }
        if (!own.items.isEmpty()) lanes.append(own);
    }
    if (lanes.isEmpty()) lanes.append(Track());
    for (int lane = 0; lane < lanes.size(); ++lane) {
        const Track &track = lanes.at(lane);
        out.trackNames.append(track.name);
        int cursor = 0, index = 0, pendingMixIn = 0;
        for (int n = 0; n < track.items.size(); ++n) {
            const Item &item = track.items.at(n);
            if (item.kind == Item::Gap) continue;               // empty stretches come from the next item's start
            std::optional<double> origin;
            if(!SourceTiming::readOrigin(item.extras,origin) || (origin && !qFuzzyCompare(scale,1.))) {
                out.entries.clear();out.error=QStringLiteral("%1: the exact source origin cannot be converted at this project rate. Import into a sequence with the timeline's frame rate.").arg(item.name);return out;
            }
            // Numbers a file can hold and a timeline cannot: a speed that is
            // not a number, a place or length past what a timeline can be.
            // Refused with the clip named, not cut down to fit and placed
            // where nothing can reach it.
            const double placed = double(item.start) * scale, lasting = double(item.duration) * scale;
            if (!std::isfinite(item.speed) || !std::isfinite(placed) || !std::isfinite(lasting)
                || std::abs(placed) > 100000000. || lasting > 100000000.) {
                out.entries.clear();
                out.error = QStringLiteral("%1: its place, length or speed in the file cannot be used on a timeline.").arg(item.name);
                return out;
            }
            const int start = frames(item.start), duration = frames(item.duration);
            if (duration <= 0) continue;
            QVariantMap e;
            e.insert(QStringLiteral("lane"), lane); e.insert(QStringLiteral("track"), lane);
            e.insert(QStringLiteral("index"), index++);
            e.insert(QStringLiteral("start"), start);
            e.insert(QStringLiteral("gapBefore"), lane == 0 ? qMax(0, start - cursor) : 0);
            const int overlap = frames(item.transitionOut);
            if (item.kind == Item::Gap) {
                e.insert(QStringLiteral("path"), QStringLiteral("color:black")); e.insert(QStringLiteral("gap"), true);
                e.insert(QStringLiteral("name"), QStringLiteral("Gap"));
                e.insert(QStringLiteral("in"), 0); e.insert(QStringLiteral("out"), duration - 1);
                e.insert(QStringLiteral("frames"), duration); e.insert(QStringLiteral("length"), duration);
            } else {
                const auto lib = libraryEntry(library, item.path);
                const double libraryRate = lib.value(QStringLiteral("fps"), targetRate).toDouble();
                const double libraryScale = libraryRate > 0 ? targetRate / libraryRate : 1.;
                const double fileClock = libraryTimecodeSeconds(lib, targetRate);
                const double speed = item.speed == 0.0 ? 1.0 : item.speed;
                int sourceIn = frames(item.sourceIn);
                // Source timecode in the document, file timecode in the library: producer frames are the difference.
                if (item.sourceTimecodeFrames >= 0 && !lib.isEmpty()) {
                    const qint64 fileTc = libraryTimecode(lib);
                    if (item.retime.isEmpty() && (item.sourceTimecodeSeconds >= 0 ? fileClock >= 0 : fileTc >= 0)) {
                        const double shift = item.sourceTimecodeSeconds >= 0
                            ? (item.sourceTimecodeSeconds - fileClock) * targetRate
                            : frames(item.sourceTimecodeFrames) - frames(fileTc);
                        sourceIn = qMax(0, qRound(sourceIn + shift));
                    }
                }
                const int in = qMax(0, sourceIn - pendingMixIn);
                const int visible = duration;
                e.insert(QStringLiteral("in"), in);
                e.insert(QStringLiteral("frames"), visible);
                e.insert(QStringLiteral("out"), in + pendingMixIn + visible - 1);
                e.insert(QStringLiteral("mixIn"), pendingMixIn);
                e.insert(QStringLiteral("mixOut"), overlap);
                int length = lib.isEmpty() ? qMax(frames(item.sourceLength), in + pendingMixIn + visible) : lib.value(QStringLiteral("duration")).toInt();
                if (item.sourceTimecodeSeconds >= 0 && item.retime.isEmpty()) {
                    const double available = lib.isEmpty() ? item.sourceLength * scale : lib.value("duration").toDouble() * libraryScale;
                    length = qMax(qCeil(available / std::abs(speed)), in + pendingMixIn + visible);
                }
                e.insert(QStringLiteral("length"), qMax(1, length));
                if (!qFuzzyCompare(speed, 1.0)) e.insert(QStringLiteral("speed"), speed);
                if (item.sampling != FrameSampling::Original) e["frameBlend"] = item.sampling;
                if (!item.retime.isEmpty()) {
                    RetimeMap map; QString why;
                    auto fail = [&](const QString &reason) {
                        out.entries.clear(); out.error = QStringLiteral("%1: %2").arg(item.name, reason);
                    };
                    if (!map.set(item.retime, &why) || !qFuzzyCompare(speed, 1.0)
                        || !FrameSampling::valid(item.sampling)) {
                        fail(why.isEmpty() ? QStringLiteral("Invalid mapped timing.") : why); return out;
                    }
                    if (map.frames() * scale > 100000000.) {
                        fail(QStringLiteral("The converted speed map exceeds the supported duration.")); return out;
                    }
                    const double forcedRate = lib.value("forceFps").toDouble();
                    if (forcedRate > 0 && !qFuzzyCompare(forcedRate, lib.value("sourceFps").toDouble())) {
                        fail(QStringLiteral("Reset this file's frame-rate interpretation before importing mapped timing.")); return out;
                    }
                    auto definition = map.definition();
                    // Both axes use project-rate frames. Keep exact curve phases,
                    // fractional source positions and jumps when changing rate.
                    // A knot between destination frames cannot be represented by
                    // the native segment model; refuse instead of shifting it.
                    auto aligned = [&](double f) { return std::abs(f * scale - std::round(f * scale)) < 1e-6; };
                    if (!aligned(item.sourceIn) || !aligned(item.duration)) {
                        fail(QStringLiteral("Speed-map cuts do not align at this project rate. Import into a sequence with the timeline's frame rate.")); return out;
                    }
                    const double sourceClock = item.sourceTimecodeSeconds >= 0 ? item.sourceTimecodeSeconds
                        : item.sourceTimecodeFrames >= 0 ? item.sourceTimecodeFrames / doc.fps() : -1.;
                    // Subtract clocks in the same units before scaling. Equal
                    // clocks stay exactly equal across repeated document passes.
                    const double shift = sourceClock >= 0 && fileClock >= 0 ? (sourceClock - fileClock) * targetRate : 0.;
                    definition["sourceIn"] = definition.value("sourceIn").toDouble() * scale + shift;
                    double sourceEnd = map.sourceLength() * scale + shift;
                    // The producer exposes a whole number of project frames
                    // (round-to-nearest), even when media duration is fractional
                    // at this rate. Keep the declared source domain, bounded by
                    // that available length; never extend its last-picture clamp.
                    if (lib.contains("duration")) sourceEnd = std::min(sourceEnd,
                        std::nearbyint(lib.value("duration").toDouble() * libraryScale));
                    definition["sourceLength"] = sourceEnd;
                    QVariantList segments;
                    for (const auto &v : definition.value("segments").toList()) {
                        auto segment = v.toMap(); const int count = segment.value("frames").toInt();
                        if (!aligned(count) || frames(count) < 1) {
                            fail(QStringLiteral("Speed-map boundaries do not align at this project rate. Import into a sequence with the timeline's frame rate.")); return out;
                        }
                        segment["frames"] = frames(count);
                        if (segment.contains("source")) segment["source"] = segment.value("source").toDouble() * scale + shift;
                        segments.append(segment);
                    }
                    definition["segments"] = segments;
                    if (!map.set(definition, &why) || sourceIn < pendingMixIn
                        || qint64(sourceIn) + duration > map.frames()) {
                        fail(why.isEmpty() ? QStringLiteral("The speed map does not cover the clip and its transition handles.") : why); return out;
                    }
                    e["retime"] = map.definition(); e["frameBlend"] = item.sampling;
                    e["length"] = map.frames(); e.remove("speed");
                }
                if (!item.generator.isEmpty()) {
                    if (item.generator == QStringLiteral("title")) {
                        e.insert(QStringLiteral("path"), QStringLiteral("color:#00000000"));
                        e.insert(QStringLiteral("title"), item.extras.value(QStringLiteral("title"), item.name).toString());
                    } else if (item.generator == QStringLiteral("solid")) {
                        e.insert(QStringLiteral("path"), QStringLiteral("color:") + item.extras.value(QStringLiteral("generatorColor"), QStringLiteral("#000000")).toString());
                    } else {
                        // bars/noise/count/nest: the editor's own generator when it has one, a gap otherwise
                        e.insert(QStringLiteral("path"), item.generator == QStringLiteral("bars") ? QStringLiteral("frei0r.test_pat_B") : QStringLiteral("color:black"));
                        if (item.generator == QStringLiteral("nest")) { e.insert(QStringLiteral("gap"), true); out.problems.append(QStringLiteral("%1: nested sequence placed as a gap").arg(item.name)); }
                    }
                    e.insert(QStringLiteral("name"), item.name);
                } else {
                    e.insert(QStringLiteral("path"), item.path);
                    e.insert(QStringLiteral("name"), item.name.isEmpty() ? QFileInfo(item.path).fileName() : item.name);
                    if (!out.mediaPaths.contains(item.path)) out.mediaPaths.append(item.path);
                }
                if (!item.enabled) e.insert(QStringLiteral("disabled"), true);
                if (item.audioOnly) e.insert(QStringLiteral("audioOnly"), true);
                if (item.videoOnly) e.insert(QStringLiteral("videoOnly"), true);
                QVariantList markers;
                for (const Marker &m : item.markers) markers.append(QVariantMap{{"frame", frames(m.frame)}, {"text", m.text}, {"type", m.type.isEmpty() ? QStringLiteral("standard") : m.type}});
                if (!markers.isEmpty()) e.insert(QStringLiteral("markers"), markers);
                for (auto it = item.extras.constBegin(); it != item.extras.constEnd(); ++it) if (!e.contains(it.key())) e.insert(it.key(), it.value());
            }
            out.entries.append(e);
            // The outgoing entry's frames include the dissolve; the next
            // starts where the mix region ends.
            cursor = start + duration;
            pendingMixIn = overlap;
            if (n + 1 < track.items.size() && overlap > 0 && frames(track.items.at(n + 1).start) != cursor) {
                out.problems.append(QStringLiteral("%1: dissolve into a non-adjacent item dropped").arg(item.name));
                pendingMixIn = 0; e.insert(QStringLiteral("mixOut"), 0); out.entries[out.entries.size() - 1] = e;
            }
        }
    }
    for (const Marker &m : doc.markers)
        out.markers.append(QVariantMap{{"frame", frames(m.frame)}, {"text", m.text}, {"type", m.type.isEmpty() ? QStringLiteral("standard") : m.type}});
    return out;
}

// ── JSON form (the AAF helper and tests speak it) ─────────────────────────
static QVariantMap markerVariant(const Marker &m) { return {{"frame", m.frame}, {"text", m.text}, {"type", m.type}}; }
static Marker markerFrom(const QVariantMap &m) { return {m.value("frame").toInt(), m.value("text").toString(), m.value("type").toString()}; }
QVariantMap toVariant(const Document &d) {
    QVariantList tracks;
    for (const Track &t : d.tracks) {
        QVariantList items;
        for (const Item &i : t.items) {
            QVariantList markers; for (const Marker &m : i.markers) markers.append(markerVariant(m));
            items.append(QVariantMap{{"kind", i.kind == Item::Gap ? "gap" : "clip"}, {"name", i.name}, {"path", i.path}, {"start", i.start},
                {"duration", i.duration}, {"sourceIn", i.sourceIn}, {"sourceLength", i.sourceLength}, {"speed", i.speed}, {"transitionOut", i.transitionOut},
                {"enabled", i.enabled}, {"audioOnly", i.audioOnly}, {"videoOnly", i.videoOnly}, {"sourceTimecodeFrames", i.sourceTimecodeFrames},
                {"sourceTimecodeSeconds", i.sourceTimecodeSeconds},
                {"generator", i.generator}, {"markers", markers}, {"extras", i.extras}, {"retime", i.retime}, {"sampling", i.sampling}});
        }
        tracks.append(QVariantMap{{"kind", t.kind == Track::Audio ? "audio" : "video"}, {"name", t.name}, {"locked", t.locked}, {"muted", t.muted}, {"disabled", t.disabled}, {"items", items}});
    }
    QVariantList markers; for (const Marker &m : d.markers) markers.append(markerVariant(m));
    return {{"name", d.name}, {"fpsNum", d.fpsNum}, {"fpsDen", d.fpsDen}, {"width", d.width}, {"height", d.height},
            {"startTimecodeFrames", d.startTimecodeFrames}, {"tracks", tracks}, {"markers", markers}, {"notes", d.notes}};
}
Document fromVariant(const QVariantMap &m) {
    Document d;
    d.name = m.value("name").toString(); d.fpsNum = m.value("fpsNum", 25).toInt(); d.fpsDen = qMax(1, m.value("fpsDen", 1).toInt());
    d.width = m.value("width", 1920).toInt(); d.height = m.value("height", 1080).toInt(); d.startTimecodeFrames = m.value("startTimecodeFrames").toLongLong();
    for (const QVariant &tv : m.value("tracks").toList()) {
        const auto t = tv.toMap(); Track track;
        track.kind = t.value("kind").toString() == "audio" ? Track::Audio : Track::Video;
        track.name = t.value("name").toString(); track.locked = t.value("locked").toBool(); track.muted = t.value("muted").toBool(); track.disabled = t.value("disabled").toBool();
        for (const QVariant &iv : t.value("items").toList()) {
            const auto i = iv.toMap(); Item item;
            item.kind = i.value("kind").toString() == "gap" ? Item::Gap : Item::Clip;
            item.name = i.value("name").toString(); item.path = i.value("path").toString(); item.start = i.value("start").toInt();
            item.duration = i.value("duration").toInt(); item.sourceIn = i.value("sourceIn").toInt(); item.sourceLength = i.value("sourceLength").toInt();
            item.speed = i.value("speed", 1.0).toDouble(); item.transitionOut = i.value("transitionOut").toInt(); item.enabled = i.value("enabled", true).toBool();
            item.retime = i.value("retime").toMap(); item.sampling = i.value("sampling").toInt();
            item.audioOnly = i.value("audioOnly").toBool(); item.videoOnly = i.value("videoOnly").toBool(); item.sourceTimecodeFrames = i.value("sourceTimecodeFrames", -1).toLongLong();
            item.sourceTimecodeSeconds = i.value("sourceTimecodeSeconds", -1.).toDouble();
            item.generator = i.value("generator").toString(); item.extras = i.value("extras").toMap();
            for (const QVariant &mv : i.value("markers").toList()) item.markers.append(markerFrom(mv.toMap()));
            track.items.append(item);
        }
        d.tracks.append(track);
    }
    for (const QVariant &mv : m.value("markers").toList()) d.markers.append(markerFrom(mv.toMap()));
    d.notes = m.value("notes").toStringList();
    return d;
}

// ── format registry ───────────────────────────────────────────────────────
QStringList formats() { return {QStringLiteral("otio"), QStringLiteral("edl"), QStringLiteral("fcpxml"), QStringLiteral("xmeml"), QStringLiteral("aaf")}; }
QString formatOf(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("otio")) return QStringLiteral("otio");
    if (suffix == QStringLiteral("edl")) return QStringLiteral("edl");
    if (suffix == QStringLiteral("fcpxml")) return QStringLiteral("fcpxml");
    if (suffix == QStringLiteral("xml")) return QStringLiteral("xmeml");
    if (suffix == QStringLiteral("aaf")) return QStringLiteral("aaf");
    return {};
}
QString formatName(const QString &format) {
    if (format == QStringLiteral("otio")) return QStringLiteral("OpenTimelineIO");
    if (format == QStringLiteral("edl")) return QStringLiteral("CMX 3600 EDL");
    if (format == QStringLiteral("fcpxml")) return QStringLiteral("XML timeline (.fcpxml)");
    if (format == QStringLiteral("xmeml")) return QStringLiteral("Legacy XML timeline (.xml)");
    if (format == QStringLiteral("aaf")) return QStringLiteral("AAF");
    return {};
}
QStringList formatPatterns(const QString &format) { return {QStringLiteral("*.") + (format == QStringLiteral("xmeml") ? QStringLiteral("xml") : format)}; }
bool write(const Document &document, const QString &path, const QString &format, QString *error) {
    if (format == QStringLiteral("otio")) return writeOtio(document, path, error);
    if (format == QStringLiteral("aaf")) return writeAaf(document, path, error);
    QString text;
    if (format == QStringLiteral("edl")) text = writeEdl(document, document.name, error);
    else if (format == QStringLiteral("fcpxml")) text = writeFcpxml(document, error);
    else if (format == QStringLiteral("xmeml")) text = writeXmeml(document, error);
    else { if (error) *error = QStringLiteral("Unknown timeline format."); return false; }
    if (text.isEmpty()) return false;
    // Publish only a complete replacement, preserving an existing export on failure.
    QSaveFile file(path);
    const auto bytes = text.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
bool requiresMappedTiming(const Document &document, QString *error) {
    for (const auto &track : document.tracks) for (const auto &item : track.items) {
        if(item.extras.contains("audioTiming")) {
            if(error)*error=QStringLiteral("%1: this timeline writer cannot preserve precise sound cuts yet. Save the native project or render its sound.").arg(item.name);
            return true;
        }
        std::optional<double> origin;
        if(!SourceTiming::readOrigin(item.extras,origin) || (origin && *origin!=0.) || item.extras.value("exactSourceTail").toBool()) {
            if(error)*error=QStringLiteral("%1: this timeline writer cannot preserve exact marked source timing yet. Render the clip in place, or save the native project.").arg(item.name);
            return true;
        }
        if (!item.retime.isEmpty()) {
            if (error) *error = QStringLiteral("%1: this timeline writer cannot preserve speed segments yet. Render the clip in place, or save the native project.").arg(item.name);
            return true;
        }
    }
    return false;
}
bool read(const QString &path, const QString &format, Document *document, QString *error, int edlFpsNum, int edlFpsDen) {
    if (format == QStringLiteral("otio")) return readOtio(path, document, error);
    if (format == QStringLiteral("aaf")) return readAaf(path, document, error);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    const QString text = QString::fromUtf8(file.readAll());
    if (format == QStringLiteral("edl")) return readEdl(text, document, error, edlFpsNum, edlFpsDen);
    if (format == QStringLiteral("fcpxml")) return readFcpxml(text, document, error);
    if (format == QStringLiteral("xmeml")) return readXmeml(text, document, error);
    if (error) *error = QStringLiteral("Unknown timeline format.");
    return false;
}

// ── CMX 3600 EDL ──────────────────────────────────────────────────────────
// One video track (V1) and its audio (AA) per EDL; further video tracks are
// written as V2… comment blocks other tools ignore. Source times are the
// file's timecode plus the producer frame; a file without timecode counts
// from zero. Reel names are the file names; a FROM CLIP NAME comment
// carries the full path.
QString writeEdl(const Document &doc, const QString &title, QString *error) {
    if (requiresMappedTiming(doc, error)) return {};
    const QList<Track> tracks = doc.tracks;
    QList<Track> video; for (const Track &t : tracks) if (t.kind == Track::Video) video.append(t);
    if (video.isEmpty()) { if (error) *error = QStringLiteral("Nothing to export: the sequence has no video track."); return {}; }
    const double fps = doc.fps();
    // Every timecode is written as the screen counts it: non-drop, at 29.97
    // and 59.94 too, so the file says what the editor saw.
    QString out;
    QTextStream s(&out);
    s << "TITLE: " << (title.isEmpty() ? QStringLiteral("OmaEdit sequence") : title) << "\n";
    s << "FCM: NON-DROP FRAME\n\n";
    int event = 0;
    auto reel = [](const Item &i) {
        QString r = i.kind == Item::Gap ? QStringLiteral("BL") : QFileInfo(i.path.isEmpty() ? i.name : i.path).completeBaseName().toUpper();
        r.replace(QRegularExpression(QStringLiteral("[^A-Z0-9_]")), QStringLiteral("_"));
        if (r.isEmpty()) r = QStringLiteral("AX");
        return r.left(8);
    };
    auto srcTc = [&](const Item &i, qint64 producerFrame) {
        const qint64 base = i.sourceTimecodeFrames >= 0 ? i.sourceTimecodeFrames : 0;
        return timecode(base + producerFrame, fps);
    };
    auto recTc = [&](qint64 frame) { return timecode(doc.startTimecodeFrames + frame, fps); };
    auto line = [&](int ev, const QString &r, const QString &ch, const QString &kind, int dur, const QString &si, const QString &so, const QString &ri, const QString &ro) {
        s << QString("%1  %2 %3 %4 %5 %6 %7 %8 %9\n").arg(ev, 3, 10, QChar('0')).arg(r, -8).arg(ch, -4).arg(kind, -1)
                 .arg(kind == "D" ? QString("%1").arg(dur, 3, 10, QChar('0')) : QString(), -4).arg(si, so, ri, ro);
    };
    for (int v = 0; v < video.size(); ++v) {
        const Track &track = video.at(v);
        if (v > 0) s << "\n* V" << (v + 1) << " " << track.name << "\n";
        int pendingOverlap = 0; Item previous;
        for (const Item &item : track.items) {
            if (item.kind == Item::Gap && pendingOverlap == 0) continue; // black between events is implied by record times
            const QString channel = item.audioOnly ? QStringLiteral("AA") : item.videoOnly || item.kind == Item::Gap ? QStringLiteral("V") : QStringLiteral("AA/V");
            const double a = qAbs(item.speed == 0.0 ? 1.0 : item.speed);
            const qint64 srcIn = qRound64(item.sourceIn * a), srcOut = qRound64((item.sourceIn + item.duration) * a);
            ++event;
            if (pendingOverlap > 0) {
                // Two-line event: the outgoing source holds, the incoming dissolves in over `pendingOverlap` frames.
                const qint64 pIn = qRound64((previous.sourceIn + previous.duration - previous.transitionOut) * qAbs(previous.speed == 0.0 ? 1.0 : previous.speed));
                line(event, reel(previous), channel, "C", 0, srcTc(previous, pIn), srcTc(previous, pIn), recTc(item.start - pendingOverlap), recTc(item.start - pendingOverlap));
                const qint64 headIn = qRound64((item.sourceIn - pendingOverlap) * a);
                line(event, reel(item), channel, "D", pendingOverlap, srcTc(item, qMax<qint64>(0, headIn)), srcTc(item, srcOut), recTc(item.start - pendingOverlap), recTc(item.start + item.duration));
            } else {
                // A cut runs to where its dissolve (if any) begins; the dissolve event carries the rest.
                line(event, reel(item), channel, "C", 0, srcTc(item, srcIn), srcTc(item, qRound64((item.sourceIn + item.duration - item.transitionOut) * a)), recTc(item.start), recTc(item.start + item.duration - item.transitionOut));
            }
            if (item.kind == Item::Clip && !item.path.isEmpty()) s << "* FROM CLIP NAME: " << item.name << "\n* SOURCE FILE: " << item.path << "\n";
            if (!qFuzzyCompare(a, 1.0) || item.speed < 0) s << "M2   " << reel(item) << "  " << QString::number(item.speed * fps, 'f', 1) << "  " << srcTc(item, srcIn) << "\n";
            if (!item.enabled) s << "* DISABLED\n";
            for (const Marker &m : item.markers) s << "* MARKER " << timecode(m.frame, fps) << " " << m.text << "\n";
            pendingOverlap = item.transitionOut;
            previous = item;
        }
    }
    if (!doc.markers.isEmpty()) { s << "\n"; for (const Marker &m : doc.markers) s << "* LOC: " << recTc(m.frame) << " " << (m.type.isEmpty() ? QStringLiteral("standard") : m.type) << " " << m.text << "\n"; }
    return out;
}

bool readEdl(const QString &text, Document *doc, QString *error, int fpsNum, int fpsDen) {
    *doc = Document();
    if (fpsNum <= 0 || fpsDen <= 0) { fpsNum = 25; fpsDen = 1; }
    doc->fpsNum = fpsNum; doc->fpsDen = fpsDen;
    static const QRegularExpression eventLine(QStringLiteral("^(\\d{3,6})\\s+(\\S+)\\s+(AA/V|A/V|AA|A\\d?|V|B|NONE)\\s+(C|D|W\\d{3}|K\\s+B?)\\s*(\\d{3})?\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s*$"), QRegularExpression::CaseInsensitiveOption);
    // An EDL names no rate: the caller's is used. One that says DROP FRAME
    // can only be 29.97 or 59.94, and is 29.97 unless the caller's is 59.94.
    double fps = doc->fps(); bool drop = false;
    // A record time earlier than the first one is after midnight.
    const auto onTimeline = [&](qint64 recorded) { const qint64 day = 86400ll * qCeil(fps - 0.001); return ((recorded - doc->startTimecodeFrames) % day + day) % day; };
    Track video; video.kind = Track::Video; video.name = QStringLiteral("V1");
    Track audio; audio.kind = Track::Audio; audio.name = QStringLiteral("A1");
    QList<Track> moreVideo;            // "* V2 name" blocks, our own convention
    QString lastEvent; Item *last = nullptr;
    QList<Item> items; QList<bool> hasAudio; QList<int> trackOf; int currentTrack = 0;
    static const QRegularExpression trackLine(QStringLiteral("^\\*\\s+V(\\d+)\\s*(.*)$"));
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("TITLE:"), Qt::CaseInsensitive)) { doc->name = line.mid(6).trimmed(); continue; }
        if (line.startsWith(QStringLiteral("FCM:"), Qt::CaseInsensitive)) { drop = line.contains(QStringLiteral("DROP"), Qt::CaseInsensitive) && !line.contains(QStringLiteral("NON")); if (drop && qAbs(fps - 59.94) >= 0.01) { doc->fpsNum = 30000; doc->fpsDen = 1001; fps = doc->fps(); } continue; }
        if (const auto tl = trackLine.match(line); tl.hasMatch() && tl.captured(1).toInt() >= 2) {
            Track t; t.kind = Track::Video; t.name = tl.captured(2).trimmed().isEmpty() ? QStringLiteral("V%1").arg(tl.captured(1)) : tl.captured(2).trimmed();
            moreVideo.append(t); currentTrack = moreVideo.size(); continue;
        }
        if (line.startsWith(QStringLiteral("* FROM CLIP NAME:"), Qt::CaseInsensitive) && !items.isEmpty()) { items.last().name = line.mid(17).trimmed(); continue; }
        if (line.startsWith(QStringLiteral("* SOURCE FILE:"), Qt::CaseInsensitive) && !items.isEmpty()) { items.last().path = line.mid(14).trimmed(); continue; }
        if (line.startsWith(QStringLiteral("* DISABLED"), Qt::CaseInsensitive) && !items.isEmpty()) { items.last().enabled = false; continue; }
        if (line.startsWith(QStringLiteral("* MARKER "), Qt::CaseInsensitive) && !items.isEmpty()) {
            const QStringList parts = line.mid(9).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (!parts.isEmpty()) items.last().markers.append({int(timecodeFrames(parts.first(), fps)), parts.mid(1).join(QLatin1Char(' ')), QString()});
            continue;
        }
        if (line.startsWith(QStringLiteral("* LOC:"), Qt::CaseInsensitive)) {
            const QStringList parts = line.mid(6).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() >= 2) doc->markers.append({int(onTimeline(timecodeFrames(parts.at(0), fps, nullptr, drop))), parts.mid(2).join(QLatin1Char(' ')), parts.at(1)});
            continue;
        }
        if (line.startsWith(QStringLiteral("M2"), Qt::CaseInsensitive) && !items.isEmpty()) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.size() >= 3) { const double rate = parts.at(2).toDouble(); if (std::isfinite(rate) && rate != 0.0) items.last().speed = rate / fps; }
            continue;
        }
        if (line.startsWith(QLatin1Char('*'))) continue;
        const auto m = eventLine.match(line);
        if (!m.hasMatch()) continue;
        const QString ev = m.captured(1), reel = m.captured(2), channel = m.captured(3).toUpper(), kind = m.captured(4).left(1).toUpper();
        const int transition = m.captured(5).toInt();
        const qint64 srcIn = timecodeFrames(m.captured(6), fps, nullptr, drop);
        const qint64 recIn = timecodeFrames(m.captured(8), fps, nullptr, drop), recOut = timecodeFrames(m.captured(9), fps, nullptr, drop);
        if (items.isEmpty() && recIn > 0 && doc->startTimecodeFrames == 0 && recIn >= 3600 * qCeil(fps - 0.001) / 2) doc->startTimecodeFrames = recIn; // 01:00:00:00-style record start
        if (kind == QStringLiteral("D") || kind == QStringLiteral("W")) {
            // Incoming side of a two-line event: the previous item (same event) is the outgoing hold line.
            if (ev == lastEvent && last && last->duration == 0) { items.removeLast(); hasAudio.removeLast(); trackOf.removeLast(); }
            if (!items.isEmpty() && transition > 0) {
                Item &outgoing = items.last();
                outgoing.duration += transition;           // the mix region belongs to the outgoing side
                outgoing.transitionOut = transition;
            }
            Item item; item.kind = reel.compare(QStringLiteral("BL"), Qt::CaseInsensitive) == 0 ? Item::Gap : Item::Clip;
            item.name = reel; item.start = int(onTimeline(recIn) + transition); item.duration = int(onTimeline(recOut) - onTimeline(recIn) - transition);
            item.sourceIn = int(srcIn + transition); item.sourceTimecodeFrames = 0;
            item.audioOnly = channel == QStringLiteral("AA") || channel.startsWith(QLatin1Char('A')) && !channel.contains(QLatin1Char('V'));
            item.videoOnly = channel == QStringLiteral("V");
            items.append(item); hasAudio.append(channel.contains(QLatin1Char('A'))); trackOf.append(currentTrack);
        } else {
            Item item; item.kind = reel.compare(QStringLiteral("BL"), Qt::CaseInsensitive) == 0 ? Item::Gap : Item::Clip;
            item.name = reel; item.start = int(onTimeline(recIn)); item.duration = int(onTimeline(recOut) - onTimeline(recIn));
            item.sourceIn = int(srcIn); item.sourceTimecodeFrames = 0;
            item.audioOnly = channel == QStringLiteral("AA") || (channel.startsWith(QLatin1Char('A')) && !channel.contains(QLatin1Char('V')));
            item.videoOnly = channel == QStringLiteral("V");
            items.append(item); hasAudio.append(channel.contains(QLatin1Char('A'))); trackOf.append(currentTrack);
        }
        lastEvent = ev; last = &items.last();
    }
    if (items.isEmpty()) { if (error) *error = QStringLiteral("No edit events found in the EDL."); return false; }
    // Zero-length hold lines that never got an incoming side are noise.
    for (int i = items.size() - 1; i >= 0; --i) if (items.at(i).duration <= 0 || items.at(i).kind == Item::Gap) { items.removeAt(i); hasAudio.removeAt(i); trackOf.removeAt(i); }
    for (int i = 0; i < items.size(); ++i) {
        const Item &item = items.at(i);
        Track &target = trackOf.at(i) == 0 ? video : moreVideo[trackOf.at(i) - 1];
        if (item.audioOnly) { audio.items.append(item); continue; }
        target.items.append(item);
        if (hasAudio.at(i) && !item.videoOnly && trackOf.at(i) == 0) audio.items.append(item);
    }
    doc->tracks = QList<Track>{video} + moreVideo + QList<Track>{audio};
    return true;
}

// ── fcpxml 1.10 timeline XML ───────────────────────────────────────
namespace {
QString rational(qint64 frames, int fpsNum, int fpsDen) {
    // frames at fpsNum/fpsDen seconds → "N/Ds"; whole seconds as "Ns".
    const qint64 num = frames * fpsDen, den = fpsNum;
    if (den > 0 && num % den == 0) return QStringLiteral("%1s").arg(num / den);
    return QStringLiteral("%1/%2s").arg(num).arg(den);
}
QString rationalPosition(double frames, int fpsNum, int fpsDen) {
    // A bounded 32-bit denominator with sub-frame precision, including source
    // timecode that is on a different media frame grid from the sequence.
    return QStringLiteral("%1/1000000000s").arg(qRound64(frames * fpsDen / fpsNum * 1000000000.));
}
qint64 framesOf(const QString &value, int fpsNum, int fpsDen, bool *ok = nullptr) {
    // "N/Ds" or "Ns" → frames at fpsNum/fpsDen.
    QString v = value.trimmed();
    if (v.endsWith(QLatin1Char('s'))) v.chop(1);
    qint64 num = 0, den = 1;
    const int slash = v.indexOf(QLatin1Char('/'));
    bool okNum = false, okDen = true;
    if (slash > 0) { num = v.left(slash).toLongLong(&okNum); den = v.mid(slash + 1).toLongLong(&okDen); }
    else { const double seconds = v.toDouble(&okNum); if (ok) *ok = okNum; return qRound64(seconds * fpsNum / fpsDen); }
    if (ok) *ok = okNum && okDen && den != 0;
    if (den == 0) return 0;
    return qRound64(double(num) * fpsNum / (double(den) * fpsDen));
}
}

QString writeFcpxml(const Document &doc, QString *error) {
    if (requiresMappedTiming(doc, error)) return {};
    QList<Track> video, audio;
    for (const Track &t : doc.tracks) (t.kind == Track::Video ? video : audio).append(t);
    if (video.isEmpty()) { if (error) *error = QStringLiteral("Nothing to export: the sequence has no video track."); return {}; }
    QString out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.writeStartDocument();
    x.writeDTD(QStringLiteral("<!DOCTYPE fcpxml>"));
    x.writeStartElement(QStringLiteral("fcpxml")); x.writeAttribute(QStringLiteral("version"), QStringLiteral("1.10"));
    x.writeStartElement(QStringLiteral("resources"));
    const QString frameDuration = rational(1, doc.fpsNum, doc.fpsDen);
    x.writeStartElement(QStringLiteral("format")); x.writeAttribute(QStringLiteral("id"), QStringLiteral("r1"));
    x.writeAttribute(QStringLiteral("name"), QStringLiteral("FFVideoFormat%1p%2").arg(doc.height).arg(QString::number(doc.fps(), 'g', 5).remove(QLatin1Char('.'))));
    x.writeAttribute(QStringLiteral("frameDuration"), frameDuration);
    x.writeAttribute(QStringLiteral("width"), QString::number(doc.width)); x.writeAttribute(QStringLiteral("height"), QString::number(doc.height));
    x.writeEndElement();
    QStringList assetIds; QList<Item> assets;
    auto assetId = [&](const Item &i) {
        const int at = assetIds.indexOf(i.path);
        if (at >= 0) return QStringLiteral("r%1").arg(at + 2);
        assetIds.append(i.path); assets.append(i);
        return QStringLiteral("r%1").arg(assetIds.size() + 1);
    };
    for (const Track &t : doc.tracks) for (const Item &i : t.items) if (i.kind == Item::Clip && !i.path.isEmpty()) assetId(i);
    for (int n = 0; n < assets.size(); ++n) {
        const Item &i = assets.at(n);
        x.writeStartElement(QStringLiteral("asset"));
        x.writeAttribute(QStringLiteral("id"), QStringLiteral("r%1").arg(n + 2));
        x.writeAttribute(QStringLiteral("name"), i.name.isEmpty() ? QFileInfo(i.path).fileName() : i.name);
        x.writeAttribute(QStringLiteral("start"), i.sourceTimecodeSeconds >= 0
            ? rationalPosition(i.sourceTimecodeSeconds * doc.fps(), doc.fpsNum, doc.fpsDen)
            : rational(qMax<qint64>(0, i.sourceTimecodeFrames), doc.fpsNum, doc.fpsDen));
        if (i.sourceLength > 0) x.writeAttribute(QStringLiteral("duration"), rational(i.sourceLength, doc.fpsNum, doc.fpsDen));
        x.writeAttribute(QStringLiteral("hasVideo"), i.audioOnly ? QStringLiteral("0") : QStringLiteral("1"));
        x.writeAttribute(QStringLiteral("hasAudio"), i.videoOnly ? QStringLiteral("0") : QStringLiteral("1"));
        x.writeAttribute(QStringLiteral("format"), QStringLiteral("r1"));
        x.writeStartElement(QStringLiteral("media-rep")); x.writeAttribute(QStringLiteral("kind"), QStringLiteral("original-media"));
        x.writeAttribute(QStringLiteral("src"), QUrl::fromLocalFile(i.path).toString()); x.writeEndElement();
        x.writeEndElement();
    }
    x.writeEndElement(); // resources
    x.writeStartElement(QStringLiteral("library"));
    x.writeStartElement(QStringLiteral("event")); x.writeAttribute(QStringLiteral("name"), QStringLiteral("OmaEdit"));
    x.writeStartElement(QStringLiteral("project")); x.writeAttribute(QStringLiteral("name"), doc.name.isEmpty() ? QStringLiteral("Sequence") : doc.name);
    x.writeStartElement(QStringLiteral("sequence")); x.writeAttribute(QStringLiteral("format"), QStringLiteral("r1"));
    qint64 total = 0;
    for (const Track &t : doc.tracks) for (const Item &i : t.items) total = qMax<qint64>(total, i.start + i.duration);
    x.writeAttribute(QStringLiteral("duration"), rational(total, doc.fpsNum, doc.fpsDen));
    x.writeAttribute(QStringLiteral("tcStart"), rational(doc.startTimecodeFrames, doc.fpsNum, doc.fpsDen));
    x.writeAttribute(QStringLiteral("tcFormat"), QStringLiteral("NDF"));
    x.writeStartElement(QStringLiteral("spine"));
    auto writeItem = [&](const Item &i, int lane, qint64 offsetOverride) {
        const qint64 offset = lane ? offsetOverride : doc.startTimecodeFrames + i.start;
        if (i.kind == Item::Gap || i.path.isEmpty()) {
            x.writeStartElement(i.generator == QStringLiteral("title") ? QStringLiteral("title") : QStringLiteral("gap"));
            x.writeAttribute(QStringLiteral("name"), i.name.isEmpty() ? QStringLiteral("Gap") : i.name);
            if (lane) x.writeAttribute(QStringLiteral("lane"), QString::number(lane));
            x.writeAttribute(QStringLiteral("offset"), rational(offset, doc.fpsNum, doc.fpsDen));
            x.writeAttribute(QStringLiteral("duration"), rational(i.duration, doc.fpsNum, doc.fpsDen));
            if (i.generator == QStringLiteral("title")) { x.writeAttribute(QStringLiteral("ref"), QStringLiteral("r1")); x.writeStartElement(QStringLiteral("text")); x.writeStartElement(QStringLiteral("text-style")); x.writeCharacters(i.extras.value(QStringLiteral("title"), i.name).toString()); x.writeEndElement(); x.writeEndElement(); }
            x.writeEndElement();
            return;
        }
        x.writeStartElement(QStringLiteral("asset-clip"));
        x.writeAttribute(QStringLiteral("ref"), assetId(i));
        x.writeAttribute(QStringLiteral("name"), i.name);
        if (lane) x.writeAttribute(QStringLiteral("lane"), QString::number(lane));
        x.writeAttribute(QStringLiteral("offset"), rational(offset, doc.fpsNum, doc.fpsDen));
        const double a = qAbs(i.speed == 0.0 ? 1.0 : i.speed);
        const bool retimed = !qFuzzyCompare(a, 1.0) || i.speed < 0 || FrameSampling::explicitPictures(i.sampling);
        const double sourceTc = i.sourceTimecodeSeconds >= 0 ? i.sourceTimecodeSeconds * doc.fps()
                                                            : qMax<qint64>(0, i.sourceTimecodeFrames);
        // A mapped clip's start is in the adjusted local timeline. Source
        // timecode belongs to the map's values, not its output-time axis.
        x.writeAttribute(QStringLiteral("start"), retimed ? rational(i.sourceIn, doc.fpsNum, doc.fpsDen)
            : rationalPosition(sourceTc + i.sourceIn, doc.fpsNum, doc.fpsDen));
        x.writeAttribute(QStringLiteral("duration"), rational(i.duration, doc.fpsNum, doc.fpsDen));
        if (i.audioOnly) x.writeAttribute(QStringLiteral("audioRole"), QStringLiteral("dialogue"));
        if (!i.enabled) x.writeAttribute(QStringLiteral("enabled"), QStringLiteral("0"));
        if (i.videoOnly) x.writeAttribute(QStringLiteral("srcEnable"), QStringLiteral("video"));
        if (i.audioOnly) x.writeAttribute(QStringLiteral("srcEnable"), QStringLiteral("audio"));
        if (retimed || FrameSampling::explicitPictures(i.sampling)) {
            x.writeStartElement(QStringLiteral("timeMap"));
            if (FrameSampling::explicitPictures(i.sampling))
                x.writeAttribute(QStringLiteral("frameSampling"), i.sampling == FrameSampling::Floor
                    ? QStringLiteral("floor") : QStringLiteral("nearest-neighbor"));
            const qint64 end = i.sourceIn + i.duration;
            const double sourceEnd = i.sourceLength > 0 ? i.sourceLength : end * a;
            auto valueTime = [&](double frames) {
                // Keep sub-frame media positions. Integer rounding changes
                // fractional speed and trimmed reverse playback.
                return rationalPosition(frames, doc.fpsNum, doc.fpsDen);
            };
            x.writeStartElement(QStringLiteral("timept")); x.writeAttribute(QStringLiteral("time"), QStringLiteral("0s"));
            x.writeAttribute(QStringLiteral("value"), valueTime(sourceTc + (i.speed < 0 ? sourceEnd : 0.)));
            x.writeAttribute(QStringLiteral("interp"), QStringLiteral("linear")); x.writeEndElement();
            x.writeStartElement(QStringLiteral("timept")); x.writeAttribute(QStringLiteral("time"), rational(end, doc.fpsNum, doc.fpsDen));
            x.writeAttribute(QStringLiteral("value"), valueTime(sourceTc + (i.speed < 0 ? sourceEnd - end * a : end * a)));
            x.writeAttribute(QStringLiteral("interp"), QStringLiteral("linear")); x.writeEndElement();
            x.writeEndElement();
        }
        for (const Marker &m : i.markers) {
            x.writeStartElement(QStringLiteral("marker"));
            x.writeAttribute(QStringLiteral("start"), rationalPosition(sourceTc + m.frame, doc.fpsNum, doc.fpsDen));
            x.writeAttribute(QStringLiteral("duration"), frameDuration); x.writeAttribute(QStringLiteral("value"), m.text);
            if (m.type == QStringLiteral("todo")) x.writeAttribute(QStringLiteral("completed"), QStringLiteral("0"));
            x.writeEndElement();
        }
        x.writeEndElement();
    };
    // V1 is the spine; every other video track is a connected lane above it,
    // audio-only items are lanes below. fcpxml transitions live between spine items.
    const Track &spine = video.first();
    qint64 spineCursor = 0;
    for (int n = 0; n < spine.items.size(); ++n) {
        const Item &i = spine.items.at(n);
        if (i.start > spineCursor) {
            x.writeStartElement(QStringLiteral("gap")); x.writeAttribute(QStringLiteral("name"), QStringLiteral("Gap"));
            x.writeAttribute(QStringLiteral("offset"), rational(doc.startTimecodeFrames + spineCursor, doc.fpsNum, doc.fpsDen));
            x.writeAttribute(QStringLiteral("duration"), rational(i.start - spineCursor, doc.fpsNum, doc.fpsDen)); x.writeEndElement();
        }
        spineCursor = i.start + i.duration;
        writeItem(i, 0, -1);
        if (i.transitionOut > 0 && n + 1 < spine.items.size()) {
            x.writeStartElement(QStringLiteral("transition"));
            x.writeAttribute(QStringLiteral("name"), QStringLiteral("Cross Dissolve"));
            x.writeAttribute(QStringLiteral("offset"), rational(doc.startTimecodeFrames + i.start + i.duration - i.transitionOut, doc.fpsNum, doc.fpsDen));
            x.writeAttribute(QStringLiteral("duration"), rational(i.transitionOut, doc.fpsNum, doc.fpsDen));
            x.writeEndElement();
        }
    }
    // Connected items are written as children of a leading gap? No: the format
    // wants them inside a spine item; the simplest valid form is a spine
    // gap spanning the sequence when the spine is empty, else lanes on the
    // first spine element via a trailing sibling gap.
    QList<QPair<Item, int>> connected;
    for (int v = 1; v < video.size(); ++v) for (const Item &i : video.at(v).items) if (i.kind == Item::Clip) connected.append({i, v});
    int audioLane = 0;
    for (const Track &a : audio) { --audioLane; for (const Item &i : a.items) {
        bool mirrored = false;
        for (const Track &vt : video) for (const Item &vi : vt.items)
            if (vi.path == i.path && vi.start == i.start && vi.duration == i.duration
                && vi.sourceIn == i.sourceIn && vi.speed == i.speed && !vi.videoOnly) mirrored = true;
        if (!mirrored) { Item copy = i; copy.audioOnly = true; connected.append({copy, audioLane}); }
    } }
    if (!connected.isEmpty()) {
        x.writeStartElement(QStringLiteral("gap")); x.writeAttribute(QStringLiteral("name"), QStringLiteral("Connected"));
        x.writeAttribute(QStringLiteral("offset"), rational(doc.startTimecodeFrames + total, doc.fpsNum, doc.fpsDen));
        x.writeAttribute(QStringLiteral("duration"), frameDuration);
        for (const auto &c : connected) writeItem(c.first, c.second, c.first.start - total);
        x.writeEndElement();
    }
    x.writeEndElement(); // spine
    for (const Marker &m : doc.markers) {
        // Sequence markers are not a spine child in fcpxml; keep them as a note element other readers ignore and we read back.
        x.writeStartElement(QStringLiteral("note")); x.writeCharacters(QStringLiteral("marker %1 %2 %3").arg(timecode(m.frame, doc.fps())).arg(m.type.isEmpty() ? QStringLiteral("standard") : m.type).arg(m.text)); x.writeEndElement();
    }
    x.writeEndElement(); x.writeEndElement(); x.writeEndElement(); x.writeEndElement(); // sequence project event library
    x.writeEndElement(); // fcpxml
    x.writeEndDocument();
    return out;
}

namespace {
// Parse source positions without rounding away fractional frames. XML times
// are rational seconds; map knots must lie on native output-frame boundaries.
bool preciseFrames(const QString &text, int num, int den, double *frames) {
    QString value = text.trimmed();
    if (!value.endsWith(QLatin1Char('s')) || num <= 0 || den <= 0) return false;
    value.chop(1);
    const auto parts = value.split(QLatin1Char('/'));
    bool a = false, b = true;
    const double numerator = parts[0].toDouble(&a);
    const double divisor = parts.size() == 2 ? parts[1].toDouble(&b) : 1.;
    if (parts.size() > 2 || !a || !b || divisor <= 0) return false;
    *frames = numerator / divisor * double(num) / den;
    return std::isfinite(*frames) && std::abs(*frames) <= 1000000000.;
}
bool readTimeMap(QXmlStreamReader &x, Item &item, qint64 localStart, int num, int den, QString *error) {
    auto fail = [&](const QString &why) {
        if (error) *error = QStringLiteral("%1: %2").arg(item.name, why);
        return false;
    };
    if (item.path.isEmpty()) return fail(QStringLiteral("A time map needs a resolved media file."));
    const auto attributes = x.attributes();
    for (const auto &attribute : attributes)
        if (attribute.name() != QStringLiteral("preservesPitch") && attribute.name() != QStringLiteral("frameSampling"))
            return fail(QStringLiteral("Unsupported time-map option: %1.").arg(attribute.name().toString()));
    if (attributes.value(QStringLiteral("preservesPitch")) == QStringLiteral("0") && !item.videoOnly)
        return fail(QStringLiteral("Speed maps with pitch preservation disabled are not supported yet."));
    const auto sampling = attributes.value(QStringLiteral("frameSampling")).toString();
    if (sampling.isEmpty() || sampling == QStringLiteral("floor")) item.sampling = FrameSampling::Floor;
    else if (sampling == QStringLiteral("nearest-neighbor")) item.sampling = FrameSampling::Nearest;
    else if (sampling == QStringLiteral("frame-blending")) item.sampling = 1;
    else if (sampling == QStringLiteral("optical-flow")) item.sampling = 2;
    else return fail(QStringLiteral("Unsupported time-map frame sampling: %1.").arg(sampling));
    struct Point { double time, value; };
    QList<Point> points;
    while (x.readNextStartElement()) {
        if (x.name() != QStringLiteral("timept")) return fail(QStringLiteral("Unexpected element in a time map."));
        const auto at = x.attributes();
        if (at.value(QStringLiteral("interp")) != QStringLiteral("linear"))
            return fail(QStringLiteral("Smooth XML time-map curves are not supported yet. Export linear time points or render the clip first."));
        Point point;
        if (!preciseFrames(at.value(QStringLiteral("time")).toString(), num, den, &point.time)
            || !preciseFrames(at.value(QStringLiteral("value")).toString(), num, den, &point.value))
            return fail(QStringLiteral("Invalid time-map point."));
        if (std::abs(point.time - std::round(point.time)) > 1e-6)
            return fail(QStringLiteral("A time-map boundary falls between project frames."));
        point.time = std::round(point.time);
        point.value -= item.sourceTimecodeSeconds >= 0 ? item.sourceTimecodeSeconds * num / den
                                                      : qMax<qint64>(0, item.sourceTimecodeFrames);
        if (!points.isEmpty() && point.time <= points.last().time)
            return fail(QStringLiteral("Time-map points must have increasing output times."));
        points.append(point);
        if (points.size() > 4097) return fail(QStringLiteral("The time map has more than 4096 segments."));
        x.skipCurrentElement();
    }
    if (x.hasError()) return fail(x.errorString());
    if (points.size() < 2) return fail(QStringLiteral("A time map needs at least two points."));
    const double outputLength = points.last().time - points.first().time;
    const qint64 first = localStart - qRound64(points.first().time);
    if (outputLength > 100000000 || first < 0 || first + item.duration > outputLength)
        return fail(QStringLiteral("The time map does not cover the clip's visible range."));
    QVariantList segments;
    double largest = 0;
    for (int n = 0; n + 1 < points.size(); ++n) {
        const auto &a = points[n], &b = points[n + 1];
        const int count = qRound(b.time - a.time);
        const double speed = (b.value - a.value) / count;
        segments.append(QVariantMap{{"frames", count}, {"speed", speed}});
        largest = std::max({largest, a.value, b.value});
    }
    const double length = item.sourceLength > 0 ? item.sourceLength : std::max(1., std::ceil(largest));
    RetimeMap map; QString why;
    if (!map.set({{"sourceIn", points.first().value}, {"sourceLength", length}, {"segments", segments}}, &why))
        return fail(why);
    item.sourceLength = qCeil(length);
    item.sourceIn = int(first);
    item.speed = 1.;
    item.retime = map.definition();
    // Keep affine timing compact while carrying explicit picture selection.
    // Dropping Floor here would switch to the legacy producer's nearest frame.
    if (points.size() == 2 && points.first().time == 0 && std::abs(points.first().value) < 1e-7
        && FrameSampling::explicitPictures(item.sampling) && segments.first().toMap().value("speed").toDouble() >= .05) {
        item.speed = segments.first().toMap().value("speed").toDouble();
        item.retime.clear();
    }
    return true;
}
}

bool readFcpxml(const QString &xml, Document *result, QString *error) {
    Document document; Document *doc = &document;
    QXmlStreamReader x(xml);
    struct Asset { QString name, path, start, duration; bool hasVideo = true, hasAudio = true; };
    struct Format { QString duration; int width = 1920, height = 1080; };
    QHash<QString, Asset> assets;
    QHash<QString, Format> formats;
    int fpsNum = 25, fpsDen = 1;
    auto useFormat = [&](const Format &f) {
        QString duration = f.duration;
        if (duration.endsWith(QLatin1Char('s'))) duration.chop(1);
        const auto parts = duration.split(QLatin1Char('/'));
        bool numeratorOk = false, denominatorOk = true;
        const int numerator = parts[0].toInt(&numeratorOk);
        const int denominator = parts.size() == 2 ? parts[1].toInt(&denominatorOk) : 1;
        if (parts.size() > 2 || !numeratorOk || !denominatorOk || numerator <= 0 || denominator <= 0) return false;
        fpsNum = denominator; fpsDen = numerator;
        doc->fpsNum = fpsNum; doc->fpsDen = fpsDen; doc->width = f.width; doc->height = f.height;
        return true;
    };
    Track spineTrack; spineTrack.kind = Track::Video; spineTrack.name = QStringLiteral("V1");
    QHash<int, Track> lanes;
    qint64 tcStart = 0, pendingTransition = 0;
    int spineDepth = 0;
    QString lastAssetId;
    // Store indices, not pointers into growing track lists. Each element owns
    // its markers and time map, including connected clips nested in other clips.
    struct Context { QString tag; int lane, index; qint64 start, localStart, handle; bool aligned, conform = false; };
    QList<Context> contexts;
    auto target = [&]() -> Item * {
        if (contexts.isEmpty() || contexts.last().index < 0) return nullptr;
        const auto &c = contexts.last();
        return c.lane == 0 ? &spineTrack.items[c.index] : &lanes[c.lane].items[c.index];
    };
    auto ensureLane = [&](int lane) -> Track & {
        if (!lanes.contains(lane)) {
            Track t; t.kind = lane < 0 ? Track::Audio : Track::Video;
            t.name = lane < 0 ? QStringLiteral("A%1").arg(-lane) : QStringLiteral("V%1").arg(lane + 1); lanes.insert(lane, t);
        }
        return lanes[lane];
    };
    while (!x.atEnd()) {
        x.readNext();
        if (x.isEndElement()) {
            if (x.name() == QStringLiteral("spine")) --spineDepth;
            if (!contexts.isEmpty() && x.name() == contexts.last().tag) contexts.removeLast();
            if (x.name() == QStringLiteral("asset")) lastAssetId.clear();
            continue;
        }
        if (!x.isStartElement()) continue;
        const QString tag = x.name().toString(); const auto at = x.attributes();
        if (tag == QStringLiteral("format")) {
            Format f{at.value(QStringLiteral("frameDuration")).toString(),
                     at.value(QStringLiteral("width")).toInt(), at.value(QStringLiteral("height")).toInt()};
            formats.insert(at.value(QStringLiteral("id")).toString(), f);
            if (formats.size() == 1) useFormat(f);
        } else if (tag == QStringLiteral("asset")) {
            Asset a; a.name = at.value(QStringLiteral("name")).toString();
            a.start = at.value(QStringLiteral("start")).toString(); a.duration = at.value(QStringLiteral("duration")).toString();
            a.hasVideo = at.value(QStringLiteral("hasVideo")) != QStringLiteral("0"); a.hasAudio = at.value(QStringLiteral("hasAudio")) != QStringLiteral("0");
            if (at.hasAttribute(QStringLiteral("src"))) a.path = QUrl(at.value(QStringLiteral("src")).toString()).toLocalFile();
            lastAssetId = at.value(QStringLiteral("id")).toString(); assets.insert(lastAssetId, a);
        } else if (tag == QStringLiteral("media-rep")) {
            const QString src = QUrl(at.value(QStringLiteral("src")).toString()).toLocalFile();
            if (auto it = assets.find(lastAssetId); it != assets.end() && it->path.isEmpty()) it->path = src;
        } else if (tag == QStringLiteral("project")) {
            if (doc->name.isEmpty()) doc->name = at.value(QStringLiteral("name")).toString();
        } else if (tag == QStringLiteral("sequence")) {
            const QString ref = at.value(QStringLiteral("format")).toString();
            if (formats.contains(ref) && !useFormat(formats.value(ref))) {
                if (error) *error = QStringLiteral("The sequence has an invalid frame duration.");
                return false;
            }
            tcStart = framesOf(at.value(QStringLiteral("tcStart")).toString(), fpsNum, fpsDen); doc->startTimecodeFrames = tcStart;
        } else if (tag == QStringLiteral("spine")) ++spineDepth;
        else if (tag == QStringLiteral("transition")) {
            if (contexts.isEmpty()) pendingTransition = framesOf(at.value(QStringLiteral("duration")).toString(), fpsNum, fpsDen);
        } else if (tag == QStringLiteral("asset-clip") || tag == QStringLiteral("clip") || tag == QStringLiteral("gap")
                   || tag == QStringLiteral("title") || tag == QStringLiteral("video") || tag == QStringLiteral("ref-clip")) {
            if (spineDepth == 0) continue;
            Item item; item.name = at.value(QStringLiteral("name")).toString();
            const int lane = at.value(QStringLiteral("lane")).toInt();
            if (lane != 0) {
                const Item *parent = target();
                if (parent && (!parent->retime.isEmpty() || !qFuzzyCompare(parent->speed, 1.0))) {
                    if (error) *error = QStringLiteral("%1: connected clips anchored to a time map are not supported yet. Render the parent clip first.").arg(parent->name);
                    return false;
                }
            }
            const qint64 offset = framesOf(at.value(QStringLiteral("offset")).toString(), fpsNum, fpsDen);
            const qint64 localStart = framesOf(at.value(QStringLiteral("start")).toString(), fpsNum, fpsDen);
            // Whole frames from the file's rationals: too large for a
            // timeline is refused here, before it is cut down to fit a number.
            const qint64 placed = contexts.isEmpty() ? offset - tcStart : contexts.last().start + offset - contexts.last().localStart;
            const qint64 lasting = framesOf(at.value(QStringLiteral("duration")).toString(), fpsNum, fpsDen);
            if (placed < -100000000 || placed > 100000000 || lasting < -100000000 || lasting > 100000000) {
                if (error) *error = QStringLiteral("%1: its place or length in the file cannot be used on a timeline.").arg(item.name);
                return false;
            }
            item.start = int(placed); item.duration = int(lasting);
            const Asset asset = assets.value(at.value(QStringLiteral("ref")).toString());
            if (tag == QStringLiteral("gap")) item.kind = Item::Gap;
            else if (tag == QStringLiteral("title")) { item.generator = QStringLiteral("title"); item.extras.insert(QStringLiteral("title"), item.name); }
            else if (asset.path.isEmpty()) { item.kind = Item::Gap; doc->notes.append(QStringLiteral("%1: no media file in the XML").arg(item.name)); }
            else {
                item.path = asset.path; if (item.name.isEmpty()) item.name = asset.name;
                item.sourceTimecodeFrames = framesOf(asset.start, fpsNum, fpsDen); item.sourceLength = int(framesOf(asset.duration, fpsNum, fpsDen));
                if (asset.start.isEmpty()) item.sourceTimecodeSeconds = 0.;
                else if (!preciseFrames(asset.start, 1, 1, &item.sourceTimecodeSeconds)) {
                    if (error) *error = QStringLiteral("%1: invalid source timecode.").arg(item.name);
                    return false;
                }
                item.sourceIn = int(localStart - item.sourceTimecodeFrames);
                const QString enable = at.value(QStringLiteral("srcEnable")).toString();
                item.audioOnly = enable == QStringLiteral("audio") || !asset.hasVideo || lane < 0;
                item.videoOnly = enable == QStringLiteral("video") || !asset.hasAudio;
                item.enabled = at.value(QStringLiteral("enabled")) != QStringLiteral("0");
            }
            double preciseStart = 0, preciseDuration = 0;
            const bool aligned = (!at.hasAttribute(QStringLiteral("start"))
                || (preciseFrames(at.value(QStringLiteral("start")).toString(), fpsNum, fpsDen, &preciseStart)
                    && std::abs(preciseStart - localStart) < 1e-6))
                && preciseFrames(at.value(QStringLiteral("duration")).toString(), fpsNum, fpsDen, &preciseDuration)
                && std::abs(preciseDuration - item.duration) < 1e-6;
            Context context{tag, lane, -1, item.start, localStart, 0, aligned};
            if (lane == 0 && spineDepth == 1 && contexts.isEmpty() && item.name != QStringLiteral("Connected")) {
                if (pendingTransition > 0 && !spineTrack.items.isEmpty()) {
                    Item &outgoing = spineTrack.items.last();
                    const int geometric = qMax(0, outgoing.start + outgoing.duration - item.start);
                    if (geometric > 0) {
                        item.start += geometric; item.duration -= geometric; item.sourceIn += geometric;
                        outgoing.transitionOut = geometric; context.handle = geometric;
                    } else outgoing.transitionOut = int(pendingTransition);
                }
                pendingTransition = 0;
                if (item.duration > 0 && item.kind != Item::Gap) { context.index = spineTrack.items.size(); spineTrack.items.append(item); }
            } else if (lane != 0 && item.duration > 0) {
                auto &track = ensureLane(lane); context.index = track.items.size(); track.items.append(item);
            }
            contexts.append(context);
        } else if (tag == QStringLiteral("conform-rate") || tag == QStringLiteral("rateConform")) {
            if (!contexts.isEmpty()) {
                contexts.last().conform = true;
                const Item *item = target();
                if (item && (!item->retime.isEmpty() || !qFuzzyCompare(item->speed, 1.0))) {
                    if (error) *error = QStringLiteral("%1: combined rate conform and time mapping are not supported yet.").arg(item->name);
                    return false;
                }
            }
        } else if (tag == QStringLiteral("timeMap")) {
            Item *item = target();
            if (!item) { if (error) *error = QStringLiteral("A time map belongs to an unsupported clip container."); return false; }
            if (contexts.last().conform) {
                if (error) *error = QStringLiteral("%1: combined rate conform and time mapping are not supported yet.").arg(item->name);
                return false;
            }
            if (!contexts.last().aligned) {
                if (error) *error = QStringLiteral("%1: a mapped clip's trim falls between project frames.").arg(item->name);
                return false;
            }
            if (!readTimeMap(x, *item, contexts.last().localStart + contexts.last().handle, fpsNum, fpsDen, error)) return false;
        } else if (tag == QStringLiteral("marker") || tag == QStringLiteral("chapter-marker")) {
            if (const Item *item = target(); item && !item->retime.isEmpty()) {
                if (error) *error = QStringLiteral("%1: markers on mapped XML clips are not supported yet. Remove those markers or render the clip before importing.").arg(item->name);
                return false;
            }
            if (Item *item = target(); item && item->kind == Item::Clip)
                item->markers.append({int(framesOf(at.value(QStringLiteral("start")).toString(), fpsNum, fpsDen)
                    - qMax<qint64>(0, item->sourceTimecodeFrames)), at.value(QStringLiteral("value")).toString(),
                    at.hasAttribute(QStringLiteral("completed")) ? QStringLiteral("todo") : QString()});
        } else if (tag == QStringLiteral("note")) {
            const QString text = x.readElementText().trimmed();
            if (text.startsWith(QStringLiteral("marker "))) {
                const QStringList parts = text.mid(7).split(QLatin1Char(' '));
                if (parts.size() >= 2) doc->markers.append({int(timecodeFrames(parts.at(0), doc->fps())), parts.mid(2).join(QLatin1Char(' ')), parts.at(1)});
            }
        }
    }
    if (x.hasError()) { if (error) *error = x.errorString(); return false; }
    if (spineTrack.items.isEmpty() && lanes.isEmpty()) { if (error) *error = QStringLiteral("No sequence items found in the FCPXML."); return false; }
    QList<int> laneNumbers = lanes.keys(); std::sort(laneNumbers.begin(), laneNumbers.end());
    QList<Track> video{spineTrack}, audio;
    for (int lane : laneNumbers) if (lane > 0) video.append(lanes.value(lane));
    Track a1; a1.kind = Track::Audio; a1.name = QStringLiteral("A1");
    for (const Item &i : spineTrack.items) if (i.kind == Item::Clip && !i.videoOnly && !i.path.isEmpty()) a1.items.append(i);
    if (!a1.items.isEmpty()) audio.append(a1);
    for (int lane : laneNumbers) if (lane < 0) audio.append(lanes.value(lane));
    doc->tracks = video + audio;
    *result = std::move(document);
    return true;
}

// ── Legacy interchange XML (xmeml version 5) ──────────────────────────────
// The .xml most editors still read: a sequence with video/audio tracks of
// clipitems (record start/end, source in/out, a file with a pathurl and
// start timecode), transitionitems overlapping their neighbours, a Time
// Remap speed filter, clip and sequence markers. Integer frames at the
// sequence timebase; NTSC rates carry the ntsc flag.
namespace {
struct Timebase { int base; bool ntsc; };
Timebase timebaseOf(const Document &doc) {
    const double fps = doc.fps();
    const bool ntsc = qAbs(fps - 23.976) < 0.01 || qAbs(fps - 29.97) < 0.01 || qAbs(fps - 59.94) < 0.01;
    return {qRound(fps), ntsc};
}
void rateElement(QXmlStreamWriter &x, const Timebase &tb) {
    x.writeStartElement(QStringLiteral("rate"));
    x.writeTextElement(QStringLiteral("timebase"), QString::number(tb.base));
    x.writeTextElement(QStringLiteral("ntsc"), tb.ntsc ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
    x.writeEndElement();
}
}

QString writeXmeml(const Document &doc, QString *error) {
    if (requiresMappedTiming(doc, error)) return {};
    QList<Track> video, audio;
    for (const Track &t : doc.tracks) (t.kind == Track::Video ? video : audio).append(t);
    if (video.isEmpty() && audio.isEmpty()) { if (error) *error = QStringLiteral("Nothing to export: the sequence has no tracks."); return {}; }
    const Timebase tb = timebaseOf(doc);
    const double fps = doc.fps();
    QString out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.writeStartDocument();
    x.writeDTD(QStringLiteral("<!DOCTYPE xmeml>"));
    x.writeStartElement(QStringLiteral("xmeml")); x.writeAttribute(QStringLiteral("version"), QStringLiteral("5"));
    x.writeStartElement(QStringLiteral("sequence")); x.writeAttribute(QStringLiteral("id"), QStringLiteral("sequence-1"));
    x.writeTextElement(QStringLiteral("name"), doc.name.isEmpty() ? QStringLiteral("Sequence") : doc.name);
    qint64 total = 0;
    for (const Track &t : doc.tracks) for (const Item &i : t.items) total = qMax<qint64>(total, i.start + i.duration);
    x.writeTextElement(QStringLiteral("duration"), QString::number(total));
    rateElement(x, tb);
    x.writeStartElement(QStringLiteral("timecode")); rateElement(x, tb);
    // Non-drop at every rate, as the screen counts: the file says what the editor saw.
    x.writeTextElement(QStringLiteral("string"), timecode(doc.startTimecodeFrames, fps));
    x.writeTextElement(QStringLiteral("frame"), QString::number(doc.startTimecodeFrames));
    x.writeTextElement(QStringLiteral("displayformat"), QStringLiteral("NDF"));
    x.writeEndElement();
    x.writeStartElement(QStringLiteral("media"));
    QHash<QString, int> fileIds; int clipCounter = 0;
    auto writeClip = [&](const Item &i, bool audioTrack, int trackIndex, int clipIndex, int linkedTrack, int linkedClip) {
        const double a = qAbs(i.speed == 0.0 ? 1.0 : i.speed);
        x.writeStartElement(QStringLiteral("clipitem")); x.writeAttribute(QStringLiteral("id"), QStringLiteral("clipitem-%1").arg(++clipCounter));
        x.writeTextElement(QStringLiteral("name"), i.name.isEmpty() ? QFileInfo(i.path).fileName() : i.name);
        x.writeTextElement(QStringLiteral("enabled"), i.enabled ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
        x.writeTextElement(QStringLiteral("duration"), QString::number(i.sourceLength > 0 ? i.sourceLength : qRound64((i.sourceIn + i.duration) * a)));
        rateElement(x, tb);
        // Record range. The mix region belongs to the outgoing clip; the
        // incoming one starts under it with its head handle (see readXmeml).
        x.writeTextElement(QStringLiteral("start"), QString::number(i.start));
        x.writeTextElement(QStringLiteral("end"), QString::number(i.start + i.duration));
        x.writeTextElement(QStringLiteral("in"), QString::number(qRound64(i.sourceIn * a)));
        x.writeTextElement(QStringLiteral("out"), QString::number(qRound64((i.sourceIn + i.duration) * a)));
        if (!i.path.isEmpty()) {
            const bool first = !fileIds.contains(i.path);
            if (first) fileIds.insert(i.path, fileIds.size() + 1);
            x.writeStartElement(QStringLiteral("file")); x.writeAttribute(QStringLiteral("id"), QStringLiteral("file-%1").arg(fileIds.value(i.path)));
            if (first) {
                x.writeTextElement(QStringLiteral("name"), QFileInfo(i.path).fileName());
                x.writeTextElement(QStringLiteral("pathurl"), QUrl::fromLocalFile(i.path).toString());
                rateElement(x, tb);
                if (i.sourceLength > 0) x.writeTextElement(QStringLiteral("duration"), QString::number(i.sourceLength));
                x.writeStartElement(QStringLiteral("timecode")); rateElement(x, tb);
                x.writeTextElement(QStringLiteral("string"), timecode(qMax<qint64>(0, i.sourceTimecodeFrames), fps));
                x.writeTextElement(QStringLiteral("frame"), QString::number(qMax<qint64>(0, i.sourceTimecodeFrames)));
                x.writeTextElement(QStringLiteral("displayformat"), QStringLiteral("NDF")); x.writeEndElement();
                x.writeStartElement(QStringLiteral("media"));
                if (!i.audioOnly) { x.writeStartElement(QStringLiteral("video")); x.writeStartElement(QStringLiteral("samplecharacteristics")); x.writeTextElement(QStringLiteral("width"), QString::number(doc.width)); x.writeTextElement(QStringLiteral("height"), QString::number(doc.height)); x.writeEndElement(); x.writeEndElement(); }
                if (!i.videoOnly) { x.writeStartElement(QStringLiteral("audio")); x.writeTextElement(QStringLiteral("channelcount"), QStringLiteral("2")); x.writeEndElement(); }
                x.writeEndElement();
            }
            x.writeEndElement();
        }
        if (audioTrack) { x.writeStartElement(QStringLiteral("sourcetrack")); x.writeTextElement(QStringLiteral("mediatype"), QStringLiteral("audio")); x.writeTextElement(QStringLiteral("trackindex"), QStringLiteral("1")); x.writeEndElement(); }
        if (!qFuzzyCompare(a, 1.0) || i.speed < 0) {
            x.writeStartElement(QStringLiteral("filter")); x.writeStartElement(QStringLiteral("effect"));
            x.writeTextElement(QStringLiteral("name"), QStringLiteral("Time Remap")); x.writeTextElement(QStringLiteral("effectid"), QStringLiteral("timeremap"));
            x.writeTextElement(QStringLiteral("effecttype"), QStringLiteral("motion")); x.writeTextElement(QStringLiteral("mediatype"), QStringLiteral("video"));
            x.writeStartElement(QStringLiteral("parameter")); x.writeTextElement(QStringLiteral("parameterid"), QStringLiteral("speed")); x.writeTextElement(QStringLiteral("name"), QStringLiteral("speed"));
            x.writeTextElement(QStringLiteral("value"), QString::number(i.speed * 100.0, 'f', 2)); x.writeEndElement();
            x.writeStartElement(QStringLiteral("parameter")); x.writeTextElement(QStringLiteral("parameterid"), QStringLiteral("reverse")); x.writeTextElement(QStringLiteral("name"), QStringLiteral("reverse"));
            x.writeTextElement(QStringLiteral("value"), i.speed < 0 ? QStringLiteral("TRUE") : QStringLiteral("FALSE")); x.writeEndElement();
            x.writeEndElement(); x.writeEndElement();
        }
        if (linkedTrack > 0) {
            for (int pass = 0; pass < 2; ++pass) {
                x.writeStartElement(QStringLiteral("link"));
                x.writeTextElement(QStringLiteral("mediatype"), pass == 0 ? (audioTrack ? QStringLiteral("audio") : QStringLiteral("video")) : (audioTrack ? QStringLiteral("video") : QStringLiteral("audio")));
                x.writeTextElement(QStringLiteral("trackindex"), QString::number(pass == 0 ? trackIndex : linkedTrack));
                x.writeTextElement(QStringLiteral("clipindex"), QString::number(pass == 0 ? clipIndex : linkedClip));
                x.writeEndElement();
            }
        }
        for (const Marker &m : i.markers) {
            x.writeStartElement(QStringLiteral("marker")); x.writeTextElement(QStringLiteral("name"), m.text);
            x.writeTextElement(QStringLiteral("comment"), m.type); x.writeTextElement(QStringLiteral("in"), QString::number(qRound64(m.frame * a)));
            x.writeTextElement(QStringLiteral("out"), QStringLiteral("-1")); x.writeEndElement();
        }
        x.writeEndElement();
    };
    auto writeTransition = [&](const Item &i) {
        x.writeStartElement(QStringLiteral("transitionitem"));
        rateElement(x, tb);
        x.writeTextElement(QStringLiteral("start"), QString::number(i.start + i.duration - i.transitionOut));
        x.writeTextElement(QStringLiteral("end"), QString::number(i.start + i.duration));
        x.writeTextElement(QStringLiteral("alignment"), QStringLiteral("center"));
        x.writeStartElement(QStringLiteral("effect")); x.writeTextElement(QStringLiteral("name"), QStringLiteral("Cross Dissolve"));
        x.writeTextElement(QStringLiteral("effectid"), QStringLiteral("Cross Dissolve")); x.writeTextElement(QStringLiteral("effecttype"), QStringLiteral("transition"));
        x.writeTextElement(QStringLiteral("mediatype"), QStringLiteral("video")); x.writeEndElement();
        x.writeEndElement();
    };
    // Audio items that mirror a video item link to it (same file, start, length).
    auto mirrorOf = [&](const Item &a, int *vTrack, int *vClip) {
        for (int t = 0; t < video.size(); ++t) for (int c = 0; c < video.at(t).items.size(); ++c) {
            const Item &v = video.at(t).items.at(c);
            if (v.kind == Item::Clip && v.path == a.path && v.start == a.start && v.duration == a.duration && !v.videoOnly) { *vTrack = t + 1; *vClip = c + 1; return true; }
        }
        return false;
    };
    x.writeStartElement(QStringLiteral("video"));
    x.writeStartElement(QStringLiteral("format")); x.writeStartElement(QStringLiteral("samplecharacteristics"));
    x.writeTextElement(QStringLiteral("width"), QString::number(doc.width)); x.writeTextElement(QStringLiteral("height"), QString::number(doc.height));
    rateElement(x, tb); x.writeEndElement(); x.writeEndElement();
    for (int t = 0; t < video.size(); ++t) {
        x.writeStartElement(QStringLiteral("track"));
        for (int c = 0; c < video.at(t).items.size(); ++c) {
            const Item &i = video.at(t).items.at(c);
            if (i.kind == Item::Gap) continue;
            int aTrack = 0, aClip = 0;
            for (int at = 0; at < audio.size() && !aTrack; ++at) for (int ac = 0; ac < audio.at(at).items.size(); ++ac) {
                const Item &a = audio.at(at).items.at(ac);
                if (a.path == i.path && a.start == i.start && a.duration == i.duration && !i.videoOnly) { aTrack = at + 1; aClip = ac + 1; break; }
            }
            writeClip(i, false, t + 1, c + 1, aTrack, aClip);
            if (i.transitionOut > 0 && c + 1 < video.at(t).items.size()) writeTransition(i);
        }
        x.writeTextElement(QStringLiteral("enabled"), video.at(t).disabled ? QStringLiteral("FALSE") : QStringLiteral("TRUE"));
        x.writeTextElement(QStringLiteral("locked"), video.at(t).locked ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
        x.writeEndElement();
    }
    x.writeEndElement(); // video
    x.writeStartElement(QStringLiteral("audio"));
    for (int t = 0; t < audio.size(); ++t) {
        x.writeStartElement(QStringLiteral("track"));
        for (int c = 0; c < audio.at(t).items.size(); ++c) {
            const Item &i = audio.at(t).items.at(c);
            if (i.kind == Item::Gap || i.path.isEmpty()) continue;
            int vTrack = 0, vClip = 0; mirrorOf(i, &vTrack, &vClip);
            writeClip(i, true, t + 1, c + 1, vTrack, vClip);
            if (i.transitionOut > 0 && c + 1 < audio.at(t).items.size()) writeTransition(i);
        }
        x.writeTextElement(QStringLiteral("enabled"), audio.at(t).disabled ? QStringLiteral("FALSE") : QStringLiteral("TRUE"));
        x.writeTextElement(QStringLiteral("locked"), audio.at(t).locked ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
        x.writeEndElement();
    }
    x.writeEndElement(); // audio
    x.writeEndElement(); // media
    for (const Marker &m : doc.markers) {
        x.writeStartElement(QStringLiteral("marker")); x.writeTextElement(QStringLiteral("name"), m.text); x.writeTextElement(QStringLiteral("comment"), m.type);
        x.writeTextElement(QStringLiteral("in"), QString::number(m.frame)); x.writeTextElement(QStringLiteral("out"), QStringLiteral("-1")); x.writeEndElement();
    }
    x.writeEndElement(); // sequence
    x.writeEndElement(); // xmeml
    x.writeEndDocument();
    return out;
}

bool readXmeml(const QString &xml, Document *doc, QString *error) {
    *doc = Document();
    QXmlStreamReader x(xml);
    // Walk the tree into a light DOM: only the elements we use.
    struct Node { QString tag; QString text; QList<Node> children; QString id; };
    std::function<Node(int)> readNode = [&](int depth) -> Node {
        Node n; n.tag = x.name().toString(); n.id = x.attributes().value(QStringLiteral("id")).toString();
        if (depth > 64) { x.skipCurrentElement(); return n; } // hostile nesting must not overflow the stack
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) n.children.append(readNode(depth + 1));
            else if (x.isCharacters() && !x.isWhitespace()) n.text += x.text().toString();
            else if (x.isEndElement()) break;
        }
        return n;
    };
    Node root;
    while (!x.atEnd()) { x.readNext(); if (x.isStartElement()) { root = readNode(0); break; } }
    if (x.hasError()) { if (error) *error = x.errorString(); return false; }
    auto child = [](const Node &n, const QString &tag) -> const Node * { for (const Node &c : n.children) if (c.tag == tag) return &c; return nullptr; };
    auto text = [&](const Node &n, const QString &tag, const QString &def = QString()) { const Node *c = child(n, tag); return c ? c->text.trimmed() : def; };
    std::function<const Node *(const Node &, const QString &)> find = [&](const Node &n, const QString &tag) -> const Node * {
        if (n.tag == tag) return &n;
        for (const Node &c : n.children) if (const Node *f = find(c, tag)) return f;
        return nullptr;
    };
    const Node *sequence = find(root, QStringLiteral("sequence"));
    if (!sequence) { if (error) *error = QStringLiteral("No sequence found in the XML."); return false; }
    doc->name = text(*sequence, QStringLiteral("name"));
    int base = 25; bool ntsc = false;
    if (const Node *rate = child(*sequence, QStringLiteral("rate"))) { base = text(*rate, QStringLiteral("timebase"), QStringLiteral("25")).toInt(); ntsc = text(*rate, QStringLiteral("ntsc")).compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0; }
    if (base <= 0) base = 25;
    doc->fpsNum = ntsc ? base * 1000 : base; doc->fpsDen = ntsc ? 1001 : 1;
    if (const Node *tc = child(*sequence, QStringLiteral("timecode"))) doc->startTimecodeFrames = text(*tc, QStringLiteral("frame"), QStringLiteral("0")).toLongLong();
    const Node *media = child(*sequence, QStringLiteral("media"));
    if (!media) { if (error) *error = QStringLiteral("The sequence has no media."); return false; }
    if (const Node *videoMedia = child(*media, QStringLiteral("video")))
        if (const Node *format = child(*videoMedia, QStringLiteral("format"))) if (const Node *sc = child(*format, QStringLiteral("samplecharacteristics"))) {
            if (!text(*sc, QStringLiteral("width")).isEmpty()) doc->width = text(*sc, QStringLiteral("width")).toInt();
            if (!text(*sc, QStringLiteral("height")).isEmpty()) doc->height = text(*sc, QStringLiteral("height")).toInt();
        }
    QHash<QString, QVariantMap> files; // id → {path, timecode, duration, hasVideo, hasAudio}
    auto readTracks = [&](const Node &parent, Track::Kind kind, const QString &prefix) {
        int n = 0;
        for (const Node &t : parent.children) {
            if (t.tag != QStringLiteral("track")) continue;
            Track track; track.kind = kind; track.name = QStringLiteral("%1%2").arg(prefix).arg(++n);
            track.disabled = text(t, QStringLiteral("enabled")).compare(QStringLiteral("FALSE"), Qt::CaseInsensitive) == 0;
            track.locked = text(t, QStringLiteral("locked")).compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
            int pendingTransition = 0;
            for (const Node &c : t.children) {
                if (c.tag == QStringLiteral("transitionitem")) {
                    pendingTransition = text(c, QStringLiteral("end"), QStringLiteral("0")).toInt() - text(c, QStringLiteral("start"), QStringLiteral("0")).toInt();
                    if (!track.items.isEmpty()) track.items.last().transitionOut = qMax(0, pendingTransition);
                    continue;
                }
                if (c.tag != QStringLiteral("clipitem")) continue;
                Item item;
                item.name = text(c, QStringLiteral("name"));
                item.enabled = text(c, QStringLiteral("enabled"), QStringLiteral("TRUE")).compare(QStringLiteral("FALSE"), Qt::CaseInsensitive) != 0;
                const int start = text(c, QStringLiteral("start"), QStringLiteral("0")).toInt(), end = text(c, QStringLiteral("end"), QStringLiteral("0")).toInt();
                int in = text(c, QStringLiteral("in"), QStringLiteral("0")).toInt();
                if (const Node *f = child(c, QStringLiteral("file"))) {
                    QVariantMap info = files.value(f->id);
                    if (const Node *url = child(*f, QStringLiteral("pathurl"))) {
                        const QUrl u(url->text.trimmed()); info["path"] = u.isLocalFile() ? u.toLocalFile() : url->text.trimmed();
                        if (const Node *tc = child(*f, QStringLiteral("timecode"))) info["timecode"] = text(*tc, QStringLiteral("frame"), QStringLiteral("0")).toLongLong();
                        info["duration"] = text(*f, QStringLiteral("duration"), QStringLiteral("0")).toInt();
                        if (const Node *m = child(*f, QStringLiteral("media"))) { info["hasVideo"] = child(*m, QStringLiteral("video")) != nullptr; info["hasAudio"] = child(*m, QStringLiteral("audio")) != nullptr; }
                        files.insert(f->id, info);
                    }
                    item.path = info.value(QStringLiteral("path")).toString();
                    item.sourceTimecodeFrames = info.value(QStringLiteral("timecode"), 0).toLongLong();
                    item.sourceLength = info.value(QStringLiteral("duration")).toInt();
                    if (info.contains(QStringLiteral("hasAudio")) && !info.value(QStringLiteral("hasAudio")).toBool()) item.videoOnly = true;
                    if (info.contains(QStringLiteral("hasVideo")) && !info.value(QStringLiteral("hasVideo")).toBool()) item.audioOnly = true;
                }
                double speed = 1.0;
                for (const Node &f : c.children) if (f.tag == QStringLiteral("filter")) if (const Node *e = child(f, QStringLiteral("effect")))
                    if (text(*e, QStringLiteral("effectid")) == QStringLiteral("timeremap") || text(*e, QStringLiteral("name")) == QStringLiteral("Time Remap")) {
                        bool reverse = false; double percent = 100.0;
                        for (const Node &p : e->children) if (p.tag == QStringLiteral("parameter")) {
                            const QString pid = text(p, QStringLiteral("parameterid"));
                            if (pid == QStringLiteral("speed")) percent = text(p, QStringLiteral("value"), QStringLiteral("100")).toDouble();
                            if (pid == QStringLiteral("reverse")) reverse = text(p, QStringLiteral("value")).compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
                        }
                        if (std::isfinite(percent) && percent != 0.0) speed = qAbs(percent) / 100.0 * (reverse || percent < 0 ? -1.0 : 1.0);
                    }
                item.speed = speed;
                const double a = qAbs(speed);
                item.start = start; item.duration = end - start;
                item.sourceIn = int(qRound64(in / a));
                for (const Node &m : c.children) if (m.tag == QStringLiteral("marker"))
                    item.markers.append({int(qRound64(text(m, QStringLiteral("in"), QStringLiteral("0")).toInt() / a)), text(m, QStringLiteral("name")), text(m, QStringLiteral("comment"))});
                if (pendingTransition > 0 && !track.items.isEmpty()) {
                    // Overlapping neighbours: the incoming clip's visible range starts where the outgoing one ends.
                    const Item &prev = track.items.last();
                    const int overlap = qMax(0, prev.start + prev.duration - item.start);
                    if (overlap > 0) { item.start += overlap; item.duration -= overlap; item.sourceIn += overlap; track.items.last().transitionOut = overlap; }
                    pendingTransition = 0;
                }
                if (item.path.isEmpty() && item.name.isEmpty()) continue;
                if (item.path.isEmpty()) { item.generator = QStringLiteral("title"); item.extras.insert(QStringLiteral("title"), item.name); }
                if (item.duration > 0) track.items.append(item);
            }
            doc->tracks.append(track);
        }
    };
    if (const Node *videoMedia = child(*media, QStringLiteral("video"))) readTracks(*videoMedia, Track::Video, QStringLiteral("V"));
    if (const Node *audioMedia = child(*media, QStringLiteral("audio"))) readTracks(*audioMedia, Track::Audio, QStringLiteral("A"));
    for (const Node &m : sequence->children) if (m.tag == QStringLiteral("marker"))
        doc->markers.append({text(m, QStringLiteral("in"), QStringLiteral("0")).toInt(), text(m, QStringLiteral("name")), text(m, QStringLiteral("comment"))});
    for (Track &t : doc->tracks) for (Item &i : t.items) if (t.kind == Track::Audio) i.audioOnly = true;
    if (doc->tracks.isEmpty()) { if (error) *error = QStringLiteral("The sequence has no tracks."); return false; }
    return true;
}
} // namespace Interchange
