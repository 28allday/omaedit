#include "editengine.h"
#include "graph.h"
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

QStringList EditEngine::parameters(const QString &group) {
    if (group == "rect")
        return {"rectX", "rectY", "rectW", "rectH", "rotation", "anchorX", "anchorY"};
    if (group == "crop")
        return {"cropL", "cropR", "cropT", "cropB", "cropFeather"};
    if (group == "color")
        return {"colorB", "colorC", "colorS"};
    return {};
}

const QStringList &EditEngine::titleLookKeys() {
    static const QStringList keys{"shadow", "shadowColor", "shadowOffset", "shadowSoftness",
                                  "outlineColor", "outlineWidth", "boxPadding", "boxRadius"};
    return keys;
}

QVariantMap EditEngine::titleStyleOfEntry(const QVariantMap &e) {
    QVariantMap style;
    for (const auto &key : QStringList{"Font", "Color", "Size", "Place", "Align", "Weight", "Italic", "Bg", "Outline",
                                       "PixelSize", "Tracking", "LineSpacing", "Margin", "Width"}) {
        QString name = key;
        name[0] = name[0].toLower();
        style[name] = e.value("title" + key);
    }
    const QVariantMap look = e.value("titleLook").toMap();
    for (const QString &key : titleLookKeys())
        if (look.contains(key))
            style[key] = look.value(key);
    return style;
}

double EditEngine::valueAt(const QVariantList &keys, int frame, double fallback, const char *valueName) {
    if (keys.isEmpty())
        return fallback;
    QVariantMap a = keys.first().toMap();
    if (frame <= a.value("frame").toInt())
        return a.value(valueName).toDouble();
    for (int i = 1; i < keys.size(); ++i) {
        const auto b = keys[i].toMap();
        if (frame < b.value("frame").toInt()) {
            if (a.value("interpolation").toString() == "hold")
                return a.value(valueName).toDouble();
            double u = double(frame - a.value("frame").toInt()) /
                       qMax(1, b.value("frame").toInt() - a.value("frame").toInt());
            if (a.value("interpolation").toString() == "smooth")
                u = u * u * (3 - 2 * u);
            return a.value(valueName).toDouble() +
                   u * (b.value(valueName).toDouble() - a.value(valueName).toDouble());
        }
        a = b;
    }
    return a.value(valueName).toDouble();
}

bool EditEngine::normalizeOpacityKeys(const QVariantList &keys, QVariantList *result) {
    auto numeric = [](const QVariant &v) {
        const int type = v.metaType().id();
        return type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::LongLong || type == QMetaType::UInt || type == QMetaType::ULongLong;
    };
    QMap<int, QVariantMap> unique;
    for (const auto &v : keys) {
        const auto k = v.toMap();
        const double frame = k.value("frame").toDouble(), alpha = k.value("alpha").toDouble();
        const auto mode = k.value("interpolation", "linear").toString();
        if (!numeric(k.value("frame")) || !std::isfinite(frame) || std::abs(frame) > 100000000
            || frame != std::round(frame) || !numeric(k.value("alpha")) || !std::isfinite(alpha)
            || alpha < 0 || alpha > 1 || (mode != "linear" && mode != "hold" && mode != "smooth")) return false;
        for (auto it = k.begin(); it != k.end(); ++it)
            if (it.key() != "frame" && it.key() != "alpha" && it.key() != "interpolation") return false;
        QVariantMap point{{"frame", int(frame)}, {"alpha", alpha}};
        if (k.contains("interpolation")) point["interpolation"] = mode;
        unique[int(frame)] = point;
    }
    result->clear();
    for (const auto &k : unique) result->append(k);
    return true;
}

QVariantList EditEngine::shiftedCurve(const QVariantList &keys, int offset) {
    QVariantList result;
    for (const auto &v : keys) {
        auto k = v.toMap();
        k["frame"] = k.value("frame").toInt() - offset;
        result << k;
    }
    return result;
}

QVariantList EditEngine::scaledCurve(const QVariantList &keys, double ratio) {
    QMap<int, QVariantMap> unique;
    for (const auto &v : keys) {
        auto k = v.toMap();
        const int frame = qRound(k.value("frame").toInt() * ratio);
        k["frame"] = frame;
        unique[frame] = k;
    }
    QVariantList result;
    for (const auto &k : unique) result << k;
    return result;
}

QVariantMap EditEngine::motionPreset(const QVariantMap &entry, const QString &preset,
                                    const QVariantMap &options, double fps, QString *error) {
    if (error) error->clear();
    auto fail = [&](const char *message) {
        if (error) *error = QCoreApplication::translate("EditEngine", message);
        return QVariantMap{};
    };
    const QStringList moves = {"zoom-in", "zoom-out", "pan-left", "pan-right", "pan-up", "pan-down"};
    const QStringList titles = {"title-fade", "title-slide-left", "title-slide-right", "title-rise", "title-scale"};
    const bool title = titles.contains(preset);
    if (!title && !moves.contains(preset)) return fail(QT_TRANSLATE_NOOP("EditEngine", "Unknown motion preset."));
    if (title && entry.value("title").toString().isEmpty())
        return fail(QT_TRANSLATE_NOOP("EditEngine", "Select a title clip for title motion."));
    const int frames = entry.value("frames").toInt(), last = frames - 1;
    if (frames < (title ? 3 : 2) || !std::isfinite(fps) || fps <= 0)
        return fail(QT_TRANSLATE_NOOP("EditEngine", "The clip is too short for this motion preset."));
    const QStringList allowed = title ? QStringList{"duration"} : QStringList{"amount", "interpolation"};
    for (auto it = options.begin(); it != options.end(); ++it)
        if (!allowed.contains(it.key())) return fail(QT_TRANSLATE_NOOP("EditEngine", "Unknown option for this motion preset."));
    const auto number = [&](const char *key, double fallback, double low, double high) {
        const auto value = options.value(key, fallback);
        const int type = value.metaType().id();
        const bool numeric = type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::LongLong || type == QMetaType::UInt || type == QMetaType::ULongLong;
        const double v = value.toDouble();
        return numeric && std::isfinite(v) && v >= low && v <= high ? v : -1.;
    };
    const double amount = number("amount", 15, 1, 100) / 100.;
    const double seconds = number("duration", .5, .04, 10);
    const auto mode = options.value("interpolation", "smooth").toString();
    if (amount < 0 || seconds < 0 || (mode != "linear" && mode != "smooth"))
        return fail(QT_TRANSLATE_NOOP("EditEngine", "Use an amount from 1–100%, a duration from 0.04–10 seconds, and Linear or Smooth motion."));

    // Presets replace position/size animation around the saved framing. Static
    // framing remains intact, so changing a preset does not compound its zoom.
    const double x = entry.value("rectX", 0.).toDouble(), y = entry.value("rectY", 0.).toDouble();
    const double w = entry.value("rectW", 1.).toDouble(), h = entry.value("rectH", 1.).toDouble();
    QVariantMap keys = entry.value("editKeys").toMap(), patch;
    auto curve = [&](const QString &key, const QList<QPair<int, double>> &points, const QString &interpolation) {
        QMap<int, QVariantMap> unique;
        for (const auto &point : points)
            unique[point.first] = {{"frame", point.first}, {"value", point.second}, {"interpolation", interpolation}};
        QVariantList list;
        for (const auto &value : unique) list << value;
        keys[key] = list;
    };
    if (!title) {
        double ax=x, ay=y, aw=w, ah=h, bx, by, bw, bh;
        if (preset.startsWith("zoom")) {
            bw = w * (1 + amount); bh = h * (1 + amount);
            bx = x - (bw - w) / 2; by = y - (bh - h) / 2;
            if (preset == "zoom-out") { std::swap(ax,bx); std::swap(ay,by); std::swap(aw,bw); std::swap(ah,bh); }
        } else {
            aw = bw = w * (1 + amount); ah = bh = h * (1 + amount);
            ax = bx = x - (aw - w) / 2; ay = by = y - (ah - h) / 2;
            if (preset == "pan-left" || preset == "pan-right") {
                ax = x; bx = x - w * amount;
                if (preset == "pan-right") std::swap(ax,bx);
            } else {
                ay = y; by = y - h * amount;
                if (preset == "pan-down") std::swap(ay,by);
            }
        }
        if (qMax(aw,bw) > 20 || qMax(ah,bh) > 20)
            return fail(QT_TRANSLATE_NOOP("EditEngine", "This motion would exceed the size limit. Reduce its amount or the clip's size."));
        curve("rectX", {{0,ax},{last,bx}}, mode); curve("rectY", {{0,ay},{last,by}}, mode);
        curve("rectW", {{0,aw},{last,bw}}, mode); curve("rectH", {{0,ah},{last,bh}}, mode);
    } else {
        const int duration = qBound(1, qRound(seconds * fps), last / 2);
        const int exit = last - duration;
        double ax=x, ay=y, aw=w, ah=h, bx=x, by=y, bw=w, bh=h;
        if (preset == "title-slide-left") { ax = x + 1; bx = x - 1; }
        else if (preset == "title-slide-right") { ax = x - 1; bx = x + 1; }
        else if (preset == "title-rise") { ay = y + 1; by = y - 1; }
        else if (preset == "title-scale") {
            aw=w*.85; ah=h*.85; bw=w*1.15; bh=h*1.15;
            ax=x+(w-aw)/2; ay=y+(h-ah)/2; bx=x+(w-bw)/2; by=y+(h-bh)/2;
        }
        if (qMax(aw,bw) > 20 || qMax(ah,bh) > 20)
            return fail(QT_TRANSLATE_NOOP("EditEngine", "This motion would exceed the size limit. Reduce its amount or the clip's size."));
        curve("rectX", {{0,ax},{duration,x},{exit,x},{last,bx}}, "smooth");
        curve("rectY", {{0,ay},{duration,y},{exit,y},{last,by}}, "smooth");
        curve("rectW", {{0,aw},{duration,w},{exit,w},{last,bw}}, "smooth");
        curve("rectH", {{0,ah},{duration,h},{exit,h},{last,bh}}, "smooth");
        const double alpha = entry.value("opacity", 1.).toDouble();
        QMap<int, double> opacity{{0,0},{duration,alpha},{exit,alpha},{last,0}};
        QVariantList list;
        for (auto it = opacity.begin(); it != opacity.end(); ++it)
            list << QVariantMap{{"frame", it.key()}, {"alpha", it.value()}, {"interpolation", "smooth"}};
        patch["opacityKf"] = list;
    }
    patch["editKeys"] = keys;
    return patch;
}

QVariantMap EditEngine::shiftedKeys(const QVariantMap &keys, int offset, int frames) {
    // Retain out-of-range control points: clipping them changes smooth curves
    // and loses the handles needed when a trimmed clip is extended again.
    Q_UNUSED(frames);
    QVariantMap result;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QVariantList list;
        for (const auto &v : it.value().toList()) {
            auto k = v.toMap();
            k["frame"] = k.value("frame").toInt() - offset;
            list << k;
        }
        result[it.key()] = list;
    }
    return result;
}
QVariantList EditEngine::shiftedEffects(const QVariantList &stack, int offset) {
    QVariantList result = stack;
    for (QVariant &v : result) {
        auto state = v.toMap();
        if (state.value("keys").toMap().isEmpty() && state.value("maskKeys").toMap().isEmpty()) continue;
        for (const char *curves : {"keys", "maskKeys"})
            if (!state.value(QLatin1String(curves)).toMap().isEmpty())
                state[QLatin1String(curves)] = shiftedKeys(state.value(QLatin1String(curves)).toMap(), offset, 0);
        v = state;
    }
    return result;
}
QVariantList EditEngine::scaledEffects(const QVariantList &stack, double ratio) {
    QVariantList result = stack;
    for (QVariant &v : result) {
        auto state = v.toMap();
        if (state.value("keys").toMap().isEmpty() && state.value("maskKeys").toMap().isEmpty()) continue;
        for (const char *curves : {"keys", "maskKeys"})
            if (!state.value(QLatin1String(curves)).toMap().isEmpty())
                state[QLatin1String(curves)] = scaledKeys(state.value(QLatin1String(curves)).toMap(), ratio, 0);
        v = state;
    }
    return result;
}
QVariantMap EditEngine::scaledKeys(const QVariantMap &keys, double ratio, int frames) {
    Q_UNUSED(frames);
    QVariantMap result;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QMap<int, QVariantMap> unique;
        for (const auto &v : it.value().toList()) {
            auto k = v.toMap();
            int f = qRound(k.value("frame").toInt() * ratio);
            k["frame"] = f;
            unique[f] = k;
        }
        QVariantList list;
        for (const auto &k : unique)
            list << k;
        result[it.key()] = list;
    }
    return result;
}

QImage EditEngine::render(const QImage &source, const QVariantMap &config, int frame) {
    const auto base = config.value("base").toMap(), keys = config.value("keys").toMap();
    auto v = [&](const char *key, double fallback = 0.) {
        return valueAt(keys.value(QLatin1String(key)).toList(), frame,
                       base.value(QLatin1String(key), fallback).toDouble());
    };
    const auto group = config.value("group").toString();
    const bool wide = source.depth() == 64;
    const bool linear = source.format() == QImage::Format_RGBA32FPx4;
    QImage result = source.convertToFormat(linear ? QImage::Format_RGBA32FPx4 : wide ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    const int w = result.width(), h = result.height();
    if (group == "rect") {
        result.fill(Qt::transparent);
        const double rw = qMax(.001, v("rectW", 1)), rh = qMax(.001, v("rectH", 1));
        const double ax = v("anchorX", .5), ay = v("anchorY", .5);
        QPainter p(&result);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate((v("rectX") + rw * ax) * w, (v("rectY") + rh * ay) * h);
        p.rotate(v("rotation"));
        p.scale(rw, rh);
        p.translate(-ax * w, -ay * h);
        p.drawImage(0, 0, source);
    } else if (group == "color" || group == "crop") {
        const double b = v("colorB"), c = v("colorC", 1), s = v("colorS", 1);
        double l = qBound(0., v("cropL"), .95), r = qBound(0., v("cropR"), .95),
               t = qBound(0., v("cropT"), .95), bt = qBound(0., v("cropB"), .95);
        if (l + r > .95) {
            const double k = .95 / (l + r);
            l *= k;
            r *= k;
        }
        if (t + bt > .95) {
            const double k = .95 / (t + bt);
            t *= k;
            bt *= k;
        }
        const double feather = qMax(0., v("cropFeather")) * qMin(w, h);
        const bool invert = base.value("cropInvert").toBool();
        for (int y = 0; y < h; ++y) {
            // Detach once per row, then use the same row for reads and writes.
            // Keep the colour arithmetic even for neutral controls: cancelling
            // terms can still affect small floating-point RGB values.
            auto *row = result.scanLine(y);
            for (int x = 0; x < w; ++x) {
                float rgba[4];
                if (linear) {
                    std::copy_n(reinterpret_cast<const float *>(row)+x*4,4,rgba);
                } else if (wide) {
                    const auto a = reinterpret_cast<const QRgba64 *>(row)[x];
                    rgba[0] = a.red() / 65535.f;
                    rgba[1] = a.green() / 65535.f;
                    rgba[2] = a.blue() / 65535.f;
                    rgba[3] = a.alpha() / 65535.f;
                } else {
                    const auto a = row + x * 4;
                    for (int j = 0; j < 4; ++j)
                        rgba[j] = a[j] / 255.f;
                }
                if (group == "color") {
                    const double lum = .2126 * rgba[0] + .7152 * rgba[1] + .0722 * rgba[2];
                    for (int j = 0; j < 3; ++j) {
                        const double value=((lum + s * (rgba[j] - lum)) - .5) * c + .5 + b;
                        rgba[j] = linear ? value : qBound(0.,value,1.);
                    }
                } else {
                    const double distance = qMin(qMin(x + .5 - l * w, (1 - r) * w - x - .5),
                                                 qMin(y + .5 - t * h, (1 - bt) * h - y - .5));
                    double alpha = feather > 0 ? qBound(0., distance / feather, 1.) : double(distance >= 0);
                    if (invert)
                        alpha = 1 - alpha;
                    rgba[3] *= alpha;
                }
                if (linear)
                    std::copy_n(rgba,4,reinterpret_cast<float *>(row)+x*4);
                else if (wide)
                    reinterpret_cast<QRgba64 *>(row)[x] =
                        QRgba64::fromRgba64(qRound(rgba[0] * 65535), qRound(rgba[1] * 65535),
                                            qRound(rgba[2] * 65535), qRound(rgba[3] * 65535));
                else {
                    auto a = row + x * 4;
                    for (int j = 0; j < 4; ++j)
                        a[j] = qRound(rgba[j] * 255);
                }
            }
        }
    } else if (group == "title") {
        const auto style = config.value("style").toMap();
        const double scale = double(h) / qMax(1, config.value("height", h).toInt());
        const auto size = style.value("size").toString();
        const int divisor = size == "small" ? 12 : size == "large" ? 5 : 8;
        // No family named: the desktop's own font. QFont("") would ask for an
        // empty family, which resolves to a different one (a monospace face here).
        const QString family = style.value("font").toString();
        QFont font = family.isEmpty() ? QFont() : QFont(family);
        const double pixels = style.value("pixelSize").toDouble();
        font.setPixelSize(qMax(1, qRound((pixels > 0 ? pixels : double(h) / scale / divisor) * scale)));
        font.setBold(style.value("weight").toString() == "bold");
        font.setItalic(style.value("italic").toBool());
        font.setLetterSpacing(QFont::AbsoluteSpacing, style.value("tracking").toDouble() * scale);
        const double lineSpacing = qBound(.5, style.value("lineSpacing", 1.).toDouble(), 4.);
        // The width the words wrap in (percent of the frame's), centred.
        const double widthShare = style.contains("width") ? qBound(20., style.value("width").toDouble(), 100.) / 100. : .9;
        const double boxWidth = w * widthShare;
        const double left = (w - boxWidth) / 2;
        // By an edge (a caption): a line wider than its width is first drawn
        // smaller, down to 60 %, so the caption keeps the lines it was given
        // rather than leaving a word on a line of its own.
        const bool edge = style.value("place") == "top" || style.value("place") == "bottom";
        if (edge) {
            const QFontMetricsF metrics(font);
            double widest = 0;
            for (const QString &line : config.value("text").toString().split('\n'))
                if (line.contains(' ')) widest = qMax(widest, metrics.horizontalAdvance(line));
            if (widest > boxWidth)
                font.setPixelSize(qMax(1, int(font.pixelSize() * qMax(.6, boxWidth / widest)))); // rounded down: it must fit
        }
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        const auto align = style.value("align").toString();
        option.setAlignment(align == "left"    ? Qt::AlignLeft
                            : align == "right" ? Qt::AlignRight
                                               : Qt::AlignHCenter);
        QTextLayout layout(config.value("text").toString().replace('\n', QChar::LineSeparator), font);
        layout.setTextOption(option);
        QTextCharFormat format;
        QColor fg(style.value("color", "#ffffff").toString());
        if (!fg.isValid())
            fg = Qt::white;
        format.setForeground(fg);
        // The outline: a dark edge of 2 pixels unless its colour or width
        // are given (titleLook).
        QColor edgeColour(style.value("outlineColor").toString());
        if (!edgeColour.isValid())
            edgeColour = Qt::black;
        const double edgeWidth = style.contains("outlineWidth") && !style.value("outlineWidth").isNull()
                                     ? qBound(0., style.value("outlineWidth").toDouble(), 20.) : 2.;
        const bool outlined = style.value("outline", true).toBool() && edgeWidth > 0;
        // Laid out again whenever the formats change (the outline pass, the
        // shadow's silhouette): the same lines each time.
        const auto layOut = [&](const QTextCharFormat &with) {
            layout.setFormats({QTextLayout::FormatRange{0, int(layout.text().size()), with}});
            layout.beginLayout();
            double y = 0;
            while (true) {
                auto line = layout.createLine();
                if (!line.isValid())
                    break;
                line.setLineWidth(boxWidth);
                line.setPosition(QPointF(0, y));
                y += line.height() * lineSpacing;
            }
            layout.endLayout();
            return y;
        };
        QTextCharFormat edged = format;
        if (outlined)
            edged.setTextOutline(QPen(edgeColour, edgeWidth * scale));
        const double total = layOut(edged);
        // Centred by the letters themselves (the union of the glyphs' outlines),
        // not by the lines' boxes, whose room for descenders and line gap would
        // sit the words low: on the frame's middle, or at 80 % for a lower third.
        QRectF ink;
        for (const QGlyphRun &run : layout.glyphRuns()) {
            const QRawFont raw = run.rawFont();
            const auto glyphs = run.glyphIndexes();
            const auto positions = run.positions();
            for (int i = 0; i < glyphs.size(); ++i)
                ink |= raw.boundingRect(glyphs[i]).translated(positions[i]);
        }
        if (ink.isEmpty())
            ink = QRectF(0, 0, boxWidth, total);
        // By an edge ("top", "bottom"): the letters' outer edge sits the
        // margin (percent of the frame's height) in from it. Otherwise
        // centred on the frame's middle, or at 80 % for a lower third.
        const QString place = style.value("place").toString();
        const double margin = qBound(0., style.value("margin", 8.).toDouble(), 45.) / 100. * h;
        const double centre = place == "lower" ? h * .8 : h / 2.;
        const double top = place == "bottom" ? h - margin - ink.bottom()
                           : place == "top"  ? margin - ink.top()
                                             : centre - ink.height() / 2 - ink.top();
        QPainter p(&result);
        p.setRenderHint(QPainter::TextAntialiasing);
        QColor bg(style.value("bg").toString());
        // The box's room round the words and its rounded corners, in pixels
        // (titleLook); unset, the box titles always had.
        const bool padded = style.contains("boxPadding") && !style.value("boxPadding").isNull();
        const double padding = padded ? qBound(0., style.value("boxPadding").toDouble(), 200.) * scale : 0.;
        const double radius = qBound(0., style.value("boxRadius", 0.).toDouble(), 200.) * scale;
        if (bg.isValid() && bg.alpha() > 0 && edge) {
            // A caption's box sits behind each line's words, edge to edge
            // between the lines.
            const double pad = padded ? padding : font.pixelSize() * .25;
            QList<QRectF> boxes;
            for (int i = 0; i < layout.lineCount(); ++i) {
                const QTextLine line = layout.lineAt(i);
                const QRectF words = line.naturalTextRect();
                if (words.width() <= 0) continue;
                const double y0 = top + line.y() - (i == 0 ? pad * .5 : 0);
                const double y1 = top + (i + 1 < layout.lineCount() ? layout.lineAt(i + 1).y() : line.y() + line.height() + pad * .5);
                boxes.append(QRectF(left + words.x() - pad, y0, words.width() + 2 * pad, y1 - y0));
            }
            if (radius <= 0) {
                for (const QRectF &box : std::as_const(boxes))
                    p.fillRect(box, bg);
            } else {
                // Rounded on the outer corners only: a line wider than the
                // one above (or below) rounds the corners that stand out,
                // and the lines join square. Filled as one shape, so a
                // see-through box is no darker where two lines meet.
                QPainterPath shape;
                for (int i = 0; i < boxes.size(); ++i) {
                    const QRectF b = boxes.at(i);
                    const QRectF *above = i > 0 ? &boxes.at(i - 1) : nullptr;
                    const QRectF *below = i + 1 < boxes.size() ? &boxes.at(i + 1) : nullptr;
                    const double r = qMin(radius, qMin(b.width(), b.height()) / 2);
                    const double tl = !above || b.left() < above->left() - .5 ? r : 0;
                    const double tr = !above || b.right() > above->right() + .5 ? r : 0;
                    const double bl = !below || b.left() < below->left() - .5 ? r : 0;
                    const double br = !below || b.right() > below->right() + .5 ? r : 0;
                    QPainterPath one;
                    one.moveTo(b.left() + tl, b.top());
                    one.lineTo(b.right() - tr, b.top());
                    if (tr > 0) one.arcTo(QRectF(b.right() - 2 * tr, b.top(), 2 * tr, 2 * tr), 90, -90);
                    one.lineTo(b.right(), b.bottom() - br);
                    if (br > 0) one.arcTo(QRectF(b.right() - 2 * br, b.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
                    one.lineTo(b.left() + bl, b.bottom());
                    if (bl > 0) one.arcTo(QRectF(b.left(), b.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
                    one.lineTo(b.left(), b.top() + tl);
                    if (tl > 0) one.arcTo(QRectF(b.left(), b.top(), 2 * tl, 2 * tl), 180, -90);
                    one.closeSubpath();
                    shape = shape.united(one);
                }
                p.save();
                p.setRenderHint(QPainter::Antialiasing);
                p.fillPath(shape, bg);
                p.restore();
            }
        } else if (bg.isValid() && bg.alpha() > 0) {
            const double pad = padded ? padding : 4 * scale;
            const QRectF box(left - pad, top - pad, boxWidth + 2 * pad, total + 2 * pad);
            if (radius <= 0)
                p.fillRect(box, bg);
            else {
                QPainterPath shape;
                const double r = qMin(radius, qMin(box.width(), box.height()) / 2);
                shape.addRoundedRect(box, r, r);
                p.save();
                p.setRenderHint(QPainter::Antialiasing);
                p.fillPath(shape, bg);
                p.restore();
            }
        }
        // A drop shadow (titleLook): the words' silhouette, edge and all, in
        // the shadow's colour, softened and set down and to the right.
        if (style.value("shadow").toBool()) {
            QColor shade(style.value("shadowColor").toString());
            if (!shade.isValid())
                shade = Qt::black;
            const double offset = qBound(-50., style.value("shadowOffset", 3.).toDouble(), 50.) * scale;
            const double softness = qBound(0., style.value("shadowSoftness", 4.).toDouble(), 50.) * scale;
            const int reach = int(std::ceil(softness * 1.5 + edgeWidth * scale + 2));
            QRect area = QRectF(left + qMin(0., offset) - reach, top + qMin(0., offset) + qMin(0., ink.top()) - reach,
                                boxWidth + qAbs(offset) + 2 * reach,
                                qMax(total, ink.bottom()) - qMin(0., ink.top()) + qAbs(offset) + 2 * reach)
                             .toAlignedRect()
                             .intersected(QRect(0, 0, w, h));
            if (!area.isEmpty()) {
                QImage silhouette(area.size(), QImage::Format_ARGB32_Premultiplied);
                silhouette.fill(Qt::transparent);
                QTextCharFormat dark = format;
                dark.setForeground(shade);
                if (outlined)
                    dark.setTextOutline(QPen(shade, edgeWidth * scale));
                layOut(dark);
                {
                    QPainter s(&silhouette);
                    s.setRenderHint(QPainter::TextAntialiasing);
                    layout.draw(&s, QPointF(left + offset - area.x(), top + offset - area.y()));
                }
                // Three box blurs each way: close to a Gaussian of the softness.
                const int radiusPx = int(std::lround(softness / 2));
                if (radiusPx > 0) {
                    const int sw = silhouette.width(), sh = silhouette.height();
                    std::vector<quint32> line(size_t(qMax(sw, sh)));
                    const auto blur = [&](bool across) {
                        const int count = across ? sw : sh, lines = across ? sh : sw;
                        for (int l = 0; l < lines; ++l) {
                            const auto at = [&](int i) -> quint32 & {
                                return across ? reinterpret_cast<quint32 *>(silhouette.scanLine(l))[i]
                                              : reinterpret_cast<quint32 *>(silhouette.scanLine(i))[l];
                            };
                            for (int i = 0; i < count; ++i) line[size_t(i)] = at(i);
                            int sum[4] = {0, 0, 0, 0};
                            const int span = 2 * radiusPx + 1;
                            const auto add = [&](int i, int sign) {
                                if (i < 0 || i >= count) return;
                                const quint32 v = line[size_t(i)];
                                for (int c = 0; c < 4; ++c) sum[c] += sign * int((v >> (8 * c)) & 0xff);
                            };
                            for (int i = -radiusPx; i <= radiusPx; ++i) add(i, 1);
                            for (int i = 0; i < count; ++i) {
                                quint32 v = 0;
                                for (int c = 0; c < 4; ++c) v |= quint32(qBound(0, (sum[c] + span / 2) / span, 255)) << (8 * c);
                                at(i) = v;
                                add(i - radiusPx, -1);
                                add(i + radiusPx + 1, 1);
                            }
                        }
                    };
                    for (int pass = 0; pass < 3; ++pass) { blur(true); blur(false); }
                }
                p.drawImage(area.topLeft(), silhouette);
            }
            layOut(edged);
        }
        layout.draw(&p, QPointF(left, top));
        // Fill outlined glyphs independently of the painter's outline brush.
        if (outlined) {
            layOut(format);
            layout.draw(&p, QPointF(left, top));
        }
    }
    return result;
}

void EditEngine::attach(edit::Profile &profile, edit::Producer &cut, const QVariantMap &entry, int in, int out,
                        bool includeStatic) {
    const auto keys = entry.value("editKeys").toMap();
    for (const QString &group : {QString("rect"), QString("color"), QString("crop")}) {
        QVariantMap subset;
        for (const auto &key : parameters(group))
            if (!keys.value(key).toList().isEmpty())
                subset[key] = keys.value(key);
        bool nonNeutral = group == "crop" && entry.value("cropInvert").toBool();
        for (const auto &key : parameters(group)) {
            const double fallback = (key == "rectW" || key == "rectH" || key == "colorC" || key == "colorS")
                                        ? 1.
                                    : (key == "anchorX" || key == "anchorY") ? .5
                                                                             : 0.;
            nonNeutral = nonNeutral || entry.value(key, fallback).toDouble() != fallback;
        }
        if (subset.isEmpty() && !(includeStatic && group != "color" && nonNeutral))
            continue;
        const QByteArray tag = "omavid:" + group.toUtf8();
        for (int i = cut.filter_count() - 1; i >= 0; --i) {
            std::unique_ptr<edit::Filter> f(cut.filter(i));
            if (f && f->get(tag.constData()))
                cut.detach(*f);
        }
        edit::Filter f(profile, "omaedit.edit");
        f.set_in_and_out(in, out);
        QStringList values;
        for (const auto &key : parameters(group))
            values << QString::number(
                entry
                    .value(key, (key == "rectW" || key == "rectH" || key == "colorC" || key == "colorS") ? 1.
                                : (key == "anchorX" || key == "anchorY")                                 ? .5
                                                                                                         : 0.)
                    .toDouble());
        if (group == "crop")
            values << QString::number(entry.value("cropInvert").toBool() ? 1 : 0);
        f.set(tag.constData(), values.join(' ').toUtf8().constData());
        if (group == "crop" || group == "rect")
            f.set("omavid:cropAlpha", 1);
        QVariantMap base;
        for (const auto &key : parameters(group))
            if (entry.contains(key))
                base[key] = entry[key];
        if (group == "crop")
            base["cropInvert"] = entry.value("cropInvert");
        f.set("config",
              QJsonDocument::fromVariant(QVariantMap{{"group", group}, {"base", base}, {"keys", subset}})
                  .toJson(QJsonDocument::Compact)
                  .constData());
        f.set("omavid:editKeys",
              QJsonDocument::fromVariant(subset).toJson(QJsonDocument::Compact).constData());
        cut.attach(f);
    }
    const auto order = entry.value("effectOrder").toStringList();
    if (!order.isEmpty()) {
        for (int i = cut.filter_count() - 1; i >= 0; --i) {
            std::unique_ptr<edit::Filter> f(cut.filter(i));
            if (f && f->get("omavid:effectOrder"))
                cut.detach(*f);
        }
        edit::Filter f(profile, "omaedit.edit");
        f.set("omavid:effectOrder",
              QJsonDocument::fromVariant(order).toJson(QJsonDocument::Compact).constData());
        f.set("disable", 1);
        cut.attach(f);
    }
}
