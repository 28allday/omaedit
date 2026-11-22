#include "previewrendercache.h"
#include "generators.h"
#include "gradeplan.h"
#include "projectfile.h"
#include "timeline.h"
#include "framesampling.h"
#include "editengine.h"
#include "rendersnapshot.h"
#include "colourpipeline.h"
#include "stagetimer.h"
#include "workingframe.h"
#include "viewergpu.h"
#include "renderrequest.h"
#include "cachecodec.h"
#include "decoder.h"
#include "media/mediaprobe.h"
#include "timelinerenderer.h"
#include "effectsteps.h"
#include "engines.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <lz4.h>
#include <unistd.h>
extern "C" {
#include <libavutil/pixdesc.h>
}
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace {
constexpr quint32 magic = 0x4f505231; // OPR1, private session cache
constexpr quint32 codedMagic = 0x4f505631; // OPV1: one frame as hardware video
constexpr qint64 maxFrameBytes = 512LL * 1024 * 1024;
constexpr qint64 freeReserve = 512LL * 1024 * 1024;
using Span = QPair<int, int>; // inclusive

QVariantMap spanMap(int first, int last) { return {{"first", first}, {"last", last}}; }

// Adds a finished frame to the sorted, merged ready spans; frames finish out
// of order when several render at once.
void markReady(QVariantList &ranges, int frame) {
    int i = 0;
    while (i < ranges.size() && ranges[i].toMap().value("last").toInt() < frame - 1) ++i;
    if (i == ranges.size()) { ranges.append(spanMap(frame, frame)); return; }
    auto range = ranges[i].toMap();
    const int first = range.value("first").toInt(), last = range.value("last").toInt();
    if (frame >= first && frame <= last) return;
    if (frame == last + 1) {
        range["last"] = frame;
        if (i + 1 < ranges.size() && ranges[i + 1].toMap().value("first").toInt() == frame + 1) {
            range["last"] = ranges[i + 1].toMap().value("last");
            ranges.removeAt(i + 1);
        }
        ranges[i] = range;
    } else if (frame == first - 1) {
        range["first"] = frame;
        ranges[i] = range;
    } else {
        ranges.insert(i, spanMap(frame, frame));
    }
}
// Renderers working at once, each with its own copy of the timeline and its
// own decoders. Frames with many layers are limited by memory speed, so a few
// are enough, and fewer for big frames: together they decode about as many
// pixels as sixteen HD layers.
int rendererCount(int layers, qint64 pixels) {
    bool set = false;
    const int wanted = qEnvironmentVariableIntValue("OMAEDIT_CACHE_WORKERS", &set);
    if (set)
        return qBound(1, wanted, 16);
    const qint64 budget = 16LL * 1920 * 1080;
    const int fit = int(qBound(qint64(1), budget / qMax(qint64(1), qint64(layers) * pixels), qint64(4)));
    return qBound(1, qMin(QThread::idealThreadCount() / 6, fit), 4);
}
// Video layers of a snapshot: V1 and every lane above it that holds pictures.
int layerCount(const QVariantList &entries) {
    int top = 0;
    for (const auto &v : entries) {
        const auto e = v.toMap();
        if (!e.value("audioOnly").toBool() && !e.value("gap").toBool()) top = qMax(top, e.value("lane").toInt());
    }
    return top + 1;
}

// Include media, proxies, LUTs, stabilizer analyses and file-based OCIO configs,
// including dependencies inside compounds and frozen nested sequences.
void collectPaths(const QVariant &value, QSet<QString> &paths) {
    if (value.metaType().id() == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it.key() == "compoundJson")
                collectPaths(QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant(), paths);
            else collectPaths(it.value(), paths);
        }
    } else if (value.metaType().id() == QMetaType::QVariantList) {
        for (const auto &v : value.toList()) collectPaths(v, paths);
    } else if (value.metaType().id() == QMetaType::QString) {
        const auto path = value.toString();
        if (QDir::isAbsolutePath(path)) paths.insert(path);
    }
}
QVariantMap fileFacts(const QSet<QString> &paths) {
    QVariantMap facts;
    for (const auto &path : paths) {
        const QFileInfo f(path);
        facts[path] = QVariantList{f.exists(), f.size(), f.lastModified().toMSecsSinceEpoch(),
                                  f.metadataChangeTime().toMSecsSinceEpoch(), f.canonicalFilePath()};
    }
    return facts;
}
int trackOf(const QVariantMap &e) { return e.value("track", e.value("lane", 0)).toInt(); }
// A file's picture facts, from the probe the timeline made of it when the
// clip went on (remembered by path, size and date): the file is not opened
// again here, on the thread that serves the viewer. "alpha" is true when the
// picture may hold transparency or it cannot be told.
QVariantMap factsOf(const QString &path) {
    const media::Probe probe = media::probeCached(path);
    if (!probe.valid || !probe.hasVideo || probe.width <= 0 || probe.height <= 0) return {};
    const QString format = probe.metadata.value(QStringLiteral("pixelFormat")).toString();
    const auto *desc = format.isEmpty() ? nullptr : av_pix_fmt_desc_get(av_get_pix_fmt(format.toLatin1().constData()));
    bool alpha = !desc || (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
    // VP8 and VP9 carry theirs beside a plain pixel format, marked in the stream's tags.
    const QVariantMap tags = probe.metadata.value(QStringLiteral("containerTags")).toMap();
    for (auto it = tags.cbegin(); !alpha && it != tags.cend(); ++it) alpha = it.key().endsWith(QLatin1String(" alpha_mode"));
    return {{"width", probe.width}, {"height", probe.height},
            {"pixelAspect", probe.sarDen > 0 && probe.sarNum > 0 ? double(probe.sarNum) / probe.sarDen : 1.0}, {"alpha", alpha}};
}
// Whether the clip hides everything under it on every one of its frames: a
// solid picture, left where it is, fully opaque and blending normally, whose
// shape is the frame's. (Moved or animated clips are decided frame by frame
// when drawn; here they count as not hiding, which only renders a little more.)
bool hidesAlways(const QVariantMap &e, const QVariantMap &profile) {
    const int width = profile.value("width").toInt(), height = profile.value("height").toInt();
    if (width <= 0 || height <= 0 || !render::solidPicture(e)) return false;
    const auto off = e.value("bypass").toStringList();
    const auto differs = [&](const char *key, double neutral = 0.) {
        return std::abs(e.value(key, neutral).toDouble() - neutral) > 0.0000001;
    };
    if (!e.value("editKeys").toMap().isEmpty() || !e.value("opacityKf").toList().isEmpty()) return false;
    if (differs("fadeIn") || differs("fadeOut") || differs("mixIn") || differs("mixOut")) return false;
    if (!off.contains("opacity") && differs("opacity", 1)) return false;
    if (!off.contains("blend") && e.value("blend").toInt() != 0) return false;
    if (!off.contains("crop") && (differs("cropL") || differs("cropR") || differs("cropT") || differs("cropB"))) return false;
    if (!off.contains("rect") && (differs("rectX") || differs("rectY") || differs("rectW", 1) || differs("rectH", 1) || differs("rotation")))
        return false;
    const QString path = e.value("path").toString();
    if (path.startsWith(QLatin1String("color:"))) return true;
    const QVariantMap facts = factsOf(path);
    if (facts.isEmpty() || facts.value("alpha", true).toBool()) return false;
    const int sarNum = profile.value("sarNum", 1).toInt(), sarDen = profile.value("sarDen", 1).toInt();
    const double frameAspect = sarDen > 0 && sarNum > 0 ? double(sarNum) / sarDen : 1.;
    const double shape = facts.value("width").toDouble() * facts.value("pixelAspect", 1.).toDouble() / facts.value("height").toDouble();
    return std::abs(shape / frameAspect - double(width) / height) * height < 0.5;  // within half a pixel of the frame's shape
}
bool affected(const QVariantMap &e, bool upperAndNotHiding) {
    if (e.value("gap").toBool() || e.value("disabled").toBool() || e.value("audioOnly").toBool()) return false;
    if (!e.value("title").toString().isEmpty() || e.contains("compoundJson")
        || ProjectFile::isProject(e.value("path").toString())
        || upperAndNotHiding || e.value("adjust").toBool()) return true;
    const auto off = e.value("bypass").toStringList();
    const auto differs = [&](const char *key, double neutral = 0.) {
        return std::abs(e.value(key, neutral).toDouble() - neutral) > 0.0000001;
    };
    const auto grade = e.value("grade").toMap();
    const auto management = grade.value("management").toMap();
    if (!off.contains("grade") && (!Timeline::gradeIsIdentity(grade)
        || (management.value("engine") == "ocio" && management.value("enabled").toBool()))) return true;
    if (!off.contains("color") && (differs("colorB") || differs("colorC", 1) || differs("colorS", 1))) return true;
    if (!off.contains("crop") && (differs("cropL") || differs("cropR") || differs("cropT") || differs("cropB") || differs("cropFeather") || e.value("cropInvert").toBool())) return true;
    if (!off.contains("rect") && (differs("rectX") || differs("rectY") || differs("rectW", 1) || differs("rectH", 1) || differs("rotation"))) return true;
    if (!off.contains("flip") && (e.value("flipH").toBool() || e.value("flipV").toBool())) return true;
    if (!off.contains("lens") && (differs("lensK1") || differs("lensK2"))) return true;
    if (!off.contains("stab") && !e.value("stab").toMap().isEmpty()) return true;
    if (!off.contains("opacity") && (differs("opacity", 1) || !e.value("opacityKf").toList().isEmpty())) return true;
    const auto source = e.value("sourceMeta").toMap();
    return differs("fadeIn") || differs("fadeOut") || differs("mixIn") || differs("mixOut")
        || differs("speed", 1) || !e.value("editKeys").toMap().isEmpty()
        || !source.value("inputLut").toString().isEmpty() || source.value("colorManaged").toBool();
}
}

QVariantList PreviewRenderCache::affectedRanges(const QVariantList &entries, int frames, const QVariantList &tracks,
                                                const QVariantMap &profile) {
    // What hides what: a clip's frames under a hiding clip on a higher track
    // are never drawn.
    QList<QPair<int, Span>> hiding;
    for (const auto &v : entries) {
        const auto e = v.toMap();
        if (render::pictureHidden(e, tracks) || !hidesAlways(e, profile)) continue;
        const int start = e.value("start").toInt();
        hiding.append({trackOf(e), Span{start, start + e.value("frames").toInt() - 1}});
    }
    QList<Span> spans;
    for (const auto &v : entries) {
        const auto e = v.toMap();
        if (render::pictureHidden(e, tracks)) continue;
        const int track = trackOf(e);
        if (!affected(e, track > 0 && !hidesAlways(e, profile))) continue;
        // Incoming dissolve handles precede the incoming clip's visible start.
        const int first = qMax(0, e.value("start").toInt() - e.value("mixIn").toInt());
        const int last = int(qMin<qint64>(frames - 1, qint64(e.value("start").toInt()) + e.value("frames").toInt() - 1));
        if (last < first) continue;
        QList<Span> visible{{first, last}};
        for (const auto &[over, cover] : hiding) {
            if (over <= track) continue;
            QList<Span> left;
            for (const Span &s : visible) {
                if (cover.second < s.first || cover.first > s.second) { left.append(s); continue; }
                if (s.first < cover.first) left.append({s.first, cover.first - 1});
                if (s.second > cover.second) left.append({cover.second + 1, s.second});
            }
            visible = left;
        }
        spans += visible;
    }
    std::sort(spans.begin(), spans.end());
    QList<Span> merged;
    for (const auto &s : spans) {
        if (!merged.isEmpty() && qint64(s.first) <= qint64(merged.last().second) + 1)
            merged.last().second = qMax(merged.last().second, s.second);
        else merged.append(s);
    }
    QVariantList result;
    for (const auto &s : merged) result.append(spanMap(s.first, s.second));
    return result;
}

bool PreviewRenderCache::writeFrame(const QString &path, const QImage &image, QString *error) {
    if (error) error->clear();
    const qint64 bytes = image.sizeInBytes();
    if (image.isNull() || bytes <= 0 || bytes > maxFrameBytes
        || (image.format() != QImage::Format_RGBA8888 && image.format() != QImage::Format_RGBA32FPx4)) {
        if (error) *error = tr("The picture format cannot be cached.");
        return false;
    }
    QVariantMap tags;
    for (const auto &key : image.textKeys()) tags[key] = image.text(key);
    const QByteArray metadata = QJsonDocument::fromVariant(tags).toJson(QJsonDocument::Compact);
    QByteArray packed(LZ4_compressBound(int(bytes)), Qt::Uninitialized);
    const int size = LZ4_compress_default(reinterpret_cast<const char *>(image.constBits()), packed.data(), int(bytes), packed.size());
    QSaveFile file(path);
    if (size <= 0 || !file.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Could not create a playback cache frame.");
        return false;
    }
    QDataStream stream(&file);
    stream << magic << qint32(image.width()) << qint32(image.height()) << qint32(image.format())
           << qint32(image.bytesPerLine()) << qint32(bytes) << qint32(size) << qint32(metadata.size());
    const bool ok = stream.status() == QDataStream::Ok
        && file.write(metadata) == metadata.size() && file.write(packed.constData(), size) == size && file.commit();
    if (!ok && error) *error = tr("Could not finish a playback cache frame. Check free disk space.");
    return ok;
}
QImage PreviewRenderCache::readFrame(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&file);
    quint32 signature;
    qint32 w, h, format, stride, bytes, packed, tags;
    stream >> signature >> w >> h >> format >> stride >> bytes >> packed >> tags;
    if (stream.status() != QDataStream::Ok || signature != magic || w < 1 || h < 1 || w > 8192 || h > 8192
        || (format != QImage::Format_RGBA8888 && format != QImage::Format_RGBA32FPx4)
        || bytes <= 0 || bytes > maxFrameBytes || stride != w * (format == QImage::Format_RGBA8888 ? 4 : 16)
        || qint64(stride) * h != bytes || tags < 0 || tags > 65536 || packed <= 0
        || packed > LZ4_compressBound(bytes) || file.bytesAvailable() != qint64(tags) + packed) return {};
    QJsonParseError error;
    const auto metadata = QJsonDocument::fromJson(file.read(tags), &error);
    if (error.error != QJsonParseError::NoError || !metadata.isObject()) return {};
    const auto input = file.read(packed);
    QImage image(w, h, QImage::Format(format));
    if (image.isNull() || input.size() != packed
        || LZ4_decompress_safe(input.constData(), reinterpret_cast<char *>(image.bits()), packed, bytes) != bytes) return {};
    const auto values = metadata.toVariant().toMap();
    for (auto it = values.begin(); it != values.end(); ++it) image.setText(it.key(), it.value().toString());
    return image;
}

namespace {
// ── Frames a new render can take over from the last one ────────────────────
// An edit starts the background render again, but most edits leave most
// pictures as they were: a cut changes none at all. Each frame is given a
// mark that says which clips can reach it and where in each it falls; a
// frame of the new timeline whose mark a rendered frame of the old one bears
// is that picture, wherever on the timeline it now sits.

// What a clip's entry says that no picture depends on.
const QSet<QString> &pictureBlind() {
    static const QSet<QString> keys{"index", "gapBefore", "volume", "pan", "eq", "volumeKf", "audioMap", "normalize",
                                    "audioCurve", "link", "label", "category", "markers", "name", "original",
                                    "silent", "clipId", "strip", "wave", "thumb",
                                    // (That a clip is a piece of a multicam clip, and of which: the
                                    // picture is the angle's own file and frames, which are said.)
                                    "mcam", "mcamAngle"};
    return keys;
}
// Whether anything in a clip's look is tied to the clip's own first frame
// or length, so that a cut changes what a frame shows. (Fades, a dissolve and
// opacity are not here: each changes only the frames it covers, and is
// marked on those frames alone, in frameMarks.)
bool tiedToItsEdges(const QVariantMap &e) {
    if (!e.value("editKeys").toMap().isEmpty()
        || render::effectsAnimated(e.value("videoEffects").toList())
        || FrameSampling::interpolates(e.value("frameBlend").toInt()) || !e.value("stab").toMap().isEmpty() || !e.value("title").toString().isEmpty()
        || e.contains("compoundJson") || e.contains("retime") || e.contains("audioTiming"))
        return true;
    // A countdown counts to the clip's end and noise is drawn for the clip's
    // own frame: neither picture is the same when the clip's edges move.
    if (const QString kind = render::generatorKind(e.value("path").toString()); kind == QLatin1String("count") || kind == QLatin1String("noise"))
        return true;
    // A grade that changes as the clip plays, as the renderer itself asks it
    // (keys and tracked windows anywhere in the node graph).
    const auto timed = [](const QVariantMap &grade) { return render::gradeAnimated(grade.value("nodes").toList()); };
    if (timed(e.value("grade").toMap())) return true;
    for (const auto &v : e.value("gradeVersions").toList())
        if (timed(v.toMap().value("grade").toMap())) return true;
    return false;
}
struct Reach {
    int first = 0, last = -1;  // the timeline frames the clip is drawn on
    int length = 0, in = 0;    // how many those are, and the source frame of the first
    bool bySource = false;     // marked by the source frame, so both halves of a cut match the whole
    QByteArray key;            // everything about the clip that every one of its frames depends on
    QList<int> clocks;         // each running effect's own time at the clip's first frame
    // What only some of its frames depend on, as the renderer applies them
    // (TimelineRenderer's `placed` and `sequence`).
    int fadeIn = 0, fadeOut = 0;  // fades from and to nothing at its ends
    int mixIn = 0, mixOut = 0;    // the frames it shares with the clip before, and after
    QByteArray mixStyle;          // how it gives way to the clip after
    bool seeThrough = false;      // an opacity is in force
    double opacity = 1;
    QVariantList opacityKeys;
};
QList<Reach> reaches(const QVariantList &entries) {
    QList<Reach> out;
    for (const auto &v : entries) {
        QVariantMap e = v.toMap();
        if (e.value("audioOnly").toBool() || e.value("gap").toBool()) continue;
        Reach r;
        r.in = e.value("in").toInt();
        r.fadeIn = e.value("fadeIn").toInt();
        r.fadeOut = e.value("fadeOut").toInt();
        // A dissolve is the only way a clip is drawn on frames before its
        // start: the clip after is drawn through the frames the two share,
        // which are the last frames of the clip before.
        r.mixIn = qMax(0, e.value("mixIn").toInt());
        r.mixOut = qMax(0, e.value("mixOut").toInt());
        r.mixStyle = e.value("mixStyle").toString().toUtf8();
        r.first = e.value("start").toInt() - r.mixIn;
        r.length = e.value("frames").toInt() + r.mixIn;
        r.last = r.first + r.length - 1;
        r.seeThrough = !e.value("bypass").toStringList().contains(QStringLiteral("opacity"));
        r.opacity = e.value("opacity", 1.).toDouble();
        r.opacityKeys = e.value("opacityKf").toList();
        r.bySource = !tiedToItsEdges(e);
        for (const QString &key : pictureBlind()) e.remove(key);
        // Where the clip sits, and what is marked frame by frame. (How a
        // dissolve was lined up and what it took from each clip's handles are
        // the editor's record of the edit; the picture is in the clips' frames.)
        for (const char *key : {"start", "fadeIn", "fadeOut", "mixIn", "mixOut", "mixStyle", "mixAlign", "mixExtA", "mixExtB",
                                "opacity", "opacityKf"})
            e.remove(QLatin1String(key));
        if (r.bySource) {
            for (const char *key : {"in", "out", "frames"}) e.remove(QLatin1String(key));
            // An effect that changes as the clip plays keeps its own clock; a
            // cut moves the clock on by what the left half used.
            QVariantList effects = e.value("videoEffects").toList();
            for (QVariant &effect : effects) {
                auto state = effect.toMap();
                r.clocks.append(state.value("clock").toInt());
                state.remove("clock");
                effect = state;
            }
            if (!effects.isEmpty()) e["videoEffects"] = effects;
        }
        r.key = QCryptographicHash::hash(QJsonDocument::fromVariant(e).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
        out.append(r);
    }
    return out;
}
// The mark of every frame in `wanted` (inclusive spans): for each clip drawn
// on the frame, what that one picture of it depends on and nothing more, so
// that an edit to part of a clip changes the marks of that part alone.
// (What one clip adds to the mark of frame `f`, when it is drawn there.)
void markFrame(const Reach &r, int f, QByteArray &mark) {
    const auto number = [](double value) { return QByteArray::number(value, 'g', 17); };
    if (f < r.first || f > r.last) return;
    const int local = f - r.first;            // the clip's own frame, as the renderer counts it
    const int fromEnd = r.length - 1 - local;
    mark += r.key;
    if (r.bySource) {
        mark += 's' + QByteArray::number(r.in + local);
        for (const int clock : r.clocks) mark += ',' + QByteArray::number(clock + local);
        // The two frames at either end of a clip may lean on a neighbour
        // that a cut takes away: they match only the same end.
        if (local < 2) mark += "<" + QByteArray::number(local);
        if (fromEnd < 2) mark += ">" + QByteArray::number(fromEnd);
    } else {
        // (The clip's length is in its key; with the frames it is
        // drawn before its start, that is every frame it has.)
        mark += QByteArray::number(local) + 'm' + QByteArray::number(r.mixIn);
    }
    // A fade is under full strength on its first frames (or last):
    // only those are other pictures for it.
    if (r.fadeIn > 0 && local < qMax(1, r.fadeIn - 1))
        mark += 'i' + QByteArray::number(local) + '/' + QByteArray::number(r.fadeIn);
    if (r.fadeOut > 0 && fromEnd < qMax(1, r.fadeOut - 1))
        mark += 'o' + QByteArray::number(fromEnd) + '/' + QByteArray::number(r.fadeOut);
    // The frames two clips share: how far through the dissolve each
    // is, and its shape (which the clip before names).
    if (local < r.mixIn) mark += 'x' + QByteArray::number(local) + '/' + QByteArray::number(r.mixIn);
    if (fromEnd < r.mixOut)
        mark += 'y' + QByteArray::number(fromEnd) + '/' + QByteArray::number(r.mixOut) + ':' + r.mixStyle.toHex();
    // Opacity as it stands on this frame (a ramp of keys is a fade too).
    if (r.seeThrough) {
        const double alpha = std::clamp(EditEngine::valueAt(r.opacityKeys, local, r.opacity, "alpha"), 0., 1.);
        if (alpha != 1.) mark += 'a' + number(alpha);
    }
    mark += '|';
}
QHash<int, QByteArray> frameMarks(const QVariantList &entries, const QList<Span> &wanted) {
    QHash<int, QByteArray> marks;
    for (const Span &span : wanted)
        for (int f = span.first; f <= span.second; ++f) marks.insert(f, QByteArray());
    for (const Reach &r : reaches(entries))
        for (auto it = marks.begin(); it != marks.end(); ++it) markFrame(r, it.key(), it.value());
    return marks;
}
// Everything but the clips that a frame's picture depends on.
QByteArray settingsMark(const QVariantMap &snapshot, const QVariantMap &facts, const QVariantMap &processing) {
    QVariantList tracks;
    for (const auto &v : snapshot.value("tracks").toList()) {
        auto track = v.toMap();
        track.remove("clips");
        track.remove("mix");
        track.remove("name");
        tracks.append(track);
    }
    const QVariantMap all{{"profile", snapshot.value("profile")}, {"project", snapshot.value("project")}, {"tracks", tracks},
                          {"nested", snapshot.value("nested")}, {"mediaSources", snapshot.value("mediaSources")},
                          {"proxyMode", snapshot.value("proxyMode")}, {"facts", facts}, {"preference", processing.value("preference")}};
    return QCryptographicHash::hash(QJsonDocument::fromVariant(all).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
}
}

QByteArray PreviewRenderCache::settingsKey(const QVariantMap &snapshot, const QString &drawnBy) {
    return settingsMark(snapshot, RenderSnapshot::filesNow(snapshot), {{"preference", drawnBy}});
}
std::function<QByteArray(int)> PreviewRenderCache::frameKeys(const QVariantList &entries) {
    const auto clips = std::make_shared<const QList<Reach>>(reaches(entries));
    return [clips](int frame) {
        QByteArray mark;
        for (const Reach &r : *clips) markFrame(r, frame, mark);
        return mark;
    };
}

bool PreviewRenderCache::samePictures(const QVariantMap &a, const QVariantMap &b) {
    for (const char *key : {"profile", "project", "mediaSources", "proxyMode", "frames"})
        if (a.value(QLatin1String(key)) != b.value(QLatin1String(key))) return false;
    // Of a track's header, only its kind and "disabled" reach a picture.
    const QVariantList tracksA = a.value("tracks").toList(), tracksB = b.value("tracks").toList();
    if (tracksA.size() != tracksB.size()) return false;
    for (int i = 0; i < tracksA.size(); ++i) {
        const QVariantMap ta = tracksA.at(i).toMap(), tb = tracksB.at(i).toMap();
        if (ta.value("audio") != tb.value("audio") || ta.value("disabled") != tb.value("disabled")) return false;
    }
    // The clips with a picture of their own (as reaches()), in order.
    const auto pictured = [](const QVariantList &entries) {
        QList<QVariantMap> out;
        for (const auto &v : entries) {
            const QVariantMap e = v.toMap();
            if (!e.value("audioOnly").toBool() && !e.value("gap").toBool()) out.append(e);
        }
        return out;
    };
    if (a.value("entries") == b.value("entries")) return true;
    const QList<QVariantMap> clipsA = pictured(a.value("entries").toList()), clipsB = pictured(b.value("entries").toList());
    if (clipsA.size() != clipsB.size()) return false;
    for (int i = 0; i < clipsA.size(); ++i) {
        if (clipsA.at(i) == clipsB.at(i)) continue;
        QVariantMap ea = clipsA.at(i), eb = clipsB.at(i);
        for (const QString &key : pictureBlind()) { ea.remove(key); eb.remove(key); }
        if (ea != eb) return false;
    }
    return true;
}

namespace {
QList<Span> spansOf(const QVariantList &ranges) {
    QList<Span> spans;
    for (const auto &v : ranges) spans.append({v.toMap().value("first").toInt(), v.toMap().value("last").toInt()});
    return spans;
}
}

struct PreviewRenderCache::Job {
    std::atomic_bool cancelled{false};
    mutable std::mutex mutex;
    QString id = "preview:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap snapshot, processing;
    // What the render depends on (media, proxies, LUTs, analyses) and how
    // each file was when the render began: read and watched off the owner's
    // thread (`watch`), under `mutex`.
    QVariantMap facts;
    bool factsRead = false;
    QSet<QString> paths;
    QString root, state = "running", error;
    qint64 limit = 0, bytes = 0;
    int playhead = 0, total = 0;
    QVariantList ranges, readyRanges;
    QMap<int, QString> ready;
    QHash<int, float> madeMs;  // how long each frame took to make, where timed
    std::unique_ptr<QLockFile> lease;
    std::unique_ptr<QTemporaryDir> directory;
    // Frames kept as hardware video when this machine has a codec for it.
    render::cachecodec::Choice codec;
    QVariantMap frameTags;  // what a working frame read back carries
    // The render this one replaced, until its finished frames have been looked
    // through for pictures that are still right.
    std::shared_ptr<Job> previous;
    int carried = 0;
    ~Job() {
        if (!directory) return;
        QLockFile sweep(QDir(root).filePath(".sessions.lock"));
        sweep.setStaleLockTime(0);
        sweep.tryLock(1000);
        lease.reset();
        directory.reset();
    }
};

struct PreviewRenderCache::Coded {
    std::map<QByteArray, std::unique_ptr<render::cachecodec::Reader>> readers;  // by choice, codec and parameters
    std::unique_ptr<engine::Engine> engine;
    int source = 0;
    std::shared_ptr<const engine::ColourProgram> program;
    render::cachecodec::Planes planes;
    // shown(): a lossless frame's source, the picture drawn at the viewer's
    // size, and the display it was last given.
    int plain = 0, viewer = 0;
    std::shared_ptr<const engine::ColourProgram> display;
    QVariantMap displayOf;
    std::vector<QImage> spares;
    ~Coded() {
        if (!engine) return;
        for (int id : {source, plain, viewer}) if (id) engine->destroySource(id);
    }
};

PreviewRenderCache::PreviewRenderCache(QObject *parent) : QObject(parent) {
    m_poll.setParent(this);
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, &PreviewRenderCache::poll);
}
PreviewRenderCache::~PreviewRenderCache() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        if (m_current) m_current->cancelled = true;
        m_current.reset();
        m_last.reset();
        m_watched.reset();
        m_pending.reset();
    }
    m_wake.notify_all();
    if (m_thread) m_thread->wait();
    if (m_watcher) m_watcher->wait();
}
// A render that is over leaves with the worker thread, which removes its
// folder of frames: never on the caller's thread, which serves the viewer.
void PreviewRenderCache::retire(std::shared_ptr<Job> job) {
    if (!job || !m_thread) return;
    {
        std::lock_guard lock(m_mutex);
        m_retired.push_back(std::move(job));
    }
    m_wake.notify_all();
}
void PreviewRenderCache::invalidate() {
    if (m_current) {
        auto row = status();
        m_current->cancelled = true;
        row["state"] = "cancelled";
        row["canCancel"] = row["canPause"] = row["canResume"] = false;
        row["canRetry"] = false;
        row["readyRanges"] = QVariantList();
        emit changed(row);
    }
    if (m_current) {
        // Held for the render that follows (with whatever it was itself still
        // holding): most edits leave most of its pictures as they were.
        retire(std::move(m_last));
        m_last = std::move(m_current);
    }
    m_current.reset();
    {
        std::lock_guard lock(m_mutex);
        m_watched.reset();
        m_pending.reset();
    }
    m_wake.notify_all();
    m_poll.stop();
    m_lastStatus.clear();
    emit changed(status());
}
void PreviewRenderCache::clear() {
    invalidate();
    retire(std::move(m_last));
    m_last.reset();
}
void PreviewRenderCache::begin(const QVariantMap &snapshot, int playhead, const QString &root, qint64 byteLimit, const QString &processing) {
    invalidate();
    std::shared_ptr<Job> last = std::move(m_last); // its frames may still be right for this timeline
    m_last.reset();
    const auto ranges = affectedRanges(snapshot.value("entries").toList(), snapshot.value("frames").toInt(),
                                       snapshot.value("tracks").toList(), snapshot.value("profile").toMap());
    if (ranges.isEmpty()) { retire(std::move(last)); return; }
    auto job = std::make_shared<Job>();
    job->snapshot = snapshot;
    job->processing = {{"preference", processing == "cpu" ? "cpu" : "auto"}, {"backend", "cpu"},
        {"gpuConversions", 0}, {"cpuConversions", 0},
        {"reason", processing == "cpu" ? tr("CPU selected for cache colour conversion.") : tr("No supported colour conversion has been measured yet.")}};
    job->ranges = ranges;
    job->root = root;
    job->playhead = qMax(0, playhead);
    job->limit = qMax<qint64>(0, byteLimit);
    for (const auto &v : ranges) job->total += v.toMap().value("last").toInt() - v.toMap().value("first").toInt() + 1;
    collectPaths(snapshot, job->paths);
    if (qEnvironmentVariable("OMAEDIT_CACHE_CARRY") != QLatin1String("0")) job->previous = last;
    else retire(std::move(last));
    last.reset();
    m_current = job;
    {
        std::lock_guard lock(m_mutex);
        m_watched = job;
        m_watchNow = true;
        m_pending = std::move(job);
    }
    if (!m_thread) {
        m_thread.reset(QThread::create([this] { run(); }));
        m_thread->start(QThread::LowPriority);
    }
    if (!m_watcher) {
        m_watcher.reset(QThread::create([this] { watch(); }));
        m_watcher->start(QThread::LowPriority);
    }
    m_wake.notify_all();
    m_poll.start();
    poll();
}
void PreviewRenderCache::setBusy(bool busy) {
    { std::lock_guard lock(m_mutex); m_busy = busy; }
    m_wake.notify_all();
    poll();
}
void PreviewRenderCache::act(const QString &action) {
    if (action == "cancel" && m_current) {
        m_current->cancelled = true;
        std::lock_guard lock(m_current->mutex);
        m_current->state = "cancelled";
    } else if (action == "pause" || action == "resume") {
        std::lock_guard lock(m_mutex);
        m_paused = action == "pause";
    }
    m_wake.notify_all();
    poll();
}
QVariantMap PreviewRenderCache::status() const {
    if (!m_current) return {{"state", "idle"}, {"percent", -1}, {"ready", 0}, {"total", 0},
                           {"ranges", QVariantList()}, {"readyRanges", QVariantList()}};
    const auto job = m_current;
    std::lock_guard lock(job->mutex);
    const bool active = job->state == "running";
    const QString state = active && m_paused ? "paused" : active && m_busy ? "waiting" : job->state;
    return {{"id", job->id}, {"name", tr("Background playback render")}, {"type", tr("Render cache")},
            {"state", state}, {"percent", job->total ? 100 * job->ready.size() / job->total : 100},
            {"ready", job->ready.size()}, {"total", job->total}, {"bytes", job->bytes},
            {"carried", job->carried}, {"processing", job->processing},
            {"ranges", job->ranges}, {"readyRanges", job->readyRanges}, {"cache", true},
            {"announce", state == "failed" || state == "partial"},
            {"canCancel", active}, {"canPause", active && !m_paused}, {"canResume", active && m_paused},
            {"canRetry", !active && state != "completed"},
            {"message", job->error.isEmpty() ? tr("%1 of %2 frames ready for playback").arg(job->ready.size()).arg(job->total) : job->error}};
}
// How the files a job depends on are now; the first reading is what the job
// was rendered from. True when they are no longer as that reading found them.
bool PreviewRenderCache::dependenciesMoved(Job &job) {
    const QVariantMap now = fileFacts(job.paths);
    std::lock_guard lock(job.mutex);
    if (!job.factsRead) {
        job.facts = now;
        job.factsRead = true;
        return false;
    }
    return now != job.facts;
}
// Four times a second, on a thread of its own: every file the current render
// depends on is looked at (several questions of the file system each, which
// a slow or sleeping disk can take its time over), and the owner told when
// one has changed. The owner's thread serves the viewer and asks nothing.
void PreviewRenderCache::watch() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(m_mutex);
            // (At once for a render just begun: its files are read as they
            // were when it began, before anything can change them.)
            m_wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return m_stopping || m_watchNow; });
            if (m_stopping) return;
            m_watchNow = false;
            job = m_watched;
        }
        if (!job) continue;
        if (dependenciesMoved(*job)) {
            {
                std::lock_guard lock(m_mutex);
                if (m_watched == job) m_watched.reset();  // said once
            }
            // (Known to the owner by its address only: the render itself is
            // let go of here and by the worker, never on the owner's thread.)
            QMetaObject::invokeMethod(this, [this, which = job.get()] {
                if (m_current.get() != which) return;
                invalidate();
                emit dependenciesChanged();
            }, Qt::QueuedConnection);
        }
        // (If this is the last hold on a render that is over, its folder goes here.)
    }
}
void PreviewRenderCache::poll() {
    const auto row = status();
    if (row != m_lastStatus) { m_lastStatus = row; emit changed(row); }
}
QImage PreviewRenderCache::image(int frame) {
    const auto job = m_current;
    if (!job) return {};
    QString path;
    { std::lock_guard lock(job->mutex); path = job->ready.value(frame); }
    if (path.isEmpty()) return {};
    const bool coded = path.endsWith(QLatin1String(".vpk"));
    m_readerUnavailable = false;
    auto picture = coded ? readCoded(path) : readFrame(path);
    if (!picture.isNull()) {
        m_readerFailures = 0;
        return picture;
    }
    // The frame's file is as it was written, but what reads it back would not
    // start just now (a hardware decoder busy elsewhere): the caller draws
    // this frame live and the finished frames stay. If it keeps failing, the
    // render starts again with frames any machine can read.
    if (coded && m_readerUnavailable && ++m_readerFailures < 3) return {};
    if (coded && m_readerUnavailable) m_codecDistrusted = true;
    m_readerFailures = 0;
    // A wiped/truncated cache is disposable; the caller decodes the source.
    // (None of its frames is offered to the next render.)
    clear();
    emit dependenciesChanged();
    return picture;
}

QImage PreviewRenderCache::readCoded(const QString &path) {
    std::lock_guard lock(m_codedMutex);
    // An engine that lost its GPU never draws again: dropped with all that
    // was made on it, and the next frame makes another.
    if (m_coded && render::engineLost(m_coded->engine.get())) m_coded.reset();
    int w = 0, h = 0;
    QVariantMap values;
    if (!loadCoded(path, &w, &h, &values)) return {};
    Coded &c = *m_coded;
    QImage image;
    const bool read = c.engine->readLinearInto(c.source, [&image](int width, int height) -> float * {
        image = QImage(width, height, QImage::Format_RGBA32FPx4);
        return image.isNull() || image.bytesPerLine() != qsizetype(width) * 16 ? nullptr : reinterpret_cast<float *>(image.bits());
    });
    if (!read) return {};
    for (auto it = values.begin(); it != values.end(); ++it) image.setText(it.key(), it.value().toString());
    return image;
}

bool PreviewRenderCache::loadCoded(const QString &path, int *width, int *height, QVariantMap *tagsOut) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&file);
    quint32 signature = 0;
    qint32 codecId = 0, w = 0, h = 0;
    QByteArray choice, extradata, tags, packet;
    stream >> signature >> codecId >> w >> h >> choice >> extradata >> tags >> packet;
    if (stream.status() != QDataStream::Ok || signature != codedMagic || w < 1 || h < 1 || w > 8192 || h > 8192 || packet.isEmpty())
        return {};
    if (!m_coded) m_coded = std::make_unique<Coded>();
    Coded &c = *m_coded;
    QString why;
    const QByteArray key = QCryptographicHash::hash(choice + QByteArray::number(codecId) + extradata, QCryptographicHash::Sha1);
    auto &reader = c.readers[key];
    if (!reader) {
        reader = std::make_unique<render::cachecodec::Reader>();
        const auto chosen = render::cachecodec::Choice::fromMap(QJsonDocument::fromJson(choice).toVariant().toMap());
        if (!chosen.valid() || !reader->open(chosen, codecId, extradata, &why)) {
            reader.reset();
            c.readers.erase(key);
            m_readerUnavailable = chosen.valid();  // the file is sound; its reader would not open
            return {};
        }
    }
    {
        StageTimer::Scope timed("cache.decode");
        if (!reader->decode(packet, c.planes, &why) || c.planes.width != w || c.planes.height != h) return {};
    }
    // Back to working light on the engine: the planes as full-range BT.709,
    // then the storage curve undone.
    if (!c.engine) {
        c.engine = render::createEngine();
        c.source = c.engine->createSource();
    }
    if (!c.program) {
        c.program = ColourPipeline::engineCacheProgram(false, &why);
        std::string failed;
        if (!c.program || !c.engine->prepareProgram(*c.program, &failed)) { c.program.reset(); m_readerUnavailable = true; return {}; }
        engine::VideoFilter undo;
        undo.type = engine::VideoFilterType::Program;
        undo.program = c.program;
        c.engine->setSourceFilters(c.source, {undo});
    }
    engine::YuvInput in;
    in.width = w;
    in.height = h;
    in.matrix = engine::YuvMatrix::BT709;
    in.fullRange = true;
    in.depth = 10;
    in.transfer = engine::Transfer::Encoded;
    in.plane[0] = reinterpret_cast<const uint8_t *>(c.planes.y.data());
    in.stride[0] = w * 2;
    in.plane[1] = reinterpret_cast<const uint8_t *>(c.planes.u.data());
    if (c.planes.full) {
        in.layout = engine::YuvLayout::I444;  // planar, the codes in the low bits
        in.stride[1] = w * 2;
        in.plane[2] = reinterpret_cast<const uint8_t *>(c.planes.v.data());
        in.stride[2] = w * 2;
    } else {
        in.layout = engine::YuvLayout::NV12;  // P010: the codes in the high bits
        in.msb = true;
        in.stride[1] = (w + 1) / 2 * 4;
    }
    if (!c.engine->updateSource(c.source, in)) { m_readerUnavailable = true; return {}; }
    c.engine->renderSourceFilters(c.source, 0);
    *width = w;
    *height = h;
    *tagsOut = QJsonDocument::fromJson(tags).toVariant().toMap();
    return true;
}

bool PreviewRenderCache::about(int frame, QString *render, int *range, double *madeMs) const {
    const auto job = m_current;
    if (!job) return false;
    std::lock_guard lock(job->mutex);
    if (!job->ready.contains(frame)) return false;
    *render = job->id;
    *range = frame;
    for (const auto &v : job->ranges) {
        const auto span = v.toMap();
        if (span.value("first").toInt() <= frame && frame <= span.value("last").toInt()) { *range = span.value("first").toInt(); break; }
    }
    const auto took = job->madeMs.constFind(frame);
    *madeMs = took == job->madeMs.cend() ? -1 : double(took.value());
    return true;
}

QImage PreviewRenderCache::shown(int frame, const QSize &draw, const QVariantMap &display) {
    const auto job = m_current;
    if (!job || display.isEmpty()) return {};
    QString path;
    { std::lock_guard lock(job->mutex); path = job->ready.value(frame); }
    if (path.isEmpty()) return {};
    std::lock_guard lock(m_codedMutex);
    if (m_coded && render::engineLost(m_coded->engine.get())) m_coded.reset();  // as readCoded
    int w = 0, h = 0, from = 0;
    QVariantMap tags;
    QImage lossless;
    if (path.endsWith(QLatin1String(".vpk"))) {
        if (!loadCoded(path, &w, &h, &tags)) return {};
        from = m_coded->source;
    } else {
        // A lossless frame: its floats, straight onto the engine.
        lossless = readFrame(path);
        if (lossless.format() != QImage::Format_RGBA32FPx4 || !WorkingFrame::isTagged(lossless)) return {};
        if (!m_coded) m_coded = std::make_unique<Coded>();
        Coded &c = *m_coded;
        if (!c.engine) { c.engine = render::createEngine(); c.source = c.engine->createSource(); }
        if (!c.plain) c.plain = c.engine->createSource();
        engine::CpuFrame in;
        in.width = w = lossless.width();
        in.height = h = lossless.height();
        in.stride = int(lossless.bytesPerLine());
        in.format = engine::PixelFormat::RGBAF32;
        in.encoded = true;  // working light already: the values as they are
        in.data = lossless.constBits();
        if (!c.engine->updateSource(c.plain, in)) return {};
        for (const QString &key : lossless.textKeys()) tags[key] = lossless.text(key);
        from = c.plain;
    }
    if (tags.value(QStringLiteral("omaedit.working.space")).toString() != QLatin1String("oma_interchange")) return {};
    Coded &c = *m_coded;
    if (!c.display || c.displayOf != display) {
        QString why;
        auto program = ColourPipeline::engineProgram(QStringLiteral("oma_interchange"),
            ColourPipeline::spaceName(display.value(QStringLiteral("gamut"), QStringLiteral("srgb")).toString(),
                                      display.value(QStringLiteral("gamma"), QStringLiteral("srgb")).toString()),
            display.value(QStringLiteral("view"), QStringLiteral("Standard")).toString(), &why);
        std::string failed;
        if (!program || !c.engine->prepareProgram(*program, &failed)) return {};
        if (!c.viewer) c.viewer = c.engine->createSource();
        engine::VideoFilter f;
        f.type = engine::VideoFilterType::Program;
        f.program = program;
        c.engine->setSourceFilters(c.viewer, {f});
        c.display = std::move(program);
        c.displayOf = display;
    }
    const bool smaller = draw.width() > 0 && draw.height() > 0 && draw.width() < w && draw.height() < h;
    const int dw = smaller ? draw.width() : w, dh = smaller ? draw.height() : h;
    engine::Layer layer;
    layer.source = from;
    layer.w = float(dw);
    layer.h = float(dh);
    c.engine->compositeInto(c.viewer, dw, dh, {layer}, {0, 0, 0, 0});
    c.engine->renderSourceFilters(c.viewer, 0);
    // Pictures used again once the viewer has let go of them.
    QImage pixels;
    for (QImage &spare : c.spares)
        if (spare.width() == dw && spare.height() == dh && spare.isDetached()) { spare.bits(); pixels = spare; break; }
    if (pixels.isNull()) {
        pixels = QImage(dw, dh, QImage::Format_ARGB32_Premultiplied);
        if (pixels.isNull()) return {};
        if (c.spares.size() >= 6) c.spares.erase(c.spares.begin());
        c.spares.push_back(pixels);
    }
    if (pixels.bytesPerLine() != qsizetype(dw) * 4 || !c.engine->readSourceShown(c.viewer, dw, dh, const_cast<uchar *>(pixels.constBits()))) return {};
    const auto tag = [&pixels](const QString &key, const QString &value) { if (pixels.text(key) != value) pixels.setText(key, value); };
    tag(QStringLiteral("omaedit.shown"), QStringLiteral("1"));
    tag(QStringLiteral("omaedit.pixelAspect"), tags.value(QStringLiteral("omaedit.pixelAspect"), QStringLiteral("1")).toString());
    tag(QStringLiteral("omaedit.fullSize"), QStringLiteral("%1x%2").arg(w).arg(h));
    return pixels;
}

void PreviewRenderCache::run() {
    std::shared_ptr<Job> job;
    std::unique_ptr<ViewerGpu> colour; // worker-owned decisions and buffers survive cache invalidation
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || m_pending || !m_retired.empty() || (job && job->cancelled); });
            if (m_stopping) return;
            // Renders that are over: let go of here, with the lock released.
            std::vector<std::shared_ptr<Job>> over;
            over.swap(m_retired);
            if (!m_pending) {
                if (job && !job->cancelled) { lock.unlock(); over.clear(); continue; }
                lock.unlock(); job.reset(); over.clear(); continue;
            }
            std::shared_ptr<Job> before = std::move(job);
            job = std::move(m_pending); // retires the previous directory on this thread
            lock.unlock();
            before.reset();
            over.clear();
        }
        const auto usable = [&] {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || job->cancelled || (!m_busy && !m_paused); });
            return !m_stopping && !job->cancelled;
        };
        const auto fail = [&](const QString &message, const QString &state = QStringLiteral("failed")) {
            std::lock_guard lock(job->mutex);
            if (job->cancelled) return;
            job->error = message;
            job->state = state;
        };
        if (!usable()) continue;
        if (!QDir().mkpath(job->root)) { fail(tr("The playback cache folder is not writable.")); continue; }
        {
            // Retire files left by interrupted sessions, but never another
            // running editor's cache. Serialize creation with this small sweep.
            QLockFile sweep(QDir(job->root).filePath(".sessions.lock"));
            sweep.setStaleLockTime(0);
            if (!sweep.tryLock(1000)) { fail(tr("The playback cache folder is busy. Try rendering again.")); continue; }
            const auto old = QDir(job->root).entryInfoList({"session-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            for (const auto &directory : old) {
                QLockFile lease(directory.filePath() + "/.owner.lock");
                lease.setStaleLockTime(0);
                if (lease.tryLock()) {
                    lease.unlock();
                    QDir(directory.filePath()).removeRecursively();
                }
            }
            job->directory = std::make_unique<QTemporaryDir>(QDir(job->root).filePath("session-XXXXXX"));
            if (!job->directory->isValid()) { fail(tr("Could not create the playback cache folder.")); continue; }
            job->lease = std::make_unique<QLockFile>(job->directory->filePath(".owner.lock"));
            job->lease->setStaleLockTime(0);
            if (!job->lease->tryLock()) { fail(tr("Could not reserve the playback cache folder.")); continue; }
        }
        QString error;
        auto entries = RenderSnapshot::materialize(job->snapshot.value("entries").toList(),
            job->snapshot.value("nested").toMap(), job->directory->path(), &error);
        if (!error.isEmpty()) { fail(error); continue; }
        // Frames in the order wanted (from the playhead on, then before it),
        // handed out in runs long enough that each renderer decodes on from
        // one seek rather than seeking every frame.
        QList<Span> ahead, behind;
        for (const auto &v : job->ranges) {
            const auto s = v.toMap();
            const int a = s.value("first").toInt(), b = s.value("last").toInt();
            if (b >= job->playhead) ahead.append({qMax(a, job->playhead), b});
            if (a < job->playhead) behind.append({a, qMin(b, job->playhead - 1)});
        }
        ahead.append(behind);
        const auto size = job->snapshot.value("profile").toMap();
        const int layers = layerCount(entries);
        const int renderers = rendererCount(layers, qint64(size.value("width").toInt()) * size.value("height").toInt());
        std::atomic<int> nextRun{0};
        std::atomic_bool stopped{false};
        std::mutex colourMutex; // one GPU converter, shared by the renderers
        const bool accelerate = job->processing.value("preference") == "auto";
        // OMAEDIT_CACHE_VIDEO=0 keeps lossless frames (for checks that compare
        // cached pictures with live ones exactly).
        if (accelerate && !m_codecDistrusted && qEnvironmentVariable("OMAEDIT_CACHE_VIDEO") != QLatin1String("0")) {
            // Frames kept as hardware video when this machine has a codec for
            // it (qualified once a day; see cachecodec.h).
            QVariantMap report;
            job->codec = render::cachecodec::choose(&report);
            const auto info = ColourPipeline::info();
            const double aspect = size.value("sarDen", 1).toDouble() > 0
                ? size.value("sarNum", 1).toDouble() / size.value("sarDen", 1).toDouble() : 1.0;
            job->frameTags = {{"omaedit.working.config", info.value("config")}, {"omaedit.working.configHash", info.value("configHash")},
                              {"omaedit.working.space", "oma_interchange"}, {"omaedit.working.alpha", "straight"},
                              {"omaedit.pixelAspect", QString::number(aspect, 'g', 16)}};
            std::lock_guard lock(job->mutex);
            job->processing["cacheCodec"] = job->codec.toMap();
            job->processing["cacheCodecReason"] = report.value("reason");
        }
        const QByteArray frameTags = QJsonDocument::fromVariant(job->frameTags).toJson(QJsonDocument::Compact);
        const QByteArray codecChoice = QJsonDocument::fromVariant(job->codec.toMap()).toJson(QJsonDocument::Compact);
        // Frames the renders before this one finished and that are still this
        // timeline's pictures are taken over, not made again. The renders
        // before have stopped by now: this thread ran them.
        {
            // (The files as they are now, if the watcher has not read them yet.)
            dependenciesMoved(*job);
            const auto factsOfJob = [](Job &j) { std::lock_guard lock(j.mutex); return j.facts; };
            const QByteArray mine = settingsMark(job->snapshot, factsOfJob(*job), job->processing);
            QHash<int, QByteArray> wanted; // made when first needed
            for (std::shared_ptr<Job> old = job->previous; old; old = old->previous) {
                if (!old->directory || old->ready.isEmpty() || old->codec.toMap() != job->codec.toMap() || old->frameTags != job->frameTags
                    || settingsMark(old->snapshot, factsOfJob(*old), old->processing) != mine)
                    continue;
                if (wanted.isEmpty()) wanted = frameMarks(job->snapshot.value("entries").toList(), spansOf(job->ranges));
                QList<Span> had;
                for (auto it = old->ready.cbegin(); it != old->ready.cend(); ++it) had.append({it.key(), it.key()});
                const auto marks = frameMarks(old->snapshot.value("entries").toList(), had);
                QHash<QByteArray, int> rendered;
                for (auto it = marks.cbegin(); it != marks.cend(); ++it)
                    if (!it.value().isEmpty()) rendered.insert(it.value(), it.key());
                for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
                    if (it.value().isEmpty() || job->ready.contains(it.key())) continue;
                    const auto found = rendered.constFind(it.value());
                    if (found == rendered.cend()) continue;
                    const QString from = old->ready.value(found.value());
                    const QString to = job->directory->filePath(QString::number(it.key()) + QLatin1Char('.') + QFileInfo(from).suffix());
                    // The same file under a second name: nothing is copied.
                    if (::link(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) != 0 && !QFile::copy(from, to))
                        continue;
                    const qint64 bytes = QFileInfo(to).size();
                    std::lock_guard lock(job->mutex);
                    if (job->bytes + bytes > job->limit || job->ready.size() >= 100000) { QFile::remove(to); break; }
                    job->ready.insert(it.key(), to);
                    if (const auto took = old->madeMs.constFind(found.value()); took != old->madeMs.cend()) job->madeMs.insert(it.key(), took.value());
                    job->bytes += bytes;
                    ++job->carried;
                    markReady(job->readyRanges, it.key());
                }
            }
            job->previous.reset(); // the old folders go with their last holder
        }
        std::vector<int> order;
        for (const auto &span : ahead)
            for (int frame = span.first; frame <= span.second; ++frame)
                if (!job->ready.contains(frame)) order.push_back(frame);
        const int run = qMax(48, int(order.size()) / (renderers * 4) + 1);
        const auto render = [&] {
            std::unique_ptr<Timeline> timeline;
            std::unique_ptr<ColourPipeline::ScopedConverter> convert;
            QString problem;
            // A finished frame's file, counted against the limit and made available.
            // From one finished frame to the next: what this frame took to make
            // (the first, with the timeline's building in it, is not counted).
            QElapsedTimer sinceLast;
            const auto keep = [&](int frame, const QString &path) {
                const qint64 bytes = QFileInfo(path).size();
                const double took = sinceLast.isValid() ? double(sinceLast.nsecsElapsed()) / 1e6 : -1;
                sinceLast.start();
                std::lock_guard lock(job->mutex);
                if (took >= 0) job->madeMs.insert(frame, float(took));
                if (job->bytes + bytes > job->limit || job->ready.size() >= 100000) {
                    QFile::remove(path);
                    job->state = "partial";
                    job->error = tr("Playback cache limit reached. Completed frames remain available; increase the limit in Playback preferences to render more.");
                    stopped = true;
                    return false;
                }
                if (job->cancelled) { stopped = true; return false; }
                job->ready.insert(frame, path);
                job->bytes += bytes;
                markReady(job->readyRanges, frame);
                return true;
            };
            // Hardware video, when the machine has a codec for it: the
            // engine's planes in, one packet a frame out.
            std::unique_ptr<render::cachecodec::Writer> writer;
            bool coding = job->codec.valid();
            engine::YuvFrame planes;
            const auto packet = [&](int frame, const QByteArray &bytes) {
                const QString path = job->directory->filePath(QString::number(frame) + ".vpk");
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)) return false;
                QDataStream stream(&file);
                stream << codedMagic << qint32(writer->codecId()) << qint32(planes.width) << qint32(planes.height) << codecChoice
                       << writer->extradata() << frameTags << bytes;
                if (stream.status() != QDataStream::Ok || !file.commit()) {
                    fail(tr("Could not finish a playback cache frame. Check free disk space."));
                    stopped = true;
                    return false;
                }
                return keep(frame, path);
            };
            // The engine made the frame's colour, on its GPU or CPU twin.
            const auto noteEngine = [&] {
                const QString drawn = timeline->engineBackend();
                if (drawn.isEmpty()) return;
                std::lock_guard lock(job->mutex);
                const QString counter = drawn == "vulkan" ? "gpuConversions" : "cpuConversions";
                job->processing[counter] = job->processing.value(counter).toULongLong() + 1;
                job->processing["backend"] = drawn;
                job->processing["reason"] = drawn == "vulkan" ? tr("The engine renders on the GPU.")
                    : accelerate ? tr("The engine renders on the CPU (no usable GPU).") : tr("CPU selected for cache colour conversion.");
            };
            for (;;) {
                const qsizetype first = qsizetype(nextRun.fetch_add(1)) * run;
                if (stopped || first >= qsizetype(order.size())) {
                    if (writer && !stopped && !writer->finish(&problem) && !stopped) fail(problem);
                    return;
                }
                if (!timeline) { // built on taking the first run
                    timeline = std::make_unique<Timeline>();
                    const auto resolved = job->snapshot.value("profile").toMap();
                    auto project = job->snapshot.value("project").toMap();
                    project["custom"] = true;
                    for (const auto &key : QStringList{"width", "height", "fpsNum", "fpsDen"}) project[key] = resolved.value(key);
                    timeline->setProjectSettings(project);
                    timeline->setMediaSources(job->snapshot.value("mediaSources").toMap(), job->snapshot.value("proxyMode", "original").toString());
                    if (!timeline->restore(entries, Timeline::trackFlagsFromVariant(job->snapshot.value("tracks").toList()))
                        || timeline->frameCount() != job->snapshot.value("frames").toInt()) {
                        fail(tr("The timeline could not be rebuilt for background rendering. Live playback is still available."));
                        stopped = true; return;
                    }
                    auto *p = timeline->profile();
                    p->set_sample_aspect(resolved.value("sarNum", 1).toInt(), resolved.value("sarDen", 1).toInt());
                    p->set_display_aspect(resolved.value("darNum", 16).toInt(), resolved.value("darDen", 9).toInt());
                    p->set_progressive(resolved.value("progressive", 1).toInt());
                    p->set_colorspace(resolved.value("colorspace", 709).toInt());
                    timeline->setMasterMixMap(job->snapshot.value("master").toMap());
                    timeline->setEngineOnCpu(!accelerate);
                    // One of `renderers` working at once, each with decoders
                    // of its own: few kept open (a run of frames needs the
                    // clips under it, no more), a gigabyte of pictures between
                    // them, and a share of the machine's threads each.
                    timeline->setDecoderLimits(6, qint64(1) << 30, renderers > 1 ? qBound(2, QThread::idealThreadCount() / (2 * renderers), 8) : 0);
                    // Colour conversions made while this thread renders come here.
                    convert = std::make_unique<ColourPipeline::ScopedConverter>([&](const QImage &input,const QString &from,const QString &to,QString *why) {
                        QImage output;QVariantMap conversion;QVariantMap engine;
                        if (accelerate) {
                            std::lock_guard gpu(colourMutex);
                            if (!colour) colour = std::make_unique<ViewerGpu>();
                            output = colour->convertSignal(input,from,to,job->cancelled,&conversion,why);
                            if (!output.isNull()) engine = colour->information();
                        } else {
                            output = ColourPipeline::convert(input,from,to,why);
                            conversion = {{"backend", "cpu"}, {"reason", tr("CPU selected for cache colour conversion.")}};
                        }
                        if (!output.isNull()) {
                            std::lock_guard lock(job->mutex);
                            const QString counter = conversion.value("backend") == "vulkan" ? "gpuConversions" : "cpuConversions";
                            job->processing[counter] = job->processing.value(counter).toULongLong() + 1;
                            job->processing["backend"] = conversion.value("backend", "cpu");
                            job->processing["reason"] = conversion.value("reason");
                            job->processing["conversion"] = conversion;
                            if (accelerate) job->processing["engine"] = engine;
                        }
                        return output;
                    });
                }
                auto *p = timeline->profile();
                for (qsizetype k = first; k < qMin(first + run, qsizetype(order.size())); ++k) {
                    const int frame = order[size_t(k)];
                    if (stopped || !usable()) { stopped = true; return; }
                    const QStorageInfo volume(job->directory->path());
                    // Reserve room for a worst-case frame before decoding/allocating it.
                    const qint64 estimate = qint64(p->width()) * p->height() * (timeline->linearRendering() ? 16 : 4) + 65536;
                    if (!volume.isValid() || volume.bytesAvailable() < freeReserve + estimate) {
                        fail(tr("Background rendering stopped because the cache volume is low on space. Completed frames remain available."), "partial"); stopped = true; return;
                    }
                    RenderScope render(std::make_shared<RenderRequest>([job] { return job->cancelled.load(); }));
                    if (coding) {
                        QString why;
                        if (timeline->cachePlanes(frame, int(job->codec.layout()), planes, &why)) {
                            if (!writer) {
                                writer = std::make_unique<render::cachecodec::Writer>();
                                if (!writer->open(job->codec, planes.width, planes.height, packet, &why)) { writer.reset(); coding = false; }
                            }
                            if (writer) {
                                // (Stopped already: the cache's limit was reached while its
                                // packets were kept, and that is what the job says.)
                                if (!writer->encode(planes, frame, &why)) { if (!stopped) fail(why); stopped = true; return; }
                                if (stopped) return;
                                noteEngine();
                                continue;
                            }
                        }
                        // Not drawn by the engine: lossless frames, as before.
                        if (job->cancelled) { stopped = true; return; }
                    }
                    const auto decoded = timeline->decodeAt(frame, false);
                    if(decoded.cancelled){stopped=true;return;}
                    if (!decoded.error.isEmpty() || decoded.image.isNull()) {
                        fail(decoded.error.isEmpty() ? tr("A frame could not be rendered. Live playback is still available.") : decoded.error); stopped = true; return;
                    }
                    if (stopped || !usable()) { stopped = true; return; }
                    noteEngine();
                    const QString path = job->directory->filePath(QString::number(frame) + ".opr");
                    if (!writeFrame(path, decoded.image, &problem)) { fail(problem); stopped = true; return; }
                    if (!keep(frame, path)) return;
                }
            }
        };
        std::vector<std::unique_ptr<QThread>> helpers;
        for (int i = 1; i < renderers && qsizetype(i) * run < qsizetype(order.size()); ++i) {
            helpers.emplace_back(QThread::create(render));
            helpers.back()->start(QThread::LowPriority);
        }
        render();
        for (auto &helper : helpers) helper->wait();
        const bool done = !stopped;
        if (done) { std::lock_guard lock(job->mutex); if (!job->cancelled) job->state = "completed"; }
    }
}
