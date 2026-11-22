#include "keyer.h"

#include <QColor>
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Keyer {
namespace {

QString tr(const char *text) { return QCoreApplication::translate("Keyer", text); }

// As the keyer reads a pixel: display values (the engine's sRGB curve), then
// Rec. 709 Y, Cb and Cr.
float display(float c) {
    if (c <= 0.f) return 0.f;
    if (c >= 1.f) return 1.f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
struct Ycc { float y = 0, cb = 0, cr = 0; };
Ycc ycc(float r, float g, float b) {
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {y, (b - y) / 1.8556f, (r - y) / 1.5748f};
}
Ycc pixelYcc(const QImage &image, int x, int y) {
    const float *p = reinterpret_cast<const float *>(image.constScanLine(y)) + size_t(x) * 4;
    return ycc(display(p[0]), display(p[1]), display(p[2]));
}
QString hex(const Ycc &c) {
    const float r = c.y + 1.5748f * c.cr, b = c.y + 1.8556f * c.cb;
    const float g = (c.y - 0.2126f * r - 0.0722f * b) / 0.7152f;
    const auto byte = [](float v) { return std::clamp(int(std::lround(v * 255.f)), 0, 255); };
    return QColor(byte(r), byte(g), byte(b)).name(QColor::HexRgb);
}
QImage linear(const QImage &picture) { return picture.convertToFormat(QImage::Format_RGBA32FPx4); }

}  // namespace

Backdrop find(const QImage &picture, const QString &screen, const QString &current) {
    if (picture.isNull()) return {{}, tr("There is no picture to look at.")};
    const QImage image = linear(picture);
    // The hue looked for, as a direction in Cb/Cr.
    Ycc hue;
    if (screen == QLatin1String("blue")) hue = ycc(0, 0, 1);
    else if (screen == QLatin1String("magenta")) hue = ycc(1, 0, 1);
    else if (screen == QLatin1String("other")) {
        const QColor c(current);
        if (!c.isValid()) return {{}, tr("Choose a backdrop colour first.")};
        hue = ycc(float(c.redF()), float(c.greenF()), float(c.blueF()));
    } else hue = ycc(0, 1, 0);
    const float hueLength = std::hypot(hue.cb, hue.cr);
    if (hueLength < 0.02f) return {{}, tr("That colour has no hue to look for.")};
    // At about 480 pixels across: a block average for each.
    const int step = std::max(1, image.width() / 480);
    const int columns = image.width() / step, rows = image.height() / step;
    constexpr int bins = 40;
    constexpr float span = 0.6f;
    std::vector<std::vector<Ycc>> grid(size_t(bins * bins));
    int total = 0, kept = 0;
    for (int by = 0; by < rows; ++by)
        for (int bx = 0; bx < columns; ++bx) {
            Ycc sum;
            for (int y = by * step; y < (by + 1) * step; ++y)
                for (int x = bx * step; x < (bx + 1) * step; ++x) {
                    const Ycc c = pixelYcc(image, x, y);
                    sum.y += c.y; sum.cb += c.cb; sum.cr += c.cr;
                }
            const float n = float(step * step);
            const Ycc c{sum.y / n, sum.cb / n, sum.cr / n};
            ++total;
            const float length = std::hypot(c.cb, c.cr);
            if (length < 0.06f || (c.cb * hue.cb + c.cr * hue.cr) / (length * hueLength) < 0.766f) continue;
            const int i = std::clamp(int((c.cb + span) / (2 * span) * bins), 0, bins - 1);
            const int j = std::clamp(int((c.cr + span) / (2 * span) * bins), 0, bins - 1);
            grid[size_t(j * bins + i)].push_back(c);
            ++kept;
        }
    if (total == 0 || kept * 33 < total)
        return {{}, tr("No screen of that colour fills enough of the picture.")};
    int fullest = 0;
    for (int k = 1; k < bins * bins; ++k)
        if (grid[size_t(k)].size() > grid[size_t(fullest)].size()) fullest = k;
    std::vector<float> ys, cbs, crs;
    const int fi = fullest % bins, fj = fullest / bins;
    for (int j = std::max(0, fj - 1); j <= std::min(bins - 1, fj + 1); ++j)
        for (int i = std::max(0, fi - 1); i <= std::min(bins - 1, fi + 1); ++i)
            for (const Ycc &c : grid[size_t(j * bins + i)]) { ys.push_back(c.y); cbs.push_back(c.cb); crs.push_back(c.cr); }
    const auto median = [](std::vector<float> &v) {
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    return {hex({median(ys), median(cbs), median(crs)}), {}};
}

Backdrop pick(const QImage &picture, double x, double y) {
    if (picture.isNull()) return {{}, tr("There is no picture to look at.")};
    const QImage image = linear(picture);
    const int cx = std::clamp(int(x * image.width()), 0, image.width() - 1);
    const int cy = std::clamp(int(y * image.height()), 0, image.height() - 1);
    const int radius = std::max(2, image.width() / 400);
    Ycc sum;
    int n = 0;
    for (int py = std::max(0, cy - radius); py <= std::min(image.height() - 1, cy + radius); ++py)
        for (int px = std::max(0, cx - radius); px <= std::min(image.width() - 1, cx + radius); ++px) {
            const Ycc c = pixelYcc(image, px, py);
            sum.y += c.y; sum.cb += c.cb; sum.cr += c.cr;
            ++n;
        }
    const Ycc c{sum.y / float(n), sum.cb / float(n), sum.cr / float(n)};
    if (std::hypot(c.cb, c.cr) < 0.03f) return {{}, tr("That spot is grey: pick the screen itself.")};
    return {hex(c), {}};
}

}  // namespace Keyer
