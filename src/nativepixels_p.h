#pragma once
#include <QImage>
#include <QVariantMap>

struct AVFrame;

// Internal decoded-frame conversion; the caller retains ownership of the frame.
namespace NativeVideoPixels {
QImage pixels(const AVFrame *frame, const QVariantMap &meta, QString *error);
}
