#pragma once

#include <QImage>
#include <QVariantMap>
#include <atomic>
#include <vector>

#include "engine/frame.h"

// CPU scopes of decoded, full-range output RGB (black 0, white 1).
// Every input pixel is accumulated; output raster size does not resample input.
// Float/16-bit precision and excursions outside 0..1 are retained. Density gain
// uses pixel populations, never a per-frame peak. Runtime work is asynchronous.
//
// Options: intensity 0.2..4; channels rgb/r/g/b/y (waveform/histogram; parade
// always RGB); clamp (0..1 plot, otherwise -0.1..1.1); graticule; width/height;
// outputSpace (rec709/rec709, rec601/rec709, rec2020/*); bitDepth (Float, 8 bit,
// 10 bit, 12 bit). Scale labels express full-range RGB codes, not encoded YUV.
// Samples beyond the plotted range accumulate at the edge in amber.
namespace Scopes {

QImage waveform(const QImage &frame, const QVariantMap &options = {});
QImage parade(const QImage &frame, const QVariantMap &options = {});
QImage vectorscope(const QImage &frame, const QVariantMap &options = {});
QImage histogram(const QImage &frame, const QVariantMap &options = {});

// One shared measurement pass, in waveform/parade/histogram/vectorscope order.
// Rows are counted in parallel pieces (OMAEDIT_SCOPE_THREADS=0 keeps one);
// maximumParts caps that for reference renders.
QList<QImage> renderAll(const QImage &frame, const QVariantMap &options = {},
                       int vectorWidth = 200, const std::atomic<bool> *cancelled = nullptr,
                       int maximumParts = 0);

// The same measurement in two steps, so an engine can do the first on the
// GPU (engine::Engine::countScopes) and only the counts come to memory.
// `layout` is what is counted and where each count lies; `counts` is this
// file's own counting, laid out the same (the engine's pass is its twin and
// must give the same numbers); `renderCounted` paints the four pictures from
// either: renderCounted(counts(frame)) is renderAll(frame).
engine::ScopeLayout layout(const QVariantMap &options = {}, int vectorWidth = 200);
std::vector<quint32> counts(const QImage &frame, const QVariantMap &options = {}, int vectorWidth = 200,
                            const std::atomic<bool> *cancelled = nullptr, int maximumParts = 0);
QList<QImage> renderCounted(const std::vector<quint32> &cells, const QVariantMap &options = {}, int vectorWidth = 200);

// Rec.709 luma of one pixel, 0..255.
inline int luma709(int r, int g, int b) {
    return qBound(0, int(0.2126 * r + 0.7152 * g + 0.0722 * b + 0.5), 255);
}

} // namespace Scopes
