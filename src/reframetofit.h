#pragma once
#include <QImage>
#include <QList>
#include <QPointF>

// Reframe to Fit: refits a picture to a frame of another shape (a 16:9 shot in
// a 9:16 timeline) by filling the frame and following what is interesting in
// it. Pure functions; the backend samples the frames and writes the result.
namespace ReframeToFit {

// Where the eye goes in a picture, as fractions (0..1) of its width and
// height: what stands apart from its surroundings on a small greyscale copy
// (centre against surround), weighted towards the strongest. The middle when
// nothing stands out.
QPointF interest(const QImage &picture);

// A path of interest points made to glide: one-off jumps dropped (a median
// over `window` samples), then averaged over the same span.
QList<QPointF> smooth(const QList<QPointF> &path, int window = 5);

// The clip transform (rectX/rectY/rectW/rectH, as the renderer reads them:
// the picture fitted to the frame, then scaled and moved) that fills a frame
// of display aspect `frameAspect` with a picture of `sourceAspect` and puts
// `focus` (fractions of the picture) as near the frame's middle as it can
// go without showing an edge.
struct Placement {
    double x = 0, y = 0, scale = 1;
};
Placement place(double sourceAspect, double frameAspect, QPointF focus);

}  // namespace ReframeToFit
