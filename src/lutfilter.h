#pragma once

#include <QImage>
#include <QString>

// A 3D LUT through FFmpeg's transform, blended by intensity, alpha kept.
namespace LutFilter {
// `image` becomes 16-bit RGBA with the LUT applied.
bool apply(QImage &image, const QString &path, double intensity, QString *error = nullptr);
QString error(const QString &path);
void clearValidationCache();
}
