#pragma once
#include <QImage>
#include <QVariantMap>
#include <array>
#include <memory>
#include <vector>

// The primary corrections of a display-referred node, in floating point:
// lift, gamma, gain with white balance and tint, contrast about the pivot,
// offset, exposure, levels, RGB curves, hue and saturation, then mid
// detail. A 16-bit picture keeps every code a 10- or 12-bit source has.
namespace DisplayGrade {
// Corrects a Format_RGBA64 (or Format_RGBA8888) picture in place; alpha is
// left alone. The node is one already evaluated at its frame
// (GradeEngine::evaluateNode).
void correct(QImage &image, const QVariantMap &node);
// Whether correct() changes anything for this node.
bool active(const QVariantMap &node);
// What correct() applies to each pixel, for a renderer that does it itself:
// each channel's table over every 16-bit code (shared, kept for the last few
// settings), then the colour matrix (row-major), then mid detail, which works
// on the picture's neighbourhood and so is only reported here.
struct Prepared {
    std::shared_ptr<const std::array<std::vector<float>, 3>> tables;  // null when nothing tonal changes
    std::array<double, 9> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool chroma = false;
    double detail = 0;
};
Prepared prepare(const QVariantMap &node);
// The smooth RGB curve through these points: a monotone cubic, so a curve
// whose points rise never turns back and none overshoots a point, clamped
// to 0..1; level beyond the first and last points, as the editor draws it.
struct Curve {
    std::vector<double> x, y, slope;
    bool identity = true;
    explicit Curve(const QVariantList &points = {});
    double at(double v) const;
};
} // namespace DisplayGrade
