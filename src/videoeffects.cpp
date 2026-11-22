#include "videoeffects.h"
#include "graph.h"
#include "timeline.h"
#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>

namespace {
QString tr(const char *s) { return QCoreApplication::translate("VideoEffects", s); }
QVariantMap number(const char *key, const char *label, double low, double high,
                   double initial, double step = .01, double scale = 1, double offset = 0) {
    return {{"key", key}, {"label", tr(label)}, {"type", "number"}, {"min", low},
            {"max", high}, {"default", initial}, {"step", step}, {"scale", scale}, {"offset", offset}};
}
QVariantMap boolean(const char *key, const char *label, bool initial = false) {
    return {{"key", key}, {"label", tr(label)}, {"type", "bool"}, {"default", initial}};
}
QVariantMap color(const char *key, const char *label, const char *initial) {
    return {{"key", key}, {"label", tr(label)}, {"type", "color"}, {"default", initial}};
}
QVariantMap choice(const char *key, const char *label, const QStringList &labels,
                   const QVariantList &values, int initial = 0) {
    return {{"key", key}, {"label", tr(label)}, {"type", "choice"},
            {"labels", labels}, {"values", values}, {"default", values.at(initial)}};
}
// A control that only matters while `key` holds `value`.
QVariantMap when(QVariantMap field, const char *key, const char *value) {
    field["whenKey"] = QString::fromLatin1(key);
    field["whenValue"] = QString::fromLatin1(value);
    return field;
}
QVariantMap effect(const char *id, const char *name, const char *category,
                    const char *service, const QVariantList &params, const QVariantMap &fixed = {}) {
    return {{"id", id}, {"key", QStringLiteral("video:") + id}, {"name", tr(name)},
            {"cat", category}, {"service", service}, {"params", params}, {"fixed", fixed}};
}

QVariantMap described(const char *text, QVariantMap definition) {
    definition["description"] = tr(text);
    return definition;
}

QVariantList definitions() {
    // Parameter identifiers as the engine's effect steps read them. Use
    // native filters, with bounded UI ranges and alpha-preserving variants.
    static const QVariantList result = {
        effect("gaussian-blur", QT_TRANSLATE_NOOP("VideoEffects", "Gaussian Blur"), "blur", "avfilter.gblur",
               {number("av.sigma", QT_TRANSLATE_NOOP("VideoEffects", "Radius"), 0, 100, 5, .1)}, {{"av.steps", 2}, {"av.threads", 2}}),
        effect("box-blur", QT_TRANSLATE_NOOP("VideoEffects", "Box Blur"), "blur", "avfilter.boxblur",
               {number("av.luma_radius", QT_TRANSLATE_NOOP("VideoEffects", "Radius"), 0, 20, 3, 1), number("av.luma_power", QT_TRANSLATE_NOOP("VideoEffects", "Passes"), 1, 5, 2, 1)},
               {{"av.chroma_radius", 0}, {"av.alpha_radius", 0}, {"av.threads", 2}}),
        effect("directional-blur", QT_TRANSLATE_NOOP("VideoEffects", "Directional Blur"), "blur", "avfilter.dblur",
               {number("av.radius", QT_TRANSLATE_NOOP("VideoEffects", "Radius"), 0, 100, 10, .1), number("av.angle", QT_TRANSLATE_NOOP("VideoEffects", "Angle"), 0, 360, 45, 1)}, {{"av.threads", 2}}),
        effect("sharpen", QT_TRANSLATE_NOOP("VideoEffects", "Sharpen"), "blur", "avfilter.unsharp",
               {number("av.luma_amount", QT_TRANSLATE_NOOP("VideoEffects", "Amount"), 0, 5, 1, .05)}, {{"av.threads", 2}}),
        effect("soft-glow", QT_TRANSLATE_NOOP("VideoEffects", "Soft Glow"), "stylise", "frei0r.softglow",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Blur"), 0, 1, .5), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Brightness"), 0, 1, .75), number("2", QT_TRANSLATE_NOOP("VideoEffects", "Sharpness"), 0, 1, .85)}),
        effect("glow", QT_TRANSLATE_NOOP("VideoEffects", "Glow"), "stylise", "frei0r.glow", {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Blur"), 0, 1, .2)}),
        effect("vignette", QT_TRANSLATE_NOOP("VideoEffects", "Vignette"), "stylise", "frei0r.vignette",
               {number("1", QT_TRANSLATE_NOOP("VideoEffects", "Clear centre"), 0, 1, .25), number("2", QT_TRANSLATE_NOOP("VideoEffects", "Softness"), 0, 1, .6), number("0", QT_TRANSLATE_NOOP("VideoEffects", "Aspect"), 0, 1, .5)}),
        effect("sepia", QT_TRANSLATE_NOOP("VideoEffects", "Sepia"), "stylise", "sepia",
               {number("u", QT_TRANSLATE_NOOP("VideoEffects", "Yellow–blue"), 0, 255, 75, 1), number("v", QT_TRANSLATE_NOOP("VideoEffects", "Cyan–red"), 0, 255, 150, 1)}),
        effect("black-white", QT_TRANSLATE_NOOP("VideoEffects", "Black & White"), "stylise", "frei0r.bw0r", {}),
        effect("invert", QT_TRANSLATE_NOOP("VideoEffects", "Invert"), "stylise", "frei0r.invert0r", {}),
        effect("posterize", QT_TRANSLATE_NOOP("VideoEffects", "Posterise"), "stylise", "frei0r.posterize",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Levels"), 2, 50, 6, 1, 1.0 / 48, -2.0 / 48)}),
        effect("threshold", QT_TRANSLATE_NOOP("VideoEffects", "Threshold"), "stylise", "frei0r.threshold0r", {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Threshold"), 0, 1, .5)}),
        effect("halftone", QT_TRANSLATE_NOOP("VideoEffects", "Colour Halftone"), "stylise", "frei0r.colorhalftone",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Dot size"), 0, 1, .4), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Cyan angle"), 0, 1, .3),
                number("2", QT_TRANSLATE_NOOP("VideoEffects", "Magenta angle"), 0, 1, .45), number("3", QT_TRANSLATE_NOOP("VideoEffects", "Yellow angle"), 0, 1, .25)}),
        effect("film-grain", QT_TRANSLATE_NOOP("VideoEffects", "Film Grain"), "texture", "frei0r.filmgrain",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Grain"), 0, 1, .3), number("4", QT_TRANSLATE_NOOP("VideoEffects", "Blur"), 0, 1, .1),
                number("5", QT_TRANSLATE_NOOP("VideoEffects", "Dust"), 0, 1, .1), number("6", QT_TRANSLATE_NOOP("VideoEffects", "Flicker"), 0, 1, .1)}),
        effect("old-film", QT_TRANSLATE_NOOP("VideoEffects", "Old Film"), "texture", "oldfilm",
               {number("delta", QT_TRANSLATE_NOOP("VideoEffects", "Jitter"), 0, 100, 14, 1), number("every", QT_TRANSLATE_NOOP("VideoEffects", "Frequency"), 0, 100, 20, 1)}),
        effect("dust", QT_TRANSLATE_NOOP("VideoEffects", "Dust"), "texture", "dust",
               {number("maxdiameter", QT_TRANSLATE_NOOP("VideoEffects", "Size"), 0, 100, 2, 1), number("maxcount", QT_TRANSLATE_NOOP("VideoEffects", "Amount"), 0, 100, 10, 1)}),
        effect("scratches", QT_TRANSLATE_NOOP("VideoEffects", "Film Scratches"), "texture", "lines",
               {number("line_width", QT_TRANSLATE_NOOP("VideoEffects", "Width"), 0, 100, 2, 1), number("num", QT_TRANSLATE_NOOP("VideoEffects", "Amount"), 0, 100, 5, 1)}),
        effect("scanlines", QT_TRANSLATE_NOOP("VideoEffects", "Scanlines"), "texture", "frei0r.scanline0r", {}),
        effect("pixelate", QT_TRANSLATE_NOOP("VideoEffects", "Pixelate"), "stylise", "frei0r.pixeliz0r",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Block width"), .001, 1, .04, .001), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Block height"), .001, 1, .04, .001)}, {{"2", 1}}),
        effect("emboss", QT_TRANSLATE_NOOP("VideoEffects", "Emboss"), "stylise", "frei0r.emboss",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Direction"), 0, 1, .375), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Elevation"), 0, 1, .333333), number("2", QT_TRANSLATE_NOOP("VideoEffects", "Depth"), 0, 1, .25)}),
        effect("cartoon", QT_TRANSLATE_NOOP("VideoEffects", "Cartoon"), "stylise", "frei0r.cartoon",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Threshold"), 0, 1, .5), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Difference"), .001, 1, .01, .001)}),
        effect("edge-glow", QT_TRANSLATE_NOOP("VideoEffects", "Edge Glow"), "stylise", "frei0r.edgeglow",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Threshold"), 0, 1, .2), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Light gain"), 0, 1, .5), number("2", QT_TRANSLATE_NOOP("VideoEffects", "Colour gain"), 0, 1, .5)}),
        effect("charcoal", QT_TRANSLATE_NOOP("VideoEffects", "Charcoal"), "stylise", "charcoal",
               {number("x_scatter", QT_TRANSLATE_NOOP("VideoEffects", "Line width"), 1, 10, 1, 1), number("y_scatter", QT_TRANSLATE_NOOP("VideoEffects", "Line height"), 1, 10, 1, 1),
                number("scale", QT_TRANSLATE_NOOP("VideoEffects", "Contrast"), 0, 5, 1.5), number("mix", QT_TRANSLATE_NOOP("VideoEffects", "Colour"), 0, 1, 0), boolean("invert", QT_TRANSLATE_NOOP("VideoEffects", "Invert"))}),
        effect("glitch", QT_TRANSLATE_NOOP("VideoEffects", "Glitch"), "texture", "frei0r.glitch0r",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Frequency"), 0, 1, .2), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Block height"), 0, 1, .2),
                number("2", QT_TRANSLATE_NOOP("VideoEffects", "Shift"), 0, 1, .2), number("3", QT_TRANSLATE_NOOP("VideoEffects", "Colour glitch"), 0, 1, .1)}),
        effect("rgb-split", QT_TRANSLATE_NOOP("VideoEffects", "RGB Split"), "distort", "frei0r.rgbsplit0r",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Vertical"), -1, 1, 0, .01, .5, .5), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Horizontal"), -1, 1, .05, .01, .5, .5)}),
        effect("wave", QT_TRANSLATE_NOOP("VideoEffects", "Wave"), "distort", "wave",
               {number("wave", QT_TRANSLATE_NOOP("VideoEffects", "Amplitude"), 1, 100, 10, 1), number("speed", QT_TRANSLATE_NOOP("VideoEffects", "Speed"), 0, 50, 5, 1),
                boolean("deformX", QT_TRANSLATE_NOOP("VideoEffects", "Horizontal"), true), boolean("deformY", QT_TRANSLATE_NOOP("VideoEffects", "Vertical"), true)}),
        effect("strobe", QT_TRANSLATE_NOOP("VideoEffects", "Strobe"), "texture", "strobe",
               {number("interval", QT_TRANSLATE_NOOP("VideoEffects", "Interval (frames)"), 1, 100, 3, 1), boolean("strobe_invert", QT_TRANSLATE_NOOP("VideoEffects", "Invert"))}),
        effect("mirror", QT_TRANSLATE_NOOP("VideoEffects", "Mirror"), "distort", "mirror",
               {choice("mirror", QT_TRANSLATE_NOOP("VideoEffects", "Direction"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Horizontal")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Vertical")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Diagonal")), tr(QT_TRANSLATE_NOOP("VideoEffects", "X diagonal"))},
                        {"horizontal", "vertical", "diagonal", "xdiagonal"}), boolean("reverse", QT_TRANSLATE_NOOP("VideoEffects", "Reverse"))}),
        effect("letterbox", QT_TRANSLATE_NOOP("VideoEffects", "Letterbox"), "stylise", "frei0r.letterb0xed",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Border width"), 0, 1, .2), boolean("1", QT_TRANSLATE_NOOP("VideoEffects", "Transparent borders"))}),
        // Image-only, safe on the decode/export workers (the editor and the
        // headless CLI run without a QApplication).
        effect("drop-shadow", QT_TRANSLATE_NOOP("VideoEffects", "Drop Shadow"), "stylise", "omaedit.shadow",
               {color("color", QT_TRANSLATE_NOOP("VideoEffects", "Colour"), "#b4000000"), number("radius", QT_TRANSLATE_NOOP("VideoEffects", "Blur"), 0, 100, 10, .1),
                number("x", QT_TRANSLATE_NOOP("VideoEffects", "Offset X"), -500, 500, 12, 1), number("y", QT_TRANSLATE_NOOP("VideoEffects", "Offset Y"), -500, 500, 12, 1)}),
        // The backdrop is the screen as filmed (Find backdrop samples it from
        // the frame under the playhead); the matte's extras follow the key.
        described(QT_TRANSLATE_NOOP("VideoEffects", "Takes out a green, blue or magenta screen. The backdrop's colour is found in the picture; hair and motion keep their soft edges, and the screen's colour is taken out of them and of what it lit."),
        effect("chroma-key", QT_TRANSLATE_NOOP("VideoEffects", "Chroma Key"), "keying", "omaedit.keyer",
               {choice("screen", QT_TRANSLATE_NOOP("VideoEffects", "Screen"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Green")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Blue")),
                       tr(QT_TRANSLATE_NOOP("VideoEffects", "Magenta")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Other"))}, {"green", "blue", "magenta", "other"}),
                color("backdrop", QT_TRANSLATE_NOOP("VideoEffects", "Backdrop"), "#3fcc4a"),
                number("similarity", QT_TRANSLATE_NOOP("VideoEffects", "Similarity"), 0, 60, 15, 1),
                number("softness", QT_TRANSLATE_NOOP("VideoEffects", "Edge softness"), 5, 100, 80, 1),
                number("spill", QT_TRANSLATE_NOOP("VideoEffects", "Spill reduction"), 0, 100, 100, 1),
                number("grow", QT_TRANSLATE_NOOP("VideoEffects", "Edge grow / shrink"), -20, 20, 0, .5),
                number("matteBlur", QT_TRANSLATE_NOOP("VideoEffects", "Matte blur"), 0, 20, 0, .5),
                number("temporal", QT_TRANSLATE_NOOP("VideoEffects", "Temporal smoothing"), 0, 100, 0, 1),
                boolean("garbage", QT_TRANSLATE_NOOP("VideoEffects", "Garbage matte")),
                number("garbageLeft", QT_TRANSLATE_NOOP("VideoEffects", "Garbage left"), 0, 1, 0, .005),
                number("garbageTop", QT_TRANSLATE_NOOP("VideoEffects", "Garbage top"), 0, 1, 0, .005),
                number("garbageRight", QT_TRANSLATE_NOOP("VideoEffects", "Garbage right"), 0, 1, 1, .005),
                number("garbageBottom", QT_TRANSLATE_NOOP("VideoEffects", "Garbage bottom"), 0, 1, 1, .005),
                boolean("garbageInvert", QT_TRANSLATE_NOOP("VideoEffects", "Remove inside the box")),
                boolean("showMatte", QT_TRANSLATE_NOOP("VideoEffects", "Show matte"))})),
        // People found by a network, with no screen; the matte's extras as the chroma key's.
        described(QT_TRANSLATE_NOOP("VideoEffects", "Finds the people in the picture and takes everything else away, with no green screen. It knows people only, not objects or pets, and works best on one or two people from the waist up."),
        effect("remove-background", QT_TRANSLATE_NOOP("VideoEffects", "Remove Background (AI)"), "keying", "omaedit.matting",
               {choice("background", QT_TRANSLATE_NOOP("VideoEffects", "Background"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Take away")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Blur"))}, {"remove", "blur"}),
                when(number("blurAmount", QT_TRANSLATE_NOOP("VideoEffects", "Blur"), .5, 10, 3, .5), "background", "blur"),
                choice("quality", QT_TRANSLATE_NOOP("VideoEffects", "Quality"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Fast")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Balanced")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Best"))},
                       {"fast", "balanced", "best"}, 2),
                number("tighten", QT_TRANSLATE_NOOP("VideoEffects", "Tighten edges"), 0, 100, 0, 1),
                number("grow", QT_TRANSLATE_NOOP("VideoEffects", "Edge grow / shrink"), -20, 20, 0, .5),
                number("matteBlur", QT_TRANSLATE_NOOP("VideoEffects", "Matte blur"), 0, 20, 0, .5),
                number("temporal", QT_TRANSLATE_NOOP("VideoEffects", "Temporal smoothing"), 0, 100, 0, 1),
                boolean("showMatte", QT_TRANSLATE_NOOP("VideoEffects", "Show matte"))})),
        described(QT_TRANSLATE_NOOP("VideoEffects", "Makes one colour transparent, brightness included."),
        effect("colour-key", QT_TRANSLATE_NOOP("VideoEffects", "Colour Key"), "keying", "omaedit.colourkey",
               {color("colour", QT_TRANSLATE_NOOP("VideoEffects", "Colour"), "#00ff00"),
                number("similarity", QT_TRANSLATE_NOOP("VideoEffects", "Similarity"), 0, 100, 10, 1),
                number("smoothness", QT_TRANSLATE_NOOP("VideoEffects", "Smoothness"), 0, 100, 5, 1)})),
        // The light of whatever is behind the clip, spilling over its edges.
        described(QT_TRANSLATE_NOOP("VideoEffects", "Lets the light of whatever is behind this clip spill a little way over its edges, so a keyed person looks lit by the scene they are put in. Put it after a key. It needs what is behind, so the preview shows the timeline while it is selected."),
        effect("light-wrap", QT_TRANSLATE_NOOP("VideoEffects", "Light Wrap"), "keying", "omaedit.lightwrap",
               {choice("style", QT_TRANSLATE_NOOP("VideoEffects", "Style"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Add light")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Blend colour"))}, {"light", "blend"}),
                number("width", QT_TRANSLATE_NOOP("VideoEffects", "Width"), .5, 25, 5, .5),
                number("strength", QT_TRANSLATE_NOOP("VideoEffects", "Strength"), 0, 300, 100, 1)})),
        effect("luma-key", QT_TRANSLATE_NOOP("VideoEffects", "Luma Key"), "keying", "avfilter.lumakey",
               {number("av.threshold", QT_TRANSLATE_NOOP("VideoEffects", "Luma"), 0, 1, 0), number("av.tolerance", QT_TRANSLATE_NOOP("VideoEffects", "Tolerance"), 0, 1, .1),
                number("av.softness", QT_TRANSLATE_NOOP("VideoEffects", "Softness"), 0, 1, .05)}, {{"av.threads", 2}}),
        effect("spill-suppress", QT_TRANSLATE_NOOP("VideoEffects", "Spill Suppress"), "keying", "frei0r.spillsupress",
               {choice("0", QT_TRANSLATE_NOOP("VideoEffects", "Spill colour"), {tr(QT_TRANSLATE_NOOP("VideoEffects", "Green")), tr(QT_TRANSLATE_NOOP("VideoEffects", "Blue"))}, {0.0, 1.0})}),
        // c0rners maps the central third of its 0..1 range onto the image.
        // Present frame coordinates to the editor instead of that plugin range.
        effect("corner-pin", QT_TRANSLATE_NOOP("VideoEffects", "Corner Pin"), "distort", "frei0r.c0rners",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Top left X"), -1, 2, 0, .01, 1./3, 1./3), number("1", QT_TRANSLATE_NOOP("VideoEffects", "Top left Y"), -1, 2, 0, .01, 1./3, 1./3),
                number("2", QT_TRANSLATE_NOOP("VideoEffects", "Top right X"), -1, 2, 1, .01, 1./3, 1./3), number("3", QT_TRANSLATE_NOOP("VideoEffects", "Top right Y"), -1, 2, 0, .01, 1./3, 1./3),
                number("4", QT_TRANSLATE_NOOP("VideoEffects", "Bottom right X"), -1, 2, 1, .01, 1./3, 1./3), number("5", QT_TRANSLATE_NOOP("VideoEffects", "Bottom right Y"), -1, 2, 1, .01, 1./3, 1./3),
                number("6", QT_TRANSLATE_NOOP("VideoEffects", "Bottom left X"), -1, 2, 0, .01, 1./3, 1./3), number("7", QT_TRANSLATE_NOOP("VideoEffects", "Bottom left Y"), -1, 2, 1, .01, 1./3, 1./3)},
               {{"12", 1}, {"13", 0}}),
        effect("fisheye", QT_TRANSLATE_NOOP("VideoEffects", "Fisheye / Defish"), "distort", "frei0r.defish0r",
               {number("0", QT_TRANSLATE_NOOP("VideoEffects", "Amount"), 0, 1, .3), boolean("1", QT_TRANSLATE_NOOP("VideoEffects", "Correct fisheye")), boolean("8", QT_TRANSLATE_NOOP("VideoEffects", "Crop"))}, {{"9", 0}})
    };
    return result;
}
void property(edit::Filter &filter, const QString &key, const QVariant &value) {
    const auto name = key.toUtf8();
    if (value.metaType().id() == QMetaType::QString)
        filter.set(name.constData(), value.toString().toUtf8().constData());
    else filter.set(name.constData(), value.toDouble());
}

}

QVariantList VideoEffects::catalogue() {
    auto result = definitions();
    for (auto &v : result) {
        auto d = v.toMap();
        d["available"] = Timeline::serviceAvailable("filter", d.value("service").toString());
        d["maskable"] = takesMask(d.value("id").toString());
        v = d;
    }
    return result;
}
QVariantMap VideoEffects::definition(const QString &id) {
    for (const auto &v : definitions()) if (v.toMap().value("id") == id) return v.toMap();
    return {};
}
QVariantMap VideoEffects::defaults(const QString &id) {
    QVariantMap values;
    for (const auto &p : definition(id).value("params").toList()) {
        const auto d = p.toMap(); values[d.value("key").toString()] = d.value("default");
    }
    return values;
}
QString VideoEffects::validate(const QString &id, const QVariantMap &params) {
    const auto d = definition(id);
    if (d.isEmpty()) return tr(QT_TRANSLATE_NOOP("VideoEffects", "Unknown video effect: %1")).arg(id);
    QMap<QString, QVariantMap> fields;
    for (const auto &p : d.value("params").toList()) fields[p.toMap().value("key").toString()] = p.toMap();
    for (auto it = params.begin(); it != params.end(); ++it) {
        const auto p = fields.value(it.key()); const auto type = p.value("type").toString();
        bool ok = !p.isEmpty();
        if (type == "number") {
            bool numeric = false; const double value = it.value().toDouble(&numeric);
            ok = numeric && it.value().metaType().id() != QMetaType::Bool && std::isfinite(value)
                 && value >= p.value("min").toDouble() && value <= p.value("max").toDouble();
            if (p.value("step").toDouble() == 1) ok = ok && value == std::floor(value);
        } else if (type == "bool") ok = it.value().metaType().id() == QMetaType::Bool;
        else if (type == "color") ok = it.value().metaType().id() == QMetaType::QString && QColor::isValidColorName(it.value().toString());
        else if (type == "choice") ok = p.value("values").toList().contains(it.value());
        if (!ok) return tr(QT_TRANSLATE_NOOP("VideoEffects", "Invalid value for %1 in %2.")).arg(p.value("label", it.key()).toString(), d.value("name").toString());
    }
    return {};
}
QString VideoEffects::validateKeys(const QString &id, const QVariantMap &keys, QVariantMap *tidy) {
    const auto d = definition(id);
    if (tidy) tidy->clear();
    if (keys.isEmpty()) return {};
    if (d.isEmpty()) return tr(QT_TRANSLATE_NOOP("VideoEffects", "Unknown video effect: %1")).arg(id);
    const auto numeric = [](const QVariant &v) {
        const int type = v.metaType().id();
        return type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::LongLong || type == QMetaType::UInt || type == QMetaType::ULongLong;
    };
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QVariantMap field;
        for (const auto &p : d.value("params").toList()) if (p.toMap().value("key") == it.key()) field = p.toMap();
        const auto bad = [&] {
            return tr(QT_TRANSLATE_NOOP("VideoEffects", "Invalid keyframes for %1 in %2.")).arg(field.value("label", it.key()).toString(), d.value("name").toString());
        };
        // A number or a colour moves between its keys; a switch or a choice
        // holds each key's setting until the next.
        const QString type = field.value("type").toString();
        if (field.isEmpty() || it.value().metaType().id() != QMetaType::QVariantList) return bad();
        QMap<int, QVariantMap> unique;
        for (const auto &v : it.value().toList()) {
            const auto k = v.toMap();
            const double frame = k.value("frame").toDouble();
            QString mode = k.value("interpolation", "linear").toString();
            if (!numeric(k.value("frame")) || frame != std::floor(frame) || std::abs(frame) > 100000000
                || (mode != "linear" && mode != "smooth" && mode != "hold")) return bad();
            QVariant value = k.value("value");
            if (type == "number") {
                const double number = value.toDouble();
                if (!numeric(value) || !std::isfinite(number) || number < field.value("min").toDouble() || number > field.value("max").toDouble()) return bad();
                value = number;
            } else if (type == "bool") {
                if (value.metaType().id() != QMetaType::Bool) return bad();
                mode = "hold";
            } else if (type == "choice") {
                if (!field.value("values").toList().contains(value)) return bad();
                mode = "hold";
            } else if (type == "color") {
                if (value.metaType().id() != QMetaType::QString || !QColor::isValidColorName(value.toString())) return bad();
            } else return bad();
            unique[int(frame)] = {{"frame", int(frame)}, {"value", value}, {"interpolation", mode}};
        }
        if (unique.size() > 4096) return bad();
        QVariantList list;
        for (const auto &k : unique) list << k;
        if (tidy && !list.isEmpty()) tidy->insert(it.key(), list);
    }
    return {};
}
bool VideoEffects::takesMask(const QString &id) {
    static const QStringList edges{"chroma-key", "colour-key", "luma-key", "remove-background", "light-wrap"};
    return !definition(id).isEmpty() && !edges.contains(id);
}
QString VideoEffects::validateMask(const QString &id, const QVariantMap &mask, QVariantMap *tidy) {
    if (tidy) tidy->clear();
    if (mask.isEmpty()) return {};
    const auto d = definition(id);
    const auto bad = [&] { return tr(QT_TRANSLATE_NOOP("VideoEffects", "Invalid mask for %1.")).arg(d.value("name", id).toString()); };
    if (!takesMask(id)) return tr(QT_TRANSLATE_NOOP("VideoEffects", "%1 works on the clip's edges and cannot be masked.")).arg(d.value("name", id).toString());
    const QString shape = mask.value("shape").toString();
    if (shape != "rectangle" && shape != "ellipse" && shape != "polygon") return bad();
    static const QStringList known{"shape", "x", "y", "w", "h", "rotation", "points", "feather", "invert"};
    for (auto it = mask.begin(); it != mask.end(); ++it) if (!known.contains(it.key())) return bad();
    const auto number = [&](const char *key, double fallback, double low, double high, double *out) {
        const QVariant v = mask.value(key, fallback);
        const int type = v.metaType().id();
        const bool numeric = type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::LongLong || type == QMetaType::UInt || type == QMetaType::ULongLong;
        *out = v.toDouble();
        return numeric && std::isfinite(*out) && *out >= low && *out <= high;
    };
    double x, y, w, h, rotation, feather;
    if (!number("x", .5, -1, 2, &x) || !number("y", .5, -1, 2, &y) || !number("w", .5, .001, 4, &w) || !number("h", .5, .001, 4, &h)
        || !number("rotation", 0, -360, 360, &rotation) || !number("feather", 0, 0, .5, &feather)) return bad();
    if (mask.contains("invert") && mask.value("invert").metaType().id() != QMetaType::Bool) return bad();
    QVariantList points;
    if (shape == "polygon") {
        const QVariantList given = mask.value("points").toList();
        if (given.size() < 3 || given.size() > 8) return bad();
        for (const QVariant &v : given) {
            const QVariantList p = v.toList();
            if (p.size() != 2) return bad();
            const double px = p[0].toDouble(), py = p[1].toDouble();
            if (!std::isfinite(px) || !std::isfinite(py) || px < -1 || px > 2 || py < -1 || py > 2) return bad();
            points << QVariant(QVariantList{px, py});
        }
    }
    if (tidy) {
        *tidy = {{"shape", shape}, {"x", x}, {"y", y}, {"w", w}, {"h", h}, {"rotation", rotation}, {"feather", feather}, {"invert", mask.value("invert").toBool()}};
        if (shape == "polygon") tidy->insert("points", points);
    }
    return {};
}
QString VideoEffects::validateMaskKeys(const QString &id, const QVariantMap &mask, const QVariantMap &keys, QVariantMap *tidy) {
    if (tidy) tidy->clear();
    if (keys.isEmpty()) return {};
    const auto bad = [&] { return tr(QT_TRANSLATE_NOOP("VideoEffects", "Invalid mask keyframes for %1.")).arg(definition(id).value("name", id).toString()); };
    const QString shape = mask.value("shape").toString();
    if (mask.isEmpty() || !takesMask(id)) return bad();
    struct Range { const char *field; double low, high; bool polygon; };
    static const Range ranges[] = {{"x", -1, 2, false}, {"y", -1, 2, false}, {"w", .001, 4, false}, {"h", .001, 4, false},
                                   {"rotation", -360, 360, false}, {"feather", 0, .5, true}};
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        if (it.value().metaType().id() != QMetaType::QVariantList) return bad();
        // A polygon's corners move together: each key holds them all, as many as the mask has.
        const bool corners = it.key() == QLatin1String("points");
        const qsizetype count = mask.value("points").toList().size();
        if (corners && shape != "polygon") return bad();
        const Range *range = nullptr;
        for (const Range &r : ranges) if (it.key() == QLatin1String(r.field)) range = &r;
        if (!corners && (!range || (shape == "polygon" && !range->polygon))) return bad();
        QMap<int, QVariantMap> unique;
        for (const auto &v : it.value().toList()) {
            const auto k = v.toMap();
            const double frame = k.value("frame").toDouble();
            const QString mode = k.value("interpolation", "linear").toString();
            if (!k.contains("frame") || !k.contains("value") || frame != std::floor(frame) || std::abs(frame) > 100000000
                || (mode != "linear" && mode != "smooth" && mode != "hold")) return bad();
            QVariant value;
            if (corners) {
                const QVariantList given = k.value("value").toList();
                if (given.size() != count) return bad();
                QVariantList points;
                for (const QVariant &g : given) {
                    const QVariantList p = g.toList();
                    if (p.size() != 2) return bad();
                    const double px = p[0].toDouble(), py = p[1].toDouble();
                    if (!std::isfinite(px) || !std::isfinite(py) || px < -1 || px > 2 || py < -1 || py > 2) return bad();
                    points << QVariant(QVariantList{px, py});
                }
                value = points;
            } else {
                const double number = k.value("value").toDouble();
                if (!std::isfinite(number) || number < range->low || number > range->high) return bad();
                value = number;
            }
            unique[int(frame)] = {{"frame", int(frame)}, {"value", value}, {"interpolation", mode}};
        }
        if (unique.size() > 4096) return bad();
        QVariantList list;
        for (const auto &k : unique) list << k;
        if (tidy && !list.isEmpty()) tidy->insert(it.key(), list);
    }
    return {};
}
QVariantList VideoEffects::read(edit::Producer *cut) {
    QVariantList stack;
    if (!cut) return stack;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->get("omavid:effectState")) continue;
        auto state = QJsonDocument::fromJson(f->get("omavid:effectState")).toVariant().toMap();
        if (state.isEmpty()) continue;
        state["bypass"] = f->get_int("disable") != 0;
        if (f->get("omavid:effectProblem")) state["problem"] = QString::fromUtf8(f->get("omavid:effectProblem"));
        stack.append(state);
    }
    return stack;
}
bool VideoEffects::apply(edit::Profile &profile, edit::Producer *cut, const QVariantList &stack,
                         bool restoring, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &why) { if (error) *error = why; return false; };
    if (!cut || stack.size() > 64) return fail(tr(QT_TRANSLATE_NOOP("VideoEffects", "A clip can hold up to 64 video effects.")));
    std::vector<std::unique_ptr<edit::Filter>> prepared;
    const auto existing = restoring ? QVariantList{} : read(cut);
    QSet<QString> ids;
    for (const auto &v : stack) {
        auto state = v.toMap(); const QString id = state.value("id").toString(), uid = state.value("uid").toString();
        state.remove("problem"); // transient constructor failures are retried on restore
        if (uid.isEmpty() || ids.contains(uid)) return fail(tr(QT_TRANSLATE_NOOP("VideoEffects", "Each video effect needs a unique ID.")));
        ids.insert(uid);
        const auto d = definition(id); const auto supplied = state.value("params").toMap();
        // Keep an unchanged unavailable effect while editing its neighbours or
        // bypassing it. New effects and changed parameters still validate; an
        // active missing effect remains an explicit export preflight problem.
        const bool retained = std::any_of(existing.cbegin(), existing.cend(), [&](const QVariant &v) {
            const auto old = v.toMap();
            return old.value("uid") == uid && old.value("id") == id && old.value("params").toMap() == supplied
                && old.value("keys").toMap() == state.value("keys").toMap() && old.value("mask").toMap() == state.value("mask").toMap()
                && old.value("maskKeys").toMap() == state.value("maskKeys").toMap();
        });
        const QString issue = validate(id, supplied);
        if (!restoring && !retained && !issue.isEmpty()) return fail(issue);
        QVariantMap keys;
        const QString keyIssue = validateKeys(id, state.value("keys").toMap(), &keys);
        if (!restoring && !retained && !keyIssue.isEmpty()) return fail(keyIssue);
        if (keyIssue.isEmpty()) { if (keys.isEmpty()) state.remove("keys"); else state["keys"] = keys; }
        QVariantMap mask;
        const QString maskIssue = validateMask(id, state.value("mask").toMap(), &mask);
        if (!restoring && !retained && !maskIssue.isEmpty()) return fail(maskIssue);
        if (maskIssue.isEmpty()) { if (mask.isEmpty()) state.remove("mask"); else state["mask"] = mask; }
        QVariantMap maskKeys;
        const QString maskKeyIssue = validateMaskKeys(id, state.value("mask").toMap(), state.value("maskKeys").toMap(), &maskKeys);
        if (!restoring && !retained && !maskKeyIssue.isEmpty()) return fail(maskKeyIssue);
        if (maskKeyIssue.isEmpty()) { if (maskKeys.isEmpty()) state.remove("maskKeys"); else state["maskKeys"] = maskKeys; }
        auto params = defaults(id);
        for (auto it = supplied.begin(); it != supplied.end(); ++it) params[it.key()] = it.value();
        state["params"] = params;
        const auto service = d.value("service").toString();
        std::unique_ptr<edit::Filter> f;
        bool constructorFailed = false;
        if (issue.isEmpty() && Timeline::serviceAvailable("filter", service)) {
            f = std::make_unique<edit::Filter>(profile, service.toUtf8().constData());
            if (!f->is_valid()) { f.reset(); constructorFailed = true; }
        }
        if (!f) {
            if (!restoring && !retained && !state.value("bypass").toBool()) return fail(tr(QT_TRANSLATE_NOOP("VideoEffects", "%1 is unavailable. Enable or install its video plug-in in Preferences.")).arg(d.value("name", id).toString()));
            f = std::make_unique<edit::Filter>(profile, "brightness"); f->set("level", 1.0);
            if (constructorFailed)
                f->set("omavid:effectProblem", tr(QT_TRANSLATE_NOOP("VideoEffects", "Could not create %1. Bypass it or repair its video plug-in before export.")).arg(d.value("name", id).toString()).toUtf8().constData());
        } else {
            const auto fixed = d.value("fixed").toMap();
            for (auto it = fixed.begin(); it != fixed.end(); ++it) property(*f, it.key(), it.value());
            for (const auto &p : d.value("params").toList()) {
                const auto field = p.toMap(); const auto key = field.value("key").toString();
                auto value = params.value(key);
                if (field.value("type") == "number") value = value.toDouble() * field.value("scale", 1).toDouble() + field.value("offset", 0).toDouble();
                if (field.value("type") == "color") {
                    const QColor c(value.toString());
                    value = service.startsWith("avfilter.") ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
                }
                property(*f, key, value);
            }
        }
        if (!f->is_valid()) return fail(tr(QT_TRANSLATE_NOOP("VideoEffects", "Could not create the video effect.")));
        f->set("omavid:effect", id.toUtf8().constData());
        f->set("omavid:effectUid", uid.toUtf8().constData());
        f->set("omavid:effectState", QJsonDocument::fromVariant(state).toJson(QJsonDocument::Compact).constData());
        f->set("disable", state.value("bypass").toBool() ? 1 : 0);
        prepared.push_back(std::move(f));
        if (id == "chroma-key" || id == "colour-key" || id == "remove-background" || id == "luma-key") {
            auto guard = std::make_unique<edit::Filter>(profile, "omaedit.rgba");
            if (!guard->is_valid()) return fail(tr(QT_TRANSLATE_NOOP("VideoEffects", "Could not create the video effect.")));
            // Paired with the real filter for ordering, bypass, adjustment
            // copies and XML. Only the real filter carries effectState.
            guard->set("omavid:effect", id.toUtf8().constData());
            guard->set("omavid:effectUid", uid.toUtf8().constData());
            guard->set("disable", state.value("bypass").toBool() ? 1 : 0);
            prepared.push_back(std::move(guard));
        }
    }
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i)); if (f && f->get("omavid:effect")) cut->detach(*f);
    }
    for (auto &f : prepared) cut->attach(*f);
    return true;
}
QStringList VideoEffects::problems(const QVariantList &stack) {
    QStringList result;
    for (const auto &v : stack) {
        const auto state = v.toMap(); if (state.value("bypass").toBool()) continue;
        if (!state.value("problem").toString().isEmpty()) { result << state.value("problem").toString(); continue; }
        const auto id = state.value("id").toString(), d = definition(id).value("service").toString();
        const auto issue = validate(id, state.value("params").toMap());
        if (!issue.isEmpty()) result << issue;
        else if (!Timeline::serviceAvailable("filter", d)) result << tr(QT_TRANSLATE_NOOP("VideoEffects", "Video effect unavailable: %1")).arg(definition(id).value("name", id).toString());
    }
    return result;
}

QStringList VideoEffects::eightBit(const QVariantList &) {
    // The engine draws every effect in float: none narrows a picture.
    return {};
}
