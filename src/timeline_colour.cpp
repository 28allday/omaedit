#include "timeline_p.h"

namespace {

bool near(double a, double b) { return qAbs(a - b) < 1e-6; }
bool curveIsIdentity(const QVariantList &pts, double flatY = -1) {
    if (pts.size() < 2)
        return true;
    for (const QVariant &v : pts) {
        const QVariantList xy = v.toList();
        if (xy.size() < 2)
            return true;
        const double x = xy.at(0).toDouble(), y = xy.at(1).toDouble();
        if (flatY >= 0 ? !near(y, flatY) : !near(x, y))
            return false;
    }
    return true;
}
}

// Pixels a colour estimate can trust: opaque, not black borders, not clipped
// and not strongly coloured. Each channel's average over them; false with
// too few.
static bool neutralSums(const QImage &frame, double sums[3]) {
    const bool linear=frame.format()==QImage::Format_RGBA32FPx4;
    const QImage rgb = linear?frame:frame.convertToFormat(QImage::Format_RGBA8888);
    const int stride = qMax(1, int(std::ceil(std::sqrt(double(rgb.width()) * rgb.height() / 65536.0))));
    sums[0] = sums[1] = sums[2] = 0;
    int eligible = 0, sampled = 0;
    for (int y = 0; y < rgb.height(); y += stride) {
        const uchar *line = rgb.constScanLine(y);
        for (int x = 0; x < rgb.width(); x += stride) {
            double pixel[4];for(int c=0;c<4;++c)pixel[c]=linear?reinterpret_cast<const float *>(line)[4*x+c]*255:line[4*x+c];
            ++sampled;
            const double low = qMin(pixel[0], qMin(pixel[1], pixel[2]));
            const double high = qMax(pixel[0], qMax(pixel[1], pixel[2]));
            if (pixel[3] < 250 || low < 8 || high > 247 || high - low > high * 0.75)
                continue;
            for (int c = 0; c < 3; ++c) sums[c] += pixel[c];
            ++eligible;
        }
    }
    if (eligible < qMax(32, sampled / 100)) return false;
    for (int c = 0; c < 3; ++c) sums[c] /= eligible;
    return true;
}

// ── Color page: grades ────────────────────────────────────────────────────

QVariantMap Timeline::defaultGradeNode(const QString &label) {
    auto wheel = [](double y) { return QVariantList{y, 0.0, 0.0, 0.0}; };
    QVariantMap primaries{
        {"lift", wheel(0.0)},   {"gamma", wheel(0.0)},   {"gain", QVariantList{1.0, 0.0, 0.0, 0.0}},
        {"offset", wheel(0.0)}, {"temp", 6500.0},        {"tint", 0.0},
        {"exposure", 0.0},      {"contrast", 1.0},       {"pivot", 0.435},
        {"saturation", 1.0},    {"hue", 0.0},            {"highlights", 0.0},
        {"shadows", 0.0},       {"whites", 0.0},         {"blacks", 0.0},
        {"midDetail", 0.0}};
    const QVariantList line{QVariantList{0.0, 0.0}, QVariantList{1.0, 1.0}};
    const QVariantList flat{QVariantList{0.0, 0.5}, QVariantList{1.0, 0.5}};
    QVariantMap curves{{"master", line}, {"red", line}, {"green", line}, {"blue", line},
                       {"hueHue", flat}, {"hueSat", flat}, {"hueLum", flat},
                       {"lumaLuma", line}, {"satSat", line}};
    QVariantMap qualifier{{"enabled", false}, {"color", "#ff0000"}, {"deltaH", 0.2},
                          {"deltaS", 0.3}, {"deltaL", 0.3}, {"softness", 0.1},
                          {"invert", false}, {"subspace", "hsl"}};
    QVariantMap window{{"enabled", false}, {"shape", "circle"}, {"x", 0.5}, {"y", 0.5},
                       {"w", 0.4}, {"h", 0.4}, {"rotation", 0.0}, {"softness", 0.1},
                       {"invert", false}, {"track", QVariantList()}};
    QVariantMap lut{{"path", ""}, {"intensity", 1.0}};
    return QVariantMap{{"id", QStringLiteral("n%1").arg(QRandomGenerator::global()->generate() % 1000000)},
                       {"label", label}, {"kind", "serial"}, {"enabled", true}, {"maskModel", "component"},
                       {"primaries", primaries}, {"curves", curves}, {"qualifier", qualifier},
                       {"window", window}, {"lut", lut}, {"keyframes", QVariantMap()}};
}

QVariantMap Timeline::defaultGrade() {
    // Colour-managed from the start, as every timeline is.
    return QVariantMap{{"version", 1},
                       {"nodes", QVariantList{defaultGradeNode(QStringLiteral("Node 1"))}},
                       {"management", ColourPipeline::effective({})},
                       {"input", "auto"}, {"output", "rec709"}, {"bypass", false}};
}

bool Timeline::gradeIsIdentity(const QVariantMap &grade) {
    // Its colour management is the clip's input (every clip has one), not a correction.
    QVariantMap corrections = grade;
    corrections.remove(QStringLiteral("management"));
    if (GradeEngine::required(corrections) || GradeEngine::spaceConversion(corrections)) return false;
    if (grade.isEmpty())
        return true;
    for (const QVariant &nv : grade.value(QStringLiteral("nodes")).toList()) {
        const QVariantMap node = nv.toMap();
        if (!node.value(QStringLiteral("enabled"), true).toBool())
            continue;
        const QVariantMap p = node.value(QStringLiteral("primaries")).toMap();
        const QVariantMap d = defaultGradeNode().value(QStringLiteral("primaries")).toMap();
        for (auto it = d.cbegin(); it != d.cend(); ++it) {
            if (it.value().typeId() == QMetaType::QVariantList) {
                const QVariantList a = p.value(it.key(), it.value()).toList(), b = it.value().toList();
                for (int i = 0; i < b.size(); ++i)
                    if (!near(a.value(i).toDouble(), b.at(i).toDouble()))
                        return false;
            } else if (!near(p.value(it.key(), it.value()).toDouble(), it.value().toDouble())) {
                return false;
            }
        }
        const QVariantMap c = node.value(QStringLiteral("curves")).toMap();
        for (const char *k : {"master", "red", "green", "blue", "lumaLuma", "satSat"})
            if (!curveIsIdentity(c.value(QLatin1String(k)).toList()))
                return false;
        if (!node.value(QStringLiteral("lut")).toMap().value(QStringLiteral("path")).toString().isEmpty())
            return false;
        if (!node.value(QStringLiteral("keyframes")).toMap().isEmpty())
            return false;
    }
    return true;
}

// The grade as the colour engine's filter on the cut, carrying the cut's
// in/out so keyframes count from the clip's first frame.
void Timeline::applyGradeFilters(edit::Profile &profile, edit::Producer *cut,
                                    const QVariantMap &grade, int cutIn, int cutOut) {
    if (!cut || gradeIsIdentity(grade))
        return;
    edit::Filter filter(profile, "omaedit.grade");
    filter.set("grade", QJsonDocument::fromVariant(grade).toJson(QJsonDocument::Compact).constData());
    filter.set("omavid:gradeFx", 1);
    filter.set_in_and_out(cutIn, cutOut);
    filter.set("disable", grade.value(QStringLiteral("bypass")).toBool());
    if (filter.is_valid()) cut->attach(filter);
}

bool Timeline::setGradeAt(int track, int index, const QVariantMap &inputGrade) {
    const QVariantMap grade = GradeEngine::pinMaskModels(inputGrade);
    if (!GradeEngine::validate(grade).isEmpty()) return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    // Keep the versions the carrier held; rebuild everything else.
    QString versions;
    int version = 0;
    bool bypass = false;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->is_valid())
            continue;
        if (f->get("omavid:grade")) {
            bypass = f->get_int("disable") != 0;
            const char *v = f->get("omavid:gradeVersions");
            if (v && *v)
                versions = QString::fromUtf8(v);
            version = f->get_int("omavid:gradeVersion");
            cut->detach(*f);
        } else if (f->get("omavid:gradeFx")) {
            cut->detach(*f);
        }
    }
    // Every wheel/curve edit belongs to the active version. Without this,
    // restore reapplies an older version snapshot and silently loses the edit.
    if (!versions.isEmpty()) {
        QVariantList list = QJsonDocument::fromJson(versions.toUtf8()).toVariant().toList();
        if (version >= 0 && version < list.size()) {
            QVariantMap current = list.at(version).toMap();
            current.insert(QStringLiteral("grade"), grade);
            list[version] = current;
            versions = QString::fromUtf8(QJsonDocument::fromVariant(list).toJson(QJsonDocument::Compact));
        }
    }
    if (!grade.isEmpty() || !versions.isEmpty()) {
        edit::Filter carrier(*m_profile, "brightness");
        if (!carrier.is_valid())
            return false;
        carrier.set("level", 1);
        carrier.set("omavid:grade", 1);
        carrier.set("omavid:gradeJson",
                    QJsonDocument::fromVariant(grade).toJson(QJsonDocument::Compact).constData());
        if (!versions.isEmpty())
            carrier.set("omavid:gradeVersions", versions.toUtf8().constData());
        carrier.set("omavid:gradeVersion", version);
        carrier.set("disable", bypass ? 1 : 0);
        cut->attach(carrier);
        applyGradeFilters(*m_profile, cut, grade, info->frame_in, info->frame_out);
        if (bypass)
            setBypassAt(track, index, QStringLiteral("grade"), true);
        reorderAttributeFilters(cut);
    }
    return true;
}

bool Timeline::setSharedLookAt(int track, int index, const QVariantList &versions, int active,
                                  const QVariantMap &grade) {
    if (track < 0 || track >= trackCount())
        return false;
    // Clip lookups refuse a locked track, so lift the lock for this write
    // only and put it back whatever happens.
    bool &locked = m_tracks[size_t(track)].locked;
    const bool wasLocked = locked;
    locked = false;
    const bool ok = setGradeVersionsAt(track, index, versions, active) && setGradeAt(track, index, grade);
    locked = wasLocked;
    return ok;
}

QVariantMap Timeline::gradeAt(int track, int index) const {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return QVariantMap();
    for (int i = 0; i < info->cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(info->cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:grade")) {
            const char *json = f->get("omavid:gradeJson");
            return GradeEngine::pinMaskModels(json && *json ? QJsonDocument::fromJson(json).object().toVariantMap() : QVariantMap());
        }
    }
    return QVariantMap();
}

bool Timeline::setGradeVersionsAt(int track, int index, const QVariantList &inputVersions, int active) {
    const QVariantList versions = GradeEngine::pinVersionMaskModels(inputVersions);
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    active = versions.isEmpty() ? 0 : qBound(0, active, versions.size() - 1);
    // The active version's grade is the applied grade.
    QVariantMap grade = gradeAt(track, index);
    if (!versions.isEmpty())
        grade = versions.at(active).toMap().value(QStringLiteral("grade")).toMap();
    if (!setGradeAt(track, index, grade))
        return false;
    // An untouched clip has no grade carrier. Even an empty look needs one
    // once named versions exist, or the first Add/Rename silently vanishes.
    if (!versions.isEmpty() && grade.isEmpty()) {
        bool hasCarrier = false;
        for (int i = 0; i < info->cut->filter_count(); ++i) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(i));
            if (f && f->is_valid() && f->get("omavid:grade")) { hasCarrier = true; break; }
        }
        if (!hasCarrier) {
            edit::Filter carrier(*m_profile, "brightness");
            if (!carrier.is_valid()) return false;
            carrier.set("level", 1);
            carrier.set("omavid:grade", 1);
            carrier.set("omavid:gradeJson", "{}");
            info->cut->attach(carrier);
        }
    }
    for (int i = 0; i < info->cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(info->cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:grade")) {
            if (versions.isEmpty()) {
                f->clear("omavid:gradeVersions");
            } else {
                f->set("omavid:gradeVersions",
                       QJsonDocument::fromVariant(versions).toJson(QJsonDocument::Compact).constData());
            }
            f->set("omavid:gradeVersion", active);
            break;
        }
    }
    if (versions.isEmpty() && grade.isEmpty())
        setGradeAt(track, index, QVariantMap()); // nothing left: the carrier goes too
    return true;
}

int Timeline::pasteColorAt(int track, int index, const QString &kind,
                             const QVariantMap &payload, int afterNode, QString *error, const QString &parentPath) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; return -1; };
    const QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool()
        || clip.value(ClipKey::audioOnly).toBool()
        || !clip.value(ClipKey::title).toString().isEmpty())
        return fail(QStringLiteral("Select a video clip to paste a correction."));
    if (trackLocked(track))
        return fail(QStringLiteral("Cannot paste a correction: the clip's track is locked."));
    if (kind != QStringLiteral("node") && kind != QStringLiteral("grade"))
        return fail(QStringLiteral("Unknown correction clipboard type."));
    QVariantList copied = kind == QStringLiteral("node") ? QVariantList{payload}
        : payload.value(QStringLiteral("nodes")).toList();
    if (copied.isEmpty()) return fail(QStringLiteral("There is no correction to paste."));
    for (const QVariant &v : copied) if (v.toMap().value("primaries").toMap().isEmpty())
        return fail(QStringLiteral("The copied correction is invalid."));
    const QVariantMap fresh = GradeEngine::freshCopy(kind == QStringLiteral("node") ? QVariantMap{{"nodes", copied}} : payload);
    copied = fresh.value("nodes").toList();
    QVariantMap grade, whole, parent;
    int selected = 0;
    if (kind == QStringLiteral("node")) {
        grade = gradeAt(track, index);
        if (grade.value(QStringLiteral("nodes")).toList().isEmpty()) grade = defaultGrade();
        if(!parentPath.isEmpty()) {
            whole=grade;parent=GradeEngine::nodeAt(whole,parentPath);
            if(parent.value("kind")!="compound")return fail(QStringLiteral("The destination compound no longer exists."));
            grade={{"nodes",parent.value("children")},{"outputNode",parent.value("childOutput")}};
        }
        QVariantList nodes = grade.value(QStringLiteral("nodes")).toList();
        if (afterNode < 0 || afterNode >= nodes.size())
            return fail(QStringLiteral("The destination node no longer exists."));
        selected = afterNode + 1;
        QVariantMap inserted=copied.first().toMap();
        inserted.remove("inputs"); inserted.remove("keyInput");
        if(inserted.value("kind")=="parallel" || inserted.value("kind")=="layer" || inserted.value("kind")=="combiner")inserted["inputs"]=QVariantList{nodes[afterNode].toMap().value("id")};
        nodes.insert(selected, inserted);
        grade.insert(QStringLiteral("nodes"), nodes);
    } else {
        grade = fresh;
        grade.insert(QStringLiteral("nodes"), copied);
    }
    if(!whole.isEmpty()) {
        parent["children"]=grade.value("nodes");parent["childOutput"]=grade.value("outputNode");
        GradeEngine::replaceNode(&whole,parentPath,parent);grade=whole;
    }
    if (!setGradeAt(track, index, grade))
        return fail(QStringLiteral("Could not paste the correction."));
    return selected;
}

QStringList Timeline::gradeLutProblems(const QVariantMap &clip) {
    QStringList problems;
    if (clip.value(ClipKey::disabled).toBool()) return problems;
    problems = VideoEffects::problems(clip.value(ClipKey::videoEffects).toList());
    const QVariantMap grade = clip.value(ClipKey::grade).toMap();
    if (grade.value(QStringLiteral("bypass")).toBool()
        || clip.value(ClipKey::bypass).toStringList().contains(QStringLiteral("grade"))) return problems;
    problems.append(GradeEngine::resourceProblems(grade));
    return problems;
}

bool Timeline::setNodeLutAt(int track, int index, int node, const QString &nodeId,
                              const QString &path, double intensity, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    const QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool()
        || clip.value(ClipKey::audioOnly).toBool() || !clip.value(ClipKey::title).toString().isEmpty())
        return fail(QStringLiteral("Select a video clip to apply a LUT."));
    if (trackLocked(track)) return fail(QStringLiteral("Cannot change the LUT: the clip's track is locked."));
    if (!std::isfinite(intensity) || intensity < 0.0 || intensity > 1.0)
        return fail(QStringLiteral("LUT intensity must be between 0 and 100%."));
    if (!path.isEmpty()) {
        const QString problem = LutFilter::error(path);
        if (!problem.isEmpty()) return fail(problem);
    }
    QVariantMap grade = gradeAt(track, index);
    const bool implicit = grade.value(QStringLiteral("nodes")).toList().isEmpty();
    if (implicit) grade = defaultGrade();
    QVariantList nodes = grade.value(QStringLiteral("nodes")).toList();
    if (node < 0 || node >= nodes.size()) return fail(QStringLiteral("The selected correction node no longer exists."));
    const QString target = nodeId.isEmpty() ? nodes.at(node).toMap().value("id").toString() : nodeId;
    if (implicit && target == QStringLiteral("n0")) { auto n=nodes[node].toMap();n["id"]=target;nodes[node]=n;grade["nodes"]=nodes; }
    if (nodes[node].toMap().value("id").toString() != target.section('/',0,0))
        return fail(QStringLiteral("The selected correction changed. Open the LUT browser again."));
    QVariantMap correction = GradeEngine::nodeAt(grade, target);
    if (correction.isEmpty()) return fail(QStringLiteral("The selected correction node no longer exists."));
    const QVariantMap lut{{QStringLiteral("path"), path}, {QStringLiteral("intensity"), intensity}};
    if (correction.value(QStringLiteral("lut")).toMap() == lut) {
        // A file can be replaced or restored at the same path. Rebuild the
        // filters without creating a project edit, including a previously
        // missing LUT which was omitted when the project loaded.
        if (!implicit && !setGradeAt(track, index, grade)) return fail(QStringLiteral("Could not reload the LUT."));
        return false;
    }
    correction.insert(QStringLiteral("lut"), lut);
    if (!GradeEngine::replaceNode(&grade, target, correction)) return fail(QStringLiteral("The selected node changed."));
    if (!setGradeAt(track, index, grade)) return fail(QStringLiteral("Could not apply the LUT."));
    return true;
}

bool Timeline::previewNodeLutAt(int track, int index, int node, const QString &nodeId,
                                  int clipFrame, const QString &path, double intensity,
                                  QImage *before, QImage *after, QString *error) {
    error->clear();
    QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::offline).toBool()
        || clipFrame < 0 || clipFrame >= clip.value(ClipKey::frames).toInt()) {
        *error = QCoreApplication::translate("Timeline", "Choose an online clip and a frame inside it to preview LUTs."); return false;
    }
    // A small, independent timeline preserves this clip's complete pipeline
    // without changing the live grade, undo stack, playhead or dirty state.
    clip.insert(ClipKey::track, 0); clip.insert(ClipKey::lane, 0);
    clip.insert(ClipKey::index, 0); clip.insert(ClipKey::start, 0);
    clip.insert(ClipKey::gapBefore, 0);
    for (const char *key : {"mixIn", "mixOut", "mixAlign", "mixExtA", "mixExtB"}) clip.remove(QLatin1String(key));
    Timeline scratch;
    QVariantMap project = projectSettings();
    project.insert(QStringLiteral("custom"), true);
    const double scale = qMin(1.0, 640.0 / qMax(1, width()));
    project.insert(QStringLiteral("width"), qMax(16, int(width() * scale) / 2 * 2));
    project.insert(QStringLiteral("height"), qMax(16, int(height() * scale) / 2 * 2));
    if (m_profile) {
        project.insert(QStringLiteral("fpsNum"), m_profile->frame_rate_num());
        project.insert(QStringLiteral("fpsDen"), m_profile->frame_rate_den());
    }
    scratch.setProjectSettings(project);
    scratch.setMediaSources(m_mediaSources, m_proxyMode);
    if (!scratch.restore({clip})) { *error = QCoreApplication::translate("Timeline", "Could not prepare this clip's LUT preview."); return false; }
    *before = scratch.decodeAt(clipFrame, false).image;
    if (before->isNull()) {
        // The node's own LUT may be gone from disk: before is then the
        // picture without it, the way it can be shown.
        QString ignored;
        if (scratch.setNodeLutAt(0, 0, node, nodeId, QString(), intensity, &ignored))
            *before = scratch.decodeAt(clipFrame, false).image;
    }
    const bool changed = scratch.setNodeLutAt(0, 0, node, nodeId, path, intensity, error);
    if (!changed && !error->isEmpty()) return false;
    *after = scratch.decodeAt(clipFrame, false).image;
    if (before->isNull() || after->isNull()) { *error = QCoreApplication::translate("Timeline", "Could not decode the LUT preview."); return false; }
    return true;
}

QImage Timeline::colorMatteAt(int track, int index, const QString &nodeId, int frame, QString *error) {
    QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || frame < 0 || frame >= clip.value(ClipKey::frames).toInt()) return {};
    QVariantMap grade = clip.value(ClipKey::grade).toMap();
    if (grade.value("nodes").toList().isEmpty()) {grade = defaultGrade();auto node=grade.value("nodes").toList().first().toMap();node["id"]="n0";grade["nodes"]=QVariantList{node};}
    grade.insert("viewNode", nodeId.isEmpty() ? grade.value("nodes").toList().first().toMap().value("id") : nodeId);
    grade.insert("bypass", false);
    clip.insert(ClipKey::grade, grade);
    clip.remove(ClipKey::gradeVersions); clip.remove(ClipKey::gradeVersion); clip.remove(ClipKey::bypass);
    clip.insert(ClipKey::track, 0); clip.insert(ClipKey::lane, 0); clip.insert(ClipKey::index, 0); clip.insert(ClipKey::start, 0); clip.insert(ClipKey::gapBefore, 0);
    for (const char *key : {"mixIn", "mixOut", "mixAlign", "mixExtA", "mixExtB"}) clip.remove(key);
    Timeline scratch;
    QVariantMap settings = projectSettings(); settings.insert("custom", true);
    const double scale = qMin(1.0, 640.0 / qMax(1, width()));
    settings.insert("width", qMax(16, int(width()*scale)/2*2)); settings.insert("height", qMax(16, int(height()*scale)/2*2));
    settings.insert("fpsNum", m_profile->frame_rate_num()); settings.insert("fpsDen", m_profile->frame_rate_den());
    scratch.setProjectSettings(settings); scratch.setMediaSources(m_mediaSources, m_proxyMode);
    if (!scratch.restore({clip})) { *error = QCoreApplication::translate("Timeline", "Could not prepare the selected matte."); return {}; }
    if(scratch.engineRendering())
        return scratch.m_renderer->working(frame,error,render::TimelineRenderer::Mode::Matte).pixels();
    if(scratch.linearRendering()) {
        settings["pixelAspect"]=double(m_profile->sample_aspect_num())/m_profile->sample_aspect_den();
        LinearRenderer renderer;renderer.configure(scratch.entries(),scratch.trackFlagsVariant(),settings,QSize(scratch.width(),scratch.height()),scratch.fps());
        return renderer.matte(frame,error);
    }
    return scratch.decodeAt(frame, false).image;
}

// The clip on its own at one of its frames, as the video effect `uid` sees
// it: its own picture through the effects before that one (linear light, the
// engine's working pixels). Null with `error` set.
QImage Timeline::effectInputAt(int track, int index, const QString &uid, int clipFrame, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return QImage(); };
    if (error) error->clear();
    QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool() || clip.value(ClipKey::audioOnly).toBool())
        return fail(QStringLiteral("Select a video clip."));
    if (clip.value(ClipKey::offline).toBool())
        return fail(QStringLiteral("The clip's media is offline."));
    if (clipFrame < 0 || clipFrame >= clip.value(ClipKey::frames).toInt())
        return fail(QStringLiteral("The requested frame is outside this clip."));
    if (!m_profile) return fail(QStringLiteral("There is no timeline to read."));
    QVariantList before;
    bool found = false;
    for (const QVariant &v : clip.value(ClipKey::videoEffects).toList()) {
        if (v.toMap().value(QStringLiteral("uid")).toString() == uid) { found = true; break; }
        before.append(v);
    }
    if (!found) return fail(QStringLiteral("That effect is no longer on the clip."));
    // A key that comes first works on the picture as filmed (see sourceKey in
    // timelinerenderer.cpp): that is the picture its backdrop is found in.
    const auto keyFirst = render::keyFirst(clip.value(ClipKey::videoEffects).toList());
    const bool asFilmed = keyFirst && keyFirst->value(QStringLiteral("uid")).toString() == uid;
    clip[ClipKey::videoEffects] = before;
    if (asFilmed) clip.remove(ClipKey::grade);
    clip[ClipKey::track] = 0; clip[ClipKey::lane] = 0; clip[ClipKey::start] = 0;
    for (const auto *key : {"mixIn", "mixOut", "gapBefore", "opacity", "opacityKf"}) clip.remove(QLatin1String(key));
    auto project = m_project;
    project[QStringLiteral("pixelAspect")] = double(m_profile->sample_aspect_num()) / m_profile->sample_aspect_den();
    project[asFilmed ? QStringLiteral("keyInput") : QStringLiteral("keyInCodes")] = true;
    // Drawn by the engine (a CPU one when the timeline is not), read back in
    // the engine's own space: the one the effect works in.
    QString problem;
    std::unique_ptr<engine::Engine> local;
    if (!render::engineRenderingRequested()) local = engine::createCpuEngine();
    else {
        if (render::engineLost(m_engine.get())) { m_renderer.reset(); m_engine.reset(); }  // see configureEngineRenderer
        if (!m_engine) m_engine = m_engineOnCpu ? engine::createCpuEngine() : render::createEngine();
    }
    render::TimelineRenderer renderer(local ? *local : *m_engine);
    renderer.setMediaSources(m_mediaSources);
    renderer.configure({clip}, {}, project, QSize(width(), height()), fps());
    QImage frame;
    if (renderer.unsupported().isEmpty()) frame = renderer.linear(clipFrame, &problem);
    else problem = renderer.unsupported();
    if (!problem.isEmpty()) return fail(problem);
    if (frame.isNull()) return fail(QStringLiteral("Could not decode this frame."));
    return frame;
}

QImage Timeline::effectPreviewAt(int track, int index, int clipFrame, bool matte, const QSize &most, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return QImage(); };
    if (error) error->clear();
    QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool() || clip.value(ClipKey::audioOnly).toBool()
        || clip.value(ClipKey::offline).toBool() || !m_profile) return {};
    clipFrame = std::clamp(clipFrame, 0, std::max(0, clip.value(ClipKey::frames).toInt() - 1));
    clip[ClipKey::track] = 0; clip[ClipKey::lane] = 0; clip[ClipKey::start] = 0;
    for (const auto *key : {"mixIn", "mixOut", "gapBefore", "opacity", "opacityKf"}) clip.remove(QLatin1String(key));
    const double scale = most.isValid() ? std::min({1.0, double(most.width()) / width(), double(most.height()) / height()}) : 1.0;
    const QSize size(std::max(16, int(std::lround(width() * scale))), std::max(16, int(std::lround(height() * scale))));
    auto project = m_project;
    project[QStringLiteral("pixelAspect")] = double(m_profile->sample_aspect_num()) / m_profile->sample_aspect_den();
    project[QStringLiteral("drawScale")] = double(size.height()) / height();
    project[QStringLiteral("fullSize")] = QSize(width(), height());
    QString problem;
    std::unique_ptr<engine::Engine> local;
    if (!render::engineRenderingRequested()) local = engine::createCpuEngine();
    else {
        if (render::engineLost(m_engine.get())) { m_renderer.reset(); m_engine.reset(); }  // see configureEngineRenderer
        if (!m_engine) m_engine = m_engineOnCpu ? engine::createCpuEngine() : render::createEngine();
    }
    render::TimelineRenderer renderer(local ? *local : *m_engine);
    renderer.setMediaSources(m_mediaSources);
    renderer.configure({clip}, {}, project, size, fps());
    QImage frame;
    if (renderer.unsupported().isEmpty()) frame = renderer.linear(clipFrame, &problem);
    else problem = renderer.unsupported();
    if (!problem.isEmpty()) return fail(problem);
    if (frame.isNull()) return fail(QStringLiteral("Could not decode this frame."));
    // Greys given as the display values a viewer shows them at, in working light.
    const auto light = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float dark = light(0.235f), bright = light(0.353f);
    for (int y = 0; y < frame.height(); ++y) {
        auto *p = reinterpret_cast<float *>(frame.scanLine(y));
        for (int x = 0; x < frame.width(); ++x, p += 4) {
            const float a = std::clamp(p[3], 0.f, 1.f);
            if (matte) {
                p[0] = p[1] = p[2] = light(a);
            } else {
                const float check = ((x / 12 + y / 12) & 1) ? bright : dark;
                for (int c = 0; c < 3; ++c) p[c] = p[c] * a + check * (1.f - a);
            }
            p[3] = 1.f;
        }
    }
    const auto info = ColourPipeline::info();
    return WorkingFrame::adoptLinear(std::move(frame), info.value("config").toString(), info.value("configHash").toString(),
                                     QStringLiteral("oma_interchange"), error).taggedImage();
}

// One frame of one clip as it is graded, drawn alone: other tracks, overlays
// and the master cannot influence it. Cut-relative, so trims, retiming and
// keyframes count. Runs on the player worker thread.
QImage Timeline::isolatedFrameAt(int track, int index, int clipFrame, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; return QImage(); };
    const QVariantMap clip = entryMapOn(track, index);
    auto info = clipInfoAt(track, index);
    if (!info || !info->cut || !m_profile)
        return fail(QStringLiteral("Could not read this clip."));
    // Decode the cut itself: other tracks, overlays and the master composite
    // cannot influence its balance. Cut-relative seeks honour trims, retiming
    // and keyframes. All work runs on the existing player worker thread.
    const int previousPosition = info->cut->position();
    QImage frame;
    if(linearRendering()) {
        auto isolated=clip;isolated[ClipKey::track]=0;isolated[ClipKey::lane]=0;isolated[ClipKey::start]=0;
        for(const auto *key:{"mixIn","mixOut","gapBefore"})isolated.remove(key);
        auto project=m_project;project["pixelAspect"]=double(m_profile->sample_aspect_num())/m_profile->sample_aspect_den();
        QString problem;
        bool drawn=false;
        if(render::engineRenderingRequested()) {
            // The engine measures the clip as it draws it.
            if(render::engineLost(m_engine.get())){m_renderer.reset();m_engine.reset();}  // see configureEngineRenderer
            if(!m_engine)m_engine=m_engineOnCpu?engine::createCpuEngine():render::createEngine();
            render::TimelineRenderer renderer(*m_engine);
            renderer.configure({isolated},{},project,QSize(width(),height()),fps());
            if(renderer.unsupported().isEmpty()){frame=renderer.working(clipFrame,&problem).pixels();drawn=true;}
        }
        if(!drawn){
            LinearRenderer renderer;renderer.configure({isolated},{},project,QSize(width(),height()),fps());
            frame=renderer.render(clipFrame,false,&problem).pixels();
        }
        if(!problem.isEmpty())return fail(problem);
    }
    info->cut->seek(previousPosition);
    if (frame.isNull())
        return fail(QStringLiteral("Could not decode this frame."));
    return frame;
}

int Timeline::autoBalanceAt(int track, int index, int clipFrame, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; return -1; };
    const QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool()
        || clip.value(ClipKey::audioOnly).toBool()
        || !clip.value(ClipKey::title).toString().isEmpty())
        return fail(QStringLiteral("Select a video clip to auto balance."));
    if (trackLocked(track))
        return fail(QStringLiteral("Cannot auto balance: the clip's track is locked."));
    if (clip.value(ClipKey::offline).toBool() || clip.value(ClipKey::disabled).toBool())
        return fail(QStringLiteral("Auto Balance needs an online, enabled video clip."));
    if (clip.value(ClipKey::generator).toString() == QStringLiteral("adjust"))
        return fail(QStringLiteral("Choose a video clip instead of an effect layer for Auto Balance."));
    QVariantMap grade = gradeAt(track, index);
    if (clip.value(ClipKey::bypass).toStringList().contains(QStringLiteral("grade"))
        || grade.value(QStringLiteral("bypass")).toBool())
        return fail(QStringLiteral("Turn off grade bypass before using Auto Balance."));
    if (clipFrame < 0 || clipFrame >= clip.value(ClipKey::frames).toInt())
        return fail(QStringLiteral("The requested frame is outside this clip."));
    QString problem;
    const QImage frame = isolatedFrameAt(track, index, clipFrame, &problem);
    if (frame.isNull())
        return fail(problem.isEmpty() ? QStringLiteral("Could not decode this frame for Auto Balance.") : problem);
    // Grey-world estimate, bounded to a twofold channel correction.
    double sums[3] = {};
    if (!neutralSums(frame, sums))
        return fail(QStringLiteral("Not enough usable colour in this frame. Choose a midtone frame and try Auto Balance again."));
    const double neutral = 0.2126 * sums[0] + 0.7152 * sums[1] + 0.0722 * sums[2];
    QVariantList gain{1.0};
    bool changed = false;
    for (int c = 0; c < 3; ++c) {
        const double factor = qBound(0.5, neutral / sums[c], 2.0);
        gain.append(factor - 1.0);
        changed = changed || qAbs(factor - 1.0) > 0.005;
    }
    if (!changed)
        return fail(QStringLiteral("This frame is already balanced; no correction added."));
    if (grade.value(QStringLiteral("nodes")).toList().isEmpty()) grade = defaultGrade();
    QVariantList nodes = grade.value(QStringLiteral("nodes")).toList();
    QVariantMap node = defaultGradeNode(QStringLiteral("Auto Balance"));
    node.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVariantMap primaries = node.value(QStringLiteral("primaries")).toMap();
    primaries.insert(QStringLiteral("gain"), gain);
    node.insert(QStringLiteral("primaries"), primaries);
    nodes.append(node);
    grade.insert(QStringLiteral("nodes"), nodes);
    if (!setGradeAt(track, index, grade))
        return fail(QStringLiteral("Could not apply Auto Balance."));
    return nodes.size() - 1;
}

int Timeline::matchColorAt(int track, int index, int clipFrame, int refTrack, int refIndex, int refFrame, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; return -1; };
    const QVariantMap clip = entryMapOn(track, index), reference = entryMapOn(refTrack, refIndex);
    const auto pictures = [](const QVariantMap &e) {
        return !e.isEmpty() && !e.value(ClipKey::gap).toBool() && !e.value(ClipKey::audioOnly).toBool()
               && e.value(ClipKey::title).toString().isEmpty() && e.value(ClipKey::generator).toString() != QStringLiteral("adjust");
    };
    if (!pictures(clip)) return fail(QStringLiteral("Select a video clip to match."));
    if (!pictures(reference)) return fail(QStringLiteral("The reference clip is gone; set a new reference."));
    if (track == refTrack && index == refIndex) return fail(QStringLiteral("Choose a different clip from the reference."));
    if (trackLocked(track)) return fail(QStringLiteral("Cannot match colour: the clip's track is locked."));
    QVariantMap grade = gradeAt(track, index);
    if (clip.value(ClipKey::bypass).toStringList().contains(QStringLiteral("grade")) || grade.value(QStringLiteral("bypass")).toBool())
        return fail(QStringLiteral("Turn off grade bypass before matching colour."));
    if (clipFrame < 0 || clipFrame >= clip.value(ClipKey::frames).toInt()
        || refFrame < 0 || refFrame >= reference.value(ClipKey::frames).toInt())
        return fail(QStringLiteral("The frame to match is outside its clip."));
    QString problem;
    const QImage target = isolatedFrameAt(track, index, clipFrame, &problem);
    if (target.isNull()) return fail(problem.isEmpty() ? QStringLiteral("Could not decode the clip's frame.") : problem);
    const QImage source = isolatedFrameAt(refTrack, refIndex, refFrame, &problem);
    if (source.isNull()) return fail(problem.isEmpty() ? QStringLiteral("Could not decode the reference frame.") : problem);
    // Both as graded now; the new correction takes the clip's channel
    // averages to the reference's: its balance and its brightness.
    double mine[3] = {}, theirs[3] = {};
    if (!neutralSums(target, mine) || !neutralSums(source, theirs))
        return fail(QStringLiteral("Not enough usable colour to compare. Choose midtone frames and try again."));
    QVariantList gain{1.0};
    bool changed = false;
    for (int c = 0; c < 3; ++c) {
        const double factor = qBound(0.25, theirs[c] / qMax(1e-9, mine[c]), 4.0);
        gain.append(factor - 1.0);
        changed = changed || qAbs(factor - 1.0) > 0.005;
    }
    if (!changed) return fail(QStringLiteral("This clip already matches the reference; no correction added."));
    if (grade.value(QStringLiteral("nodes")).toList().isEmpty()) grade = defaultGrade();
    QVariantList nodes = grade.value(QStringLiteral("nodes")).toList();
    QVariantMap node = defaultGradeNode(QStringLiteral("Match Colour"));
    node.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVariantMap primaries = node.value(QStringLiteral("primaries")).toMap();
    primaries.insert(QStringLiteral("gain"), gain);
    node.insert(QStringLiteral("primaries"), primaries);
    nodes.append(node);
    grade.insert(QStringLiteral("nodes"), nodes);
    if (!setGradeAt(track, index, grade)) return fail(QStringLiteral("Could not apply Match Colour."));
    return nodes.size() - 1;
}

bool Timeline::editGradeVersionAt(int track, int index, const QString &action,
                                    int version, const QString &name, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty() || clip.value(ClipKey::gap).toBool()
        || clip.value(ClipKey::audioOnly).toBool()
        || !clip.value(ClipKey::title).toString().isEmpty())
        return fail(QStringLiteral("Select a video clip to manage grade versions."));
    if (trackLocked(track))
        return fail(QStringLiteral("Cannot change grade versions: the clip's track is locked."));
    QVariantList versions = clip.value(ClipKey::gradeVersions).toList();
    const QVariantMap currentGrade = gradeAt(track, index);
    const bool implicit = versions.isEmpty();
    if (implicit)
        versions.append(QVariantMap{{QStringLiteral("name"), QStringLiteral("Version 1")},
                                    {QStringLiteral("grade"), currentGrade}});
    int active = qBound(0, clip.value(ClipKey::gradeVersion).toInt(), int(versions.size()) - 1);
    QVariantMap current = versions.at(active).toMap();
    current.insert(QStringLiteral("grade"), currentGrade);
    versions[active] = current;
    auto taken = [&](const QString &candidate, int except) {
        for (int i = 0; i < versions.size(); ++i)
            if (i != except && versions.at(i).toMap().value(QStringLiteral("name")).toString()
                    .compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    if (action == QStringLiteral("add") || action == QStringLiteral("rename")) {
        if (action == QStringLiteral("rename") && (version < 0 || version >= versions.size()))
            return fail(QStringLiteral("That grade version no longer exists."));
        QString label = name.trimmed();
        if (action == QStringLiteral("add") && label.isEmpty()) {
            int n = versions.size() + 1;
            do { label = QStringLiteral("Version %1").arg(n++); } while (taken(label, -1));
        }
        if (label.isEmpty())
            return fail(QStringLiteral("Enter a name for the grade version."));
        if (taken(label, action == QStringLiteral("rename") ? version : -1))
            return fail(QStringLiteral("A grade version named “%1” already exists.").arg(label));
        if (action == QStringLiteral("add")) {
            versions.append(QVariantMap{{QStringLiteral("name"), label},
                                        {QStringLiteral("grade"), currentGrade}});
            active = versions.size() - 1;
        } else {
            QVariantMap renamed = versions.at(version).toMap();
            if (!implicit && renamed.value(QStringLiteral("name")).toString() == label)
                return false;
            renamed.insert(QStringLiteral("name"), label);
            versions[version] = renamed;
        }
    } else if (action == QStringLiteral("switch") || action == QStringLiteral("remove")) {
        if (version < 0 || version >= versions.size())
            return fail(QStringLiteral("That grade version no longer exists."));
        if (action == QStringLiteral("switch")) {
            if (version == active) return false;
            active = version;
        } else {
            if (versions.size() == 1)
                return fail(QStringLiteral("Keep at least one grade version for this clip."));
            versions.removeAt(version);
            if (version < active) --active;
            else if (version == active) active = qMin(active, int(versions.size()) - 1);
        }
    } else {
        return fail(QStringLiteral("Unknown grade version action."));
    }
    for (int i = 0; i < versions.size(); ++i) {
        QVariantMap item = versions.at(i).toMap();
        if (item.value(QStringLiteral("id")).toString().isEmpty()) {
            item.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            versions[i] = item;
        }
    }
    if (!setGradeVersionsAt(track, index, versions, active))
        return fail(QStringLiteral("Could not apply the grade version."));
    return true;
}
