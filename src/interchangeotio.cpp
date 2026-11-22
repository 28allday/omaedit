#include "interchange.h"
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QUrl>
#include <QtMath>
#include <opentimelineio/clip.h>
#include <opentimelineio/deserialization.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/generatorReference.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/serialization.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;
using opentime::OPENTIME_VERSION::RationalTime;
using opentime::OPENTIME_VERSION::TimeRange;

namespace Interchange {
namespace {
std::string str(const QString &s) { return s.toStdString(); }
QString qstr(const std::string &s) { return QString::fromStdString(s); }
otio::AnyDictionary editorMetadata(const QVariantMap &values) {
    otio::AnyDictionary inner;
    inner["extras"] = str(QString::fromUtf8(QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact)));
    otio::AnyDictionary md; md["omaedit"] = inner; return md;
}
QVariantMap editorExtras(const otio::AnyDictionary &md) {
    auto it = md.find("omaedit");
    if (it == md.end()) return {};
    try {
        const auto inner = std::any_cast<otio::AnyDictionary>(it->second);
        auto e = inner.find("extras");
        if (e == inner.end()) return {};
        return QJsonDocument::fromJson(QByteArray::fromStdString(std::any_cast<std::string>(e->second))).toVariant().toMap();
    } catch (const std::bad_any_cast &) { return {}; }
}
int frames(const RationalTime &t, double fps) { return int(qRound64(t.rescaled_to(fps).value())); }
}

bool writeOtio(const Document &doc, const QString &path, QString *error) {
    if (requiresMappedTiming(doc, error)) return false;
    const double fps = doc.fps();
    otio::SerializableObject::Retainer<otio::Timeline> timeline(new otio::Timeline(str(doc.name.isEmpty() ? QStringLiteral("Sequence") : doc.name)));
    timeline->set_global_start_time(RationalTime(double(doc.startTimecodeFrames), fps));
    otio::AnyDictionary meta; meta["generator"] = std::string("OmaEdit");
    meta["width"] = int64_t(doc.width); meta["height"] = int64_t(doc.height);
    otio::AnyDictionary top; top["omaedit"] = meta; timeline->metadata() = top;
    otio::ErrorStatus err;
    for (const Marker &m : doc.markers)
        timeline->tracks()->markers().push_back(new otio::Marker(str(m.text), TimeRange(RationalTime(m.frame, fps), RationalTime(1, fps)),
            m.type == QStringLiteral("todo") ? otio::Marker::Color::red : otio::Marker::Color::green));
    for (const Track &track : doc.tracks) {
        otio::SerializableObject::Retainer<otio::Track> t(new otio::Track(str(track.name), std::nullopt, track.kind == Track::Audio ? otio::Track::Kind::audio : otio::Track::Kind::video));
        otio::AnyDictionary flags; flags["locked"] = track.locked; flags["muted"] = track.muted; flags["disabled"] = track.disabled;
        otio::AnyDictionary tm; tm["omaedit"] = flags; t->metadata() = tm;
        t->set_enabled(!track.disabled);
        int cursor = 0;
        for (const Item &item : track.items) {
            if (item.start > cursor) t->append_child(new otio::Gap(RationalTime(item.start - cursor, fps)), &err);
            if (item.kind == Item::Gap) {
                t->append_child(new otio::Gap(RationalTime(item.duration, fps), str(item.name)), &err);
            } else {
                const double a = qAbs(item.speed == 0.0 ? 1.0 : item.speed);
                // OTIO: the range's duration is the clip's length on the timeline; a
                // LinearTimeWarp says how much media that consumes. Its start is media time.
                const TimeRange range(RationalTime(qRound64(item.sourceIn * a), fps), RationalTime(item.duration, fps));
                otio::MediaReference *ref = nullptr;
                if (!item.path.isEmpty()) {
                    std::optional<TimeRange> available;
                    if (item.sourceLength > 0) available = TimeRange(RationalTime(0, fps), RationalTime(item.sourceLength, fps));
                    otio::AnyDictionary rm; rm["startTimecodeFrames"] = int64_t(item.sourceTimecodeFrames);
                    otio::AnyDictionary rmd; rmd["omaedit"] = rm;
                    ref = new otio::ExternalReference(str(QUrl::fromLocalFile(item.path).toString()), available, rmd);
                } else {
                    otio::AnyDictionary params; params["kind"] = str(item.generator);
                    ref = new otio::GeneratorReference(str(item.name), str(QStringLiteral("omaedit:") + item.generator), std::nullopt, params);
                }
                otio::Clip *clip = new otio::Clip(str(item.name), ref, range, editorMetadata(item.extras));
                clip->set_enabled(item.enabled);
                if (!qFuzzyCompare(a, 1.0) || item.speed < 0) clip->effects().push_back(new otio::LinearTimeWarp("Speed", "LinearTimeWarp", item.speed));
                for (const Marker &m : item.markers)
                    clip->markers().push_back(new otio::Marker(str(m.text), TimeRange(RationalTime(qRound64(m.frame * a) - range.start_time().value() < 0 ? 0 : qRound64(m.frame * a) - range.start_time().value(), fps), RationalTime(1, fps)),
                        m.type == QStringLiteral("todo") ? otio::Marker::Color::red : otio::Marker::Color::green));
                t->append_child(clip, &err);
                if (item.transitionOut > 0)
                    t->append_child(new otio::Transition("Cross Dissolve", otio::Transition::Type::SMPTE_Dissolve, RationalTime(item.transitionOut, fps), RationalTime(0, fps)), &err);
            }
            cursor = item.start + item.duration;
            if (otio::is_error(err)) { if (error) *error = qstr(err.full_description); return false; }
        }
        timeline->tracks()->append_child(t, &err);
    }
    // As the other formats: a complete replacement or none (to_json_file
    // would truncate an existing export before writing).
    const std::string json = timeline->to_json_string(&err);
    if (otio::is_error(err)) { if (error) *error = qstr(err.full_description); return false; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json.data(), qint64(json.size())) != qint64(json.size()) || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool readOtio(const QString &path, Document *doc, QString *error) {
    *doc = Document();
    otio::ErrorStatus err;
    otio::SerializableObject::Retainer<otio::SerializableObject> object(otio::SerializableObject::from_json_file(str(path), &err));
    if (otio::is_error(err) || !object) { if (error) *error = otio::is_error(err) ? qstr(err.full_description) : QStringLiteral("Not an OpenTimelineIO file."); return false; }
    auto *timeline = dynamic_cast<otio::Timeline *>(object.value);
    if (!timeline) { if (error) *error = QStringLiteral("The file holds no timeline."); return false; }
    // A file can hold JSON nulls where objects belong ("tracks": null,
    // "markers": [null]); the reader keeps them as empty retainers.
    if (!timeline->tracks()) { if (error) *error = QStringLiteral("The timeline has no tracks."); return false; }
    doc->name = qstr(timeline->name());
    // Rate: the global start's, else the first item's, else 25.
    double fps = 0;
    if (timeline->global_start_time()) fps = timeline->global_start_time()->rate();
    for (const auto &child : timeline->tracks()->children()) {
        if (fps > 0) break;
        if (auto *t = dynamic_cast<otio::Track *>(child.value)) for (const auto &c : t->children()) {
            if (auto *item = dynamic_cast<otio::Item *>(c.value)) { const auto d = item->duration(&err); if (!otio::is_error(err) && d.rate() > 0) { fps = d.rate(); break; } }
        }
    }
    if (fps <= 0) fps = 25.0;
    // Express the rate as a fraction the editor knows.
    struct R { double fps; int num, den; };
    static const R known[] = {{23.976, 24000, 1001}, {24, 24, 1}, {25, 25, 1}, {29.97, 30000, 1001}, {30, 30, 1}, {48, 48, 1}, {50, 50, 1}, {59.94, 60000, 1001}, {60, 60, 1}};
    doc->fpsNum = qRound(fps * 1000); doc->fpsDen = 1000;
    for (const R &r : known) if (qAbs(r.fps - fps) < 0.002) { doc->fpsNum = r.num; doc->fpsDen = r.den; }
    if (timeline->global_start_time()) doc->startTimecodeFrames = qRound64(timeline->global_start_time()->rescaled_to(fps).value());
    try {
        auto it = timeline->metadata().find("omaedit");
        if (it != timeline->metadata().end()) {
            const auto m = std::any_cast<otio::AnyDictionary>(it->second);
            if (m.count("width")) doc->width = int(std::any_cast<int64_t>(m.at("width")));
            if (m.count("height")) doc->height = int(std::any_cast<int64_t>(m.at("height")));
        }
    } catch (const std::bad_any_cast &) {}
    for (const auto &m : timeline->tracks()->markers())
        if (m) doc->markers.append({frames(m->marked_range().start_time(), fps), qstr(m->name()), m->color() == otio::Marker::Color::red ? QStringLiteral("todo") : QString()});
    std::function<void(otio::Track *, int)> readTrack = [&](otio::Track *t, int depth) {
        Track track;
        track.kind = t->kind() == otio::Track::Kind::audio ? Track::Audio : Track::Video;
        track.name = qstr(t->name());
        track.disabled = !t->enabled();
        try {
            auto it = t->metadata().find("omaedit");
            if (it != t->metadata().end()) { const auto m = std::any_cast<otio::AnyDictionary>(it->second);
                if (m.count("locked")) track.locked = std::any_cast<bool>(m.at("locked"));
                if (m.count("muted")) track.muted = std::any_cast<bool>(m.at("muted")); }
        } catch (const std::bad_any_cast &) {}
        int cursor = 0; int pendingIn = 0;
        for (const auto &c : t->children()) {
            if (auto *tr = dynamic_cast<otio::Transition *>(c.value)) {
                const int in = frames(tr->in_offset(), fps), out = frames(tr->out_offset(), fps);
                if (!track.items.isEmpty()) {
                    // The outgoing side owns the whole overlap: extend it by the part after the cut.
                    Item &prev = track.items.last();
                    prev.duration += out; prev.transitionOut = in + out;
                }
                pendingIn = out; // the next item starts after the cut + out
                continue;
            }
            auto *item = dynamic_cast<otio::Item *>(c.value);
            if (!item) continue;
            const auto durationTime = item->duration(&err);
            if (otio::is_error(err)) { err = otio::ErrorStatus(); continue; }
            const int duration = frames(durationTime, fps);
            Item out;
            out.name = qstr(item->name());
            out.start = cursor + pendingIn; out.duration = duration - pendingIn;
            out.enabled = item->enabled();
            if (auto *clip = dynamic_cast<otio::Clip *>(item)) {
                double speed = 1.0;
                for (const auto &e : clip->effects()) if (auto *warp = dynamic_cast<otio::LinearTimeWarp *>(e.value)) speed = warp->time_scalar();
                if (speed == 0.0) speed = 1.0;
                out.speed = speed;
                const double a = qAbs(speed);
                const auto range = clip->source_range() ? *clip->source_range() : clip->available_range(&err);
                out.sourceIn = int(qRound64(frames(range.start_time(), fps) / a)) + pendingIn;
                if (auto *ext = dynamic_cast<otio::ExternalReference *>(clip->media_reference())) {
                    const QUrl url(qstr(ext->target_url()));
                    out.path = url.isLocalFile() ? url.toLocalFile() : qstr(ext->target_url());
                    if (ext->available_range()) out.sourceLength = frames(ext->available_range()->duration(), fps);
                    try { auto it = ext->metadata().find("omaedit"); if (it != ext->metadata().end()) { const auto m = std::any_cast<otio::AnyDictionary>(it->second); if (m.count("startTimecodeFrames")) out.sourceTimecodeFrames = std::any_cast<int64_t>(m.at("startTimecodeFrames")); } } catch (const std::bad_any_cast &) {}
                    if (out.sourceTimecodeFrames < 0 && ext->available_range() && ext->available_range()->start_time().value() > 0) {
                        // Media that starts at a timecode: the clip's start is expressed in that timecode.
                        out.sourceTimecodeFrames = frames(ext->available_range()->start_time(), fps);
                        out.sourceIn = int(qRound64(frames(range.start_time(), fps) / a)) + pendingIn; // producer frames come out of toEntries' timecode subtraction
                    }
                    if (out.name.isEmpty()) out.name = QFileInfo(out.path).fileName();
                } else if (auto *gen = dynamic_cast<otio::GeneratorReference *>(clip->media_reference())) {
                    const QString kind = qstr(gen->generator_kind());
                    out.generator = kind.startsWith(QStringLiteral("omaedit:")) ? kind.mid(8) : QStringLiteral("solid");
                    if (out.generator == QStringLiteral("title")) out.extras.insert(QStringLiteral("title"), out.name);
                } else {
                    out.kind = Item::Gap; doc->notes.append(QStringLiteral("%1: no media reference, placed as a gap").arg(out.name));
                }
                out.audioOnly = track.kind == Track::Audio;
                for (const auto &m : clip->markers())
                    if (m) out.markers.append({int(qRound64((frames(range.start_time(), fps) + frames(m->marked_range().start_time(), fps)) / a)), qstr(m->name()), m->color() == otio::Marker::Color::red ? QStringLiteral("todo") : QString()});
                const QVariantMap extras = editorExtras(clip->metadata());
                for (auto e = extras.constBegin(); e != extras.constEnd(); ++e) out.extras.insert(e.key(), e.value());
            } else if (dynamic_cast<otio::Gap *>(item)) {
                // Empty stretches are implied by the next item's start.
                cursor += duration; pendingIn = 0; continue;
            } else if (auto *stack = dynamic_cast<otio::Stack *>(item)) {
                out.kind = Item::Gap; out.generator = QStringLiteral("nest");
                doc->notes.append(QStringLiteral("%1: nested stack placed as a gap").arg(out.name.isEmpty() ? QStringLiteral("Nested") : out.name));
                Q_UNUSED(stack);
            } else if (auto *inner = dynamic_cast<otio::Track *>(item)) {
                if (depth < 2) readTrack(inner, depth + 1);
                out.kind = Item::Gap;
            }
            if (out.duration > 0) track.items.append(out);
            cursor += duration; pendingIn = 0;
        }
        doc->tracks.append(track);
    };
    for (const auto &child : timeline->tracks()->children())
        if (auto *t = dynamic_cast<otio::Track *>(child.value)) readTrack(t, 0);
    // Video before audio, as the editor lays them out.
    QList<Track> video, audio;
    for (const Track &t : doc->tracks) (t.kind == Track::Video ? video : audio).append(t);
    doc->tracks = video + audio;
    if (doc->tracks.isEmpty()) { if (error) *error = QStringLiteral("The timeline has no tracks."); return false; }
    return true;
}
} // namespace Interchange
