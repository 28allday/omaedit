#pragma once

#include <QImage>
#include <QString>

// The chroma key's backdrop: the screen as filmed, found in a frame of the
// clip as the keyer sees it (linear-light RGBA, the engine's working
// pixels). Colours come back display-encoded ("#rrggbb"), as the keyer's
// Backdrop control holds them.
namespace Keyer {
struct Backdrop {
    QString colour;  // empty when none was found
    QString error;
};
// The screen that fills most of the picture: the commonest colour within
// 40° of `screen`'s hue ("green", "blue", "magenta", or "other" for
// `current`'s), its median.
Backdrop find(const QImage &picture, const QString &screen, const QString &current = {});
// The colour around a point (0..1 across and down), averaged over a small patch.
Backdrop pick(const QImage &picture, double x, double y);
}
