#include "generators.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace render {

QString generatorKind(const QString &path) {
    if (path == QLatin1String("frei0r.test_pat_B")) return QStringLiteral("bars");
    if (path == QLatin1String("noise")) return QStringLiteral("noise");
    if (path == QLatin1String("count")) return QStringLiteral("count");
    return {};
}

namespace {

// 75 % colour bars over a strip of reversed blue bars and a row of the
// set-up steps (with white and the two chroma references) for setting a
// monitor up. A full-range picture: white is 255 and black 0, so the step
// that would be blacker than black is black, and the lighter one 4 % up.
QImage bars(const QSize &size) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    QPainter p(&image);
    const int w = size.width(), h = size.height();
    const int top = h * 2 / 3, mid = top + h / 12;
    static const QColor upper[7] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
                                    {191, 0, 191}, {191, 0, 0}, {0, 0, 191}};
    static const QColor middle[7] = {{0, 0, 191}, {0, 0, 0}, {191, 0, 191}, {0, 0, 0},
                                     {0, 191, 191}, {0, 0, 0}, {191, 191, 191}};
    for (int i = 0; i < 7; ++i) {
        const int x0 = w * i / 7, x1 = w * (i + 1) / 7;
        p.fillRect(x0, 0, x1 - x0, top, upper[i]);
        p.fillRect(x0, top, x1 - x0, mid - top, middle[i]);
    }
    // -I, white, +Q, black, then the three pluge steps, black.
    const int x5 = w * 5 / 7, x6 = w * 6 / 7, step = (x6 - x5) / 3;
    struct Patch { int x0, x1; QColor c; };
    const Patch lower[] = {{0, w * 5 / 28, {0, 33, 76}}, {w * 5 / 28, w * 10 / 28, {255, 255, 255}},
                           {w * 10 / 28, w * 15 / 28, {50, 0, 106}}, {w * 15 / 28, x5, {0, 0, 0}},
                           {x5, x5 + step, {0, 0, 0}}, {x5 + step, x5 + 2 * step, {0, 0, 0}},
                           {x5 + 2 * step, x6, {10, 10, 10}}, {x6, w, {0, 0, 0}}};
    for (const Patch &patch : lower) p.fillRect(patch.x0, mid, patch.x1 - patch.x0, h - mid, patch.c);
    return image;
}

// Grey noise, different every frame and the same whenever that frame is drawn.
QImage noise(const QSize &size, int frame) {
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uint32_t v = uint32_t(x) * 0x9e3779b1u ^ uint32_t(y) * 0x85ebca6bu ^ uint32_t(frame) * 0xc2b2ae35u;
            v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
            const uchar g = uchar(v & 255);
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = g;
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

// A countdown: the seconds left, large, in a ring whose sweep shows how much
// of the current second is gone.
QImage count(const QSize &size, int local, int length, double fps) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(32, 32, 32));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const double remaining = std::max(0.0, (length - local) / std::max(1.0, fps));
    const int seconds = int(std::ceil(remaining - 1e-9));
    const double part = remaining - std::floor(remaining - 1e-9);  // of the current second, 1 down to 0
    const QPointF centre(size.width() / 2.0, size.height() / 2.0);
    const double radius = std::min(size.width(), size.height()) * 0.38;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(80, 80, 80));
    QPainterPath sweep;
    sweep.moveTo(centre);
    sweep.arcTo(QRectF(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius), 90, -360.0 * (1.0 - part));
    sweep.closeSubpath();
    p.drawPath(sweep);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(220, 220, 220), std::max(2.0, radius / 30)));
    p.drawEllipse(centre, radius, radius);
    p.drawLine(QPointF(centre.x() - radius * 1.2, centre.y()), QPointF(centre.x() + radius * 1.2, centre.y()));
    p.drawLine(QPointF(centre.x(), centre.y() - radius * 1.2), QPointF(centre.x(), centre.y() + radius * 1.2));
    QFont font;
    font.setPixelSize(int(radius * 1.1));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size.width(), size.height()), Qt::AlignCenter, QString::number(seconds));
    return image;
}

}  // namespace

QImage generatorFrame(const QString &kind, const QSize &size, int local, int length, double fps, bool *moves) {
    *moves = kind != QLatin1String("bars");
    if (kind == QLatin1String("bars")) return bars(size);
    if (kind == QLatin1String("noise")) return noise(size, local);
    if (kind == QLatin1String("count")) return count(size, local, length, fps);
    return {};
}

}  // namespace render
