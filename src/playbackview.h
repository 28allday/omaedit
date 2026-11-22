#pragma once
#include <QImage>
#include <QRectF>
#include <QVariantMap>

namespace PlaybackView {
// Mid-dark neutral grey: a black picture edge still shows against it.
inline constexpr const char *defaultCanvas = "#333333";
QVariantMap normalized(const QVariantMap &settings);
QRectF imageRect(const QSizeF &viewport, const QSize &nativeSize, double pixelAspect,
                 const QVariantMap &settings, double devicePixelRatio = 1.);
QImage preview(const QImage &frame, int divisor);
// A picture's size at a playback resolution of 1/divisor (even, at least 2).
QSize reduced(const QSize &whole, int divisor);
}
