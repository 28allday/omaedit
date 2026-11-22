#pragma once

// How an export of another shape takes the timeline's picture: fitted whole
// with black bars, filled with the edges cropped, or fitted over a blurred,
// dimmed copy of itself that fills the frame. The engine's renderer and the
// colour-managed renderer on the CPU place the picture by the same numbers.

#include <QString>
#include <algorithm>

namespace render {

enum class Framing { Fit, Fill, Blur };

inline Framing framingFrom(const QString &key) {
    if (key == QLatin1String("fill")) return Framing::Fill;
    if (key == QLatin1String("blur")) return Framing::Blur;
    return Framing::Fit;
}

// The backdrop is drawn this many times smaller than the frame, blurred by
// kBackdropRadius of its own pixels and shown at kBackdropOpacity over black.
constexpr int kBackdropDivisor = 8;
constexpr float kBackdropRadius = 12.f;
constexpr float kBackdropOpacity = 0.5f;

// Where a picture whose shape (display width over height) is `aspect` lands
// in a frame w × h: the box it covers and the part of it shown (u, v from 0
// to 1). Fit shows all of it inside the frame; fill covers the frame and
// shows the middle of it.
struct Placement {
    double x = 0, y = 0, w = 0, h = 0;
    double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};
inline Placement placeIn(double aspect, int w, int h, bool fill) {
    Placement p;
    if (aspect <= 0 || w <= 0 || h <= 0) return p;
    const double frame = double(w) / h;
    if (fill) {
        p.w = w;
        p.h = h;
        if (aspect > frame) {  // wider: the sides go
            const double shown = frame / aspect;
            p.u0 = (1 - shown) / 2;
            p.u1 = 1 - p.u0;
        } else {  // taller: the top and bottom go
            const double shown = aspect / frame;
            p.v0 = (1 - shown) / 2;
            p.v1 = 1 - p.v0;
        }
        return p;
    }
    if (aspect > frame) {
        p.w = w;
        p.h = w / aspect;
        p.y = (h - p.h) / 2;
    } else {
        p.h = h;
        p.w = h * aspect;
        p.x = (w - p.w) / 2;
    }
    return p;
}

inline int backdropSide(int side) { return std::max(16, side / kBackdropDivisor); }

// How a clip whose size or shape differs from the timeline's frame takes it:
// fitted whole (bars where the shapes differ), filling the frame (its edges
// past the frame unseen), stretched to the frame's corners, or at its own
// pixel size. The timeline chooses for every clip; a clip may choose for
// itself. A clip of the frame's own size and shape looks the same in all of
// them but Original size, where it is also unchanged.
enum class Scaling { Fit, Fill, Stretch, Original };

inline Scaling scalingFrom(const QString &key) {
    if (key == QLatin1String("fill")) return Scaling::Fill;
    if (key == QLatin1String("stretch")) return Scaling::Stretch;
    if (key == QLatin1String("original")) return Scaling::Original;
    return Scaling::Fit;
}
inline bool isScalingKey(const QString &key) {
    return key == QLatin1String("fit") || key == QLatin1String("fill") || key == QLatin1String("stretch")
        || key == QLatin1String("original");
}
// A clip's own choice when it has one, else the timeline's.
inline Scaling scalingFor(const QString &clip, const QString &timeline) {
    return scalingFrom(isScalingKey(clip) ? clip : timeline);
}

// The horizontal and vertical scale taking a picture of sw × sh pixels, whose
// pixels are `ratio` times as wide as the frame's, into a w × h frame. It is
// centred there.
struct Scale { double x = 1, y = 1; };
inline Scale scaleInto(Scaling scaling, double sw, double sh, double ratio, double w, double h) {
    if (sw <= 0 || sh <= 0 || ratio <= 0) return {};
    switch (scaling) {
    case Scaling::Stretch: return {w / sw, h / sh};
    case Scaling::Original: return {ratio, 1};
    case Scaling::Fill: {
        const double f = std::max(w / (sw * ratio), h / sh);
        return {f * ratio, f};
    }
    case Scaling::Fit: break;
    }
    const double f = std::min(w / (sw * ratio), h / sh);
    return {f * ratio, f};
}

}  // namespace render
