#include "timelinerenderer.h"
#include "framing.h"
#include "stagetimer.h"
#include "bands.h"
#include "imagebands.h"
#include "projectfile.h"
#include "mediaprobe.h"
#include <atomic>

#include "editengine.h"
#include "gradeengine.h"
#include "displaygrade.h"
#include "colourpipeline.h"
#include "linearrenderer.h"
#include "workingframe.h"
#include "lookuptables.h"
#include "generators.h"
#include "effectsteps.h"
#include "transitionstyles.h"
#include "stabilise.h"
#include "gradeplan.h"
#include "solidcolour.h"
#include "motionflow.h"
#include "projectsafety.h"
#include "framesampling.h"
#include "retimemap.h"
#include "sourcetiming.h"

#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <map>
#include <QMap>
#include <QScopedValueRollback>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <condition_variable>
#include <mutex>
#include "renderrequest.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>

extern "C" {
#include <libswscale/swscale.h>
}

namespace render {

bool engineRenderingRequested() {
    // The engine draws unless asked not to (OMAEDIT_RENDERER=cpu: the
    // colour-managed renderer on the CPU, for comparison).
    static const bool on = qEnvironmentVariable("OMAEDIT_RENDERER") != QLatin1String("cpu");
    return on;
}

namespace {

constexpr int kMaxDepth = 8;

// Not drawn: sound-only, switched off, a gap, or on a track that is switched
// off or holds sound. Tracks as the editor lists them ("kind") or as a saved
// timeline does ("audio"). Nor is a recording held on a picture track (a
// project made before recordings went to audio tracks): it has no picture
// stream to open, and asking for one failed the frame and stopped playback.
bool hidden(const QVariantMap &e, const QVariantList &tracks) {
    const int t = e.value("track", e.value("lane", 0)).toInt();
    if (e.value("audioOnly").toBool() || e.value("disabled").toBool() || e.value("gap").toBool()) return true;
    const QString path = e.value("path").toString();
    if (path.startsWith(QLatin1String("tone:"))) return true;  // a sound generator: no picture
    if (t >= 0 && t < tracks.size()) {
        const QVariantMap track = tracks[t].toMap();
        if (track.value("disabled").toBool() || track.value("kind") == "audio" || track.value("audio").toBool()) return true;
    }
    return e.value("title").toString().isEmpty() && !e.contains("compoundJson") && media::soundOnly(path);
}

double parameter(const QVariantMap &e, const char *name, int frame, double fallback = 0) {
    return EditEngine::valueAt(e.value("editKeys").toMap().value(name).toList(), frame, e.value(name, fallback).toDouble());
}

bool isStill(const QString &path) {
    static const QStringList stills{"png", "jpg", "jpeg", "tif", "tiff", "exr", "bmp", "psd"};
    return stills.contains(QFileInfo(path).suffix().toLower());
}

// The editor's blend modes (the values clips store) as the engine's, where
// the engine has them.
std::optional<engine::BlendMode> blendOf(int mode) {
    switch (mode) {
    case 0: return engine::BlendMode::Normal;
    case 12: return engine::BlendMode::Add;
    case 13: return engine::BlendMode::Multiply;
    case 14: return engine::BlendMode::Screen;
    case 15: return engine::BlendMode::Overlay;
    case 16: return engine::BlendMode::Darken;
    case 17: return engine::BlendMode::Lighten;
    case 18: return engine::BlendMode::ColourDodge;
    case 19: return engine::BlendMode::ColourBurn;
    case 20: return engine::BlendMode::HardLight;
    case 21: return engine::BlendMode::SoftLight;
    case 22: return engine::BlendMode::Difference;
    case 23: return engine::BlendMode::Exclusion;
    default: return std::nullopt;
    }
}

// Which attribute groups a clip actually uses, so the unsupported ones can be
// named and the neutral ones ignored.
bool neutralColour(const QVariantMap &e) {
    const auto keys = e.value("editKeys").toMap();
    return !keys.contains("colorB") && !keys.contains("colorC") && !keys.contains("colorS")
        && e.value("colorB", 0.).toDouble() == 0 && e.value("colorC", 1.).toDouble() == 1
        && e.value("colorS", 1.).toDouble() == 1;
}

}  // namespace

bool pictureHidden(const QVariantMap &clip, const QVariantList &tracks) { return hidden(clip, tracks); }

bool solidPicture(const QVariantMap &e) {
    const QString path = e.value("path").toString();
    if (e.value("adjust").toBool() || !e.value("title").toString().isEmpty() || e.contains("compoundJson")
        || ProjectFile::isProject(path) || hasActiveEffects(e.value("videoEffects").toList()))
        return false;
    const auto off = e.value("bypass").toStringList();
    const auto keys = e.value("editKeys").toMap();
    if (!off.contains("crop") && (e.value("cropFeather").toDouble() != 0 || e.value("cropInvert").toBool() || keys.contains("cropFeather")))
        return false;
    if (!off.contains("lens") && (e.value("lensK1").toDouble() != 0 || e.value("lensK2").toDouble() != 0 || keys.contains("lensK1") || keys.contains("lensK2")))
        return false;
    if (!off.contains("stab") && !e.value("stab").toMap().isEmpty()) return false;
    if (path.startsWith(QLatin1String("color:"))) {
        const QColor colour = solidColour(path.mid(6));
        return colour.isValid() && colour.alpha() == 255;
    }
    return generatorKind(path).isEmpty() && QFileInfo(path).isAbsolute();
}

struct TimelineRenderer::State {
    std::function<QString(const QString &)> pictureSource;
    QVariantMap mediaSources;  // the project's media settings, by path (for nested clips)
    engine::Engine &engine;
    explicit State(engine::Engine &e) : engine(e) {}

    QVariantList entries, tracks;
    QVariantMap project;
    QSize size;
    double fps = 25;
    QString problem;
    MediaPool pool;
    QVector<QPair<QVariantMap, RetimeMap>> timingMaps;
    // Engine sources by role, kept from frame to frame; those not used by a
    // frame are released after it.
    std::unordered_map<QString, int> sources;
    QSet<QString> touched;
    Stats stats;
    // Pictures last uploaded per source, so a still or a held frame is not
    // decoded and uploaded again.
    std::unordered_map<QString, QString> shown;
    // A delivery's picture: the frame is drawn into this source rather than
    // the canvas (when set), and its output transform runs there.
    int delivery = 0;
    bool intoDelivery = false;
    // A viewer's picture (shown()): drawn into this source with its
    // transparency, and the display transform runs there.
    int viewer = 0;
    bool intoViewer = false;
    std::shared_ptr<const engine::ColourProgram> viewerProgram;
    // The scopes' transform this engine declined to count through last
    // (and that it declined at all): not asked again frame after frame,
    // each of which would be drawn twice.
    std::shared_ptr<const engine::ColourProgram> scopesDeclinedProgram;
    bool scopesDeclined = false;
    // shown()'s pictures, used again as readbackImage's are.
    std::vector<QImage> shownSpares;
    QImage shownImage(int w, int h, const QSize &stands) {
        const QString whole = QStringLiteral("%1x%2").arg(stands.width()).arg(stands.height());
        const QString aspect = QString::number(framed() ? 1.0 : pixelAspect(), 'g', 16);
        for (QImage &spare : shownSpares)
            if (spare.width() == w && spare.height() == h && spare.isDetached()) {
                if (spare.text(QStringLiteral("omaedit.pixelAspect")) != aspect) spare.setText(QStringLiteral("omaedit.pixelAspect"), aspect);
                if (spare.text(QStringLiteral("omaedit.fullSize")) != whole) spare.setText(QStringLiteral("omaedit.fullSize"), whole);
                spare.bits();  // new contents, a new cacheKey()
                return spare;
            }
        QImage made(w, h, QImage::Format_ARGB32_Premultiplied);
        if (made.isNull()) return made;
        made.setText(QStringLiteral("omaedit.pixelAspect"), aspect);
        made.setText(QStringLiteral("omaedit.shown"), QStringLiteral("1"));
        made.setText(QStringLiteral("omaedit.fullSize"), whole);
        if (shownSpares.size() >= 6) shownSpares.erase(shownSpares.begin());
        shownSpares.push_back(made);
        return made;
    }
    // An export of another size: the timeline drawn into `unframed` at its
    // own size, then placed on a frame of `framedSize` by `framing` (over
    // `backdrop`, a small blurred copy, for Blur).
    QSize framedSize;
    Framing framing = Framing::Fit;
    int unframed = 0, backdrop = 0;
    bool backdropFiltered = false;
    bool framed() const { return framedSize.isValid() && framedSize != size; }
    QSize outputSize() const { return framed() ? framedSize : size; }
    // The layers that place the unframed timeline on the export's frame:
    // for Blur, first the backdrop (the picture filling a small frame,
    // blurred, stretched back over the whole frame and dimmed), then the
    // picture fitted or filled.
    std::vector<engine::Layer> frameLayers() {
        const QSize out = framedSize;
        const double aspect = size.height() > 0 ? size.width() * pixelAspect() / size.height() : 1.0;
        const auto layerAt = [](int source, const Placement &p) {
            engine::Layer l;
            l.source = source;
            l.x = float(p.x); l.y = float(p.y); l.w = float(p.w); l.h = float(p.h);
            l.u0 = float(p.u0); l.v0 = float(p.v0); l.u1 = float(p.u1); l.v1 = float(p.v1);
            return l;
        };
        std::vector<engine::Layer> placed;
        if (framing == Framing::Blur) {
            if (!backdrop) { backdrop = engine.createSource(); backdropFiltered = false; }
            const int bw = backdropSide(out.width()), bh = backdropSide(out.height());
            engine.compositeInto(backdrop, bw, bh, {layerAt(unframed, placeIn(aspect, bw, bh, true))}, {0, 0, 0, 1});
            if (!backdropFiltered) {
                engine::VideoFilter blur;
                blur.type = engine::VideoFilterType::Blur;
                blur.radius = kBackdropRadius;
                engine.setSourceFilters(backdrop, {blur});
                backdropFiltered = true;
            }
            engine.renderSourceFilters(backdrop, 0);
            engine::Layer back;
            back.source = backdrop;
            back.w = float(out.width());
            back.h = float(out.height());
            back.opacity = kBackdropOpacity;
            placed.push_back(back);
        }
        placed.push_back(layerAt(unframed, placeIn(aspect, out.width(), out.height(), framing == Framing::Fill)));
        return placed;
    }
    // Readback pictures handed out before: one nothing else holds any more
    // is filled again rather than a new one allocated (and faulted in).
    std::vector<QImage> spares;
    // Its pixel aspect is written while only this list holds it (a shared
    // picture would copy itself to take it).
    QImage readbackImage(int w, int h) {
        const QString aspect = QString::number(framed() ? 1.0 : pixelAspect(), 'g', 16);
        for (QImage &spare : spares)
            if (spare.width() == w && spare.height() == h && spare.isDetached()) {
                if (spare.text(QStringLiteral("omaedit.pixelAspect")) != aspect) spare.setText(QStringLiteral("omaedit.pixelAspect"), aspect);
                // New contents, a new cacheKey(): nothing keyed by the old
                // picture takes this one for it.
                spare.bits();
                return spare;
            }
        QImage made(w, h, QImage::Format_RGBA32FPx4);
        if (made.isNull()) return made;
        made.setText(QStringLiteral("omaedit.pixelAspect"), aspect);
        if (spares.size() >= 4) spares.erase(spares.begin());
        spares.push_back(made);
        return made;
    }
    std::shared_ptr<const engine::ColourProgram> deliveryProgram;

    int sourceFor(const QString &role) {
        touched.insert(role);
        auto it = sources.find(role);
        if (it != sources.end()) return it->second;
        const int id = engine.createSource();
        sources.emplace(role, id);
        return id;
    }
    void releaseUnused() {
        for (auto it = sources.begin(); it != sources.end();) {
            if (touched.contains(it->first)) { ++it; continue; }
            engine.destroySource(it->second);
            shown.erase(it->first);
            it = sources.erase(it);
        }
        touched.clear();
    }

    double pixelAspect() const { return project.value("pixelAspect", 1.).toDouble(); }
    // The timeline's whole frame, which `size` is a smaller drawing of while
    // the viewer plays at its own size ("fullSize", set beside "drawScale").
    // What a clip stores in pixels (its stabilisation) is in pixels of this.
    QSize wholeSize() const {
        const QSize whole = project.value("fullSize").toSize();
        return whole.isValid() && !whole.isEmpty() ? whole : size;
    }
    Scaling scalingOf(const QVariantMap &e) const {
        return scalingFor(e.value("scaling").toString(), project.value("scaling").toString());
    }

    // A nested timeline: the saved timeline its project file carries,
    // kept while the file is unchanged.
    struct Nest {
        QVariantList entries, tracks;
        QSize size;
        double fps = 25, aspect = 1;
        int frames = 0;
        QDateTime modified;
    };
    // (Held by shared pointers: drawing a nest reaches nest() again for a
    // nest inside it, which may empty or refill this list meanwhile.)
    QHash<QString, std::shared_ptr<const Nest>> nests;
    // While drawing (or checking) a nested timeline: its size, pixel shape
    // and rate stand in for the timeline's.
    struct Inside {
        State &s;
        QSize size;
        double fps;
        QVariantMap project;
        Inside(State &state, const Nest &n) : s(state), size(state.size), fps(state.fps), project(state.project) {
            s.size = n.size;
            s.fps = n.fps;
            s.project["pixelAspect"] = n.aspect;
            // A nest is drawn at its own whole size whatever the viewer draws
            // at: its effects' pixels are not scaled.
            s.project.remove("drawScale");
            s.project.remove("fullSize");
        }
        ~Inside() {
            s.size = size;
            s.fps = fps;
            s.project = project;
        }
    };
    std::shared_ptr<const Nest> nest(const QString &path, QString *error) const {
        const QFileInfo info(path);
        auto it = nests.constFind(path);
        if (it != nests.constEnd() && (*it)->modified == info.lastModified()) return *it;
        const QVariantMap doc = ProjectSafety::readDocument(path, error);
        if (doc.isEmpty()) { if (error && error->isEmpty()) *error = QStringLiteral("Cannot open nested sequence: ") + path; return nullptr; }
        Nest n;
        n.entries = doc.value("entries").toList();
        // The project's own media settings (an interpreted rate, a colour
        // space) reach the nest's clips of the same media, as they always have.
        for (auto &v : n.entries) {
            auto e = v.toMap();
            const QVariantMap settings = mediaSources.value(e.value("path").toString()).toMap();
            if (!settings.isEmpty()) { e["sourceMeta"] = settings; v = e; }
        }
        n.tracks = doc.value("tracks").toList();
        const QVariantMap profile = doc.value("profile").toMap();
        n.size = QSize(profile.value("width", 0).toInt(), profile.value("height", 0).toInt());
        const double num = profile.value("fpsNum", 25).toDouble(), den = profile.value("fpsDen", 1).toDouble();
        n.fps = den > 0 ? num / den : 25;
        const double sarNum = profile.value("sarNum", 1).toDouble(), sarDen = profile.value("sarDen", 1).toDouble();
        n.aspect = sarDen > 0 ? sarNum / sarDen : 1;
        for (const auto &v : n.entries) {
            const auto e = v.toMap();
            n.frames = std::max(n.frames, e.value("start").toInt() + e.value("frames").toInt());
        }
        n.modified = info.lastModified();
        auto *self = const_cast<State *>(this);
        if (self->nests.size() > 32) self->nests.clear();
        auto kept = std::make_shared<const Nest>(std::move(n));
        self->nests.insert(path, kept);
        return kept;
    }
    // A clip whose grade does something: its picture is graded on the CPU,
    // as the clip's grade filter does it, until the grade runs on the engine.
    static bool graded(const QVariantMap &e) {
        if (e.value("bypass").toStringList().contains("grade")) return false;
        const QVariantMap grade = e.value("grade").toMap();
        if (grade.isEmpty() || grade.value("bypass").toBool()) return false;
        for (const auto &node : grade.value("nodes").toList())
            if (node.toMap().value("enabled", true).toBool()) return true;
        return grade.contains("management") || grade.contains("outputNode") || grade.contains("viewNode");
    }
    // A grade the engine does itself: one node of primaries and curves,
    // without mid detail. The node at this frame, or empty.
    static QVariantMap engineGrade(const QVariantMap &e, int local) {
        if (!graded(e)) return {};
        const QVariantMap node = GradeEngine::plainNode(e.value("grade").toMap(), local);
        if (node.isEmpty() || DisplayGrade::prepare(node).detail != 0) return {};
        return node;
    }
    // On the CPU: a grade the engine cannot do yet (the display-referred
    // grade path; never on a colour-managed timeline, whose grades are its own).
    bool cpuGraded(const QVariantMap &e, int local) const { return !managed() && graded(e) && engineGrade(e, local).isEmpty(); }
    // The engine's grade step for a node, kept while its tables and matrix stay
    // the same so the engine keeps the tables it has.
    std::shared_ptr<const engine::GradeTables> gradeStep(const QVariantMap &node) {
        const DisplayGrade::Prepared p = DisplayGrade::prepare(node);
        if (lastGrade && lastGrade->channels == p.tables && lastGrade->chroma == p.chroma
            && std::equal(p.matrix.begin(), p.matrix.end(), lastGrade->matrix, [](double a, float b) { return float(a) == b; }))
            return lastGrade;
        auto step = std::make_shared<engine::GradeTables>();
        step->channels = p.tables;
        step->chroma = p.chroma;
        for (int i = 0; i < 9; ++i) step->matrix[i] = float(p.matrix[size_t(i)]);
        lastGrade = step;
        return step;
    }
    std::shared_ptr<const engine::GradeTables> lastGrade;
    SwsContext *rgbConverter = nullptr;
    ~State() { sws_freeContext(rgbConverter); }

    // ------------------------------------------------------------ checks

    QString check(const QVariantList &list, int depth, const QVariantList *trackList = nullptr) const {
        if (!size.isValid() || !std::isfinite(fps) || fps <= 0) return QStringLiteral("The timeline has no picture size or frame rate.");
        if (depth > kMaxDepth) return QStringLiteral("Nested clips go too many levels deep.");
        const QVariantList &lanes = trackList ? *trackList : depth ? QVariantList{} : tracks;
        for (const auto &v : list) {
            const auto e = v.toMap();
            if (hidden(e, lanes)) continue;
            const QString name = e.value("name", e.value("path")).toString();
            const auto bypass = e.value("bypass").toStringList();
            const auto off = [&](const char *key) { return bypass.contains(QLatin1String(key)); };
            if (const QString effect = effectsUnsupported(e.value("videoEffects").toList()); !effect.isEmpty())
                return name + QStringLiteral(": the ") + effect + QStringLiteral(" effect");
            if (managed()) {
                // As the colour-managed renderer asks of every clip: OCIO
                // management, valid, and an input transform it can make.
                const QVariantMap m = managementFor(e);
                if (const QString problem = ColourPipeline::validate(m); !problem.isEmpty()) return name + QStringLiteral(": ") + problem;
                QString why;
                if (!inputProgram(e, &why)) return name + QStringLiteral(": its input transform (") + why + QLatin1Char(')');
                const QVariantMap grade = e.value("grade").toMap();
                if (!off("grade") && gradeActive(grade)) {
                    QVariantMap shown = grade;
                    shown.remove("viewNode");
                    if (!gradePlan(shown, 0, size.width(), &why)) return name + QStringLiteral(": its grade (") + why + QLatin1Char(')');
                }
            } else if (!off("grade") && e.value("grade").toMap().value("management").toMap().value("enabled").toBool()) {
                return name + QStringLiteral(": colour-managed grade");
            }
            if (!off("stab") && !e.value("stab").toMap().isEmpty()) {
                QString why;
                if (!stabilised(e, &why)) return name + QStringLiteral(": stabilisation (") + why + QLatin1Char(')');
            }
            if (!off("blend") && !blendOf(e.value("blend").toInt())) return name + QStringLiteral(": this blend mode");
            // The clip stack's own order, with its video effects after the grade.
            auto order = e.value("effectOrder").toStringList();
            order.removeAll(QStringLiteral("effects"));
            const auto effectsAt = e.value("effectOrder").toStringList().indexOf(QStringLiteral("effects"));
            const auto gradeAt = e.value("effectOrder").toStringList().indexOf(QStringLiteral("grade"));
            if ((!order.isEmpty() && order.mid(0, 6) != QStringList{"flip", "color", "grade", "crop", "rect", "opacity"})
                || (effectsAt >= 0 && effectsAt != gradeAt + 1))
                return name + QStringLiteral(": a changed effect order");
            if (e.value("frameBlend").toInt() == FrameSampling::Blend && cpuGraded(e, 0))
                return name + QStringLiteral(": frame blending with this grade");
            if (!FrameSampling::valid(e.value("frameBlend").toInt())) return name + QStringLiteral(": invalid frame sampling");
            if (e.value("adjust").toBool() && cpuGraded(e, 0)) return name + QStringLiteral(": this grade on an effect layer");
            if (cpuGraded(e, 0) && (!e.value("title").toString().isEmpty() || e.value("path").toString().startsWith("color:")
                              || !e.value("compoundJson").toString().isEmpty()))
                return name + QStringLiteral(": a grade on a generated or nested clip");
            if (e.contains("generator") && !QStringList{"solid", "adjust", "bars", "noise", "count"}.contains(e.value("generator").toString()))
                return name + QStringLiteral(": this generator");
            const QString path = e.value("offline").toBool() ? QString() : e.value("path").toString();
            if (ProjectFile::isProject(path)) {
                QString why;
                const auto n = nest(path, &why);
                if (!n) return name + QStringLiteral(": ") + why;
                if (!n->size.isValid() || n->fps <= 0) return name + QStringLiteral(": a nested timeline with no picture size");
                auto *self = const_cast<State *>(this);
                const Inside inside(*self, *n);
                const auto inner = check(n->entries, depth + 1, &n->tracks);
                if (!inner.isEmpty()) return inner;
            }
            if (path.endsWith(".webp", Qt::CaseInsensitive)) return name + QStringLiteral(": WebP pictures");
            if (const QString lut = e.value("sourceMeta").toMap().value("inputLut").toString(); !lut.isEmpty()) {
                QString why;
                if (!loadLookupTable(lut, false, &why)) return name + QStringLiteral(": input LUT (") + why + QLatin1Char(')');
            }
            const auto style = e.value("mixStyle").toString();
            if (!transitionStyle(style)) return name + QStringLiteral(": this transition");
            const auto compound = e.value("compoundJson").toString();
            if (!compound.isEmpty()) {
                const auto inner = check(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(), depth + 1);
                if (!inner.isEmpty()) return inner;
            }
        }
        return {};
    }

    // ------------------------------------------------------------ timing

    const RetimeMap *timingFor(const QVariantMap &entry, QString *error) {
        if (!entry.contains("retime")) return nullptr;
        const auto definition = entry.value("retime").toMap();
        if (definition.isEmpty() || !qFuzzyCompare(entry.value("speed", 1.).toDouble(), 1.)) {
            *error = QStringLiteral("The clip has an invalid speed ramp.");
            return nullptr;
        }
        for (const auto &cached : std::as_const(timingMaps))
            if (cached.first == definition) return &cached.second;
        RetimeMap map;
        if (!map.set(definition, error)) return nullptr;
        timingMaps.append({definition, map});
        return &timingMaps.last().second;
    }

    // Where in the source the clip's picture at `local` is, in seconds; the
    // same choice the linear renderer makes (recorded-picture conform, speed
    // ramps, reverse, freeze, interpreted rates).
    // Where in the source the clip's picture at `local` is, in seconds; with
    // frame blending, a second picture and how much of it to mix in.
    struct Pick { double seconds = 0, other = 0, mix = 0; };
    std::optional<Pick> sourcePick(const QVariantMap &e, int local, const QVariantMap &facts, QString *error) {
        const double speed = e.value("speed", 1.).toDouble();
        const int mode = e.value("frameBlend").toInt();
        const bool explicitPictures = FrameSampling::explicitPictures(mode);
        std::optional<double> origin;
        if (!SourceTiming::readOrigin(e, origin)) { *error = QStringLiteral("The clip has an invalid source origin."); return {}; }
        const bool nativePictures = explicitPictures || origin.has_value();
        const int engineMode = origin && !e.contains("retime") && mode == FrameSampling::Original ? FrameSampling::Nearest : mode;
        const int outputPosition = e.contains("freeze") ? e.value("freeze").toInt() : e.value("in").toInt() + local;
        double position = outputPosition;
        if (!nativePictures && !e.contains("freeze") && speed < 0) position = e.value("length").toInt() - 1 - position;
        const RetimeMap *timing = timingFor(e, error);
        if (!error->isEmpty()) return {};
        RetimeMap conform;
        if (timing) {
            const auto point = timing->sample(outputPosition);
            if (!point.valid) { *error = QStringLiteral("The speed ramp frame is outside its timing map."); return {}; }
            position = point.source;
        }
        const QString path = e.value("path").toString();
        if (isStill(path)) return Pick{};
        const double sourceFps = facts.value("fps", fps).toDouble();
        const double factor = timelineFactor(e.value("sourceMeta").toMap());
        const int nativeFrames = facts.value("frameCount").toInt();
        if (nativePictures && !timing) {
            if (!conform.set(RetimeMap::conform(nativeFrames, sourceFps, fps, speed, factor, origin), error)) return {};
            timing = &conform;
        }
        double seconds = position * std::abs(speed) * factor / fps;
        if (!timing && qFuzzyCompare(std::abs(speed), 1.0) && sourceFps > 0) {
            // Played as recorded (or at an interpreted rate): the nearest
            // source frame, as speed maps pick it (a half-way frame rounds up).
            seconds = std::floor(seconds * sourceFps + .5 + 1e-9) / sourceFps;
        }
        Pick pick;
        const bool blend = timing && FrameSampling::interpolates(mode);
        if (timing) {
            if (!std::isfinite(sourceFps) || sourceFps <= 0 || !std::isfinite(factor) || factor <= 0
                || timing->sourceLength() * factor * sourceFps / fps > 2147483646.) {
                *error = QStringLiteral("The speed ramp has an invalid source frame rate.");
                return {};
            }
            const bool interpreted = !qFuzzyCompare(factor, 1.0);
            auto sample = nativePictures ? timing->pictureSample(outputPosition, fps / factor, sourceFps, engineMode)
                : blend ? timing->imageSample(outputPosition, fps, interpreted ? fps : sourceFps)
                : RetimeMap::ImageSample{timing->sourceFrame(outputPosition, fps, interpreted ? fps : sourceFps), -1, 0};
            if (sample.first < 0) { *error = QStringLiteral("The speed ramp has an invalid source frame rate."); return {}; }
            if (nativePictures) {
                if (nativeFrames < 1) { *error = QStringLiteral("Cannot determine the source picture bounds."); return {}; }
                sample.first = qMin(sample.first, nativeFrames - 1);
                sample.second = qMin(sample.second, nativeFrames - 1);
            } else if (interpreted) {
                // As saved legacy maps select: through the timeline rate first.
                const int last = qMax(0, int(std::ceil(timing->sourceLength() * factor * sourceFps / fps)) - 1);
                sample.first = qBound(0, qRound(sample.first * factor * sourceFps / fps), last);
                if (sample.mix > 0) sample.second = qBound(0, qRound(sample.second * factor * sourceFps / fps), last);
            }
            seconds = sample.first / sourceFps;
            if (sample.mix > 0 && sample.second >= 0) { pick.other = sample.second / sourceFps; pick.mix = sample.mix; }
        }
        pick.seconds = std::max(0., seconds);
        return pick;
    }
    // Timeline::interpretationFactor, without the timeline header.
    static double timelineFactor(const QVariantMap &settings) {
        const double forced = settings.value(QStringLiteral("forceFps")).toDouble();
        const double source = settings.value(QStringLiteral("sourceFps"), settings.value(QStringLiteral("fps"))).toDouble();
        if (forced <= 0.0 || source <= 0.0 || qFuzzyCompare(forced, source)) return 1.0;
        return forced / source;
    }

    // ------------------------------------------------------------ pictures

    struct Shown {
        int source = -1;
        int width = 0, height = 0;
        double aspect = 1.0;  // the picture's pixel aspect
        bool encoded = false; // holds encoded values for its clip's input transform (managed)
    };

    // ------------------------------------------------------------ colour management

    // A colour-managed timeline (OCIO, for the project or any clip): every
    // picture goes to the engine as encoded, its clip's input transform (source
    // LUT, then its colour space into the working one) turns it into linear
    // working light, everything after works there, and the frame comes back
    // as a working frame for the viewer's and the export's own transforms.
    bool managed() const { return managedTimeline; }
    // What a frame shows: the timeline, its pictures before the clips'
    // corrections (the before/after view), or a grade's view node (matte).
    TimelineRenderer::Mode mode = TimelineRenderer::Mode::Normal;
    // A key's temporal matte smoothing for the clip being drawn: its
    // neighbouring frames' brightness and mean alpha (see clipLayer).
    std::shared_ptr<const engine::FilterImage> temporal;
    float temporalStrength = 0;
    bool neighbourPass = false;  // drawing those neighbours: no smoothing of their own
    // A clip with its corrections switched off, as the before view shows it:
    // its picture through its input transform, fitted, nothing else.
    static QVariantMap beforeOf(QVariantMap e) {
        QStringList bypass = e.value("bypass").toStringList();
        bypass << "flip" << "color" << "grade" << "crop" << "rect" << "opacity" << "blend" << "lens" << "stab";
        e["bypass"] = bypass;
        e["videoEffects"] = QVariantList{};
        e["fadeIn"] = 0;
        e["fadeOut"] = 0;
        return e;
    }
    bool managedTimeline = false;  // as LinearRenderer::enabled decides it, set with the timeline
    // A colour-managed clip's grade as an engine grade plan at `local`, for
    // its frame-sized picture; kept while the grade (and, when it moves, the
    // frame) stays the same. Null when it does nothing; null with `error`
    // when it cannot be made at this frame, or cannot run on this engine.
    struct Planned { QVariantMap grade; int width = 0, frame = -1; quint64 luts = 0; std::shared_ptr<const engine::GradePlan> plan; QString error; };
    QHash<QString, Planned> plans;
    std::shared_ptr<const engine::GradePlan> managedGrade(const QVariantMap &e, int local, const QString &role, QString *error) {
        if (!managed() || e.value("bypass").toStringList().contains("grade")) return nullptr;
        QVariantMap grade = e.value("grade").toMap();
        if (!gradeActive(grade)) return nullptr;
        if (mode != TimelineRenderer::Mode::Matte) grade.remove("viewNode");  // a matte view is the viewer's, not the picture's
        const int frame = gradeAnimated(grade.value("nodes").toList()) ? local : -1;
        Planned &kept = plans[role];
        const quint64 luts = ColourPipeline::lutGeneration();
        if (!kept.plan || kept.grade != grade || kept.width != size.width() || kept.frame != frame || kept.luts != luts) {
            kept.error.clear();
            kept.plan = gradePlan(grade, std::max(0, local), size.width(), &kept.error);
            // Its programs made ready for this engine: a plan one of them
            // cannot run in is no plan (the clip would be drawn ungraded).
            if (kept.plan)
                for (const auto &o : kept.plan->ops) {
                    std::string failed;
                    if (o.program && !engine.prepareProgram(*o.program, &failed)) {
                        kept.error = QString::fromStdString(failed);
                        if (kept.error.isEmpty()) kept.error = QStringLiteral("a colour transform this engine cannot run");
                        kept.plan.reset();
                        break;
                    }
                }
            kept.luts = luts;
            kept.grade = grade;
            kept.width = size.width();
            kept.frame = frame;
        }
        if (!kept.plan && error) *error = kept.error;
        return kept.plan;
    }
    // A clip's colour management, as the colour-managed renderer reads it:
    // its grade's, or the defaults (titles as sRGB).
    static QVariantMap managementFor(const QVariantMap &e) {
        return ColourPipeline::effective(e.value("grade").toMap(), !e.value("title").toString().isEmpty());
    }
    std::shared_ptr<const engine::ColourProgram> inputProgram(const QVariantMap &e, QString *error) const {
        const QVariantMap m = managementFor(e);
        static const bool trace = qEnvironmentVariableIsSet("OMAEDIT_RENDERER_TRACE");
        if (trace)
            qInfo("engine input transform for %s: %s", qPrintable(e.value("name", e.value("path")).toString()),
                  qPrintable(ColourPipeline::spaceName(m.value("input").toString(), m.value("inputGamma").toString())));
        return ColourPipeline::engineInputProgram(e.value("sourceMeta").toMap().value("inputLut").toString(),
                                                  ColourPipeline::spaceName(m.value("input").toString(), m.value("inputGamma").toString()),
                                                  QStringLiteral("oma_interchange"), error);
    }
    // A CPU picture for the engine, encoded when the timeline is managed.
    engine::CpuFrame upload(const QImage &image) const {
        engine::CpuFrame f{image.width(), image.height(), int(image.bytesPerLine()), engine::PixelFormat::RGBA, image.constBits()};
        f.encoded = managed();
        return f;
    }
    // Two pictures of one clip before they are mixed (frame blending): each
    // through its input transform first, as the managed renderer mixes
    // working frames.
    bool inputStage(const QVariantMap &e, int source, QString *error) {
        if (!managed()) return true;
        auto program = inputProgram(e, error);
        std::string why;
        if (!program || !engine.prepareProgram(*program, &why)) {
            if (error->isEmpty()) *error = QString::fromStdString(why);
            return false;
        }
        engine::VideoFilter f;
        f.type = engine::VideoFilterType::Program;
        f.program = std::move(program);
        engine.setSourceFilters(source, {f});
        engine.renderSourceFilters(source, 0);
        return true;
    }

    struct Dims { int w = 0, h = 0; double aspect = 1.0; };
    QHash<QString, Dims> pictureSizes;

    // ------------------------------------------------------------ layers fetched side by side
    //
    // Who owns what. A renderer, its media pool and its engine belong to one
    // thread, the render thread (whichever calls render()); none of them is
    // touched by any other. What other threads do is read decoders: before a
    // frame is drawn, the pictures it will ask for are worked out (foresee),
    // the render thread picks each one's source from the media pool, and a
    // small pool of threads shared by the program's renderers reads them, each
    // source by one thread, under the request (RenderScope) of the frame that
    // asked. The render thread then draws the frame as it always did, taking
    // a fetched picture where it would have decoded one. And while the engine
    // finishes that frame, the pictures of the frame expected next are begun
    // the same way.
    //
    // - A Fetch is written by the one thread that took it from its batch, and
    //   read by the render thread only after Batch::wait() has returned.
    // - A batch never holds one MediaSource twice, so a decoder is read by one
    //   thread at a time; and no source can close under its reader, because
    //   everything that touches the media pool first waits for the batch in
    //   flight (settle()).
    // - A fetched Picture holds its own reference on the decoder's frame: the
    //   decoder reading on does not change it.
    // - Pool threads make no engine calls and read nothing of the renderer.
    //
    // What is fetched is only ever what the frame would have decoded: a
    // picture foreseen wrongly is not used, one not foreseen is decoded in
    // its turn as before, and a fetch that failed for a frame other than the
    // one drawn (its request was cancelled) is read again by that frame.
    struct Fetch {
        QString read;        // the file
        double seconds = 0;  // where in it
        int space = 0;       // how its Y'CbCr is read (the clip's input setting), 0 as the file says
        int small = 1;       // made a half or a quarter of its size before it is sent up (2, 4), or not (1)
        MediaSource *from = nullptr;
        std::shared_ptr<Picture> picture;
        bool ok = false;
        QString error;
    };
    struct Batch {
        int position = -1;
        quint64 generation = 0;
        bool ahead = false;
        std::shared_ptr<RenderRequest> request;
        std::vector<Fetch> items;  // not resized once work has begun
        std::atomic<int> next{0};
        std::mutex mutex;
        std::condition_variable finished;
        int done = 0;
        // Reads pictures until none is left to take: run by pool threads, and
        // by the render thread while it waits.
        void work() {
            for (;;) {
                const int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= int(items.size())) return;
                Fetch &f = items[size_t(i)];
                {
                    // The reads belong to the request that asked: cancelled
                    // with it, their failures reported by whoever draws.
                    RenderScope scope(request);
                    f.ok = f.from->read(f.seconds, *f.picture, &f.error);
                    if (f.ok) readAs(*f.picture, f.space);
                    if (f.ok && f.small > 1) f.picture->makeSmaller(f.small);
                }
                {
                    std::lock_guard lock(mutex);
                    ++done;
                }
                finished.notify_all();
            }
        }
        void wait() {
            work();
            std::unique_lock lock(mutex);
            finished.wait(lock, [&] { return done >= int(items.size()); });
        }
        bool whole() const {
            return std::all_of(items.begin(), items.end(), [](const Fetch &f) { return f.ok; });
        }
    };
    // A decoded picture read as the clip's input settings say: how its
    // Y'CbCr is read when the file says otherwise or nothing.
    static void readAs(Picture &p, int space) {
        if (p.kind == Picture::Kind::Yuv && (space == 601 || space == 709 || space == 2020))
            p.yuv.matrix = space == 601 ? engine::YuvMatrix::BT601 : space == 2020 ? engine::YuvMatrix::BT2020 : engine::YuvMatrix::BT709;
    }
    // The threads that read: enough for a frame of many layers, each of which
    // mostly waits on its decoder's own threads (or on the GPU's decoder).
    static QThreadPool &fetchers() {
        static QThreadPool *pool = [] {
            auto *made = new QThreadPool;
            made->setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 2, 16));
            made->setObjectName(QStringLiteral("layer fetch"));
            return made;
        }();
        return *pool;
    }
    static std::atomic_bool &sideBySide() {
        static std::atomic_bool on{qEnvironmentVariable("OMAEDIT_SERIAL_LAYERS") != QLatin1String("1")};
        return on;
    }
    static std::atomic_bool &fetchesAhead() {
        static std::atomic_bool on{qEnvironmentVariable("OMAEDIT_LAYERS_AHEAD") != QLatin1String("0")};
        return on;
    }
    std::vector<std::shared_ptr<Batch>> ready;  // this frame's pictures, fetched
    std::shared_ptr<Batch> ahead;               // begun for the frame expected next; may be in flight
    quint64 generation = 0;                     // of the timeline as configured
    int lastPosition = -1;                      // the frame drawn before this one
    // Waits for the batch in flight. Before anything touches the media pool.
    void settle() {
        if (ahead) ahead->wait();
    }
    Fetch *fetched(const QString &read, double seconds, int space, int small) {
        for (const auto &batch : ready)
            for (Fetch &f : batch->items)
                if (f.seconds == seconds && f.space == space && f.small == small && f.read == read) return &f;
        return nullptr;
    }
    struct Ask { QString read; double seconds; int space; int small; };
    // Picks the asks' sources and has them read; `mine`: the render thread
    // reads too and waits for them all.
    std::shared_ptr<Batch> launch(const std::vector<Ask> &asks, int position, bool mine) {
        auto batch = std::make_shared<Batch>();
        batch->position = position;
        batch->generation = generation;
        batch->ahead = !mine;
        // This frame's reads are its request's own. Reads begun for the frame
        // to come stop when this frame's request is given up, but what they
        // report stays with them: that frame, when it is drawn, reads again
        // whatever did not arrive, and reports it then.
        if (mine) batch->request = RenderScope::current();
        else batch->request = std::make_shared<RenderRequest>([parent = RenderScope::current()] { return parent && parent->cancelled(); });
        for (const Ask &a : asks) {
            if (fetched(a.read, a.seconds, a.space, a.small)) continue;
            if (std::any_of(batch->items.begin(), batch->items.end(),
                            [&](const Fetch &f) { return f.seconds == a.seconds && f.space == a.space && f.small == a.small && f.read == a.read; }))
                continue;
            QString ignored;
            MediaSource *from = pool.sourceAt(a.read, a.seconds, &ignored);
            // (None, or one this batch reads already, more parts of one file
            // than it may have decoders: that picture is read in its turn.)
            if (!from || std::any_of(batch->items.begin(), batch->items.end(), [&](const Fetch &f) { return f.from == from; })) continue;
            Fetch f;
            f.read = a.read;
            f.seconds = a.seconds;
            f.space = a.space;
            f.small = a.small;
            f.from = from;
            f.picture = std::make_shared<Picture>();
            batch->items.push_back(std::move(f));
        }
        if (batch->items.empty()) return nullptr;
        const int helpers = std::min(int(batch->items.size()) - (mine ? 1 : 0), fetchers().maxThreadCount());
        for (int k = 0; k < helpers; ++k) fetchers().start([batch] { batch->work(); });
        if (mine) batch->wait();
        return batch;
    }
    // Before frame `position` is drawn: its pictures made ready side by side.
    void fetchFor(int position) {
        settle();
        ready.clear();
        if (ahead) {
            // Begun for this very frame of this very timeline, and all there:
            // taken. Otherwise let go, its sources free again.
            if (ahead->position == position && ahead->generation == generation && ahead->whole() && ahead->request->error().isEmpty())
                ready.push_back(ahead);
            else pool.unclaim();
            ahead.reset();
        }
        if (!sideBySide() || neighbourPass) return;
        StageTimer::Scope timed("render.ready");
        std::vector<Ask> asks;
        foresee(entries, position, 0, QString(), nullptr, asks);
        // One picture gains nothing from another thread.
        if (asks.size() + (ready.empty() ? 0 : 1) < 2) return;
        if (auto batch = launch(asks, position, true)) ready.push_back(std::move(batch));
    }
    // After frame `position` is composited: the next one's pictures begun,
    // while the engine finishes this one and it is read back. Only for a
    // frame that follows the one before (playing, an export, a render), and
    // not for a request that has been given up.
    void fetchAhead(int position) {
        ready.clear();
        const bool follows = lastPosition >= 0 && position == lastPosition + 1;
        lastPosition = position;
        if (!follows || !sideBySide() || !fetchesAhead() || neighbourPass || RenderScope::cancelled()) return;
        std::vector<Ask> asks;
        foresee(entries, position + 1, 0, QString(), nullptr, asks);
        if (asks.empty()) return;
        ahead = launch(asks, position + 1, false);
    }
    // The decoded pictures frame `position` of `list` will ask for, as
    // sequence() and picture() come to them, without drawing anything. (A
    // nested timeline's, an optical-flow pair and a key's neighbouring frames
    // are left to be read in their turn.)
    void foresee(const QVariantList &list, int position, int depth, const QString &prefix, const QVariantList *trackList,
                 std::vector<Ask> &asks) {
        if (depth > kMaxDepth) return;
        const QVariantList &lanes = trackList ? *trackList : depth ? QVariantList{} : tracks;
        QMap<int, QVariantList> active;
        // The frame first: what is drawn is asked about (hidden() may look
        // at the file), not every clip of the timeline on every frame.
        for (const auto &v : list) {
            const auto e = v.toMap();
            const int start = e.value("start").toInt() - e.value("mixIn").toInt();
            if (position < start || position >= e.value("start").toInt() + e.value("frames").toInt() || hidden(e, lanes)) continue;
            active[e.value("track", e.value("lane", 0)).toInt()] << e;
        }
        auto lowest = active.cbegin();
        for (auto it = active.cend(); it != active.cbegin();) {
            --it;
            if (wouldHide(it.value(), position, prefix + QStringLiteral("t%1").arg(it.key()))) { lowest = it; break; }
        }
        for (auto it = lowest; it != active.cend(); ++it) {
            const QString track = prefix + QStringLiteral("t%1").arg(it.key());
            const auto &clips = it.value();
            if (clips.size() > 2 || (clips.size() == 1 && clips.first().toMap().value("adjust").toBool())) continue;
            for (int k = 0; k < clips.size(); ++k) {
                const auto e = clips[k].toMap();
                foreseeClip(e, position - e.value("start").toInt() + e.value("mixIn").toInt(),
                            track + (k ? QStringLiteral("/b") : QStringLiteral("/a")), depth, asks);
            }
        }
    }
    // hidesBelow(), from what is known before the picture is read: its size
    // as last drawn, or as its file gives it.
    bool wouldHide(const QVariantList &clips, int position, const QString &track) {
        if (mode != TimelineRenderer::Mode::Normal || clips.size() != 1) return false;
        const auto e = clips.first().toMap();
        if (!solidPicture(e)) return false;
        QString ignored;
        const QString own = e.value("path").toString();
        Dims dims{size.width(), size.height(), pixelAspect()};
        if (!own.startsWith(QLatin1String("color:"))) {
            if (!QFileInfo::exists(own)) return false;
            MediaSource *media = pool.source(pictureSource ? pictureSource(own) : own, &ignored);
            if (!media) return false;
            const auto facts = media->information();
            if (facts.value("alpha", true).toBool()) return false;
            const auto known = pictureSizes.constFind(track + QStringLiteral("/a"));
            dims = known != pictureSizes.constEnd() ? *known : Dims{facts.value("width").toInt(), facts.value("height").toInt(), 1.0};
        }
        if (dims.w <= 0 || dims.h <= 0) return false;
        const int local = position - e.value("start").toInt() + e.value("mixIn").toInt();
        const auto l = placed(e, local, Shown{0, dims.w, dims.h, dims.aspect});
        constexpr float slack = 1e-3f;
        return l && l->opacity >= 1.f && l->blend == engine::BlendMode::Normal && std::abs(l->rotation) <= 1e-6f
            && l->x <= slack && l->y <= slack && l->x + l->w >= size.width() - slack && l->y + l->h >= size.height() - slack;
    }
    // What picture() would decode for the clip, were it asked now: nothing
    // for a picture its source holds already.
    void foreseeClip(const QVariantMap &e, int local, const QString &role, int depth, std::vector<Ask> &asks) {
        const QString own = e.value("path").toString();
        const bool standIn = pictureSource && pictureSource(own) != own;
        const bool missing = e.value("offline").toBool() || (QFileInfo(own).isAbsolute() && !QFileInfo::exists(own));
        if (missing && !standIn) return;
        if (ProjectFile::isProject(own)) return;
        if (const auto compound = e.value("compoundJson").toString(); !compound.isEmpty()) {
            const double speed = std::abs(e.value("speed", 1.).toDouble());
            foresee(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(), qRound((e.value("in").toInt() + local) * speed),
                    depth + 1, role + QLatin1Char('/'), nullptr, asks);
            return;
        }
        if (!e.value("title").toString().isEmpty() || !generatorKind(own).isEmpty() || own.startsWith(QLatin1String("color:"))) return;
        const QString read = pictureSource ? pictureSource(own) : own;
        QString ignored;
        MediaSource *media = pool.source(read, &ignored);
        if (!media) return;
        const auto facts = media->information();
        if (!facts.value("displayMatrixSupported", true).toBool()) return;
        const QString file = read + QLatin1Char('|') + pool.stamp(read);
        const auto chosen = sourcePick(e, local, facts, &ignored);
        if (!chosen) return;
        const bool grade = cpuGraded(e, local);
        const int space = e.value("sourceMeta").toMap().value("inputSpace").toInt();
        const auto wanted = [&](const QString &as, const QString &key, double at, int small = 1) {
            const auto held = shown.find(as);
            if (held == shown.end() || held->second != key) asks.push_back({read, at, space, small});
        };
        if (chosen->mix > 0 && e.value("frameBlend").toInt() == FrameSampling::OpticalFlow) return;
        if (chosen->mix > 0 && !grade) {
            wanted(role + QStringLiteral("/blendA"), file + QLatin1Char('@') + QString::number(chosen->seconds, 'g', 17), chosen->seconds);
            wanted(role + QStringLiteral("/blendB"), file + QLatin1Char('@') + QString::number(chosen->other, 'g', 17), chosen->other);
            return;
        }
        const int reduce = grade ? 1 : reduceFor(e, local, facts);
        wanted(role, file + QLatin1Char('@') + QString::number(chosen->seconds, 'g', 17) + (grade ? QStringLiteral("#%1").arg(local) : QString())
                         + (reduce > 1 ? QStringLiteral("/%1").arg(reduce) : QString()),
               chosen->seconds, smallFor(reduce, grade));
    }
    // A picture that is converted small (reduceFor) is made small before it
    // is sent to the engine where that gives the engine's own reduction: on a
    // colour-managed timeline, whose pictures the engine keeps as encoded,
    // so that the mean of a block is the same taken before the conversion or
    // after it. Elsewhere the engine reduces it, in light, as it did.
    int smallFor(int reduce, bool grade) const { return reduce > 1 && !grade && managed() && reducesBeforeUpload() ? reduce : 1; }
    static std::atomic_bool &reductionBeforeUpload() {
        static std::atomic_bool on{qEnvironmentVariable("OMAEDIT_REDUCE_BEFORE_UPLOAD") != QLatin1String("0")};
        return on;
    }
    static bool reducesBeforeUpload() { return reductionBeforeUpload().load(std::memory_order_relaxed); }

    // The clip's picture for `local` in an engine source: decoded media, a
    // solid colour, a title, or a compound clip's own frame.
    std::optional<Shown> picture(const QVariantMap &e, int local, const QString &role, int depth, QString *error) {
        // An offline clip (its file is gone) shows a dark slate until relinked,
        // unless a proxy stands in for it.
        const QString own = e.value("path").toString();
        const bool standIn = pictureSource && pictureSource(own) != own;
        // Missing files inside a nest are not marked offline: look.
        const bool missing = e.value("offline").toBool()
            || (QFileInfo(own).isAbsolute() && !QFileInfo::exists(own));
        const QString path = missing && !standIn ? QStringLiteral("color:#181820") : own;
        const int id = sourceFor(role);
        if (ProjectFile::isProject(path)) {
            const auto n = nest(path, error);
            if (!n) return {};
            // Its frame (or two, mixed) as the colour-managed renderer picks
            // them: the clip's position (reversed or frozen) through its speed,
            // speed ramp or picture timing into the nest's rate.
            const double speed = e.value("speed", 1.).toDouble();
            const int mode = e.value("frameBlend").toInt();
            std::optional<double> origin;
            if (!SourceTiming::readOrigin(e, origin)) { *error = QStringLiteral("The clip has an invalid source origin."); return {}; }
            const bool nativePictures = FrameSampling::explicitPictures(mode) || origin.has_value();
            const int engineMode = origin && !e.contains("retime") && mode == FrameSampling::Original ? FrameSampling::Nearest : mode;
            const int outputPosition = e.contains("freeze") ? e.value("freeze").toInt() : e.value("in").toInt() + local;
            double position = outputPosition;
            if (!nativePictures && !e.contains("freeze") && speed < 0) position = e.value("length").toInt() - 1 - position;
            const RetimeMap *timing = timingFor(e, error);
            if (!error->isEmpty()) return {};
            RetimeMap conform;
            if (timing) {
                const auto point = timing->sample(outputPosition);
                if (!point.valid) { *error = QStringLiteral("The speed ramp frame is outside its timing map."); return {}; }
                position = point.source;
            }
            const double factor = timelineFactor(e.value("sourceMeta").toMap());
            if (nativePictures && !timing) {
                if (!conform.set(RetimeMap::conform(n->frames, n->fps, fps, speed, factor, origin), error)) return {};
                timing = &conform;
            }
            const int last = std::max(0, n->frames - 1);
            int first = std::clamp(int(std::lround(position * std::abs(speed) * n->fps / fps)), 0, last), second = -1;
            double mix = 0;
            if (timing) {
                const auto sample = timing->pictureSample(outputPosition, nativePictures ? fps / factor : fps, n->fps, engineMode);
                if (sample.first < 0) { *error = QStringLiteral("The speed ramp has an invalid source frame rate."); return {}; }
                first = std::min(sample.first, last);
                if (sample.mix > 0) { second = std::min(sample.second, last); mix = sample.mix; }
            }
            // Drawn at its own size and rate, then placed like any picture
            // (as the colour-managed renderer does).
            auto draw = [&](int target, int frame, const QString &inner) {
                std::vector<engine::Layer> layers;
                {
                    const Inside inside(*this, *n);
                    if (!sequence(n->entries, frame, depth + 1, inner, layers, error, &n->tracks)) return false;
                }
                engine.compositeInto(target, n->size.width(), n->size.height(), layers, {0, 0, 0, 0});
                return true;
            };
            if (mix > 0 && mode == FrameSampling::OpticalFlow) {
                // Optical flow: the nest's two frames read back, the picture
                // between them by their motion (as the colour-managed renderer).
                const int a = sourceFor(role + QStringLiteral("/blendA")), b = sourceFor(role + QStringLiteral("/blendB"));
                if (!draw(a, first, role + QStringLiteral("/a/")) || !draw(b, second, role + QStringLiteral("/b/"))) return {};
                engine::FrameF fa, fb;
                if (!engine.readLinear(a, fa) || !engine.readLinear(b, fb)) {
                    *error = QStringLiteral("The engine could not read the nested pictures back.");
                    return {};
                }
                auto asImage = [](const engine::FrameF &f) {
                    QImage image(f.width, f.height, QImage::Format_RGBA32FPx4);
                    for (int y = 0; y < f.height; ++y)
                        std::memcpy(image.scanLine(y), f.rgba.data() + size_t(y) * f.width * 4, size_t(f.width) * 16);
                    return image;
                };
                const QImage one = asImage(fa), two = asImage(fb);
                const auto motion = MotionFlow::cached(one, two, error);
                const QImage between = motion ? motion->interpolate(one, two, mix, error) : QImage();
                if (between.isNull()) {
                    if (error->isEmpty()) *error = QStringLiteral("Could not interpolate the nested pictures.");
                    return {};
                }
                const QImage floats = between.convertToFormat(QImage::Format_RGBA32FPx4);
                engine::CpuFrame f{floats.width(), floats.height(), int(floats.bytesPerLine()), engine::PixelFormat::RGBAF32, floats.constBits()};
                f.encoded = true;  // working light already: kept as it is
                if (!engine.updateSource(id, f)) {
                    *error = QStringLiteral("The engine refused the interpolated nested picture.");
                    return {};
                }
            } else if (mix > 0) {
                const int a = sourceFor(role + QStringLiteral("/blendA")), b = sourceFor(role + QStringLiteral("/blendB"));
                if (!draw(a, first, role + QStringLiteral("/a/")) || !draw(b, second, role + QStringLiteral("/b/"))) return {};
                engine::TransitionStyle mixStyle;
                mixStyle.type = engine::Transition::Fade;
                engine.transitionInto(id, n->size.width(), n->size.height(), a, b, mixStyle, float(mix));
            } else if (!draw(id, first, role + QLatin1Char('/'))) {
                return {};
            }
            shown.erase(role);
            return Shown{id, n->size.width(), n->size.height(), n->aspect};
        }
        const auto compound = e.value("compoundJson").toString();
        if (!compound.isEmpty()) {
            const double speed = std::abs(e.value("speed", 1.).toDouble());
            const int inner = qRound((e.value("in").toInt() + local) * speed);
            std::vector<engine::Layer> layers;
            if (!sequence(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(), inner, depth + 1,
                          role + QLatin1Char('/'), layers, error))
                return {};
            engine.compositeInto(id, size.width(), size.height(), layers, {0, 0, 0, 0});
            shown.erase(role);
            return Shown{id, size.width(), size.height(), pixelAspect()};
        }
        if (!e.value("title").toString().isEmpty()) {
            QVariantMap style;
            for (const auto &key : QStringList{"Font", "Color", "Size", "Place", "Align", "Weight", "Italic", "Bg", "Outline",
                                               "PixelSize", "Tracking", "LineSpacing"}) {
                QString name = key;
                name[0] = name[0].toLower();
                style[name] = e.value("title" + key);
            }
            // The same text and style draw the same picture on every frame:
            // drawn once and kept in its source.
            const QString key = QStringLiteral("title %1x%2 ").arg(size.width()).arg(size.height())
                              + QString::fromUtf8(QJsonDocument::fromVariant(QVariantMap{{"style", style}, {"text", e.value("title")}})
                                                      .toJson(QJsonDocument::Compact));
            if (shown[role] != key) {
                StageTimer::Scope timed("render.title");
                QImage canvas(size, QImage::Format_RGBA8888);
                canvas.fill(Qt::transparent);
                const QVariantMap title{{"group", "title"}, {"style", style}, {"text", e.value("title")}, {"height", size.height()}};
                // Drawn twice, the second kept: a font's first drawing comes
                // out a little differently from every later one, and this one
                // is kept for every frame.
                EditEngine::render(canvas, title, local);
                const QImage drawn = EditEngine::render(canvas, title, local).convertToFormat(QImage::Format_RGBA8888);
                if (!engine.updateSource(id, upload(drawn))) {
                    *error = QStringLiteral("The title could not be drawn.");
                    return {};
                }
                shown[role] = key;
            }
            return Shown{id, size.width(), size.height(), pixelAspect(), managed()};
        }
        if (const QString kind = generatorKind(path); !kind.isEmpty()) {
            // Bars stay put; noise and the countdown change every frame.
            const QString key = QStringLiteral("generated %1 %2x%3").arg(kind).arg(size.width()).arg(size.height())
                              + (kind == QLatin1String("bars") ? QString() : QStringLiteral("#%1").arg(local));
            if (shown[role] != key) {
                bool moves = false;
                const QImage drawn = generatorFrame(kind, size, local, e.value("frames").toInt() + e.value("mixIn").toInt(), fps, &moves);
                if (!engine.updateSource(id, upload(drawn))) {
                    *error = QStringLiteral("The generated picture could not be drawn.");
                    return {};
                }
                shown[role] = key;
            }
            return Shown{id, size.width(), size.height(), pixelAspect(), managed()};
        }
        if (path.startsWith("color:")) {
            const QColor colour = solidColour(path.mid(6));
            if (!colour.isValid()) { *error = QStringLiteral("Invalid solid source colour."); return {}; }
            const QString key = QStringLiteral("solid ") + colour.name(QColor::HexArgb);
            if (shown[role] != key) {
                // Its float values: a managed solid is its colour as encoded, exactly.
                const float rgba[4] = {float(colour.redF()), float(colour.greenF()), float(colour.blueF()), float(colour.alphaF())};
                engine::CpuFrame f{1, 1, 16, engine::PixelFormat::RGBAF32, reinterpret_cast<const uint8_t *>(rgba)};
                f.encoded = managed();
                if (!engine.updateSource(id, f)) {
                    *error = QStringLiteral("The solid colour could not be drawn.");
                    return {};
                }
                shown[role] = key;
            }
            return Shown{id, size.width(), size.height(), pixelAspect(), managed()};
        }
        // The file the pictures come from (a proxy, maybe): what is kept is keyed by it.
        const QString read = pictureSource ? pictureSource(path) : path;
        MediaSource *media = pool.source(read, error);
        if (!media) return {};
        const auto facts = media->information();
        // (A recording that is to be shown turned comes from its decoder
        // upright; only a slanted or scaled display matrix is left.)
        if (!facts.value("displayMatrixSupported", true).toBool()) {
            *error = QStringLiteral("Recordings shown at a slant are not drawn by the engine yet.");
            return {};
        }
        // The file as it is now: a picture kept from it is kept for this
        // file, so one saved again or replaced is read afresh.
        const QString file = read + QLatin1Char('|') + pool.stamp(read);
        const auto chosen = sourcePick(e, local, facts, error);
        if (!chosen) return {};
        const std::optional<double> seconds = chosen->seconds;
        const bool grade = cpuGraded(e, local);
        const QVariantMap meta = e.value("sourceMeta").toMap();
        // A decoded picture, read as the clip's input settings say: how its
        // Y'CbCr is read when the file says otherwise or nothing.
        // (Null with `error` when it cannot be read. A picture fetched for
        // this frame is taken as it is, its failure too: the read was this
        // frame's own, made a little earlier on another thread.)
        auto decode = [&](double at, int small = 1) -> std::shared_ptr<Picture> {
            StageTimer::Scope timed("render.decode");
            const int space = meta.value("inputSpace").toInt();
            if (Fetch *f = fetched(read, at, space, small)) {
                if (!f->ok) { *error = f->error; return nullptr; }
                ++stats.decoded;
                ++stats.fetched;
                return f->picture;
            }
            // (The source for that part of the file: two parts of one
            // recording in a frame each keep a decoder.)
            auto p = std::make_shared<Picture>();
            MediaSource *from = pool.sourceAt(read, at, error);
            if (!from || !from->read(at, *p, error)) return nullptr;
            readAs(*p, space);
            if (small > 1) p->makeSmaller(small);
            ++stats.decoded;
            return p;
        };
        // A picture on the CPU (16-bit, display-encoded) to the engine, with
        // the clip's input LUT, colour and grade first when it is graded here.
        auto send = [&](QImage image, const char *what) {
            if (grade) {
                if (const QString lut = meta.value("inputLut").toString(); !lut.isEmpty()) {
                    image = ColourPipeline::applyLut(image, lut, error).convertToFormat(QImage::Format_RGBA64);
                    if (image.isNull()) return false;
                }
                if (!e.value("bypass").toStringList().contains("color") && !neutralColour(e))
                    image = EditEngine::render(image, {{"group", "color"}, {"base", e}, {"keys", e.value("editKeys")}}, local);
                if (!GradeEngine::correct(image, e.value("grade").toMap(), local, error)) return false;
            }
            engine::CpuFrame frame{image.width(), image.height(), int(image.bytesPerLine()), engine::PixelFormat::RGBA64, image.constBits()};
            frame.encoded = managed();
            if (!engine.updateSource(id, frame)) {
                *error = QStringLiteral("The engine refused a %1 picture.").arg(QLatin1String(what));
                return false;
            }
            return true;
        };
        if (chosen->mix > 0 && e.value("frameBlend").toInt() == FrameSampling::OpticalFlow) {
            // Optical flow: the picture between the two, by their motion.
            const QString key = file + QLatin1Char('@') + QString::number(chosen->seconds, 'g', 17) + QLatin1Char('~')
                              + QString::number(chosen->other, 'g', 17) + QLatin1Char('*') + QString::number(chosen->mix, 'g', 17)
                              + (grade ? QStringLiteral("#%1").arg(local) : QString());
            auto &last = shown[role];
            if (last != key) {
                const auto pa = decode(chosen->seconds);
                const auto pb = pa ? decode(chosen->other) : nullptr;
                if (!pa || !pb) return {};
                const Picture &a = *pa, &b = *pb;
                const QImage first = a.rgba64(&rgbConverter, error), second = b.rgba64(&rgbConverter, error);
                if (first.isNull() || second.isNull()) return {};
                // The pair named by what made it (the file as it is on disk,
                // the two times, how its Y'CbCr is read), so the motion found
                // for it before is taken without reading both pictures through.
                const QByteArray made = QStringLiteral("%1|%2|%3|%4")
                                            .arg(file).arg(chosen->seconds, 0, 'g', 17).arg(chosen->other, 0, 'g', 17)
                                            .arg(meta.value("inputSpace").toInt()).toUtf8();
                const auto motion = MotionFlow::cached(first, second, error, {}, made);
                const QImage between = motion ? motion->interpolate(first, second, chosen->mix, error) : QImage();
                if (between.isNull()) {
                    if (error->isEmpty()) *error = QStringLiteral("Could not interpolate the source pictures.");
                    return {};
                }
                if (!send(between.convertToFormat(QImage::Format_RGBA64), "interpolated")) return {};
                last = key;
                pictureSizes[role] = {a.width, a.height, a.pixelAspect};
            }
            const auto dims = pictureSizes.value(role);
            return Shown{id, dims.w, dims.h, dims.aspect, managed()};
        }
        if (chosen->mix > 0 && !grade) {
            // Frame blending: both source pictures, mixed in linear light.
            const int a = sourceFor(role + QStringLiteral("/blendA")), b = sourceFor(role + QStringLiteral("/blendB"));
            const QString keyA = file + QLatin1Char('@') + QString::number(chosen->seconds, 'g', 17);
            const QString keyB = file + QLatin1Char('@') + QString::number(chosen->other, 'g', 17);
            const QString roles[2] = {role + QStringLiteral("/blendA"), role + QStringLiteral("/blendB")};
            const QString keys[2] = {keyA, keyB};
            const double times[2] = {chosen->seconds, chosen->other};
            const int ids[2] = {a, b};
            for (int k = 0; k < 2; ++k) {
                if (shown[roles[k]] == keys[k]) continue;
                const auto decoded = decode(times[k]);
                if (!decoded) return {};
                const Picture &p = *decoded;
                if (!p.upload(engine, ids[k], managed())) { *error = QStringLiteral("The engine refused a decoded picture."); return {}; }
                if (!inputStage(e, ids[k], error)) return {};
                shown[roles[k]] = keys[k];
                pictureSizes[roles[k]] = {p.width, p.height, p.pixelAspect};
            }
            const auto dims = pictureSizes.value(roles[0]);
            engine::TransitionStyle mixStyle;
            mixStyle.type = engine::Transition::Fade;
            engine.transitionInto(id, dims.w, dims.h, a, b, mixStyle, float(chosen->mix));
            shown.erase(role);
            return Shown{id, dims.w, dims.h, dims.aspect};
        }
        // Drawn small (playing at a half or a quarter), a picture nothing
        // works on at its own size is converted that small.
        const int reduce = grade ? 1 : reduceFor(e, local, facts);
        // A graded picture can change with the clip's frame (animated grades).
        const QString key = file + QLatin1Char('@') + QString::number(*seconds, 'g', 17)
                          + (grade ? QStringLiteral("#%1").arg(local) : QString())
                          + (reduce > 1 ? QStringLiteral("/%1").arg(reduce) : QString());
        auto &last = shown[role];
        if (last != key) {
            const auto decoded = decode(*seconds, smallFor(reduce, grade));
            if (!decoded) return {};
            // (Held by this thread alone by now; a picture two clips share
            // is given each one's own reduction in its turn. One made small
            // already is sent as it is.)
            Picture &p = *decoded;
            p.yuv.reduce = p.reducedBy == reduce ? 1 : reduce;
            if (reduce > 1 && p.reducedBy == reduce) ++stats.small;
            if (reduce > 1 && (p.kind == Picture::Kind::Yuv || p.kind == Picture::Kind::Gpu)) ++stats.reduced;
            if (grade) {
                // The clip's input LUT and colour controls come before its
                // grade, as in the clip's own stack.
                const QImage image = p.rgba64(&rgbConverter, error);
                if (image.isNull() || !send(image, "graded")) return {};
            } else {
                StageTimer::Scope timed("render.upload");
                if (!p.upload(engine, id, managed())) {
                    *error = QStringLiteral("The engine refused a decoded picture.");
                    return {};
                }
            }
            last = key;
            pictureSizes[role] = {p.width, p.height, p.pixelAspect};
        }
        const auto dims = pictureSizes.value(role);
        return Shown{id, dims.w, dims.h, dims.aspect, managed()};
    }

    // OMAEDIT_REDUCE_SOURCES=0: every picture is converted whole, as it was
    // (for comparing the two).
    static std::atomic_bool &sourceReduction() {
        static std::atomic_bool on{qEnvironmentVariable("OMAEDIT_REDUCE_SOURCES") != QLatin1String("0")};
        return on;
    }
    static bool reducesSources() { return sourceReduction().load(std::memory_order_relaxed); }
    // By how much a clip's decoded picture may be converted smaller than it
    // is (1, 2 or 4 each way) without the frame as it is drawn now showing
    // less than it would: only while the viewer plays at a reduced size, and
    // only by as much as leaves a pixel of the picture for every pixel it
    // covers on the frame drawn. Whatever works on the picture at its own
    // size keeps it whole: a key that comes first (its numbers and edges are
    // in the picture's own pixels), lens correction, stabilising, a grade
    // drawn on it. What comes after the
    // clip is fitted to the frame (its effects, a soft crop) already works at
    // the size drawn.
    int reduceFor(const QVariantMap &e, int local, const QVariantMap &facts) {
        const double scale = project.value("drawScale", 1.0).toDouble();
        if (!(scale > 0) || scale >= 1 || !reducesSources() || mode != TimelineRenderer::Mode::Normal || neighbourPass
            || project.value("keyInput").toBool())
            return 1;
        const auto off = e.value("bypass").toStringList();
        if (sourceKey(e, local)) return 1;
        if (!off.contains("lens") && (e.value("lensK1").toDouble() != 0 || e.value("lensK2").toDouble() != 0)) return 1;
        if (!off.contains("stab") && !e.value("stab").toMap().isEmpty()) return 1;
        if (!managed() && !engineGrade(e, local).isEmpty()) return 1;
        const double w = facts.value("width").toDouble(), h = facts.value("height").toDouble();
        const QSize frame = wholeSize();
        if (w <= 0 || h <= 0 || frame.isEmpty()) return 1;
        // Pixels of the picture to a pixel of the whole frame, as it is fitted...
        const double across = w / frame.width(), down = h / frame.height();
        double perPixel = 1;
        switch (scalingOf(e)) {
        case Scaling::Fit: perPixel = std::max(across, down); break;
        case Scaling::Fill: case Scaling::Stretch: perPixel = std::min(across, down); break;
        case Scaling::Original: perPixel = 1; break;
        }
        // ...made larger by the clip's own size, and smaller by the size drawn.
        double zoom = 1;
        // (Its size on this frame: a clip that grows as it plays is converted
        // less small as it does.)
        if (!off.contains("rect")) zoom = std::max({1.0, std::abs(parameter(e, "rectW", local, 1)), std::abs(parameter(e, "rectH", local, 1))});
        perPixel /= zoom * scale;
        return perPixel >= 4 ? 4 : perPixel >= 2 ? 2 : 1;
    }

    // ------------------------------------------------------------ layers

    // The clip as a layer: fit into the frame keeping its shape, cropped in
    // frame coordinates, flipped, then moved, scaled and turned about its
    // anchor, then faded. As the clip stack does it, in its default order.
    std::optional<engine::Layer> placed(const QVariantMap &e, int local, const Shown &s, bool flipped = false,
                                        bool cropped = false) const {
        const double W = size.width(), H = size.height();
        auto bypass = e.value("bypass").toStringList();
        // A picture already fitted and flipped onto a frame of its own (for
        // its effects or a soft crop), and maybe cropped there too.
        if (flipped) bypass << QStringLiteral("flip");
        if (cropped) bypass << QStringLiteral("crop");
        const auto scale = scaleInto(scalingOf(e), s.width, s.height, s.aspect / pixelAspect(), W, H);
        const double fw = s.width * scale.x, fh = s.height * scale.y;
        double x0 = (W - fw) / 2, y0 = (H - fh) / 2, x1 = x0 + fw, y1 = y0 + fh;
        const double fx0 = x0, fy0 = y0;
        // A picture larger than the frame (filled, or at its own size) is
        // seen only inside it, before it is moved or scaled, as on the CPU.
        x0 = std::max(x0, 0.); y0 = std::max(y0, 0.); x1 = std::min(x1, W); y1 = std::min(y1, H);
        if (!bypass.contains("crop")) {
            x0 = std::max(x0, parameter(e, "cropL", local) * W);
            x1 = std::min(x1, W - parameter(e, "cropR", local) * W);
            y0 = std::max(y0, parameter(e, "cropT", local) * H);
            y1 = std::min(y1, H - parameter(e, "cropB", local) * H);
        }
        if (x1 <= x0 || y1 <= y0) return std::nullopt;
        double u0 = (x0 - fx0) / fw, u1 = (x1 - fx0) / fw, v0 = (y0 - fy0) / fh, v1 = (y1 - fy0) / fh;
        if (!bypass.contains("flip")) {
            if (e.value("flipH").toBool()) { u0 = 1 - u0; u1 = 1 - u1; }
            if (e.value("flipV").toBool()) { v0 = 1 - v0; v1 = 1 - v1; }
        }
        double rw = 1, rh = 1, rx = 0, ry = 0, angle = 0, ax = W / 2, ay = H / 2;
        if (!bypass.contains("rect")) {
            rw = parameter(e, "rectW", local, 1);
            rh = parameter(e, "rectH", local, 1);
            rx = parameter(e, "rectX", local);
            ry = parameter(e, "rectY", local);
            angle = parameter(e, "rotation", local);
            ax = parameter(e, "anchorX", local, .5) * W;
            ay = parameter(e, "anchorY", local, .5) * H;
        }
        if (rw == 0 || rh == 0) return std::nullopt;
        // The box scaled about the anchor, turned about the frame origin, then
        // moved: a box of the scaled size turned about its own centre, whose
        // centre lands where the turn takes it.
        const double bw = (x1 - x0) * rw, bh = (y1 - y0) * rh;
        const double cx = (x0 - ax) * rw + bw / 2, cy = (y0 - ay) * rh + bh / 2;
        const double theta = angle * M_PI / 180.0, c = std::cos(theta), sn = std::sin(theta);
        const double tx = rx * W + rw * ax, ty = ry * H + rh * ay;
        const double centreX = c * cx - sn * cy + tx, centreY = sn * cx + c * cy + ty;
        engine::Layer l;
        l.w = float(std::abs(bw));
        l.h = float(std::abs(bh));
        l.x = float(centreX - l.w / 2);
        l.y = float(centreY - l.h / 2);
        l.rotation = float(theta);
        l.u0 = float(rw < 0 ? u1 : u0);
        l.u1 = float(rw < 0 ? u0 : u1);
        l.v0 = float(rh < 0 ? v1 : v0);
        l.v1 = float(rh < 0 ? v0 : v1);
        double alpha = 1;
        if (!bypass.contains("opacity")) {
            alpha = e.value("opacity", 1.).toDouble();
            alpha = std::clamp(EditEngine::valueAt(e.value("opacityKf").toList(), local, alpha, "alpha"), 0., 1.);
        }
        const int fi = e.value("fadeIn").toInt(), fo = e.value("fadeOut").toInt();
        const int length = e.value("frames").toInt() + e.value("mixIn").toInt();
        double fade = 1.;
        if (fi > 0) fade = std::min(fade, double(local) / std::max(1, fi - 1));
        if (fo > 0) fade = std::min(fade, double(length - 1 - local) / std::max(1, fo - 1));
        l.opacity = float(alpha * std::clamp(fade, 0., 1.));
        if (!bypass.contains("blend")) l.blend = blendOf(e.value("blend").toInt()).value_or(engine::BlendMode::Normal);
        return l;
    }

    // The clip's colour controls as an engine colour step on its picture:
    // ((luma + s·(v − luma)) − ½)·c + ½ + b on display values, clamped, as
    // the editor has always applied them. None when neutral or bypassed.
    // A key that comes first in the clip's stack, as it runs on its source:
    // on the picture as filmed, at its own size, before the clip's input
    // transform, colour, grade or fitting to the frame. So its numbers and its
    // edges do not depend on the timeline's size, on what the viewer draws at,
    // or on how the clip is graded. (A clip graded on the CPU has its grade in
    // the picture already; its key stays with the other effects.)
    // Whether a step works on "display values". Those are the timeline's codes,
    // what the viewer shows: their curve is sRGB inside the engine, so a step
    // reads them through towardsCodes rather than working light re-encoded.
    static bool readsCodes(const engine::VideoFilter &f) {
        using T = engine::VideoFilterType;
        using E = engine::Effect;
        if (f.type == T::Shadow) return f.shadow.r > 0 || f.shadow.g > 0 || f.shadow.b > 0;  // its colour
        if (f.type == T::ChromaKey || f.type == T::ColourKey || f.type == T::LumaKey || f.type == T::RemoveBackground) return true;
        if (f.type != T::Effect) return false;
        switch (f.effect) {
        case E::Invert: case E::BlackWhite: case E::Sepia: case E::Posterize: case E::Threshold: case E::SpillSuppress:
        case E::Glow: case E::SoftGlow: case E::EdgeGlow: case E::Emboss: case E::Cartoon: case E::Charcoal: case E::Halftone:
        case E::FilmGrain: case E::OldFilm: case E::Dust: case E::Scratches: case E::LumaBand: case E::MatteRefine:
            return true;
        default:
            return false;
        }
    }
    // Working light to the timeline's codes and back, through OpenColorIO, with
    // the engine's sRGB curve undone: inside them, display values are those codes.
    // Made for the project's colour space (`codesFor`), and again when it changes.
    std::shared_ptr<const engine::ColourProgram> codesIn, codesOut;
    int codesFor = -1;
    // False, with why, when the programs cannot be made or cannot run here.
    bool towardsCodes(std::vector<engine::VideoFilter> &chain, bool back, QString *error) {
        const int space = project.value("colorspace").toInt();
        if (!codesIn || !codesOut || codesFor != space) {
            const QString codes = space == 601 ? QStringLiteral("rec601/rec709") : space == 2020 ? QStringLiteral("rec2020/rec709") : QStringLiteral("rec709/rec709");
            const QString working = QStringLiteral("oma_interchange");
            codesIn.reset();
            codesOut.reset();
            auto in = ColourPipeline::engineProgram(working, codes, {}, error);
            auto out = in ? ColourPipeline::engineProgram(codes, working, {}, error) : nullptr;
            std::string failed;
            if (!in || !out || !engine.prepareProgram(*in, &failed) || !engine.prepareProgram(*out, &failed)) {
                if (error->isEmpty()) *error = failed.empty() ? QStringLiteral("no transform to the timeline's colour space") : QString::fromStdString(failed);
                return false;
            }
            codesIn = std::move(in);
            codesOut = std::move(out);
            codesFor = space;
        }
        engine::VideoFilter program, curve;
        program.type = engine::VideoFilterType::Program;
        curve.type = engine::VideoFilterType::Filmed;
        curve.filmedBack = back;
        program.program = back ? codesOut : codesIn;
        if (back) { chain.push_back(curve); chain.push_back(program); }
        else { chain.push_back(program); chain.push_back(curve); }
        return true;
    }
    std::optional<engine::VideoFilter> sourceKey(const QVariantMap &e, int local) {
        const auto first = keyFirst(e.value("videoEffects").toList());
        if (!first || cpuGraded(e, local)) return std::nullopt;
        // Its first step alone: the matte's extras follow on the framed picture.
        // The draw scale reaches a key only as how finely Remove Background looks for people.
        auto key = effectSteps(QVariantList{QVariant(*first)}, local, project.value("drawScale", 1.0).toDouble()).front();
        if (neighbourPass) { key.matteOnly = true; key.blurBackground = false; }
        return key;
    }
    bool colour(const QVariantMap &e, int local, int source, bool encoded, QString *error,
                const std::optional<engine::VideoFilter> &key = std::nullopt) {
        std::vector<engine::VideoFilter> chain;
        // Lens correction first (before flip and colour, as the clip stack has it).
        if (!e.value("bypass").toStringList().contains("lens") && (e.value("lensK1").toDouble() != 0 || e.value("lensK2").toDouble() != 0)) {
            engine::VideoFilter lens;
            lens.type = engine::VideoFilterType::Effect;
            lens.effect = engine::Effect::Lens;
            lens.values[0] = float(e.value("lensK1").toDouble());
            lens.values[1] = float(e.value("lensK2").toDouble());
            lens.values[2] = float(e.value("lensCx", .5).toDouble());
            lens.values[3] = float(e.value("lensCy", .5).toDouble());
            chain.push_back(lens);
        }
        // Kept encoded, read through the sRGB curve: the key's display values
        // are then the codes as filmed, not the working light re-encoded (a
        // Rec. 709 screen comes back from that paler and less saturated).
        auto filmed = [&](bool back) {
            engine::VideoFilter f;
            f.type = engine::VideoFilterType::Filmed;
            f.filmedBack = back;
            chain.push_back(f);
        };
        // Reading the picture for a key's backdrop (Timeline::effectInputAt): as the key will.
        if (project.value("keyInput").toBool()) {
            if (encoded) filmed(false);
            engine.setSourceFilters(source, chain);
            if (!chain.empty()) engine.renderSourceFilters(source, 0);
            return true;
        }
        if (key) {
            if (encoded) filmed(false);
            chain.push_back(*key);
            if (encoded) filmed(true);
        }
        if (managed()) {
            // Its input transform (with its source LUT) on an encoded picture,
            // then its colour controls on working values.
            if (encoded) {
                auto program = inputProgram(e, error);
                std::string why;
                if (!program || !engine.prepareProgram(*program, &why)) {
                    if (error->isEmpty()) *error = QString::fromStdString(why);
                    return false;
                }
                engine::VideoFilter f;
                f.type = engine::VideoFilterType::Program;
                f.program = std::move(program);
                chain.push_back(f);
            }
            if (!e.value("bypass").toStringList().contains("color") && !neutralColour(e)) {
                engine::VideoFilter f;
                f.type = engine::VideoFilterType::Effect;
                f.effect = engine::Effect::WorkingColour;
                f.values[0] = float(parameter(e, "colorB", local));
                f.values[1] = float(parameter(e, "colorC", local, 1));
                f.values[2] = float(parameter(e, "colorS", local, 1));
                chain.push_back(f);
            }
            engine.setSourceFilters(source, chain);
            if (!chain.empty()) engine.renderSourceFilters(source, 0);
            return true;
        }
        // The source's input LUT, on the picture as filmed.
        if (const QString lut = e.value("sourceMeta").toMap().value("inputLut").toString(); !lut.isEmpty()) {
            QString why;
            if (auto table = loadLookupTable(lut, false, &why)) {
                engine::VideoFilter f;
                f.type = engine::VideoFilterType::Lut;
                f.lut = std::move(table);
                f.tetrahedral = true;
                chain.push_back(f);
            }
        }
        appendColour(e, local, chain);
        if (const QVariantMap node = engineGrade(e, local); !node.isEmpty()) {
            engine::VideoFilter g;
            g.type = engine::VideoFilterType::Grade;
            g.grade = gradeStep(node);
            chain.push_back(g);
        }
        engine.setSourceFilters(source, chain);
        if (!chain.empty()) engine.renderSourceFilters(source, 0);
        return true;
    }
    void appendColour(const QVariantMap &e, int local, std::vector<engine::VideoFilter> &chain) {
        if (!e.value("bypass").toStringList().contains("color") && !neutralColour(e)) {
            const double b = parameter(e, "colorB", local), c = parameter(e, "colorC", local, 1), s = parameter(e, "colorS", local, 1);
            engine::VideoFilter f;
            f.type = engine::VideoFilterType::Colour;
            f.brightness = float(b);
            f.contrast = float(c >= 1 ? (c - 1) / 3 : c - 1);
            f.saturation = float(s - 1);
            chain.push_back(f);
        }
    }
    // The clip's stabilisation corrections, when it is stabilised.
    std::shared_ptr<const Corrections> stabilised(const QVariantMap &e, QString *error = nullptr) const {
        if (e.value("bypass").toStringList().contains("stab")) return nullptr;
        const QVariantMap stab = e.value("stab").toMap();
        const QString results = stab.value("results").toString();
        if (results.isEmpty()) return nullptr;
        // Worked out for the frame the analysis saw (the clip's picture, its
        // size in the file's name, or the timeline's whole frame): its shifts
        // are in those pixels, whatever size is drawn.
        return stabilisation(results, stab.value("smoothing", 15).toInt(), wholeSize(), error);
    }
    bool softCrop(const QVariantMap &e) const {
        if (e.value("bypass").toStringList().contains("crop")) return false;
        return e.value("cropFeather").toDouble() != 0 || e.value("cropInvert").toBool()
            || e.value("editKeys").toMap().contains("cropFeather");
    }
    // A feathered or inverted crop: the clip fitted and flipped onto a frame
    // of its own, then masked by the crop box in frame coordinates (fading
    // over cropFeather of the shorter side), as the editor's crop does.
    // None, with why, when the clip's grade or its effects' colour steps
    // cannot be made for this frame: the frame fails rather than showing the
    // clip without them.
    std::optional<Shown> canvas(const QVariantMap &e, int local, const Shown &s, const QString &role, bool mask, QString *error) {
        const double W = size.width(), H = size.height();
        const QString name = e.value("name", e.value("path")).toString();
        const auto scale = scaleInto(scalingOf(e), s.width, s.height, s.aspect / pixelAspect(), W, H);
        engine::Layer l;
        l.source = s.source;
        l.w = float(s.width * scale.x);
        l.h = float(s.height * scale.y);
        l.x = float((W - l.w) / 2);
        l.y = float((H - l.h) / 2);
        const auto bypass = e.value("bypass").toStringList();
        if (!bypass.contains("flip")) {
            if (e.value("flipH").toBool()) std::swap(l.u0, l.u1);
            if (e.value("flipV").toBool()) std::swap(l.v0, l.v1);
        }
        double left = std::clamp(parameter(e, "cropL", local), 0., .95), right = std::clamp(parameter(e, "cropR", local), 0., .95);
        double top = std::clamp(parameter(e, "cropT", local), 0., .95), bottom = std::clamp(parameter(e, "cropB", local), 0., .95);
        if (left + right > .95) { const double k = .95 / (left + right); left *= k; right *= k; }
        if (top + bottom > .95) { const double k = .95 / (top + bottom); top *= k; bottom *= k; }
        engine::VideoFilter box;
        box.type = engine::VideoFilterType::BoxMask;
        box.box[0] = float(left * W);
        box.box[1] = float(top * H);
        box.box[2] = float((1 - right) * W);
        box.box[3] = float((1 - bottom) * H);
        box.feather = float(std::max(0., parameter(e, "cropFeather", local)) * std::min(W, H));
        box.invert = e.value("cropInvert").toBool();
        // Its stabilisation first (it comes before the flip in the clip's
        // stack: a flipped picture takes the correction mirrored), then its
        // video effects, then (for a soft crop) the mask.
        std::vector<engine::VideoFilter> chain;
        // A colour-managed grade first, on the fitted, flipped picture (as the
        // colour-managed renderer grades it, after its colour controls).
        QString why;
        if (const auto plan = managedGrade(e, local, role, &why)) {
            engine::VideoFilter g;
            g.type = engine::VideoFilterType::GradePlan;
            g.plan = plan;
            chain.push_back(g);
        } else if (!why.isEmpty()) {
            *error = name + QStringLiteral(": its grade (") + why + QLatin1Char(')');
            return std::nullopt;
        }
        if (const auto corrections = stabilised(e); corrections && !corrections->empty()) {
            const Correction c = (*corrections)[size_t(std::clamp(local, 0, int(corrections->size()) - 1))];
            engine::VideoFilter stab;
            stab.type = engine::VideoFilterType::Effect;
            stab.effect = engine::Effect::Stabilise;
            const bool h = !bypass.contains("flip") && e.value("flipH").toBool(), v = !bypass.contains("flip") && e.value("flipV").toBool();
            // The shifts in pixels of the picture drawn (the turn and the
            // zoom are the same at any size): from pixels of the picture the
            // analysis saw to this fitted one's. An analysis from before the
            // file's name gave that picture saw it stretched over the whole
            // frame: for a clip of the frame's shape that is the same thing.
            const QSize seen = corrections->picture.isValid() ? corrections->picture : wholeSize();
            const double sx = l.w / seen.width(), sy = l.h / seen.height();
            stab.values[0] = float((h ? -c.x : c.x) * sx);
            stab.values[1] = float((v ? -c.y : c.y) * sy);
            stab.values[2] = float(h != v ? -c.alpha : c.alpha);
            stab.values[3] = float(c.zoom);
            chain.push_back(stab);
        }
        // A key's temporal smoothing comes straight after the key, before its
        // matte's extras; the neighbours it compares with stop there.
        auto smoothed = [&] {
            if (!temporal) return;
            engine::VideoFilter t;
            t.type = engine::VideoFilterType::Mask;
            t.image = temporal;
            t.mask = engine::MaskMode::TemporalAlpha;
            t.opacity = temporalStrength;
            chain.push_back(t);
        };
        auto steps = effectSteps(e.value("videoEffects").toList(), local, project.value("drawScale", 1.0).toDouble());
        const bool atSource = sourceKey(e, local).has_value();  // done, on the clip's own picture
        if (atSource) {
            steps.erase(steps.begin());
            if (neighbourPass) steps.clear();
            else smoothed();
        }
        bool inCodes = false;
        for (const auto &f : steps) {
            if (const bool codes = readsCodes(f); codes != inCodes) {
                if (!towardsCodes(chain, !codes, &why)) {
                    *error = name + QStringLiteral(": its effects' colour (") + why + QLatin1Char(')');
                    return std::nullopt;
                }
                inCodes = codes;
            }
            const bool isKey = f.type == engine::VideoFilterType::ChromaKey || f.type == engine::VideoFilterType::ColourKey
                               || f.type == engine::VideoFilterType::RemoveBackground;
            if (isKey && neighbourPass) {
                engine::VideoFilter matte = f;
                matte.matteOnly = true;
                matte.blurBackground = false;
                chain.push_back(matte);
                break;
            }
            chain.push_back(f);
            if (isKey) smoothed();
        }
        // Reading the picture for a key that comes later (Timeline::effectInputAt):
        // left in codes, as that key will read it.
        if (const bool codes = project.value("keyInCodes").toBool(); codes != inCodes && !towardsCodes(chain, !codes, &why)) {
            *error = name + QStringLiteral(": its effects' colour (") + why + QLatin1Char(')');
            return std::nullopt;
        }
        if (mask) chain.push_back(box);
        const int id = sourceFor(role + QStringLiteral("/canvas"));
        engine.compositeInto(id, size.width(), size.height(), {l}, {0, 0, 0, 0});
        engine.setSourceFilters(id, chain);
        if (!chain.empty()) engine.renderSourceFilters(id, local / fps);
        return Shown{id, size.width(), size.height(), pixelAspect()};
    }
    // The clip as a layer, its colour and crop applied.
    std::optional<engine::Layer> clipLayer(const QVariantMap &e, int local, const QString &role, int depth, QString *error,
                                           bool *failed) {
        *failed = false;
        const auto shownPicture = picture(e, local, role, depth, error);
        if (!shownPicture) { *failed = true; return std::nullopt; }
        const float strength = mode == TimelineRenderer::Mode::Normal && !neighbourPass ? temporalSmoothing(e) : 0.f;
        if (strength > 0 && !neighbours(e, local, role, depth, strength, error)) { *failed = true; return std::nullopt; }
        auto layer = finished(e, local, *shownPicture, role, error, failed);
        temporal.reset();
        return layer;
    }
    // How much a chroma key smooths its matte over time (0..1): its first
    // active one's setting.
    static float temporalSmoothing(const QVariantMap &e) {
        for (const QVariant &v : e.value("videoEffects").toList()) {
            const QVariantMap state = v.toMap();
            const QString id = state.value("id").toString();
            if (state.value("bypass").toBool() || (id != QLatin1String("chroma-key") && id != QLatin1String("remove-background"))) continue;
            return float(std::clamp(state.value("params").toMap().value("temporal").toDouble(), 0.0, 100.0) / 100.0);
        }
        return 0.f;
    }
    // The clip's frames either side and this one, keyed for their mattes but
    // in their own colours, read back: the picture the temporal step works
    // from (red, green and blue the frame before's, after's and this one's
    // display brightness; alpha the neighbours' mean). At its ends a frame
    // stands in for the one it lacks.
    bool neighbours(const QVariantMap &e, int local, const QString &role, int depth, float strength, QString *error) {
        const int last = std::max(0, e.value("frames").toInt() + e.value("mixIn").toInt() - 1);
        const int at[3] = {std::clamp(local - 1, 0, last), std::clamp(local, 0, last), std::clamp(local + 1, 0, last)};
        // Each neighbour depends on its own frame alone, so the two this
        // frame shares with the one before are kept: one new frame each
        // time, read in order, while playing or exporting.
        Sides &kept = sides[role];
        if (kept.entry != e || kept.size != size) kept = Sides{e, size, {}};
        for (auto it = kept.frames.begin(); it != kept.frames.end();)
            it = std::find(std::begin(at), std::end(at), it->first) == std::end(at) ? kept.frames.erase(it) : std::next(it);
        for (const int frame : at) {
            if (kept.frames.count(frame)) continue;
            engine::Bgra8Frame picture;
            {
                QScopedValueRollback<bool> guard(neighbourPass, true);
                const QString own = role + QStringLiteral("/side");
                const auto shown = this->picture(e, frame, own, depth, error);
                if (!shown) return false;
                if (!colour(cpuGraded(e, frame) ? QVariantMap{} : e, frame, shown->source, shown->encoded, error, sourceKey(e, frame))) return false;
                const auto drawn = canvas(e, frame, *shown, own, softCrop(e), error);
                if (!drawn) return false;
                const Shown framed = *drawn;
                // At its own size, as display bytes with alpha: the brightness
                // the step compares is of those values anyway.
                if (!engine.readSourcePreview(framed.source, framed.width, framed.height, picture)) {
                    *error = QStringLiteral("The engine could not read a neighbouring frame back.");
                    return false;
                }
            }
            // Its display brightness and its coverage, as bytes.
            Side side{picture.width, picture.height, std::vector<uint8_t>(size_t(picture.width) * picture.height),
                      std::vector<uint8_t>(size_t(picture.width) * picture.height)};
            const int rows = Bands::rowsPerBand(picture.width, 4), bands = (picture.height + rows - 1) / rows;
            Bands::run(bands, [&](int band) {
                for (size_t i = size_t(band) * rows * picture.width,
                            end = std::min(size_t(band + 1) * rows, size_t(picture.height)) * picture.width; i < end; ++i) {
                    const uint8_t *c = picture.data.data() + i * 4;  // B, G, R, A
                    side.luma[i] = byte((0.2126f * c[2] + 0.7152f * c[1] + 0.0722f * c[0]) / 255.f);
                    side.alpha[i] = c[3];
                }
                return true;
            });
            kept.frames.emplace(frame, std::move(side));
        }
        const Side &before = kept.frames.at(at[0]), &now = kept.frames.at(at[1]), &after = kept.frames.at(at[2]);
        for (const Side *f : {&before, &now, &after})
            if (f->width != before.width || f->height != before.height || f->width < 1) return true;
        // Red, green and blue the frame before's, after's and this one's
        // brightness; alpha the neighbours' mean. At its ends a frame stands
        // in for the one it lacks.
        auto image = std::make_shared<engine::FilterImage>();
        image->width = before.width;
        image->height = before.height;
        image->rgba.resize(size_t(image->width) * image->height * 4);
        const int rows = Bands::rowsPerBand(image->width, 4), bands = (image->height + rows - 1) / rows;
        Bands::run(bands, [&](int band) {
            for (size_t i = size_t(band) * rows * image->width,
                        end = std::min(size_t(band + 1) * rows, size_t(image->height)) * image->width; i < end; ++i) {
                uint8_t *o = image->rgba.data() + i * 4;
                o[0] = before.luma[i]; o[1] = after.luma[i]; o[2] = now.luma[i];
                o[3] = uint8_t((int(before.alpha[i]) + int(after.alpha[i]) + 1) / 2);
            }
            return true;
        });
        temporal = image;
        temporalStrength = strength;
        return true;
    }
    static uint8_t byte(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }
    // A keyed clip's neighbouring frames, as the temporal step reads them.
    struct Side {
        int width = 0, height = 0;
        std::vector<uint8_t> luma, alpha;
    };
    struct Sides {
        QVariantMap entry;
        QSize size;
        std::map<int, Side> frames;
    };
    std::unordered_map<QString, Sides> sides;  // by the clip's role
    // A clip's picture through the rest of its stack: colour, grade, crop,
    // placement, opacity.
    std::optional<engine::Layer> finished(const QVariantMap &entry, int local, const Shown &shownPicture, const QString &role,
                                          QString *error, bool *failed) {
        ++stats.clips;
        const QVariantMap e = mode == TimelineRenderer::Mode::Before ? beforeOf(entry) : entry;
        // A clip graded on the CPU had its colour controls applied with its grade.
        if (!colour(cpuGraded(e, local) ? QVariantMap{} : e, local, shownPicture.source, shownPicture.encoded, error, sourceKey(e, local))) {
            *failed = true;
            return std::nullopt;
        }
        const bool soft = softCrop(e);
        if (soft || hasActiveEffects(e.value("videoEffects").toList()) || stabilised(e)
            || (managed() && !e.value("bypass").toStringList().contains("grade") && gradeActive(e.value("grade").toMap()))) {
            const auto framed = canvas(e, local, shownPicture, role, soft, error);
            if (!framed) {
                *failed = true;
                return std::nullopt;
            }
            auto l = placed(e, local, *framed, true, soft);
            if (l) l->source = framed->source;
            return l;
        }
        auto l = placed(e, local, shownPicture);
        if (l) l->source = shownPicture.source;
        return l;
    }

    // A whole-frame layer showing a picture the size of the frame.
    engine::Layer full(int source) const {
        engine::Layer l;
        l.source = source;
        l.w = float(size.width());
        l.h = float(size.height());
        return l;
    }

    // Whether a track's clip at `position` hides everything below it: a solid
    // picture filling the frame, wholly opaque there, blending normally. Looks
    // at a picture this frame draws anyway (reading it again is no new
    // decode); a fault in it shows when it is drawn.
    bool hidesBelow(const QVariantList &clips, int position, const QString &track, int depth) {
        if (mode != TimelineRenderer::Mode::Normal || clips.size() != 1) return false;
        const auto e = clips.first().toMap();
        if (!solidPicture(e)) return false;
        QString ignored;
        const QString own = e.value("path").toString();
        if (!own.startsWith(QLatin1String("color:")) && QFileInfo::exists(own)) {
            MediaSource *media = pool.source(pictureSource ? pictureSource(own) : own, &ignored);
            if (!media || media->information().value("alpha", true).toBool()) return false;
        }
        const int local = position - e.value("start").toInt() + e.value("mixIn").toInt();
        const auto shownPicture = picture(e, local, track + QStringLiteral("/a"), depth, &ignored);
        if (!shownPicture) return false;
        const auto l = placed(e, local, *shownPicture);
        constexpr float slack = 1e-3f;
        return l && l->opacity >= 1.f && l->blend == engine::BlendMode::Normal && std::abs(l->rotation) <= 1e-6f
            && l->x <= slack && l->y <= slack && l->x + l->w >= size.width() - slack && l->y + l->h >= size.height() - slack;
    }

    // The layers of a list of clips at `position`, bottom track first. Tracks
    // under one that hides them are neither decoded nor drawn.
    bool sequence(const QVariantList &list, int position, int depth, const QString &prefix, std::vector<engine::Layer> &layers,
                  QString *error, const QVariantList *trackList = nullptr) {
        if (depth > kMaxDepth) { *error = QStringLiteral("Nested clips go too many levels deep."); return false; }
        // The timeline's own tracks at the top; a nested timeline's its own;
        // a compound clip's none (its clips are all drawn).
        const QVariantList &lanes = trackList ? *trackList : depth ? QVariantList{} : tracks;
        QMap<int, QVariantList> active;
        // The frame first: what is drawn is asked about (hidden() may look
        // at the file), not every clip of the timeline on every frame.
        for (const auto &v : list) {
            const auto e = v.toMap();
            const int start = e.value("start").toInt() - e.value("mixIn").toInt();
            if (position < start || position >= e.value("start").toInt() + e.value("frames").toInt() || hidden(e, lanes)) continue;
            active[e.value("track", e.value("lane", 0)).toInt()] << e;
        }
        auto lowest = active.cbegin();
        for (auto it = active.cend(); it != active.cbegin();) {
            --it;
            if (hidesBelow(it.value(), position, prefix + QStringLiteral("t%1").arg(it.key()), depth)) { lowest = it; break; }
        }
        for (auto it = lowest; it != active.cend(); ++it) {
            const QString track = prefix + QStringLiteral("t%1").arg(it.key());
            const auto &clips = it.value();
            if (clips.size() == 1 && clips.first().toMap().value("adjust").toBool()) {
                // An adjustment clip: everything below it, drawn as one
                // picture, goes through the clip's own stack in its place.
                const auto e = clips.first().toMap();
                const int local = position - e.value("start").toInt() + e.value("mixIn").toInt();
                const int below = sourceFor(track + QStringLiteral("/adjust"));
                engine.compositeInto(below, size.width(), size.height(), layers,
                                     depth ? engine::Color{0, 0, 0, 0} : engine::Color{0, 0, 0, 1});
                layers.clear();
                bool failed = false;
                if (auto l = finished(e, local, Shown{below, size.width(), size.height(), pixelAspect()}, track + QStringLiteral("/a"),
                                      error, &failed))
                    layers.push_back(*l);
                if (failed) return false;
                continue;
            }
            if (clips.size() == 1) {
                const auto e = clips.first().toMap();
                const int local = position - e.value("start").toInt() + e.value("mixIn").toInt();
                bool failed = false;
                if (auto l = clipLayer(e, local, track + QStringLiteral("/a"), depth, error, &failed)) layers.push_back(*l);
                if (failed) return false;
                continue;
            }
            if (clips.size() != 2) { *error = QStringLiteral("Unexpected overlapping clips on one track."); return false; }
            // A dissolve: each clip drawn on its own frame, then the two mixed.
            const auto a = clips[0].toMap(), b = clips[1].toMap();
            const int localA = position - a.value("start").toInt() + a.value("mixIn").toInt();
            const int localB = position - b.value("start").toInt() + b.value("mixIn").toInt();
            const int overlap = b.value("mixIn").toInt();
            if (overlap <= 0) { *error = QStringLiteral("Unexpected overlapping clips on one track."); return false; }
            int framed[2] = {-1, -1};
            const QVariantMap pair[2] = {a, b};
            const int locals[2] = {localA, localB};
            for (int k = 0; k < 2; ++k) {
                const QString role = track + (k ? QStringLiteral("/b") : QStringLiteral("/a"));
                bool failed = false;
                std::vector<engine::Layer> one;
                if (auto l = clipLayer(pair[k], locals[k], role, depth, error, &failed)) one.push_back(*l);
                if (failed) return false;
                framed[k] = sourceFor(role + QStringLiteral("/frame"));
                engine.compositeInto(framed[k], size.width(), size.height(), one, {0, 0, 0, 0});
            }
            const double progress = std::clamp(double(localB) / std::max(1, overlap - 1), 0., 1.);
            // As the clip names it (transitionstyles.h); a name not known dissolves.
            const QString named = a.value("mixStyle").toString();
            const engine::TransitionStyle style = transitionStyle(named).value_or(engine::TransitionStyle{});
            const TransitionMade made = transitionMade(named);
            const int mixed = sourceFor(track + QStringLiteral("/mix"));
            // Blur Dissolve: both pictures blurred as they dissolve, most half
            // way (a twenty-fifth of the frame's height, within the blur's
            // reach), sharp at either end. The two pictures keep their
            // sources from cut to cut, so any other style clears the blur.
            std::vector<engine::VideoFilter> soften;
            if (made == TransitionMade::Blur) {
                engine::VideoFilter blur;
                blur.type = engine::VideoFilterType::Blur;
                blur.radius = float(std::min(64.0, size.height() / 25.0) * (1.0 - std::abs(2.0 * progress - 1.0)));
                if (blur.radius >= 0.5f) soften.push_back(blur);
            }
            for (int k = 0; k < 2; ++k) {
                engine.setSourceFilters(framed[k], soften);
                if (!soften.empty()) engine.renderSourceFilters(framed[k], locals[k] / fps);
            }
            if (made == TransitionMade::Zoom) {
                // The first picture grows by half about its middle and fades,
                // over the next one standing as it will.
                const double grow = 1.0 + 0.5 * progress * progress;
                engine::Layer front = full(framed[0]);
                front.w = float(size.width() * grow);
                front.h = float(size.height() * grow);
                front.x = float((size.width() - front.w) / 2);
                front.y = float((size.height() - front.h) / 2);
                front.opacity = float(1.0 - progress);
                engine.compositeInto(mixed, size.width(), size.height(), {full(framed[1]), front}, {0, 0, 0, 0});
            } else {
                engine.transitionInto(mixed, size.width(), size.height(), framed[0], framed[1], style, float(progress));
            }
            layers.push_back(full(mixed));
        }
        return true;
    }
};

TimelineRenderer::TimelineRenderer(engine::Engine &engine) : d(std::make_unique<State>(engine)) {
    // The engine's own stages beside the renderer's, when stages are timed.
    if (StageTimer::on()) engine::stageProbe = [](const char *stage, long long ns) { StageTimer::add(stage, ns); };
}
TimelineRenderer::~TimelineRenderer() {
    d->settle();  // no reader is left on a source that is about to close
    for (const auto &[role, id] : d->sources) d->engine.destroySource(id);
    if (d->delivery) d->engine.destroySource(d->delivery);
    if (d->viewer) d->engine.destroySource(d->viewer);
    if (d->unframed) d->engine.destroySource(d->unframed);
    if (d->backdrop) d->engine.destroySource(d->backdrop);
}

void TimelineRenderer::setFraming(const QSize &size, Framing framing) {
    d->framedSize = size;
    d->framing = framing;
}

void TimelineRenderer::setPictureSource(std::function<QString(const QString &)> source) { d->pictureSource = std::move(source); }
void TimelineRenderer::setMediaSources(const QVariantMap &sources) {
    if (d->mediaSources == sources) return;
    d->mediaSources = sources;
    d->nests.clear();  // their clips take these settings when read again
}

void TimelineRenderer::configure(const QVariantList &entries, const QVariantList &tracks, const QVariantMap &project,
                                 const QSize &size, double fps) {
    if (d->entries == entries && d->tracks == tracks && d->project == project && d->size == size && d->fps == fps) return;
    // (Pictures begun for the timeline as it was are not this one's.)
    ++d->generation;
    if (d->entries != entries) {
        d->timingMaps.clear();
        // A clip's input settings or grade may have changed: pictures are
        // decoded and prepared afresh.
        d->shown.clear();
        d->sides.clear();
    }
    d->entries = entries;
    d->tracks = tracks;
    d->project = project;
    d->size = size;
    d->fps = fps;
    d->managedTimeline = LinearRenderer::enabled(entries, project);
    d->problem = d->check(entries, 0);
}

QString TimelineRenderer::unsupported() const { return d->problem; }
void TimelineRenderer::setDecoderLimits(int sources, qint64 bytes, int threads, int perFile) {
    d->settle();
    d->pool.setLimits(sources, bytes, threads, perFile);
}
void TimelineRenderer::setAside(bool aside) {
    d->settle();
    d->pool.setAside(aside);
}
void TimelineRenderer::setSourceReduction(bool on) { State::sourceReduction() = on; }
void TimelineRenderer::setReductionBeforeUpload(bool on) { State::reductionBeforeUpload() = on; }
void TimelineRenderer::setLayerFetch(bool sideBySide, bool ahead) {
    State::sideBySide() = sideBySide;
    State::fetchesAhead() = ahead;
}
int TimelineRenderer::openSources(const QString &path) const {
    d->settle();
    return path.isEmpty() ? d->pool.open() : d->pool.open(path);
}

bool TimelineRenderer::render(int position, QString *error) {
    QString local;
    if (!error) error = &local;
    error->clear();
    if (!d->problem.isEmpty()) { *error = d->problem; return false; }
    d->stats = {};
    // The canvas is the picture's size: the export's frame when framed.
    d->engine.setCanvasSize(d->outputSize().width(), d->outputSize().height());
    std::vector<engine::Layer> layers;
    // Its decoded pictures first, side by side; then drawn as ever.
    d->fetchFor(position);
    for (const auto &batch : d->ready)
        if (batch->ahead) d->stats.ahead += int(batch->items.size());
    const bool ok = d->sequence(d->entries, position, 0, QString(), layers, error);
    d->releaseUnused();
    d->pool.frameDone();
    if (!ok) {
        d->ready.clear();
        d->lastPosition = -1;
        return false;
    }
    // The next frame's pictures are begun while the engine finishes this one.
    d->fetchAhead(position);
    d->stats.sources = int(d->sources.size());
    // A colour-managed frame keeps its transparency, as the colour-managed
    // renderer's does: the viewer shows it over black (or a checker), an
    // export without alpha flattens it over black. Otherwise black under
    // everything, unless the project asks for transparency.
    const bool transparent = d->managedTimeline || d->project.value("transparent").toBool();
    if (d->framed()) {
        // The timeline at its own size, then placed on the export's frame.
        if (!d->unframed) d->unframed = d->engine.createSource();
        d->engine.compositeInto(d->unframed, d->size.width(), d->size.height(), layers, {0, 0, 0, transparent ? 0.f : 1.f});
        layers = d->frameLayers();
    }
    const QSize out = d->outputSize();
    // A delivery is flattened over black as it is drawn.
    if (d->intoDelivery) d->engine.compositeInto(d->delivery, out.width(), out.height(), layers, {0, 0, 0, 1});
    else if (d->intoViewer) d->engine.compositeInto(d->viewer, out.width(), out.height(), layers, {0, 0, 0, transparent ? 0.f : 1.f});
    else {
        d->engine.composite(layers, {0, 0, 0, transparent ? 0.f : 1.f});
    }
    return true;
}

QImage TimelineRenderer::linear(int position, QString *error, Mode mode) {
    QString local;
    if (!error) error = &local;
    d->mode = mode;
    bool drawn;
    { StageTimer::Scope timed("render"); drawn = render(position, error); }
    d->mode = Mode::Normal;
    if (!drawn) return {};
    StageTimer::Scope timed("readback");
    QImage pixels;
    const bool read = d->engine.readLinearInto(-1, [&](int w, int h) -> float * {
        pixels = d->readbackImage(w, h);
        if (pixels.isNull() || pixels.bytesPerLine() != qsizetype(w) * 16) return nullptr;
        // Shared with the spare it came from: written in place, not copied.
        return reinterpret_cast<float *>(const_cast<uchar *>(pixels.constBits()));
    });
    if (!read) {
        *error = QStringLiteral("The engine could not read the frame back.");
        return {};
    }
    return pixels;
}

bool TimelineRenderer::deliver(int position, const QString &space, engine::OutputColour colour, engine::YuvFrame &out,
                               QString *error) {
    QString local;
    if (!error) error = &local;
    if (!d->managedTimeline) {
        *error = QStringLiteral("Only a colour-managed timeline is delivered by the engine.");
        return false;
    }
    auto program = ColourPipeline::engineProgram(QStringLiteral("oma_interchange"), space, {}, error);
    if (!program) return false;
    return deliver(position, std::move(program), colour, out, error);
}

bool TimelineRenderer::deliver(int position, std::shared_ptr<const engine::ColourProgram> program, engine::OutputColour colour,
                               engine::YuvFrame &out, QString *error) {
    QString local;
    if (!error) error = &local;
    if (!d->managedTimeline) {
        *error = QStringLiteral("Only a colour-managed timeline is delivered by the engine.");
        return false;
    }
    std::string why;
    if (!program || !d->engine.prepareProgram(*program, &why)) {
        if (error->isEmpty()) *error = QString::fromStdString(why);
        return false;
    }
    if (!d->delivery) d->delivery = d->engine.createSource();
    d->intoDelivery = true;
    bool drawn;
    {
        StageTimer::Scope timed("render");
        drawn = render(position, error);
    }
    d->intoDelivery = false;
    if (!drawn) return false;
    if (program != d->deliveryProgram) {
        engine::VideoFilter f;
        f.type = engine::VideoFilterType::Program;
        f.program = program;
        d->engine.setSourceFilters(d->delivery, {f});
        d->deliveryProgram = std::move(program);
    }
    d->engine.renderSourceFilters(d->delivery, 0);
    colour.encoded = true;
    StageTimer::Scope timed("readback.planes");
    if (!d->engine.readSourceOutput(d->delivery, colour, out, d->outputSize().width(), d->outputSize().height())) {
        *error = QStringLiteral("The engine could not read the frame back.");
        return false;
    }
    return true;
}

QImage TimelineRenderer::shown(int position, std::shared_ptr<const engine::ColourProgram> program, const QSize &stands,
                              QString *error, ScopeCount *scopes) {
    QString local;
    if (!error) error = &local;
    if (!d->managedTimeline) {
        *error = QStringLiteral("Only a colour-managed timeline is shown by the engine.");
        return {};
    }
    std::string why;
    if (!program || !d->engine.prepareProgram(*program, &why)) {
        if (error->isEmpty()) *error = QString::fromStdString(why);
        return {};
    }
    const QString cannotCount = QStringLiteral("The engine does not count the scopes.");
    if (scopes && d->scopesDeclined && d->scopesDeclinedProgram == scopes->program) {
        *error = cannotCount;
        return {};
    }
    if (!d->viewer) d->viewer = d->engine.createSource();
    d->intoViewer = true;
    bool drawn;
    {
        StageTimer::Scope timed("render");
        drawn = render(position, error);
    }
    d->intoViewer = false;
    if (!drawn) return {};
    if (scopes) {
        // Of the working picture, before the display is made of it.
        StageTimer::Scope timed("scopes.count");
        const bool counted = (!scopes->program || d->engine.prepareProgram(*scopes->program, &why))
                             && d->engine.countScopes(d->viewer, scopes->program.get(), scopes->layout, scopes->cells);
        d->scopesDeclined = !counted;
        d->scopesDeclinedProgram = counted ? nullptr : scopes->program;
        if (!counted) {
            *error = cannotCount;
            return {};
        }
    }
    if (program != d->viewerProgram) {
        engine::VideoFilter f;
        f.type = engine::VideoFilterType::Program;
        f.program = program;
        d->engine.setSourceFilters(d->viewer, {f});
        d->viewerProgram = std::move(program);
    }
    d->engine.renderSourceFilters(d->viewer, 0);
    StageTimer::Scope timed("readback.shown");
    const QSize size = d->outputSize();
    QImage pixels = d->shownImage(size.width(), size.height(), stands.isValid() ? stands : size);
    if (pixels.isNull() || pixels.bytesPerLine() != qsizetype(size.width()) * 4
        || !d->engine.readSourceShown(d->viewer, size.width(), size.height(), const_cast<uchar *>(pixels.constBits()))) {
        *error = QStringLiteral("The engine could not read the frame back.");
        return {};
    }
    return pixels;
}

WorkingFrame TimelineRenderer::working(int position, QString *error, Mode mode) {
    QString local;
    if (!error) error = &local;
    QImage pixels = linear(position, error, mode);
    if (pixels.isNull()) return {};
    const auto info = ColourPipeline::info();
    StageTimer::Scope timed("working.adopt");
    // (Mended, not refused: playback's picture of the same frame is shown
    // without this check, and one pixel that is not a number must not stop
    // the paused frame or an export.)
    return WorkingFrame::adoptLinear(std::move(pixels), info.value("config").toString(), info.value("configHash").toString(),
                                    QStringLiteral("oma_interchange"), error, true);
}

bool TimelineRenderer::managed() const { return d->managedTimeline; }

QImage TimelineRenderer::image(int position, QString *error) {
    if (d->managedTimeline) return working(position, error).taggedImage();
    if (!render(position, error)) return {};
    engine::Bgra8Frame preview;
    if (!d->engine.readPreview(d->size.width(), d->size.height(), preview)) {
        if (error) *error = QStringLiteral("The engine could not read the frame back.");
        return {};
    }
    // B, G, R, A bytes are Qt's ARGB32 on this byte order; Qt's own
    // converter turns them into the RGBA the viewer and scopes take.
    QImage out = QImage(preview.data.data(), preview.width, preview.height, preview.width * 4, QImage::Format_ARGB32)
                     .convertToFormat(QImage::Format_RGBA8888);
    out.setText(QStringLiteral("omaedit.pixelAspect"), QString::number(d->pixelAspect(), 'g', 16));
    return out;
}

TimelineRenderer::Stats TimelineRenderer::stats() const { return d->stats; }

QString engineUnsupported(const QVariantList &entries, const QVariantList &tracks, const QVariantMap &project,
                          const QSize &size, double fps) {
    // Its checks make no engine calls: the CPU twin stands in.
    auto cpu = engine::createCpuEngine();
    TimelineRenderer renderer(*cpu);
    renderer.configure(entries, tracks, project, size, fps);
    return renderer.unsupported();
}

}  // namespace render
