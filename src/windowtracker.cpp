#include "windowtracker.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QImage trackingImage(const QImage &frame) {
    return frame.format()==QImage::Format_RGBA32FPx4?frame:frame.convertToFormat(QImage::Format_Grayscale8);
}
double luminance(const QImage &image,int x,int y) {
    if(image.format()!=QImage::Format_RGBA32FPx4)return image.constScanLine(y)[x];
    const auto *p=reinterpret_cast<const float *>(image.constScanLine(y))+4*x;
    return 255*(.2126*p[0]+.7152*p[1]+.0722*p[2]);
}
}

bool WindowTracker::start(const QImage &frame, const QRectF &window) {
    m_error.clear();
    m_samples.clear();
    const QImage gray = trackingImage(frame);
    if (gray.isNull()) {
        m_error = QStringLiteral("Could not decode the tracking frame.");
        return false;
    }
    m_size = gray.size();
    const QRectF box(window.x() * gray.width(), window.y() * gray.height(), window.width() * gray.width(),
                     window.height() * gray.height());
    const int padding = std::max(4, int(std::min(box.width(), box.height()) * .2));
    const QRect region = box.adjusted(-padding, -padding, padding, padding).toAlignedRect().intersected(gray.rect());
    if (region.width() < 8 || region.height() < 8) {
        m_error = QStringLiteral("Choose a larger window inside the image.");
        return false;
    }
    m_patch = region.size();
    m_position = region.topLeft();
    m_offset = box.center() - QPointF(m_position);
    double sum = 0;
    const int columns = std::min(20, region.width()), rows = std::min(20, region.height());
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x) {
            const int dx = x * (region.width() - 1) / std::max(1, columns - 1),
                      dy = y * (region.height() - 1) / std::max(1, rows - 1);
            const double v = luminance(gray,region.x()+dx,region.y()+dy);
            m_samples.push_back({dx, dy, v});
            sum += v;
        }
    const double mean = sum / m_samples.size();
    m_variance = 0;
    for (Sample &s : m_samples) {
        s.value -= mean;
        m_variance += s.value * s.value;
    }
    if (m_variance / m_samples.size() < 4) {
        m_error = QStringLiteral("The window has too little detail to track. Include a visible edge or texture.");
        return false;
    }
    return true;
}

bool WindowTracker::next(const QImage &frame, QPointF *centre, double *confidence) {
    const QImage gray = trackingImage(frame);
    if (gray.size() != m_size || m_samples.empty()) {
        m_error = QStringLiteral("The tracking frame changed size.");
        return false;
    }
    const int radius = std::clamp(gray.width() / 10, 12, 64);
    const QRect search(QPoint(std::max(0, m_position.x() - radius), std::max(0, m_position.y() - radius)),
                       QPoint(std::min(gray.width() - m_patch.width(), m_position.x() + radius),
                              std::min(gray.height() - m_patch.height(), m_position.y() + radius)));
    auto score = [&](int x, int y) {
        double sum = 0, square = 0, cross = 0;
        for (const Sample &s : m_samples) {
            const double b = luminance(gray,x+s.x,y+s.y);
            sum += b;
            square += b * b;
            cross += s.value * b;
        }
        const double variance = square - sum * sum / m_samples.size();
        return variance > 1e-6 ? cross / std::sqrt(m_variance * variance) : -1.;
    };
    QPoint best = m_position;
    double highest = -2;
    auto consider = [&](int x, int y) {
        const double s = score(x, y);
        const int distance = std::abs(x - m_position.x()) + std::abs(y - m_position.y());
        const int oldDistance = std::abs(best.x() - m_position.x()) + std::abs(best.y() - m_position.y());
        if (s > highest + 1e-8 || (std::abs(s - highest) <= 1e-8 && distance < oldDistance)) {
            highest = s;
            best = QPoint(x, y);
        }
    };
    consider(m_position.x(), m_position.y());
    for (int y = search.top(); y <= search.bottom(); y += 2)
        for (int x = search.left(); x <= search.right(); x += 2)
            consider(x, y);
    const QPoint coarse = best;
    for (int y = std::max(search.top(), coarse.y() - 2); y <= std::min(search.bottom(), coarse.y() + 2); ++y)
        for (int x = std::max(search.left(), coarse.x() - 2); x <= std::min(search.right(), coarse.x() + 2); ++x)
            consider(x, y);
    if (confidence)
        *confidence = highest;
    if (highest < .55) {
        m_error = QStringLiteral("Tracking lost the window. Reposition it and continue from this frame.");
        return false;
    }
    m_position = best;
    *centre = QPointF((best.x() + m_offset.x()) / gray.width(), (best.y() + m_offset.y()) / gray.height());
    return true;
}
