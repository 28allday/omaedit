#include "gradeengine.h"
#include "bands.h"
#include "imagebands.h"
#include "colourpipeline.h"
#include "displaygrade.h"
#include "lineargrade.h"
#include "lutfilter.h"
#include "timeline.h"
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPolygonF>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace {
double unit(double x) { return std::clamp(x, 0.0, 1.0); }
double value(const QVariantMap &m, const char *key, double fallback = 0) {
    const double x = m.value(QLatin1String(key), fallback).toDouble();
    return std::isfinite(x) ? x : fallback;
}
double smooth(double lo, double hi, double x) {
    if (hi <= lo)
        return x >= hi ? 1 : 0;
    x = unit((x - lo) / (hi - lo));
    return x * x * (3 - 2 * x);
}
double sample(const QVariantList &keys, double position, const QString &field, double fallback) {
    if (keys.isEmpty())
        return fallback;
    QVariantMap left, right;
    double lf = -1e30, rf = 1e30;
    for (const QVariant &v : keys) {
        const QVariantMap k = v.toMap();
        const double f = value(k, "frame");
        if (f <= position && f >= lf) {
            left = k;
            lf = f;
        }
        if (f >= position && f <= rf) {
            right = k;
            rf = f;
        }
    }
    if (left.isEmpty())
        return right.value(field, fallback).toDouble();
    if (right.isEmpty() || lf == rf || left.value("interpolation") == "hold")
        return left.value(field, fallback).toDouble();
    double t = (position - lf) / (rf - lf);
    if (left.value("interpolation") == "smooth")
        t = t * t * (3 - 2 * t);
    return left.value(field, fallback).toDouble() * (1 - t) + right.value(field, fallback).toDouble() * t;
}
QVariant setPath(QVariant target, const QStringList &path, int index, double number) {
    if (index == path.size())
        return number;
    if (target.typeId() == QMetaType::QVariantList) {
        QVariantList list = target.toList();
        const int i = QStringLiteral("yrgb").indexOf(path[index]);
        if (i < 0 || i >= list.size())
            return target;
        list[i] = setPath(list[i], path, index + 1, number);
        return list;
    }
    QVariantMap map = target.toMap();
    map[path[index]] = setPath(map.value(path[index]), path, index + 1, number);
    return map;
}
bool changedCurve(const QVariantList &points, bool flat) {
    for (const QVariant &v : points) {
        const QVariantList p = v.toList();
        if (p.size() > 1 && std::abs(p[1].toDouble() - (flat ? .5 : p[0].toDouble())) > 1e-6)
            return true;
    }
    return false;
}
bool changedHdr(const QVariantMap &hdr) {
    for (const char *zone : {"black", "dark", "shadow", "light", "highlight", "specular"})
        for (const auto &channel : hdr.value(zone).toList())
            if (channel.toDouble() != 0) return true;
    return false;
}
bool changedWarper(const QVariantList &mesh) {
    for (const auto &point : mesh) {
        const auto p = point.toMap();
        if (value(p, "dh") != 0 || value(p, "ds") != 0) return true;
    }
    return false;
}
using CurveSamples = std::vector<std::pair<double, double>>;
CurveSamples prepareCurve(const QVariantList &points) {
    CurveSamples result;
    result.reserve(points.size());
    for (const auto &v : points) {
        const auto p = v.toList();
        if (p.size() >= 2) result.emplace_back(p[0].toDouble(), p[1].toDouble());
    }
    return result;
}
double curve(const CurveSamples &points, double x, double fallback) {
    if (points.empty())
        return fallback;
    double lx = -1, ly = fallback, rx = 2, ry = fallback;
    for (const auto &p : points) {
        const double px = p.first, py = p.second;
        if (px <= x && px > lx) {
            lx = px;
            ly = py;
        }
        if (px >= x && px < rx) {
            rx = px;
            ry = py;
        }
    }
    if (lx < 0)
        return ry;
    if (rx > 1 || rx == lx)
        return ly;
    return ly + (ry - ly) * (x - lx) / (rx - lx);
}
QVariantMap trackedWindow(QVariantMap window, int frame) {
    const QVariantList track = window.value("track").toList();
    for (const char *key : {"x", "y", "w", "h", "rotation"})
        window[key] = sample(track, frame, QLatin1String(key),
                             value(window, key,
                                   key[0] == 'w' || key[0] == 'h' ? .4
                                   : key[0] == 'r'                ? 0
                                                                  : .5));
    return window;
}
struct MaskWindow {
    enum Shape { Ellipse, Rectangle, Diamond, Gradient, Polygon } shape = Ellipse;
    enum Combine { Intersect, Add, Subtract } combine = Intersect;
    double cx, cy, cosine, sine, xScale, yScale, softness;
    bool invert;
    QPolygonF polygon;
    MaskWindow(const QVariantMap &window, double aspect) {
        const double angle = -value(window, "rotation") * M_PI / 180;
        cosine = std::cos(angle); sine = std::sin(angle);
        cx = value(window, "x", .5); cy = value(window, "y", .5);
        xScale = std::max(.0001, value(window, "w", .4) * .5 * aspect);
        yScale = std::max(.0001, value(window, "h", .4) * .5);
        softness = value(window, "softness", .1); invert = window.value("invert").toBool();
        const QString name = window.value("shape", "circle").toString();
        shape = name == "rect" ? Rectangle : name == "diamond" ? Diamond : name == "gradient" ? Gradient
              : name == "polygon" ? Polygon : Ellipse;
        const QString op = window.value("combine", "intersect").toString();
        combine = op == "add" ? Add : op == "subtract" ? Subtract : Intersect;
        if (shape == Polygon) {
            for (const auto &v : window.value("points").toList()) {
                const auto p = v.toList();
                if (p.size() == 2) polygon << QPointF(p[0].toDouble(), p[1].toDouble());
            }
            if (polygon.size() < 3) polygon << QPointF(0, -1) << QPointF(1, 1) << QPointF(-1, 1);
        }
    }
    double at(double x, double y, double aspect) const {
        const double dx = (x - cx) * aspect, dy = y - cy;
        const double X = (cosine * dx - sine * dy) / xScale;
        const double Y = (sine * dx + cosine * dy) / yScale;
        double result;
        if (shape == Gradient)
            result = smooth(-std::max(.0001, softness), std::max(.0001, softness), X);
        else if (shape == Polygon) {
            double distance = 1e9;
            const QPointF p(X, Y);
            for (int i = 0; i < polygon.size(); ++i) {
                const QPointF a = polygon[i], b = polygon[(i + 1) % polygon.size()], ab = b - a;
                const double t = unit(QPointF::dotProduct(p - a, ab) / std::max(1e-12, QPointF::dotProduct(ab, ab)));
                const QPointF delta = p - (a + ab * t);
                distance = std::min(distance, std::hypot(delta.x(), delta.y()));
            }
            result = polygon.containsPoint(p, Qt::OddEvenFill) ? (softness > 0 ? smooth(0, softness, distance) : 1) : 0;
        } else {
            const double d = shape == Rectangle ? std::max(std::abs(X), std::abs(Y))
                           : shape == Diamond ? std::abs(X) + std::abs(Y) : std::hypot(X, Y);
            result = 1 - smooth(std::max(0.0, 1 - softness), 1, d);
        }
        return invert ? 1 - result : result;
    }
};
QVector<float> blurMask(const QVector<float> &in, int w, int h, int radius) {
    if (radius < 1)
        return in;
    QVector<float> temp(in.size()), out(in.size());
    const float *source = in.constData();
    float *horizontal = temp.data(), *destination = out.data();
    Bands::run((h + 31) / 32, [&](int band) {
      for (int y = band * 32; y < std::min(h, (band + 1) * 32); ++y) {
        double total = 0;
        int count = 0;
        for (int x = -radius; x < w; ++x) {
            if (x + radius < w) { total += source[y * w + x + radius]; ++count; }
            if (x - radius - 1 >= 0) { total -= source[y * w + x - radius - 1]; --count; }
            if (x >= 0) horizontal[y * w + x] = total / std::max(1, count);
        }
      }
      return true;
    });
    // Keep adjacent columns together for cache locality, with the original
    // double-precision accumulation order for each pixel's vertical sum.
    constexpr int columnsPerBand = 128;
    Bands::run((w + columnsPerBand - 1) / columnsPerBand, [&](int band) {
        const int start = band * columnsPerBand, end = std::min(w, start + columnsPerBand);
        double totals[columnsPerBand] = {};
        int count = 0;
        for (int y = -radius; y < h; ++y) {
            if (y + radius < h) {
                for (int x = start; x < end; ++x) totals[x - start] += horizontal[(y + radius) * w + x];
                ++count;
            }
            if (y - radius - 1 >= 0) {
                for (int x = start; x < end; ++x) totals[x - start] -= horizontal[(y - radius - 1) * w + x];
                --count;
            }
            if (y >= 0)
                for (int x = start; x < end; ++x) destination[y * w + x] = totals[x - start] / std::max(1, count);
        }
        return true;
    });
    return out;
}
QVector<float> makeMask(const QImage &image, const QVariantMap &node, int frame) {
    const int w = image.width(), h = image.height();
    // Every node's mask is worked out here in floating point. (Nodes from
    // before the mask models were pinned "legacy" once took an 8-bit
    // selection; they share this one now.)
    QVector<float> out(w * h, 1);
    const QVariantMap q = node.value("qualifier").toMap();
    const QColor picked(q.value("color", "#ff0000").toString());
    const bool qualifierEnabled = q.value("enabled").toBool();
    const bool rgbQualifier = q.value("subspace") == "rgb";
    const bool invertQualifier = q.value("invert").toBool();
    const double bounds[] = {value(q, "deltaH", .2), value(q, "deltaS", .3), value(q, "deltaL", .3)};
    const double softness = std::max(.00001, value(q, "softness", .1));
    const auto pickedRed = picked.redF(), pickedGreen = picked.greenF(), pickedBlue = picked.blueF();
    const auto pickedHue = picked.hslHueF(), pickedSat = picked.hslSaturationF(), pickedLum = picked.lightnessF();
    QVariantList windows;
    windows.append(node.value("window"));
    windows.append(node.value("windows").toList());
    for (QVariant &v : windows)
        v = trackedWindow(v.toMap(), frame);
    const QString external = node.value("externalMatte").toString();
    bool active = qualifierEnabled || !external.isEmpty();
    std::vector<MaskWindow> shapes;
    for (const QVariant &v : windows)
        if (v.toMap().value("enabled").toBool()) {
            shapes.emplace_back(v.toMap(), double(w) / h);
            active = true;
        }
    if (!active)
        return out;
    QImage matte;
    if (!external.isEmpty()) {
        QImageReader reader(external);
        const QSize size = reader.size();
        if (size.isValid() && qint64(size.width()) * size.height() <= 32 * 1024 * 1024)
            matte = reader.read().scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (matte.isNull()) {
            out.fill(0);
            return out;
        }
    }
    auto *maskPixels = out.data();
    const bool alphaMatte = node.value("matteChannel") == "alpha";
    constexpr int rowsPerBand = 32;
    Bands::run((h + rowsPerBand - 1) / rowsPerBand, [&](int band) {
      for (int y = band * rowsPerBand; y < std::min(h, (band + 1) * rowsPerBand); ++y)
        for (int x = 0; x < w; ++x) {
            double key = maskPixels[y * w + x];
            if (qualifierEnabled) {
                const QColor c = image.pixelColor(x, y);
                double d[3];
                if (rgbQualifier) {
                    d[0] = std::abs(c.redF() - pickedRed);
                    d[1] = std::abs(c.greenF() - pickedGreen);
                    d[2] = std::abs(c.blueF() - pickedBlue);
                } else {
                    const QColor sample = c.toHsl();
                    double dh = std::abs(sample.hslHueF() - pickedHue);
                    d[0] = std::min(dh, 1 - dh);
                    d[1] = std::abs(sample.hslSaturationF() - pickedSat);
                    d[2] = std::abs(sample.lightnessF() - pickedLum);
                }
                for (int i = 0; i < 3; ++i)
                    key *= 1 - smooth(bounds[i], bounds[i] + softness, d[i]);
                if (invertQualifier)
                    key = 1 - key;
            }
            double combined = 1;
            bool first = true;
            for (const MaskWindow &win : shapes) {
                const double a = win.at((x + .5) / w, (y + .5) / h, double(w) / h);
                if (first) {
                    combined = a;
                    first = false;
                } else if (win.combine == MaskWindow::Add)
                    combined = std::max(combined, a);
                else if (win.combine == MaskWindow::Subtract)
                    combined *= 1 - a;
                else
                    combined *= a;
            }
            key *= combined;
            if (!matte.isNull()) {
                const QColor c = matte.pixelColor(x, y);
                key *= alphaMatte ? c.alphaF()
                    : image.format()==QImage::Format_RGBA32FPx4 ? (11*c.redF()+16*c.greenF()+5*c.blueF())/32
                    : qGray(c.rgb()) / 255.0;
            }
            maskPixels[y * w + x] = unit(key);
        }
      return true;
    });
    // Cleanup in preview-relative pixels, so the look scales with the frame.
    const int grow = std::clamp(int(std::round(value(q, "grow") * w / 1920.0)), -24, 24);
    if (grow) {
        const int radius = std::abs(grow);
        auto pass = [&](const QVector<float> &in, bool vertical) {
            QVector<float> result(in.size());
            const int length = vertical ? h : w, lines = vertical ? w : h;
            for (int line = 0; line < lines; ++line) {
                std::deque<int> queue;
                int next = 0;
                auto at = [&](int p) { return vertical ? p * w + line : line * w + p; };
                for (int x = 0; x < length; ++x) {
                    for (; next <= std::min(length - 1, x + radius); ++next) {
                        while (!queue.empty() &&
                               (grow > 0 ? in[at(queue.back())] <= in[at(next)] : in[at(queue.back())] >= in[at(next)]))
                            queue.pop_back();
                        queue.push_back(next);
                    }
                    while (!queue.empty() && queue.front() < x - radius)
                        queue.pop_front();
                    result[at(x)] = in[at(queue.front())];
                }
            }
            return result;
        };
        out = pass(pass(out, false), true);
    }
    return blurMask(out, w, h, std::clamp(int(std::round(value(q, "blur") * w / 1920.0)), 0, 64));
}
QImage mixImages(const QImage &base, const QImage &corrected, const QVector<float> &mask, double opacity = 1) {
    if (opacity <= 0 || std::all_of(mask.cbegin(), mask.cend(), [](float a) { return a <= 0; }))
        return base;
    if (opacity == 1 && std::all_of(mask.cbegin(), mask.cend(), [](float a) { return a >= 1; }))
        return corrected;
    const int width = base.width(), height = base.height();
    constexpr int rowsPerBand = 32;
    const float *selection = mask.constData();
    if (base.format() == QImage::Format_RGBA32FPx4 || corrected.format() == QImage::Format_RGBA32FPx4) {
        QImage out = base.convertToFormat(QImage::Format_RGBA32FPx4), effect = corrected.convertToFormat(QImage::Format_RGBA32FPx4);
        auto *pixels = out.bits();
        const auto *effectPixels = effect.constBits();
        const int stride = out.bytesPerLine(), effectStride = effect.bytesPerLine();
        Bands::run((height + rowsPerBand - 1) / rowsPerBand, [&](int band) {
            for (int y = band * rowsPerBand; y < std::min(height, (band + 1) * rowsPerBand); ++y) {
                auto *p = reinterpret_cast<float *>(pixels + y * stride);
                const auto *q = reinterpret_cast<const float *>(effectPixels + y * effectStride);
                for (int x = 0; x < width; ++x) {
                    const float a = unit(selection[y * width + x] * opacity);
                    for (int c = 0; c < 4; ++c) p[x * 4 + c] += (q[x * 4 + c] - p[x * 4 + c]) * a;
                }
            }
            return true;
        });
        return out;
    }
    QImage out = base.convertToFormat(QImage::Format_RGBA64), effect = corrected.convertToFormat(QImage::Format_RGBA64);
    auto *pixels = out.bits();
    const auto *effectPixels = effect.constBits();
    const int stride = out.bytesPerLine(), effectStride = effect.bytesPerLine();
    Bands::run((height + rowsPerBand - 1) / rowsPerBand, [&](int band) {
        for (int y = band * rowsPerBand; y < std::min(height, (band + 1) * rowsPerBand); ++y) {
            auto *p = reinterpret_cast<QRgba64 *>(pixels + y * stride);
            const auto *q = reinterpret_cast<const QRgba64 *>(effectPixels + y * effectStride);
            for (int x = 0; x < width; ++x) {
                const double a = unit(selection[y * width + x] * opacity);
                p[x] = QRgba64::fromRgba64(std::lround(p[x].red() + (q[x].red() - p[x].red()) * a),
                                           std::lround(p[x].green() + (q[x].green() - p[x].green()) * a),
                                           std::lround(p[x].blue() + (q[x].blue() - p[x].blue()) * a),
                                           std::lround(p[x].alpha() + (q[x].alpha() - p[x].alpha()) * a));
            }
        }
        return true;
    });
    return out;
}
void secondary(QImage &image, const QVariantMap &node) {
    const QVariantMap curves = node.value("curves").toMap(), hdr = node.value("hdr").toMap();
    const QVariantList mesh = node.value("warper").toList();
    bool active = changedHdr(hdr) || changedWarper(mesh);
    for (const char *key : {"hueHue", "hueSat", "hueLum", "lumaLuma", "satSat"})
        active |= changedCurve(curves.value(key).toList(), QByteArray(key).startsWith("hue"));
    if (!active)
        return;
    const char *zones[] = {"black", "dark", "shadow", "light", "highlight", "specular"};
    const double centres[] = {.04, .14, .3, .525, .75, .925};
    // Decode the UI's variant maps once per frame. This loop can visit eight
    // million pixels; looking up every point and channel there dominated edits.
    const auto hueHue = prepareCurve(curves.value("hueHue").toList());
    const auto hueSat = prepareCurve(curves.value("hueSat").toList());
    const auto hueLum = prepareCurve(curves.value("hueLum").toList());
    const auto satSat = prepareCurve(curves.value("satSat").toList());
    const auto lumaLuma = prepareCurve(curves.value("lumaLuma").toList());
    struct MeshPoint { double hue, sat, dh, ds; };
    std::vector<MeshPoint> points;
    points.reserve(mesh.size());
    for (const auto &v : mesh) {
        const auto p = v.toMap();
        points.push_back({value(p, "hue"), value(p, "sat"), value(p, "dh"), value(p, "ds")});
    }
    double wheels[6][4] = {};
    for (int i = 0; i < 6; ++i) {
        const auto wheel = hdr.value(zones[i]).toList();
        for (int k = 0; k < 4; ++k) wheels[i][k] = wheel.value(k).toDouble();
    }
    // Detach before sharing the buffer: workers write disjoint rows and never
    // call a mutating QImage method. Keep the same QColor quantisation as before.
    auto *pixels = image.bits();
    const int stride = image.bytesPerLine(), width = image.width(), height = image.height();
    constexpr int rowsPerBand = 32;
    Bands::run((height + rowsPerBand - 1) / rowsPerBand, [&](int band) {
      for (int y = band * rowsPerBand; y < std::min(height, (band + 1) * rowsPerBand); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(pixels + y * stride);
        for (int x = 0; x < width; ++x) {
            const QColor c = QColor::fromRgba64(row[x]);
            const QColor hsl = c.toHsl();
            // A neutral pixel has no hue (Qt reports −1), and a grain-level
            // wobble around neutral has a random one, so the hue-indexed
            // curves fade in over absolute chroma: nothing below about 8
            // code values (camera noise on grey), fully from about 25.
            const double high = std::max({c.redF(), c.greenF(), c.blueF()}),
                         low = std::min({c.redF(), c.greenF(), c.blueF()});
            const double amount = smooth(.03, .1, high - low);
            double h = std::max(0.0, double(hsl.hslHueF())), s = hsl.hslSaturationF(), l = hsl.lightnessF();
            double dh = amount * (curve(hueHue, h, .5) - .5);
            double ds = amount * (curve(hueSat, h, .5) - .5) * 2;
            double dl = amount * (curve(hueLum, h, .5) - .5);
            // The warper's rings are measured as chroma over the brightest
            // channel, as on the scene-linear engine. Its tent weights sum to
            // one between neighbouring points and rings, so a lone moved
            // point fades out towards its neighbours and neutral rather than
            // pushing every colour it barely reaches by the full amount.
            const double ring = high > 1e-9 ? (high - low) / high : 0;
            double weights = 0, mh = 0, ms = 0;
            for (const auto &p : points) {
                double distance = std::abs(h - p.hue);
                distance = std::min(distance, 1 - distance);
                const double weight =
                    std::max(0.0, 1 - distance * 12) * std::max(0.0, 1 - std::abs(ring - p.sat) * 3);
                weights += weight;
                mh += weight * p.dh;
                ms += weight * p.ds;
            }
            const double mesh = amount / std::max(1.0, weights);
            dh += mh * mesh;
            const double hh = h + dh - std::floor(h + dh);
            s = unit((curve(satSat, s, s) + ds) * std::max(0.0, 1 + ms * mesh));
            l = unit(l + dl);
            QColor corrected = QColor::fromHslF(hh, s, l, c.alphaF()).toRgb();
            double r = corrected.redF(), g = corrected.greenF(), b = corrected.blueF(),
                   lum = .2126 * r + .7152 * g + .0722 * b;
            const double newLum = curve(lumaLuma, lum, lum);
            if (lum > 1e-9) {
                r *= newLum / lum;
                g *= newLum / lum;
                b *= newLum / lum;
            } else
                r = g = b = newLum;
            double weightSum = 0, push[4] = {};
            for (int i = 0; i < 6; ++i) {
                const double width = i == 0 ? .1 : i == 5 ? .15 : (centres[i + 1] - centres[i - 1]) * .7;
                const double weight = std::max(0.0, 1 - std::abs(lum - centres[i]) / width);
                weightSum += weight;
                for (int k = 0; k < 4; ++k)
                    push[k] += weight * wheels[i][k];
            }
            if (weightSum > 0) {
                for (double &v : push)
                    v /= weightSum;
                r *= std::exp2(push[0] + push[1]);
                g *= std::exp2(push[0] + push[2]);
                b *= std::exp2(push[0] + push[3]);
            }
            row[x] = QColor::fromRgbF(unit(r), unit(g), unit(b), c.alphaF()).rgba64();
        }
      }
      return true;
    });
}
struct NodeRuntime {
    // The node's primaries in floating point on a 16-bit copy.
    QImage process(const QImage &input, const QVariantMap &node) {
        QImage out = Bands::convertToFormat(input, QImage::Format_RGBA64);
        if (!DisplayGrade::active(node))
            return out;
        if (out.constBits() == input.constBits()) { // a copy to correct, made in bands
            QImage copy(out.size(), QImage::Format_RGBA64);
            const qsizetype row = qsizetype(out.width()) * 8;
            Bands::run((out.height() + 63) / 64, [&](int band) {
                for (int y = band * 64; y < std::min(out.height(), (band + 1) * 64); ++y)
                    std::memcpy(copy.scanLine(y), out.constScanLine(y), size_t(row));
                return true;
            });
            out = std::move(copy);
        }
        DisplayGrade::correct(out, node);
        return out;
    }
    QImage applyLut(const QImage &input, const QVariantMap &settings, QString *error) {
        const QString path = settings.value("path").toString();
        const double amount = value(settings, "intensity", 1);
        if (path.isEmpty() || amount <= 0)
            return input;
        if (!LutFilter::error(path).isEmpty())
            return input;
        QImage out = input;
        if (!LutFilter::apply(out, path, amount, error) && error->isEmpty())
            *error = QStringLiteral("Could not render the node LUT.");
        return out;
    }
};
struct Result {
    QImage image, raw;
    QVector<float> key;
};
struct Runtime {
    QHash<QString, std::shared_ptr<NodeRuntime>> processors;
    QImage run(const QImage &source, const QVariantMap &grade, int frame, QString *error, int depth = 0,
               const QString &prefix = QString(), bool inheritedLinear = false, bool workingSource = false) {
        if (depth > 8) {
            *error = QStringLiteral("Nested nodes go too many levels deep.");
            return source;
        }
        if (grade.value("bypass").toBool())
            return source;
        const QVariantMap management = grade.value("management").toMap();
        if (management.value("enabled").toBool() && !management.value("engine").toString().isEmpty()
            && management.value("engine") != "legacy") {
            *error = ColourPipeline::validate(management);
            if (!error->isEmpty()) return source;
        }
        const bool ocio = management.value("engine") == "ocio" && management.value("enabled").toBool();
        // A plain grade's clip space (Rec.601 or Rec.2020 primaries) comes
        // into its output's before the corrections; the transfer is kept.
        const QString clipSpace = grade.value("input").toString(), outputSpace = grade.value("output").toString();
        const bool plainSpaces = depth == 0 && !management.value("enabled").toBool() && !workingSource
                                 && GradeEngine::spaceConversion(grade);
        const bool linear = ocio || inheritedLinear || workingSource;
        QImage input = linear ? source : Bands::convertToFormat(source, QImage::Format_RGBA64);
        if (ocio && !workingSource) {
            input = ColourPipeline::stage(input, management, "input", "working", error);
            if (input.isNull()) return source;
        } else if (plainSpaces) {
            input = GradeEngine::convert(input, clipSpace, "rec709", outputSpace.isEmpty() ? "rec709" : outputSpace, "rec709");
        } else if (management.value("enabled").toBool() && !workingSource)
            input = GradeEngine::convert(input, management.value("input", "rec709").toString(),
                                         management.value("inputGamma", "rec709").toString(),
                                         management.value("working", "rec709").toString(),
                                         management.value("workingGamma", "rec709").toString());
        const QVariantList nodes = grade.value("nodes").toList();
        QHash<QString, int> ids;
        for (int i = 0; i < nodes.size(); ++i)
            ids.insert(nodes[i].toMap().value("id").toString(), i);
        const QString requestedOutput = grade.value("outputNode").toString();
        const int outputIndex = requestedOutput == "input" ? -1 : ids.value(requestedOutput, nodes.size() - 1);
        const QString requestedView = grade.value("viewNode").toString().section('/', 0, 0);
        const int viewIndex = ids.value(requestedView, -1);
        auto dependencies = [&](int index) {
            QList<int> list;
            const auto n = nodes[index].toMap();
            const auto kind = n.value("kind").toString();
            if (n.contains("inputs")) {
                for (const QVariant &v : n.value("inputs").toList())
                    list << ids.value(v.toString(), -1);
            } else {
                list << index - 1;
                if ((kind == "parallel" || kind == "layer") && index > 0)
                    list << index - 2;
            }
            if (!n.value("keyInput").toString().isEmpty())
                list << ids.value(n.value("keyInput").toString(), -1);
            return list;
        };
        QHash<int, int> remaining;
        QSet<int> counted;
        std::function<void(int)> claim = [&](int index) {
            if (index < 0)
                return;
            ++remaining[index];
            if (counted.contains(index))
                return;
            counted.insert(index);
            for (int d : dependencies(index))
                claim(d);
        };
        claim(outputIndex);
        claim(viewIndex);
        QImage nestedMatte;
        QHash<int, Result> cache;
        QSet<int> visiting;
        Result original{input, input, QVector<float>(input.width() * input.height(), 1)};
        std::function<Result(int)> evaluate = [&](int index) -> Result {
            if (!error->isEmpty()) return original;
            if (index < 0)
                return original;
            if (cache.contains(index))
                return cache.value(index);
            if (visiting.contains(index)) {
                *error = QStringLiteral("The node graph contains a cycle.");
                return original;
            }
            visiting.insert(index);
            const QVariantMap node = GradeEngine::evaluateNode(nodes[index].toMap(), frame);
            const QString kind = node.value("kind", "serial").toString();
            QList<Result> incoming;
            if (node.contains("inputs")) {
                for (const QVariant &v : node.value("inputs").toList())
                    incoming << evaluate(v.toString() == "input" ? -1 : ids.value(v.toString(), -1));
            } else if ((kind == "parallel" || kind == "layer") && index > 0) {
                incoming << evaluate(index - 1) << evaluate(index - 2);
            } else
                incoming << evaluate(index - 1);
            if (!error->isEmpty()) return original;
            Result base = incoming.isEmpty() ? original : incoming.first();
            if (incoming.isEmpty()) {
                base.image.fill(Qt::transparent);
                base.raw = base.image;
                std::fill(base.key.begin(), base.key.end(), 0);
            }
            if (!node.value("enabled", true).toBool()) {
                visiting.remove(index);
                cache.insert(index, base);
                for (int d : dependencies(index))
                    if (d >= 0 && --remaining[d] == 0)
                        cache.remove(d);
                return base;
            }
            if (kind == "parallel" && incoming.size() > 1) {
                base.image = incoming.first().image;
                for (int i = 1; i < incoming.size(); ++i)
                    base.image = mixImages(base.image, incoming[i].image, original.key, 1.0 / (i + 1));
            } else if (kind == "layer" && incoming.size() > 1) {
                base.image = incoming.first().image;
                for (int i = 1; i < incoming.size(); ++i)
                    base.image = mixImages(base.image, incoming[i].raw, incoming[i].key);
            } else if (kind == "combiner" && incoming.size() > 1) {
                base.image = input.copy();
                for (int y = 0; y < input.height(); ++y)
                    for (int x = 0; x < input.width(); ++x) {
                        double rgb[3] = {};
                        for (const Result &r : incoming) {
                            if (linear) {
                                const auto *p=reinterpret_cast<const float *>(r.image.constScanLine(y))+x*4;
                                for(int c=0;c<3;++c)rgb[c]+=p[c];
                            } else {
                                const QColor c = r.image.pixelColor(x, y);
                                rgb[0] += c.redF();
                                rgb[1] += c.greenF();
                                rgb[2] += c.blueF();
                            }
                        }
                        if (linear) {
                            auto *p = reinterpret_cast<float *>(base.image.scanLine(y)) + x*4;
                            for (int c=0;c<3;++c)p[c]=rgb[c];
                        } else base.image.setPixelColor(x, y,
                                                 QColor::fromRgbF(unit(rgb[0]), unit(rgb[1]), unit(rgb[2]),
                                                                  input.pixelColor(x, y).alphaF()));
                    }
            }
            if (!node.contains("inputs") && (kind == "parallel" || kind == "layer") && incoming.size() > 1)
                base = incoming.last();
            const QString id = prefix + node.value("id").toString();
            if (!processors.contains(id))
                processors.insert(id, std::make_shared<NodeRuntime>());
            auto processor = processors.value(id);
            QImage corrected;
            if (linear) {
                corrected = LinearGrade::correct(base.image, node, error);
                if (corrected.isNull()) return base;
            } else {
                corrected = processor->process(base.image, node);
                secondary(corrected, node);
                corrected = processor->applyLut(corrected, node.value("lut").toMap(), error);
            }
            if (kind == "splitter") {
                const QString channel = node.value("channel", "red").toString();
                for (int y = 0; y < corrected.height(); ++y)
                    for (int x = 0; x < corrected.width(); ++x) {
                        const QColor c = corrected.pixelColor(x, y);
                        if (linear) {
                            auto *p = reinterpret_cast<float *>(corrected.scanLine(y))+x*4;
                            if(channel!="red")p[0]=0;
                            if(channel!="green")p[1]=0;
                            if(channel!="blue")p[2]=0;
                        } else corrected.setPixelColor(x, y,
                                                QColor::fromRgbF(channel == "red" ? c.redF() : 0,
                                                                 channel == "green" ? c.greenF() : 0,
                                                                 channel == "blue" ? c.blueF() : 0, c.alphaF()));
                    }
            }
            if (kind == "compound") {
                const QString view = grade.value("viewNode").toString();
                const QString childPrefix = node.value("id").toString() + "/";
                const bool viewingChild = view.startsWith(childPrefix);
                corrected = run(corrected,
                                {{"nodes", node.value("children")},
                                 {"outputNode", node.value("childOutput")},
                                 {"viewNode", viewingChild ? view.mid(childPrefix.size()) : QString()}},
                                frame, error, depth + 1, id + "/", linear);
                if (!error->isEmpty()) return base;
                if (viewingChild)
                    nestedMatte = corrected;
            }
            auto key = makeMask(base.image, node, frame);
            const QString keyInput = node.value("keyInput").toString();
            if (!keyInput.isEmpty() && ids.contains(keyInput)) {
                const auto other = evaluate(ids.value(keyInput)).key;
                for (qsizetype i = 0; i < key.size(); ++i)
                    key[i] *= other[i];
            }
            // evaluateNode already resolved this frame's animation. These
            // controls are constant across its mask, including linked keys.
            const bool keyInvert = node.value("keyInvert").toBool();
            const double keyGain = value(node, "keyGain", 1);
            const double opacity = value(node, "opacity", 1);
            for (float &a : key)
                a = unit((keyInvert ? 1 - a : a) * keyGain * opacity);
            Result result{mixImages(base.image, corrected, key), corrected, key};
            if (!node.contains("inputs") && incoming.size() > 1) {
                if (kind == "parallel")
                    result.image = mixImages(incoming.first().image, result.image, original.key, .5);
                else if (kind == "layer")
                    result.image = mixImages(incoming.first().image, corrected, key);
            }
            visiting.remove(index);
            cache.insert(index, result);
            for (int d : dependencies(index))
                if (d >= 0 && --remaining[d] == 0)
                    cache.remove(d);
            return result;
        };
        const QString output = grade.value("outputNode").toString();
        const Result final = output == "input" ? original : evaluate(ids.value(output, nodes.size() - 1));
        if (!error->isEmpty()) return {};
        QImage image = final.image;
        const QString view = grade.value("viewNode").toString();
        if (view.contains('/') && ids.contains(view.section('/', 0, 0))) {
            evaluate(ids.value(view.section('/', 0, 0)));
            if (!nestedMatte.isNull())
                return nestedMatte;
        }
        if (!view.isEmpty() && ids.contains(view)) {
            const Result viewed = evaluate(ids.value(view));
            if (!error->isEmpty()) return {};
            image = input.copy();
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const double a = viewed.key[y * image.width() + x];
                    image.setPixelColor(x, y, QColor::fromRgbF(a, a, a));
                }
            return image;
        }
        if (ocio && !workingSource)
            image = ColourPipeline::stage(image, management, "working", "output", error);
        else if (management.value("enabled").toBool() && !workingSource)
            image = GradeEngine::convert(image, management.value("working", "rec709").toString(),
                                         management.value("workingGamma", "rec709").toString(),
                                         management.value("output", "rec709").toString(),
                                         management.value("outputGamma", "rec709").toString());
        return image;
    }
};
// The node of a grade that is one plain correction at this frame — no mask,
// LUT, secondaries, detail, blend or colour management — or nothing. Such a
// grade is corrected on the frame's own picture, 8- or 16-bit, with no copy
// and no conversion: the same arithmetic as the full engine's.
QVariantMap plainCorrection(const QVariantMap &grade, int frame) {
    const QVariantList nodes = grade.value("nodes").toList();
    if (nodes.size() != 1 || grade.value("bypass").toBool() || grade.value("management").toMap().value("enabled").toBool()
        || !grade.value("outputNode").toString().isEmpty() || !grade.value("viewNode").toString().isEmpty()
        || GradeEngine::spaceConversion(grade))
        return {};
    const QVariantMap node = GradeEngine::evaluateNode(nodes.first().toMap(), frame);
    if (node.value("kind", "serial").toString() != "serial" || !node.value("enabled", true).toBool()
        || node.contains("inputs") || !node.value("keyInput").toString().isEmpty()
        || !node.value("externalMatte").toString().isEmpty() || value(node, "opacity", 1) != 1
        || value(node, "keyGain", 1) != 1 || node.value("keyInvert").toBool()
        || node.value("window").toMap().value("enabled").toBool() || node.value("qualifier").toMap().value("enabled").toBool()
        || node.value("aiMask").toMap().value("enabled").toBool()
        || value(node.value("primaries").toMap(), "midDetail") != 0 || changedHdr(node.value("hdr").toMap())
        || changedWarper(node.value("warper").toList()))
        return {};
    for (const QVariant &v : node.value("windows").toList())
        if (v.toMap().value("enabled").toBool()) return {};
    const QVariantMap lut = node.value("lut").toMap();
    if (!lut.value("path").toString().isEmpty() && value(lut, "intensity", 1) > 0)
        return {};
    const QVariantMap curves = node.value("curves").toMap();
    for (const char *key : {"hueHue", "hueSat", "hueLum", "lumaLuma", "satSat"})
        if (changedCurve(curves.value(key).toList(), QByteArray(key).startsWith("hue")))
            return {};
    return node;
}
} // namespace

QVariantMap GradeEngine::trackedWindowAt(const QVariantMap &window, int frame) { return trackedWindow(window, frame); }

QVariantMap GradeEngine::evaluateNode(const QVariantMap &node, int frame) {
    QVariantMap result = node;
    const QVariantMap keys = node.value("keyframes").toMap();
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        QStringList path = it.key().split('.');
        if (!QStringList{"window", "qualifier", "hdr", "lut", "opacity", "keyGain"}.contains(path.first()))
            path.prepend("primaries");
        // A key can be added before an HDR zone has ever been adjusted.
        // Initialise that sparse wheel as channels, not a { "y": value } map.
        if (path.first() == "hdr" && path.size() == 3) {
            auto hdr = result.value("hdr").toMap();
            if (hdr.value(path[1]).typeId() != QMetaType::QVariantList)
                hdr[path[1]] = QVariantList{0., 0., 0., 0.};
            result["hdr"] = hdr;
        }
        result = setPath(result, path, 0, sample(it.value().toList(), frame, "value", 0)).toMap();
    }
    result.remove("keyframes");
    return result;
}
bool GradeEngine::required(const QVariantMap &grade) {
    if (grade.contains("management") || grade.contains("outputNode") || grade.contains("viewNode"))
        return true;
    for (const QVariant &v : grade.value("nodes").toList()) {
        const QVariantMap n = v.toMap();
        if (value(n.value("primaries").toMap(), "midDetail") < 0)
            return true;
        const bool legacy = n.value("maskModel") == "legacy";
        if (n.value("kind", "serial") != "serial" || n.contains("inputs") ||
            (legacy ? !n.value("windows").toList().isEmpty() : n.contains("windows")) ||
            n.contains("aiMask") ||
            (legacy ? !n.value("externalMatte").toString().isEmpty() : n.contains("externalMatte")) ||
            (legacy ? !n.value("keyInput").toString().isEmpty() : n.contains("keyInput")) ||
            (legacy ? value(n, "opacity", 1) != 1 : n.contains("opacity")) ||
            (legacy ? changedHdr(n.value("hdr").toMap()) : n.contains("hdr")) ||
            (legacy ? changedWarper(n.value("warper").toList()) : n.contains("warper")))
            return true;
        if (n.value("maskModel") == "component" &&
            (n.value("qualifier").toMap().value("enabled").toBool() ||
             n.value("window").toMap().value("enabled").toBool()))
            return true;
        const QVariantMap q = n.value("qualifier").toMap();
        if (legacy ? value(q, "blur") != 0 || value(q, "grow") != 0
                   : q.contains("blur") || q.contains("grow"))
            return true;
        const QVariantMap c = n.value("curves").toMap();
        for (const char *key : {"hueHue", "hueSat", "hueLum", "lumaLuma", "satSat"})
            if (changedCurve(c.value(key).toList(), QByteArray(key).startsWith("hue")))
                return true;
        const QVariantMap k = n.value("keyframes").toMap();
        for (auto it = k.cbegin(); it != k.cend(); ++it)
            if (!QStringList{"lift.y", "gamma.y", "gain.y", "offset.y", "contrast", "saturation"}.contains(it.key()))
                return true;
    }
    return false;
}
bool GradeEngine::spaceConversion(const QVariantMap &grade) {
    const QString in = grade.value("input").toString(), out = grade.value("output", "rec709").toString();
    auto known = [](const QString &s) { return s == "rec601" || s == "rec2020"; };
    return !in.isEmpty() && in != "auto" && in != out && in != "srgb" && (known(in) || known(out));
}
QVariantMap GradeEngine::pinMaskModels(const QVariantMap &grade) {
    if (!grade.contains("nodes")) return grade;
    // Infer once from the complete old graph, before adding annotations: an
    // extension anywhere used to switch every node to the component algorithm.
    // (Colour management is every clip's input, not a graph extension.)
    QVariantMap graph = grade;
    graph.remove("management");
    const QString model = required(graph) ? QStringLiteral("component") : QStringLiteral("legacy");
    std::function<QVariantList(QVariantList)> pin = [&](QVariantList nodes) {
        for (QVariant &v : nodes) {
            auto n = v.toMap();
            if (!n.contains("maskModel")) n["maskModel"] = model;
            if (n.contains("children")) n["children"] = pin(n.value("children").toList());
            v = n;
        }
        return nodes;
    };
    auto result = grade;
    result["nodes"] = pin(grade.value("nodes").toList());
    return result;
}
QVariantList GradeEngine::pinVersionMaskModels(const QVariantList &versions) {
    auto result = versions;
    for (auto &v : result) {
        auto version = v.toMap();
        version["grade"] = pinMaskModels(version.value("grade").toMap());
        v = version;
    }
    return result;
}
QString GradeEngine::validate(const QVariantMap &grade) {
    int totalNodes = 0;
    std::function<QString(const QVariantList &, int)> check = [&](const QVariantList &nodes, int depth) -> QString {
        totalNodes += nodes.size();
        if (totalNodes > 128 || depth > 8)
            return QStringLiteral("A grade supports up to 128 nodes in total and "
                                  "eight nested levels.");
        QHash<QString, int> ids;
        for (int i = 0; i < nodes.size(); ++i) {
            const QString id = nodes[i].toMap().value("id").toString();
            if (id.isEmpty() || id == "input" || id.contains('/') || ids.contains(id))
                return QStringLiteral("Correction nodes need unique IDs.");
            ids[id] = i;
        }
        QHash<int, int> marks;
        std::function<bool(int)> visit = [&](int i) {
            if (marks.value(i) == 1)
                return false;
            if (marks.value(i) == 2)
                return true;
            marks[i] = 1;
            const QVariantMap node = nodes[i].toMap();
            QVariantList links = node.contains("inputs") ? node.value("inputs").toList()
                                 : i > 0                 ? QVariantList{nodes[i - 1].toMap().value("id")}
                                                         : QVariantList{};
            if (!node.value("keyInput").toString().isEmpty())
                links << node.value("keyInput");
            for (const QVariant &link : links) {
                const QString id = link.toString();
                if (id == "input")
                    continue;
                if (!ids.contains(id) || !visit(ids[id]))
                    return false;
            }
            marks[i] = 2;
            return true;
        };
        for (int i = 0; i < nodes.size(); ++i) {
            if (!visit(i))
                return QStringLiteral("Connections must refer to existing nodes and "
                                      "cannot form a cycle.");
            const QVariantMap n = nodes[i].toMap();
            if (!QStringList{"serial", "parallel", "layer", "splitter", "combiner", "compound"}.contains(
                    n.value("kind", "serial").toString()))
                return QStringLiteral("Unknown correction node type.");
            if (n.value("kind") == "compound") {
                const QString childOutput = n.value("childOutput").toString();
                if (!childOutput.isEmpty() && childOutput != "input") {
                    bool found = false;
                    for (const QVariant &v : n.value("children").toList())
                        found |= v.toMap().value("id") == childOutput;
                    if (!found)
                        return QStringLiteral("The nested node's output correction no longer exists.");
                }
                const QString e = check(n.value("children").toList(), depth + 1);
                if (!e.isEmpty())
                    return e;
            }
        }
        return {};
    };
    const QString error = check(grade.value("nodes").toList(), 0);
    if (!error.isEmpty())
        return error;
    const QString output = grade.value("outputNode").toString();
    if (!output.isEmpty() && output != "input") {
        bool found = false;
        for (const QVariant &v : grade.value("nodes").toList())
            found |= v.toMap().value("id") == output;
        if (!found)
            return QStringLiteral("The output correction no longer exists.");
    }
    const QVariantMap m = grade.value("management").toMap();
    if (m.value("enabled").toBool() && !m.value("engine").toString().isEmpty() && m.value("engine") != "legacy")
        return ColourPipeline::validate(m);
    if (m.value("enabled").toBool())
        for (const char *key : {"input", "working", "output"})
            if (!space(m.value(key, "rec709").toString(),
                       m.value(QString::fromLatin1(key) + "Gamma", "rec709").toString())
                     .isValid())
                return QStringLiteral("Choose a supported colour space and gamma.");
    return {};
}
QColorSpace GradeEngine::space(const QString &gamut, const QString &gamma) {
    QColorSpace result;
    if (gamut == "srgb" || gamut == "rec709" || gamut == "auto")
        result = QColorSpace(QColorSpace::SRgb);
    else if (gamut == "rec2020")
        result = QColorSpace(QColorSpace::Bt2020);
    else if (gamut == "p3")
        result = QColorSpace(QColorSpace::DisplayP3);
    else if (gamut == "rec601")
        result = QColorSpace(QPointF(.3127, .3290), QPointF(.630, .340), QPointF(.310, .595), QPointF(.155, .070),
                             QColorSpace::TransferFunction::Gamma, 2.4);
    else
        return {};
    const QString transfer = gamma.isEmpty() ? (gamut == "srgb" || gamut == "p3" ? "srgb" : "rec709") : gamma;
    if (transfer == "linear")
        result.setTransferFunction(QColorSpace::TransferFunction::Linear);
    else if (transfer == "srgb")
        result.setTransferFunction(QColorSpace::TransferFunction::SRgb);
    else if (transfer == "pq")
        result.setTransferFunction(QColorSpace::TransferFunction::St2084);
    else if (transfer == "hlg")
        result.setTransferFunction(QColorSpace::TransferFunction::Hlg);
    else if (transfer == "2.2" || transfer == "2.4" || transfer == "2.6")
        result.setTransferFunction(QColorSpace::TransferFunction::Gamma, transfer.toFloat());
    else if (transfer == "rec709") {
        QList<uint16_t> table;
        table.reserve(4096);
        for (int i = 0; i < 4096; ++i) {
            const double x = i / 4095.;
            table << uint16_t(std::lround((x < .081 ? x / 4.5 : std::pow((x + .099) / 1.099, 1 / .45)) * 65535));
        }
        result.setTransferFunction(table);
    } else
        return {};
    return result;
}
QImage GradeEngine::convert(const QImage &image, const QString &from, const QString &fromGamma, const QString &to,
                            const QString &toGamma) {
    if (image.isNull())
        return image;
    const QColorSpace a = space(from, fromGamma), b = space(to, toGamma);
    if (!a.isValid() || !b.isValid() || a == b)
        return image;
    QImage out = image.convertToFormat(QImage::Format_RGBA64);
    out.setColorSpace(a);
    out.convertToColorSpace(b);
    return out;
}
QImage GradeEngine::mask(const QImage &image, const QVariantMap &node, int frame) {
    const auto values = makeMask(image, evaluateNode(node, frame), frame);
    QImage out(image.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x)
            out.scanLine(y)[x] = std::lround(values[y * out.width() + x] * 255);
    return out;
}
QImage GradeEngine::render(const QImage &image, const QVariantMap &grade, int frame, QString *error) {
    QString local;
    QString *problem = error ? error : &local;
    *problem = validate(grade);
    if (!problem->isEmpty())
        return {};
    Runtime runtime;
    return runtime.run(image, grade, frame, problem);
}

QVariantMap GradeEngine::plainNode(const QVariantMap &grade, int frame) { return plainCorrection(grade, frame); }

bool GradeEngine::correct(QImage &image, const QVariantMap &grade, int frame, QString *error) {
    QString local;
    QString *problem = error ? error : &local;
    problem->clear();
    if (const QVariantMap plain = plainCorrection(grade, frame); !plain.isEmpty()) {
        DisplayGrade::correct(image, plain);
        return true;
    }
    const QImage output = render(image, grade, frame, problem);
    if (!problem->isEmpty() || output.isNull()) return false;
    image = output.convertToFormat(image.format());
    return true;
}

WorkingFrame GradeEngine::renderWorking(const WorkingFrame &image, const QVariantMap &grade,
                                       int frame, QString *error, bool matteView) {
    QString local;
    QString *problem = error ? error : &local;
    *problem = image.validate();
    if (!problem->isEmpty()) return {};
    *problem = validate(grade);
    if (!problem->isEmpty()) return {};
    // Matte views are presentation operations, not the image to composite.
    auto corrections = grade;
    if(!matteView)corrections.remove("viewNode");
    Runtime runtime;
    const QImage result = runtime.run(image.pixels(), corrections, frame, problem, 0, {}, true, true);
    if (!problem->isEmpty() || result.isNull()) return {};
    return image.withPixels(result, problem);
}

QVariantMap GradeEngine::freshCopy(const QVariantMap &grade) {
    QVariantMap result = pinMaskModels(grade);
    QVariantList nodes = result.value("nodes").toList();
    QHash<QString, QString> ids;
    for (const QVariant &v : nodes)
        ids[v.toMap().value("id").toString()] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (QVariant &v : nodes) {
        QVariantMap n = v.toMap();
        n["id"] = ids.value(n.value("id").toString());
        n.remove("frames");
        if (n.contains("inputs")) {
            QVariantList inputs;
            for (const QVariant &input : n.value("inputs").toList())
                inputs << ids.value(input.toString(), QStringLiteral("input"));
            n["inputs"] = inputs;
        }
        if (n.contains("keyInput"))
            n["keyInput"] = ids.value(n.value("keyInput").toString());
        if (n.value("kind") == "compound") {
            const QVariantMap child =
                freshCopy({{"nodes", n.value("children")}, {"outputNode", n.value("childOutput")}});
            n["children"] = child.value("nodes");
            n["childOutput"] = child.value("outputNode");
        }
        v = n;
    }
    result["nodes"] = nodes;
    if (result.contains("outputNode") && !result.value("outputNode").toString().isEmpty())
        result["outputNode"] = ids.value(result.value("outputNode").toString(), QStringLiteral("input"));
    result.remove("viewNode");
    result.remove("sharing");
    return result;
}
QStringList GradeEngine::resourceProblems(const QVariantMap &grade) {
    QStringList problems;
    if (grade.value("bypass").toBool())
        return problems;
    const QString managementProblem = validate(grade);
    if (!managementProblem.isEmpty()) problems << managementProblem;
    const auto nodes = grade.value("nodes").toList();
    QHash<QString, int> ids;
    for (int i = 0; i < nodes.size(); ++i)
        ids[nodes[i].toMap().value("id").toString()] = i;
    QSet<int> visited;
    std::function<void(int)> inspect = [&](int i) {
        if (i < 0 || i >= nodes.size() || visited.contains(i))
            return;
        visited.insert(i);
        const auto n = nodes[i].toMap();
        const QString kind = n.value("kind").toString();
        if (n.contains("inputs")) {
            for (const QVariant &v : n.value("inputs").toList())
                inspect(ids.value(v.toString(), -1));
        } else {
            inspect(i - 1);
            if (kind == "parallel" || kind == "layer")
                inspect(i - 2);
        }
        if (!n.value("enabled", true).toBool())
            return;
        inspect(ids.value(n.value("keyInput").toString(), -1));
        const auto lut = n.value("lut").toMap();
        const QString path = lut.value("path").toString();
        bool usesLut = value(lut, "intensity", 1) > 0;
        for (const QVariant &v : n.value("keyframes").toMap().value("lut.intensity").toList())
            usesLut |= v.toMap().value("value").toDouble() > 0;
        if (!path.isEmpty() && usesLut) {
            const QString error = LutFilter::error(path);
            if (!error.isEmpty())
                problems << path + QStringLiteral(" — ") + error;
        }
        const QString matte = n.value("externalMatte").toString();
        if (!matte.isEmpty()) {
            QImageReader reader(matte);
            const QSize size = reader.size();
            if (!reader.canRead() || !size.isValid() || qint64(size.width()) * size.height() > 32 * 1024 * 1024)
                problems << matte + QStringLiteral(" — external matte is missing, unreadable or exceeds 32 megapixels");
        }
        if (kind == "compound")
            problems << resourceProblems({{"nodes", n.value("children")}, {"outputNode", n.value("childOutput")}});
    };
    const QString output = grade.value("outputNode").toString();
    if (output != "input")
        inspect(ids.value(output, nodes.size() - 1));
    return problems;
}

QVariantMap GradeEngine::nodeAt(const QVariantMap &grade, const QString &path) {
    const QStringList ids = path.split('/');
    QVariantList nodes = grade.value("nodes").toList();
    QVariantMap found;
    for (const QString &id : ids) {
        found.clear();
        for (const QVariant &v : nodes)
            if (v.toMap().value("id").toString() == id) {
                found = v.toMap();
                break;
            }
        if (found.isEmpty())
            return {};
        nodes = found.value("children").toList();
    }
    return found;
}
bool GradeEngine::replaceNode(QVariantMap *grade, const QString &path, const QVariantMap &node) {
    const int slash = path.indexOf('/');
    const QString id = slash < 0 ? path : path.left(slash);
    QVariantList nodes = grade->value("nodes").toList();
    for (int i = 0; i < nodes.size(); ++i) {
        QVariantMap current = nodes[i].toMap();
        if (current.value("id").toString() != id)
            continue;
        if (slash < 0)
            current = node;
        else {
            QVariantMap child{{"nodes", current.value("children")}};
            if (!replaceNode(&child, path.mid(slash + 1), node))
                return false;
            current["children"] = child.value("nodes");
        }
        nodes[i] = current;
        (*grade)["nodes"] = nodes;
        return true;
    }
    return false;
}
