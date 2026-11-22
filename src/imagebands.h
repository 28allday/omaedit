#pragma once
#include <QImage>
namespace Bands {
// QImage::convertToFormat split into row bands across the pool, with the
// same pixels. Conversions that are not row-independent (indexed and
// one-bit formats, colour tables) go to Qt unchanged.
QImage convertToFormat(const QImage &image, QImage::Format format);
// Rows per band so that one band of pixels stays cache resident.
int rowsPerBand(int width, int bytesPerPixel);
// OMAEDIT_DISPLAY_THREADS=0 keeps every display band on the calling thread.
bool displayThreads();
// Everything QImage::convertToFormat carries across besides pixels.
void copyMetadata(QImage &to, const QImage &from);
} // namespace Bands
