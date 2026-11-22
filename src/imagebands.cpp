#include "imagebands.h"
#include "bands.h"
#include <QColorSpace>
#include <cstring>

namespace Bands {
bool displayThreads() {
    static const bool on = qEnvironmentVariable("OMAEDIT_DISPLAY_THREADS") != QStringLiteral("0");
    return on;
}
int rowsPerBand(int width, int bytesPerPixel) {
    return qBound(1, int((1 << 20) / qMax<qint64>(1, qint64(width) * qMax(1, bytesPerPixel))), 256);
}
void copyMetadata(QImage &to, const QImage &from) {
    to.setDotsPerMeterX(from.dotsPerMeterX());
    to.setDotsPerMeterY(from.dotsPerMeterY());
    to.setOffset(from.offset());
    to.setColorSpace(from.colorSpace());
    for (const auto &key : from.textKeys())
        to.setText(key, from.text(key));
}
namespace {
bool rowIndependent(QImage::Format format) {
    switch (format) {
    case QImage::Format_Invalid:
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB:
    case QImage::Format_Indexed8:
    case QImage::Format_RGB16:
    case QImage::Format_RGB555:
    case QImage::Format_ARGB8555_Premultiplied:
    case QImage::Format_ARGB8565_Premultiplied:
    case QImage::Format_ARGB4444_Premultiplied:
    case QImage::Format_RGB444:
    case QImage::Format_RGB666:
    case QImage::Format_ARGB6666_Premultiplied:
    case QImage::Format_BGR30:
    case QImage::Format_A2BGR30_Premultiplied:
    case QImage::Format_RGB30:
    case QImage::Format_A2RGB30_Premultiplied:
    case QImage::Format_CMYK8888:
        return false;
    default:
        return true;
    }
}
} // namespace
QImage convertToFormat(const QImage &image, QImage::Format format) {
    if (image.isNull() || image.format() == format)
        return image;
    if (!rowIndependent(image.format()) || !rowIndependent(format) || image.colorCount() > 0)
        return image.convertToFormat(format);
    const int width = image.width(), height = image.height();
    const int rows = rowsPerBand(width, qMax(image.depth(), int(QImage::toPixelFormat(format).bitsPerPixel())) / 8);
    const int bands = (height + rows - 1) / rows;
    if (bands < 2 || !displayThreads())
        return image.convertToFormat(format);
    QImage out(width, height, format);
    if (out.isNull())
        return image.convertToFormat(format);
    const qsizetype bytes = qsizetype(width) * out.depth() / 8;
    const bool ok = Bands::run(bands, [&](int index) {
        const int y = index * rows, count = qMin(rows, height - y);
        const QImage view(image.constScanLine(y), width, count, image.bytesPerLine(), image.format());
        const QImage converted = view.convertToFormat(format);
        if (converted.isNull())
            return false;
        for (int r = 0; r < count; ++r)
            std::memcpy(out.scanLine(y + r), converted.constScanLine(r), bytes);
        return true;
    });
    if (!ok)
        return image.convertToFormat(format);
    copyMetadata(out, image);
    return out;
}
} // namespace Bands
