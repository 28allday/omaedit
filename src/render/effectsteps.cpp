#include "effectsteps.h"

#include "videoeffects.h"
#include "editengine.h"

#include <QColor>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <optional>

namespace render {
namespace {

using E = engine::Effect;
// The catalogue's effects drawn as engine effects, by catalogue id.
const QHash<QString, E> &engineEffects() {
    static const QHash<QString, E> map{
        {"invert", E::Invert}, {"black-white", E::BlackWhite}, {"sepia", E::Sepia}, {"posterize", E::Posterize},
        {"threshold", E::Threshold}, {"vignette", E::Vignette}, {"letterbox", E::Letterbox}, {"scanlines", E::Scanlines},
        {"mirror", E::Mirror}, {"rgb-split", E::RgbSplit}, {"pixelate", E::Pixelate},
        {"strobe", E::Strobe}, {"wave", E::Wave}, {"corner-pin", E::CornerPin}, {"fisheye", E::Fisheye},
        {"spill-suppress", E::SpillSuppress}, {"directional-blur", E::DirectionalBlur},
        {"glow", E::Glow}, {"soft-glow", E::SoftGlow}, {"edge-glow", E::EdgeGlow}, {"emboss", E::Emboss},
        {"cartoon", E::Cartoon}, {"charcoal", E::Charcoal}, {"halftone", E::Halftone}, {"film-grain", E::FilmGrain},
        {"old-film", E::OldFilm}, {"dust", E::Dust}, {"scratches", E::Scratches}, {"glitch", E::Glitch},
    };
    return map;
}
// Done by the engine's own steps.
const QStringList &ownSteps() {
    static const QStringList ids{"gaussian-blur", "box-blur", "sharpen", "drop-shadow", "chroma-key", "colour-key", "luma-key", "remove-background", "light-wrap"};
    return ids;
}

// The keys that take a backdrop or people out of the picture as filmed.
bool isKey(const QString &id) {
    return id == QLatin1String("chroma-key") || id == QLatin1String("colour-key") || id == QLatin1String("remove-background");
}

// The effect's controls in its catalogue order, as numbers: a number as it
// is, a switch 0 or 1, a choice the index of its value.
void readValues(const QString &id, const QVariantMap &params, float out[16]) {
    const QVariantList fields = VideoEffects::definition(id).value("params").toList();
    for (int i = 0; i < fields.size() && i < 16; ++i) {
        const QVariantMap field = fields[i].toMap();
        const QVariant value = params.value(field.value("key").toString(), field.value("default"));
        const QString type = field.value("type").toString();
        if (type == "bool") out[i] = value.toBool() ? 1.f : 0.f;
        else if (type == "choice") out[i] = float(std::max(0, int(field.value("values").toList().indexOf(value))));
        else out[i] = float(value.toDouble());
    }
}

// The luma key takes out the band threshold ± tolerance and keeps what lies
// beyond it, fading in over the softness outside the band's edges (as the
// filter it stands for does): one step wherever the band lies (it may run
// past black or white), so the matte's edge moves evenly with the threshold.
std::optional<engine::VideoFilter> lumaKey(const QVariantMap &params) {
    const double threshold = params.value("av.threshold", 0.).toDouble(), tolerance = params.value("av.tolerance", .1).toDouble();
    const double soft = params.value("av.softness", .05).toDouble();
    engine::VideoFilter f;
    f.type = engine::VideoFilterType::Effect;
    f.effect = engine::Effect::LumaBand;
    f.values[0] = float(threshold - tolerance);
    f.values[1] = float(threshold + tolerance);
    f.values[2] = float(soft);
    return f;
}

// The key, then, when any is set, its matte's extras: grow or shrink, blur,
// the garbage matte, the matte shown.
std::vector<engine::VideoFilter> keyed(const QString &id, const QVariantMap &params, double scale) {
    std::vector<engine::VideoFilter> out;
    engine::VideoFilter key;
    if (id == QLatin1String("remove-background")) {
        key.type = engine::VideoFilterType::RemoveBackground;
        key.quality = std::max(0, int(QStringList{"fast", "balanced", "best"}.indexOf(params.value("quality").toString())));
        // Playback drawn at half or a quarter finds the people at half or a quarter the size.
        key.matteShare = float(std::clamp(scale, 0.0, 1.0));
        key.tighten = float(std::clamp(params.value("tighten").toDouble(), 0.0, 100.0) / 100.0);
        key.blurBackground = params.value("background").toString() == QLatin1String("blur");
        key.radius = float(std::clamp(params.value("blurAmount").toDouble(), 0.5, 10.0) / 100.0);
    } else if (id == QLatin1String("colour-key")) {
        const QColor c(params.value("colour").toString());
        key.type = engine::VideoFilterType::ColourKey;
        key.key = {float(c.redF()), float(c.greenF()), float(c.blueF()), 1};
        key.similarity = float(std::clamp(params.value("similarity").toDouble(), 0.0, 100.0) / 100.0);
        key.smoothness = float(std::clamp(params.value("smoothness").toDouble(), 0.0, 100.0) / 100.0);
    } else {
        const QColor c(params.value("backdrop").toString());
        key.type = engine::VideoFilterType::ChromaKey;
        key.key = {float(c.redF()), float(c.greenF()), float(c.blueF()), 1};
        key.similarity = float(std::clamp(params.value("similarity").toDouble(), 0.0, 60.0) / 100.0);
        key.smoothness = float(std::clamp(params.value("softness").toDouble(), 5.0, 100.0) / 100.0);
        key.spill = float(std::clamp(params.value("spill").toDouble(), 0.0, 100.0) / 100.0);
    }
    out.push_back(key);
    const double grow = params.value("grow").toDouble(), blur = params.value("matteBlur").toDouble();
    const bool garbage = params.value("garbage").toBool(), show = params.value("showMatte").toBool();
    if (grow == 0 && blur <= 0 && !garbage && !show) return out;
    engine::VideoFilter refine;
    refine.type = engine::VideoFilterType::Effect;
    refine.effect = engine::Effect::MatteRefine;
    refine.values[0] = float(std::clamp(grow, -20.0, 20.0));
    refine.values[1] = float(std::clamp(blur, 0.0, 20.0));
    refine.values[2] = garbage ? 1.f : 0.f;
    refine.values[3] = float(std::clamp(params.value("garbageLeft").toDouble(), 0.0, 1.0));
    refine.values[4] = float(std::clamp(params.value("garbageTop").toDouble(), 0.0, 1.0));
    refine.values[5] = float(std::clamp(params.value("garbageRight", 1.).toDouble(), 0.0, 1.0));
    refine.values[6] = float(std::clamp(params.value("garbageBottom", 1.).toDouble(), 0.0, 1.0));
    refine.values[7] = params.value("garbageInvert").toBool() ? 1.f : 0.f;
    refine.values[8] = show ? 1.f : 0.f;
    out.push_back(refine);
    return out;
}

// Where a frame falls among keys of any kind of value: the key before it, the
// one after and how far along between them (0..1, by the earlier key's easing).
struct Between { int from = 0, to = 0; double along = 0; };
Between between(const QVariantList &keys, int frame) {
    // The keys' own places as the values: the curve reader then gives the place.
    QVariantList places;
    for (int i = 0; i < keys.size(); ++i) {
        QVariantMap k = keys[i].toMap();
        k["value"] = i;
        places << k;
    }
    const double at = std::clamp(EditEngine::valueAt(places, frame, 0.), 0.0, double(std::max<qsizetype>(0, keys.size() - 1)));
    Between b;
    b.from = int(std::floor(at));
    b.to = std::min(b.from + 1, int(keys.size()) - 1);
    b.along = at - b.from;
    return b;
}

// The effect's controls at a frame of the clip: as set, with each animated
// control taken from its keys (a whole-number control stays whole; a colour
// moves between its keys; a switch or a choice holds each key's setting).
QVariantMap paramsAt(const QVariantMap &state, int frame) {
    const QString id = state.value("id").toString();
    QVariantMap params = VideoEffects::defaults(id);
    const QVariantMap given = state.value("params").toMap();
    for (auto it = given.begin(); it != given.end(); ++it) params[it.key()] = it.value();
    const QVariantMap keys = state.value("keys").toMap();
    const QVariantList fields = VideoEffects::definition(id).value("params").toList();
    // A project restored from its file is not validated: a number outside its
    // control's range (or none at all) is brought inside it here, so the
    // engine's steps and both its twins are handed the same, sane value.
    for (const QVariant &v : fields) {
        const QVariantMap field = v.toMap();
        if (field.value("type").toString() != QLatin1String("number")) continue;
        const QString key = field.value("key").toString();
        const double value = params.value(key).toDouble(), low = field.value("min").toDouble(), high = field.value("max").toDouble();
        if (!std::isfinite(value)) params[key] = field.value("default");
        else if (value < low || value > high) params[key] = std::clamp(value, low, high);
    }
    if (keys.isEmpty()) return params;
    for (const QVariant &v : fields) {
        const QVariantMap field = v.toMap();
        const QString key = field.value("key").toString(), type = field.value("type").toString();
        const QVariantList curve = keys.value(key).toList();
        if (curve.isEmpty()) continue;
        if (type == QLatin1String("number")) {
            double value = EditEngine::valueAt(curve, frame, params.value(key).toDouble());
            if (field.value("step").toDouble() == 1) value = std::round(value);
            params[key] = std::clamp(value, field.value("min").toDouble(), field.value("max").toDouble());
            continue;
        }
        const Between at = between(curve, frame);
        const QVariant from = curve[at.from].toMap().value("value"), to = curve[at.to].toMap().value("value");
        if (type == QLatin1String("color")) {
            // Each channel on its own way between the two colours.
            const QColor a(from.toString()), b(to.toString());
            const auto mix = [&](float x, float y) { return float(x + (y - x) * at.along); };
            params[key] = QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()),
                                           mix(a.alphaF(), b.alphaF())).name(QColor::HexArgb);
        } else params[key] = from;  // a switch or a choice holds until the next key
    }
    return params;
}

// The effect's mask as the shape its step is kept within.
engine::StepShape shapeOf(const QVariantMap &state, int frame) {
    engine::StepShape s;
    QVariantMap mask = state.value("mask").toMap();
    // Its animated fields at this frame of the clip.
    const QVariantMap maskKeys = state.value("maskKeys").toMap();
    for (auto it = maskKeys.begin(); it != maskKeys.end(); ++it) {
        const QVariantList curve = it.value().toList();
        if (curve.isEmpty()) continue;
        if (it.key() != QLatin1String("points")) {
            mask[it.key()] = EditEngine::valueAt(curve, frame, mask.value(it.key()).toDouble());
            continue;
        }
        // A polygon's corners, each on its way between the two keys.
        const Between at = between(curve, frame);
        const QVariantList a = curve[at.from].toMap().value("value").toList(), b = curve[at.to].toMap().value("value").toList();
        QVariantList points;
        for (int i = 0; i < a.size() && i < b.size(); ++i) {
            const QVariantList p = a[i].toList(), q = b[i].toList();
            points << QVariant(QVariantList{p.value(0).toDouble() + (q.value(0).toDouble() - p.value(0).toDouble()) * at.along,
                                            p.value(1).toDouble() + (q.value(1).toDouble() - p.value(1).toDouble()) * at.along});
        }
        if (points.size() == mask.value("points").toList().size()) mask["points"] = points;
    }
    const QString shape = mask.value("shape").toString();
    if (mask.isEmpty() || !VideoEffects::takesMask(state.value("id").toString())) return s;
    if (shape == QLatin1String("polygon")) {
        const QVariantList points = mask.value("points").toList();
        if (points.size() < 3 || points.size() > 8) return s;
        s.kind = 3;
        s.count = int(points.size());
        for (int i = 0; i < s.count; ++i) {
            const QVariantList p = points[i].toList();
            s.points[2 * i] = float(p.value(0).toDouble());
            s.points[2 * i + 1] = float(p.value(1).toDouble());
        }
    } else if (shape == QLatin1String("rectangle") || shape == QLatin1String("ellipse")) {
        s.kind = shape == QLatin1String("ellipse") ? 2 : 1;
        s.box[0] = float(mask.value("x", .5).toDouble());
        s.box[1] = float(mask.value("y", .5).toDouble());
        s.box[2] = float(mask.value("w", .5).toDouble() / 2);
        s.box[3] = float(mask.value("h", .5).toDouble() / 2);
        s.box[4] = float(mask.value("rotation").toDouble());
    } else return s;
    s.feather = float(std::clamp(mask.value("feather").toDouble(), 0.0, 0.5));
    s.invert = mask.value("invert").toBool();
    return s;
}

std::optional<engine::VideoFilter> shapeless(const QVariantMap &state, int frame, double scale);
std::optional<engine::VideoFilter> step(const QVariantMap &state, int frame, double scale) {
    auto f = shapeless(state, frame, scale);
    if (f) f->within = shapeOf(state, frame);
    return f;
}

std::optional<engine::VideoFilter> shapeless(const QVariantMap &state, int frame, double scale) {
    const QString id = state.value("id").toString();
    // Its own clock: the frames it had run on the part of a cut clip before.
    const QVariantMap params = paramsAt(state, frame);
    frame += state.value("clock").toInt();
    engine::VideoFilter f;
    if (const auto it = engineEffects().constFind(id); it != engineEffects().constEnd()) {
        f.type = engine::VideoFilterType::Effect;
        f.effect = *it;
        f.frame = frame;
        f.scale = float(scale);
        readValues(id, params, f.values);
        return f;
    }
    if (id == "gaussian-blur") {
        // A Gaussian of this sigma (pixels of the frame); the engine's blur
        // reaches three sigma, up to its limit (beyond the whole slider). The
        // limit is in pixels of the whole picture, so a smaller drawing shows
        // the same blur.
        f.type = engine::VideoFilterType::Blur;
        f.radius = float(std::min(double(engine::kBlurReach), 3.0 * params.value("av.sigma").toDouble()) * scale);
        return f;
    }
    if (id == "box-blur") {
        // A box of this radius run this many times spreads as a Gaussian of
        // this sigma (the variance of n boxes); drawn as that Gaussian.
        const double r = params.value("av.luma_radius").toDouble(), n = std::max(1.0, params.value("av.luma_power").toDouble());
        f.type = engine::VideoFilterType::Blur;
        f.radius = float(std::min(double(engine::kBlurReach), 3.0 * std::sqrt(n * ((2 * r + 1) * (2 * r + 1) - 1) / 12.0)) * scale);
        return f;
    }
    if (id == "sharpen") {
        f.type = engine::VideoFilterType::Sharpen;
        f.amount = float(std::clamp(params.value("av.luma_amount").toDouble() / 5.0, 0.0, 1.0));
        return f;
    }
    if (id == "drop-shadow") {
        const QColor c(params.value("color").toString());
        f.type = engine::VideoFilterType::Shadow;
        f.radius = float(std::clamp(params.value("radius").toDouble(), 0.0, double(engine::kBlurReach)) * scale);
        f.offsetX = int(std::lround(params.value("x").toDouble() * scale));
        f.offsetY = int(std::lround(params.value("y").toDouble() * scale));
        f.shadow = {float(c.redF()), float(c.greenF()), float(c.blueF()), 1};
        f.opacity = float(c.alphaF());
        return f;
    }
    if (id == "luma-key") return lumaKey(params);
    if (id == "light-wrap") {
        f.type = engine::VideoFilterType::LightWrap;
        f.radius = float(std::clamp(params.value("width").toDouble(), 0.5, 25.0) / 100.0);
        f.opacity = float(std::clamp(params.value("strength").toDouble(), 0.0, 300.0) / 100.0);
        f.blendColour = params.value("style").toString() == QLatin1String("blend");
        return f;
    }
    return std::nullopt;
}

}  // namespace

std::optional<QVariantMap> keyFirst(const QVariantList &stack) {
    for (const QVariant &v : stack) {
        const QVariantMap state = v.toMap();
        if (state.value("bypass").toBool()) continue;
        const QString id = state.value("id").toString();
        if (isKey(id) || id == QLatin1String("luma-key")) return state;
        return std::nullopt;
    }
    return std::nullopt;
}

bool effectsAnimated(const QVariantList &stack) {
    return std::any_of(stack.begin(), stack.end(), [](const QVariant &v) {
        return !v.toMap().value("keys").toMap().isEmpty() || !v.toMap().value("maskKeys").toMap().isEmpty();
    });
}

bool hasActiveEffects(const QVariantList &stack) {
    return std::any_of(stack.begin(), stack.end(), [](const QVariant &v) { return !v.toMap().value("bypass").toBool(); });
}

QString effectsUnsupported(const QVariantList &stack) {
    for (const QVariant &v : stack) {
        const QVariantMap state = v.toMap();
        if (state.value("bypass").toBool()) continue;
        const QString id = state.value("id").toString();
        if (!engineEffects().contains(id) && !ownSteps().contains(id)) return id;
        if (!isKey(id) && !step(state, 0, 1.0)) return id;
    }
    return {};
}

std::vector<engine::VideoFilter> effectSteps(const QVariantList &stack, int frame, double scale) {
    std::vector<engine::VideoFilter> steps;
    for (const QVariant &v : stack) {
        const QVariantMap state = v.toMap();
        if (state.value("bypass").toBool()) continue;
        if (const QString id = state.value("id").toString(); isKey(id)) {
            for (const auto &f : keyed(id, paramsAt(state, frame), scale)) steps.push_back(f);
            continue;
        }
        if (auto f = step(state, frame, scale)) steps.push_back(*f);
    }
    return steps;
}

}  // namespace render
