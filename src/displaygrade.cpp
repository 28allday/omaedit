#include "displaygrade.h"
#include "bands.h"
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <memory>

namespace {
double number(const QVariantMap &map, const char *key, double fallback = 0) {
    const double v = map.value(QLatin1String(key), fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}
double wheel(const QVariantMap &map, const char *key, int channel, double fallback) {
    const QVariantList values = map.value(QLatin1String(key)).toList();
    const double v = values.value(channel, fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}
constexpr double weights[3] = {.2126, .7152, .0722}; // Rec.709 luma

// Everything a channel's table depends on.
struct Settings {
    std::array<double, 3> lift{}, gamma{1, 1, 1}, gain{1, 1, 1}, offset{};
    double contrast = 1, pivot = .435, exposure = 1, black = 0, white = 0;
    DisplayGrade::Curve master, channel[3];
    double hue = 0, saturation = 1, detail = 0;
    explicit Settings(const QVariantMap &node) {
        const QVariantMap p = node.value(QStringLiteral("primaries")).toMap();
        const double shadows = number(p, "shadows"), highlights = number(p, "highlights");
        // White balance and tint are gains in linear light, carried into
        // this display-referred picture through a 2.2 power: neutral at
        // 6500 K and at no tint, smooth and monotonic over their range.
        const double temperature = std::clamp(number(p, "temp", 6500), 1000., 40000.);
        const double tint = std::clamp(number(p, "tint") / 100, -1., 1.);
        for (int c = 0; c < 3; ++c) {
            lift[c] = std::clamp(wheel(p, "lift", 0, 0) + wheel(p, "lift", c + 1, 0) + shadows * .3, -1., 1.);
            gamma[c] = std::clamp(1 + wheel(p, "gamma", 0, 0) + wheel(p, "gamma", c + 1, 0), .1, 5.);
            gain[c] = std::clamp(wheel(p, "gain", 0, 1) * (1 + wheel(p, "gain", c + 1, 0)) * (1 + highlights * .5), 0., 4.);
            gain[c] *= std::pow(temperature / 6500, (c == 0 ? -.5 : c == 2 ? .5 : 0.) / 2.2);
            gain[c] *= std::exp2((c == 1 ? -tint * .5 : tint * .25) / 2.2);
            offset[c] = std::clamp(wheel(p, "offset", 0, 0) + wheel(p, "offset", c + 1, 0), -4., 4.);
        }
        contrast = std::clamp(number(p, "contrast", 1), 0., 4.);
        pivot = std::clamp(number(p, "pivot", .435), 0., 1.);
        exposure = std::exp2(std::clamp(number(p, "exposure"), -3., 3.));
        black = std::clamp(number(p, "blacks"), -4., 4.) * .25;
        white = std::clamp(number(p, "whites"), -4., 4.) * .25;
        const QVariantMap curves = node.value(QStringLiteral("curves")).toMap();
        master = DisplayGrade::Curve(curves.value(QStringLiteral("master")).toList());
        static const char *const names[3] = {"red", "green", "blue"};
        for (int c = 0; c < 3; ++c)
            channel[c] = DisplayGrade::Curve(curves.value(QLatin1String(names[c])).toList());
        hue = number(p, "hue") * M_PI / 180;
        saturation = std::clamp(number(p, "saturation", 1), 0., 4.);
        detail = std::clamp(number(p, "midDetail"), -1., 1.);
    }
    bool tonal() const {
        bool on = contrast != 1 || exposure != 1 || black != 0 || white != 0 || !master.identity;
        for (int c = 0; c < 3; ++c)
            on |= lift[c] != 0 || gamma[c] != 1 || gain[c] != 1 || offset[c] != 0 || !channel[c].identity;
        return on;
    }
    bool chroma() const { return hue != 0 || saturation != 1; }
    // One channel, 0..1 in and out.
    double apply(int c, double v) const {
        v += lift[c] * (1 - v);
        if (gamma[c] != 1)
            v = std::pow(std::max(v, 0.), 1 / gamma[c]);
        v *= gain[c];
        v = (v - pivot) * contrast + pivot + offset[c];
        v *= exposure;
        v = (v - std::max(0., black)) / std::max(.01, 1 - std::max(0., white) - std::max(0., black));
        v = v * (1 + std::min(0., white) - std::max(0., -black)) + std::max(0., -black);
        v = std::clamp(v, 0., 1.);
        return master.at(channel[c].at(v));
    }
    std::vector<double> key() const {
        std::vector<double> k{contrast, pivot, exposure, black, white};
        for (int c = 0; c < 3; ++c)
            k.insert(k.end(), {lift[c], gamma[c], gain[c], offset[c]});
        for (const auto *curve : {&master, &channel[0], &channel[1], &channel[2]}) {
            k.push_back(curve->x.size());
            k.insert(k.end(), curve->x.begin(), curve->x.end());
            k.insert(k.end(), curve->y.begin(), curve->y.end());
        }
        return k;
    }
};

// The three channels' tables over every 16-bit code, kept for the last few
// settings: a node holds still for many frames, and a keyframed one changes
// every frame either way.
using Tables = std::array<std::vector<float>, 3>;  // as DisplayGrade::Prepared::tables
std::shared_ptr<const Tables> tablesFor(const Settings &s) {
    static QMutex mutex;
    static std::vector<std::pair<std::vector<double>, std::shared_ptr<const Tables>>> kept;
    const std::vector<double> key = s.key();
    {
        QMutexLocker lock(&mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (it->first == key) {
                auto found = it->second;
                std::rotate(kept.begin(), it, it + 1);
                return found;
            }
    }
    auto made = std::make_shared<Tables>();
    for (auto &table : *made)
        table.resize(65536);
    constexpr int perBand = 4096;
    Bands::run(3 * 65536 / perBand, [&](int band) {
        const int c = band / (65536 / perBand), first = band % (65536 / perBand) * perBand;
        for (int i = first; i < first + perBand; ++i)
            (*made)[size_t(c)][size_t(i)] = float(s.apply(c, i / 65535.));
        return true;
    });
    QMutexLocker lock(&mutex);
    kept.insert(kept.begin(), {key, made});
    if (kept.size() > 16)
        kept.pop_back();
    return made;
}

// Hue rotation and saturation about each pixel's luma, in the luma/chroma
// basis of the scene-linear engine: a fixed RGB matrix.
std::array<double, 9> chromaMatrix(double angle, double saturation) {
    const double cs = std::cos(angle) * saturation, sn = std::sin(angle) * saturation;
    std::array<double, 9> m{};
    for (int c = 0; c < 3; ++c) {
        m[0 * 3 + c] = weights[c] * (1 - sn - cs) + (c == 0 ? cs : c == 2 ? sn : 0);
        m[2 * 3 + c] = weights[c] * (1 - cs + sn) + (c == 2 ? cs : c == 0 ? -sn : 0);
        m[1 * 3 + c] = (weights[c] - .2126 * m[0 * 3 + c] - .0722 * m[2 * 3 + c]) / .7152;
    }
    return m;
}

// A box blur of the picture's colour, separable, edges extended.
std::vector<float> blurred(const QImage &image, int radius) {
    const int w = image.width(), h = image.height();
    std::vector<float> across(size_t(w) * h * 3), out(size_t(w) * h * 3);
    Bands::run((h + 31) / 32, [&](int band) {
        for (int y = band * 32; y < std::min(h, (band + 1) * 32); ++y) {
            const auto *row = reinterpret_cast<const quint16 *>(image.constScanLine(y));
            float *dst = across.data() + size_t(y) * w * 3;
            for (int c = 0; c < 3; ++c) {
                double sum = 0;
                for (int k = -radius; k <= radius; ++k)
                    sum += row[std::clamp(k, 0, w - 1) * 4 + c];
                for (int x = 0; x < w; ++x) {
                    dst[x * 3 + c] = float(sum / (2 * radius + 1) / 65535);
                    sum += row[std::min(w - 1, x + radius + 1) * 4 + c] - row[std::max(0, x - radius) * 4 + c];
                }
            }
        }
        return true;
    });
    constexpr int columns = 64;
    Bands::run((w + columns - 1) / columns, [&](int band) {
        for (int x = band * columns; x < std::min(w, (band + 1) * columns); ++x)
            for (int c = 0; c < 3; ++c) {
                double sum = 0;
                for (int k = -radius; k <= radius; ++k)
                    sum += across[(size_t(std::clamp(k, 0, h - 1)) * w + x) * 3 + c];
                for (int y = 0; y < h; ++y) {
                    out[(size_t(y) * w + x) * 3 + c] = float(sum / (2 * radius + 1));
                    sum += across[(size_t(std::min(h - 1, y + radius + 1)) * w + x) * 3 + c]
                           - across[(size_t(std::max(0, y - radius)) * w + x) * 3 + c];
                }
            }
        return true;
    });
    return out;
}
// Mid detail: positive sharpens, negative softens, both most in the
// midtones (4·l·(1 − l) of each pixel's luma) so blacks and whites stay put.
// Softening reaches further the more it is turned up; sharpening works at a
// fixed small radius. Both radii follow the frame's width.
void detail(QImage &image, double amount) {
    if (std::abs(amount) < 1e-6)
        return;
    const int w = image.width(), h = image.height();
    const int radius = amount < 0 ? std::max(1, int(std::lround(8 * -amount * w / 1920.)))
                                  : std::max(1, int(std::lround(2. * w / 1920.)));
    const std::vector<float> blur = blurred(image, radius);
    Bands::run((h + 31) / 32, [&](int band) {
        for (int y = band * 32; y < std::min(h, (band + 1) * 32); ++y) {
            auto *row = reinterpret_cast<quint16 *>(image.scanLine(y));
            const float *soft = blur.data() + size_t(y) * w * 3;
            for (int x = 0; x < w; ++x) {
                double rgb[3];
                for (int c = 0; c < 3; ++c)
                    rgb[c] = row[x * 4 + c] / 65535.;
                const double l = weights[0] * rgb[0] + weights[1] * rgb[1] + weights[2] * rgb[2];
                const double a = 4 * l * (1 - l) * amount;
                for (int c = 0; c < 3; ++c) {
                    const double v = amount < 0 ? rgb[c] + (soft[x * 3 + c] - rgb[c]) * -a
                                                : rgb[c] + (rgb[c] - soft[x * 3 + c]) * a;
                    row[x * 4 + c] = quint16(std::lround(std::clamp(v, 0., 1.) * 65535));
                }
            }
        }
        return true;
    });
}
} // namespace

DisplayGrade::Curve::Curve(const QVariantList &points) {
    std::vector<std::pair<double, double>> sorted;
    for (const QVariant &v : points) {
        const QVariantList p = v.toList();
        if (p.size() < 2)
            continue;
        const double px = p[0].toDouble(), py = p[1].toDouble();
        if (std::isfinite(px) && std::isfinite(py))
            sorted.emplace_back(std::clamp(px, 0., 1.), std::clamp(py, 0., 1.));
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &p : sorted) {
        if (!x.empty() && p.first <= x.back())
            continue; // a second point at the same input: the editor refuses those
        x.push_back(p.first);
        y.push_back(p.second);
    }
    for (size_t i = 0; i < x.size(); ++i)
        identity &= std::abs(x[i] - y[i]) < 1e-9;
    if (x.size() < 2) {
        identity = x.empty();
        return;
    }
    // Fritsch–Carlson tangents: the secants' mean where they agree, flat at
    // a turn, limited so no segment overshoots its ends.
    const size_t n = x.size();
    std::vector<double> secant(n - 1);
    for (size_t i = 0; i + 1 < n; ++i)
        secant[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
    slope.assign(n, 0);
    slope[0] = secant[0];
    slope[n - 1] = secant[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        slope[i] = secant[i - 1] * secant[i] <= 0 ? 0 : (secant[i - 1] + secant[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (secant[i] == 0) {
            slope[i] = slope[i + 1] = 0;
            continue;
        }
        const double a = slope[i] / secant[i], b = slope[i + 1] / secant[i], r = a * a + b * b;
        if (r > 9) {
            const double t = 3 / std::sqrt(r);
            slope[i] = t * a * secant[i];
            slope[i + 1] = t * b * secant[i];
        }
    }
}

double DisplayGrade::Curve::at(double v) const {
    if (identity)
        return v;
    if (x.size() == 1)
        return y[0];
    if (v <= x.front())
        return y.front();
    if (v >= x.back())
        return y.back();
    const size_t i = size_t(std::upper_bound(x.begin(), x.end(), v) - x.begin()) - 1;
    const double h = x[i + 1] - x[i], t = (v - x[i]) / h, t2 = t * t, t3 = t2 * t;
    const double value = (2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * slope[i]
                         + (-2 * t3 + 3 * t2) * y[i + 1] + (t3 - t2) * h * slope[i + 1];
    return std::clamp(value, 0., 1.);
}

DisplayGrade::Prepared DisplayGrade::prepare(const QVariantMap &node) {
    const Settings s(node);
    Prepared p;
    if (s.tonal()) p.tables = tablesFor(s);
    p.chroma = s.chroma();
    if (p.chroma) p.matrix = chromaMatrix(s.hue, s.saturation);
    p.detail = s.detail;
    return p;
}

bool DisplayGrade::active(const QVariantMap &node) {
    const Settings s(node);
    return s.tonal() || s.chroma() || std::abs(s.detail) >= 1e-6;
}

void DisplayGrade::correct(QImage &image, const QVariantMap &node) {
    if (image.isNull() || (image.format() != QImage::Format_RGBA64 && image.format() != QImage::Format_RGBA8888))
        return;
    const Settings s(node);
    if (image.format() == QImage::Format_RGBA8888 && std::abs(s.detail) >= 1e-6) { // detail works on 16 bits
        QImage wide = image.convertToFormat(QImage::Format_RGBA64);
        correct(wide, node);
        const QImage narrow = wide.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < image.height(); ++y)
            std::memcpy(image.scanLine(y), narrow.constScanLine(y), size_t(image.width()) * 4);
        return;
    }
    const bool wide = image.format() == QImage::Format_RGBA64;
    const bool tonal = s.tonal(), chroma = s.chroma();
    if (tonal || chroma) {
        const auto tables = tonal ? tablesFor(s) : nullptr;
        const auto m = chromaMatrix(s.hue, s.saturation);
        const int w = image.width(), h = image.height();
        uchar *bits = image.bits();
        const qsizetype stride = image.bytesPerLine();
        Bands::run((h + 31) / 32, [&](int band) {
            // An 8-bit picture reads the tables at its codes' 16-bit values.
            const auto pixel = [&](auto *p, int scale, double top) {
                double rgb[3];
                for (int c = 0; c < 3; ++c)
                    rgb[c] = tables ? (*tables)[size_t(c)][size_t(p[c]) * scale] : p[c] / top;
                if (chroma) {
                    const double r = rgb[0], g = rgb[1], b = rgb[2];
                    for (int c = 0; c < 3; ++c)
                        rgb[c] = m[size_t(c) * 3] * r + m[size_t(c) * 3 + 1] * g + m[size_t(c) * 3 + 2] * b;
                }
                for (int c = 0; c < 3; ++c)
                    p[c] = std::remove_reference_t<decltype(*p)>(std::lround(std::clamp(rgb[c], 0., 1.) * top));
            };
            for (int y = band * 32; y < std::min(h, (band + 1) * 32); ++y) {
                if (wide) {
                    auto *row = reinterpret_cast<quint16 *>(bits + y * stride);
                    for (int x = 0; x < w; ++x) pixel(row + x * 4, 1, 65535.);
                } else {
                    auto *row = bits + y * stride;
                    for (int x = 0; x < w; ++x) pixel(row + x * 4, 257, 255.);
                }
            }
            return true;
        });
    }
    detail(image, s.detail);
}
