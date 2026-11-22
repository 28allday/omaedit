#pragma once

// The pictures of generated clips, drawn on the CPU at the frame's size:
// colour bars, noise and a countdown. (Solid colours and titles are drawn by
// the timeline renderer itself.)

#include <QImage>
#include <QSize>
#include <QString>

namespace render {
// The generator a clip's source names ("bars", "noise", "count"), or empty.
QString generatorKind(const QString &path);
// Its picture at the clip's frame `local` of `length`, at `fps`; whether it
// changes from frame to frame is `moves`.
QImage generatorFrame(const QString &kind, const QSize &size, int local, int length, double fps, bool *moves);
}
