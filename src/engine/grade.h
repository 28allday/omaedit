#pragma once

// A colour-managed grade as the engine runs it. The editor compiles a grade's
// node graph (render/gradeplan) into a GradePlan: operations over pictures
// the step holds, numbered from 0 (register 0 is the step's input), the last
// operation's target being what the step shows. The arithmetic is the
// colour-managed grade's (LinearGrade's corrections, GradeEngine's masks and
// mixes on working frames); grade.comp does the same per pixel on the GPU,
// and the CPU twin uses the functions here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine {

struct ColourProgram;

// Where things sit in a node's parameter table (GradeOp::table for Correct).
namespace node {
constexpr int Lift = 0, Gamma = 3, Gain = 6, Offset = 9, Contrast = 12, Pivot = 13, Exposure = 14, Black = 15,
              White = 16, Angle = 17, Saturation = 18, Flags = 19, Pushes = 20, WarpCount = 44, Warps = 45;
constexpr int MaxWarps = 64, MaxPoints = 64;
constexpr int Curves = Warps + 4 * MaxWarps;  // nine curves: a count, then x, y pairs
constexpr int CurveStride = 1 + 2 * MaxPoints;
constexpr int Size = Curves + 9 * CurveStride;
enum Curve { Master, Red, Green, Blue, HueHue, HueSat, HueLum, Luma, Sat };
enum Flag { Chroma = 1, Hdr = 2 };
}  // namespace node

// Where things sit in a mask's table (GradeOp::table for Mask).
namespace mask {
constexpr int Qualifier = 0, Rgb = 1, Invert = 2, Picked = 3 /* r g b */, PickedHsl = 6 /* h s l */,
              Bounds = 9 /* h s l */, Softness = 12, WindowCount = 13, Windows = 14;
constexpr int MaxWindows = 8, WindowStride = 12, MaxPolygon = 32;
// Per window: shape, combine, cx, cy, cos, sin, xScale, yScale, softness, invert, polygon count, polygon start.
constexpr int Polygons = Windows + MaxWindows * WindowStride;  // x, y pairs
constexpr int Size = Polygons + MaxWindows * MaxPolygon * 2;
enum Shape { Ellipse, Rectangle, Diamond, Gradient, Polygon };
enum Combine { Intersect, Add, Subtract };
}  // namespace mask

struct GradeOp {
    // Correct: a node's corrections (table). Box: `a`'s RGB box-blurred by
    // `amount` pixels along `channel` (0 across, 1 down), edges extended.
    // Detail: mid detail of `amount` on `a`, `b` being `a` box-blurred.
    // Program: an OCIO program (a node LUT), mixed in by `amount`. Mask: the
    // node's qualifier and windows on `a`'s picture into the target's red.
    // Grow, Blur: a mask (red) grown (amount > 0) or shrunk, or box-blurred,
    // by `amount` pixels along `channel` (0 across, 1 down). Key: `a`'s mask
    // times `b`'s (when b >= 0; a < 0 keys everything), inverted when
    // channel is 1, times amount, clamped. Mix: `a` towards `b` by mask register `mask`'s red times
    // amount (amount alone when mask < 0), all four channels. Sum: `a` + `b`
    // in RGB, alpha from register `mask`. Split: only `channel`'s light kept.
    // Copy, Clear (transparent). MatteView: `a`'s mask as an opaque grey
    // (a < 0: white, everything keyed). People: the people found in `a`
    // (matting.h, as RemoveBackground) into the target's red, at quality
    // `channel`, `amount` its tighten; white when no model can run.
    enum class Kind { Correct, Box, Detail, Program, Mask, Grow, Blur, Key, Mix, Sum, Split, Copy, Clear, MatteView, People };
    Kind kind = Kind::Copy;
    int target = 0, a = 0, b = -1, mask = -1;
    std::shared_ptr<const std::vector<float>> table;
    std::shared_ptr<const ColourProgram> program;
    float amount = 1.f;
    int channel = 0;
    bool operator==(const GradeOp &) const = default;
};
struct GradePlan {
    std::vector<GradeOp> ops;
    int registers = 1;
    bool operator==(const GradePlan &) const = default;
};

namespace grade {

inline float unit(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float signedPower(float v, float p) { return std::copysign(std::pow(std::abs(v), p), v); }
// A curve's value at x: straight between points, the end segments' slopes
// carried on beyond them (scene values are not clipped); the fallback when
// it has no points.
inline float curveAt(const float *t, int curve, float x, float fallback) {
    const float *c = t + node::Curves + curve * node::CurveStride;
    const int n = int(c[0]);
    if (n <= 0) return fallback;
    if (n == 1) return c[2];
    int right = 1;
    while (right < n - 1 && c[1 + right * 2] < x) ++right;
    const float ax = c[1 + (right - 1) * 2], ay = c[2 + (right - 1) * 2], bx = c[1 + right * 2], by = c[2 + right * 2];
    if (bx <= ax) return by;
    return ay + (x - ax) * (by - ay) / (bx - ax);
}
inline float luminance(const float *rgb) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }
// The node's corrections on one pixel's working RGB, as LinearGrade's
// general path (its affine shortcut is the same maths).
inline void correct(const float *t, float rgb[3]) {
    const float black = t[node::Black], white = t[node::White];
    for (int c = 0; c < 3; ++c) {
        float v = rgb[c];
        v += t[node::Lift + c] * (1.f - v);
        if (t[node::Gamma + c] != 1.f) v = signedPower(v, t[node::Gamma + c]);
        v *= t[node::Gain + c];
        v = (v - t[node::Pivot]) * t[node::Contrast] + t[node::Pivot] + t[node::Offset + c];
        v *= t[node::Exposure];
        v = (v - std::max(0.f, black)) / std::max(0.01f, 1.f - std::max(0.f, white) - std::max(0.f, black));
        v = v * (1.f + std::min(0.f, white) - std::max(0.f, -black)) + std::max(0.f, -black);
        rgb[c] = curveAt(t, node::Master, curveAt(t, node::Red + c, v, v), v);
    }
    const int flags = int(t[node::Flags]);
    const float lum = luminance(rgb);
    if (flags & node::Chroma) {
        // Hue and saturation where the colour looks: display-like values
        // (a 2.4 power), the editors' colour-wheel hue, 0..1 saturation.
        float d[3];
        for (int c = 0; c < 3; ++c) d[c] = signedPower(rgb[c], 1.f / 2.4f);
        const float high = std::max({d[0], d[1], d[2]}), low = std::min({d[0], d[1], d[2]}), range = high - low;
        const float saturation = high > 1e-8f ? std::min(1.f, range / high) : 0.f;
        float hue = 0.f;
        if (range > 0.f) {
            float h = high == d[0] ? (d[1] - d[2]) / range : high == d[1] ? 2.f + (d[2] - d[0]) / range : 4.f + (d[0] - d[1]) / range;
            h /= 6.f;
            hue = h - std::floor(h);
        }
        const float s = std::clamp((range - 0.03f) / 0.07f, 0.f, 1.f), amount = s * s * (3.f - 2.f * s);
        float angle = t[node::Angle] + amount * (curveAt(t, node::HueHue, hue, 0.5f) - 0.5f) * 2.f * float(M_PI);
        float sat = t[node::Saturation] * std::max(0.f, 1.f + amount * (curveAt(t, node::HueSat, hue, 0.5f) - 0.5f) * 2.f);
        if (saturation > 1e-8f) sat *= std::max(0.f, curveAt(t, node::Sat, saturation, saturation)) / saturation;
        float weights = 0.f, meshHue = 0.f, meshSat = 0.f;
        const int warps = int(t[node::WarpCount]);
        for (int i = 0; i < warps; ++i) {
            const float *w = t + node::Warps + i * 4;
            float dh = std::abs(hue - w[0]);
            dh = std::min(dh, 1.f - dh);
            const float weight = std::max(0.f, 1.f - dh * 12.f) * std::max(0.f, 1.f - std::abs(saturation - w[1]) * 3.f);
            weights += weight;
            meshHue += weight * w[2];
            meshSat += weight * w[3];
        }
        const float scale = amount / std::max(1.f, weights);
        angle += meshHue * scale * 2.f * float(M_PI);
        sat *= std::max(0.f, 1.f + meshSat * scale);
        // Chroma turned about neutral in a luminance / chroma basis.
        const float newLuma = curveAt(t, node::Luma, lum, lum) + amount * (curveAt(t, node::HueLum, hue, 0.5f) - 0.5f);
        const float y = luminance(rgb), u = rgb[2] - y, v = rgb[0] - y, co = std::cos(angle), si = std::sin(angle);
        rgb[2] = newLuma + sat * (u * co - v * si);
        rgb[0] = newLuma + sat * (u * si + v * co);
        rgb[1] = (newLuma - 0.2126f * rgb[0] - 0.0722f * rgb[2]) / 0.7152f;
    }
    if (flags & node::Hdr) {
        static const float centres[6] = {0.04f, 0.14f, 0.3f, 0.525f, 0.75f, 0.925f};
        float weights = 0.f, push[4] = {};
        for (int z = 0; z < 6; ++z) {
            const float width = z == 0 ? 0.1f : z == 5 ? 0.15f : (centres[z + 1] - centres[z - 1]) * 0.7f;
            const float weight = z == 5 && lum >= centres[z] ? 1.f : std::max(0.f, 1.f - std::abs(lum - centres[z]) / width);
            weights += weight;
            for (int c = 0; c < 4; ++c) push[c] += weight * t[node::Pushes + z * 4 + c];
        }
        if (weights > 0.f)
            for (int c = 0; c < 3; ++c) rgb[c] *= std::exp2((push[0] + push[c + 1]) / weights);
    }
}

// Qt's HSL of a colour (QColor::toHsl on its 0..1 components): hue in turns,
// -1 for a grey.
inline void hsl(const float rgb[3], float *h, float *s, float *l) {
    const float r = unit(rgb[0]), g = unit(rgb[1]), b = unit(rgb[2]);
    const float high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
    *l = (high + low) * 0.5f;
    if (delta <= 1e-6f) { *h = -1.f; *s = 0.f; return; }
    *s = *l < 0.5f ? delta / (high + low) : delta / (2.f - high - low);
    float hue = high == r ? (g - b) / delta : high == g ? 2.f + (b - r) / delta : 4.f + (r - g) / delta;
    hue *= 60.f;
    if (hue < 0.f) hue += 360.f;
    *h = hue / 360.f;
}
inline float smooth(float lo, float hi, float x) {
    if (hi <= lo) return x < lo ? 0.f : 1.f;
    const float t = std::clamp((x - lo) / (hi - lo), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
// One window's coverage at (x, y) in 0..1 picture units.
inline float window(const float *t, int i, float x, float y, float aspect) {
    const float *w = t + mask::Windows + i * mask::WindowStride;
    const int shape = int(w[0]);
    const float dx = (x - w[2]) * aspect, dy = y - w[3];
    const float X = (w[4] * dx - w[5] * dy) / std::max(0.0001f, w[6] * aspect), Y = (w[5] * dx + w[4] * dy) / w[7];
    const float softness = w[8];
    float result;
    if (shape == mask::Gradient) {
        const float s = std::max(0.0001f, softness);
        result = smooth(-s, s, X);
    } else if (shape == mask::Polygon) {
        const int n = int(w[10]);
        const float *p = t + int(w[11]);
        float distance = 1e9f;
        bool inside = false;
        for (int k = 0, j = n - 1; k < n; j = k++) {
            const float ax = p[k * 2], ay = p[k * 2 + 1], bx = p[((k + 1) % n) * 2], by = p[((k + 1) % n) * 2 + 1];
            const float abx = bx - ax, aby = by - ay;
            const float u = unit(((X - ax) * abx + (Y - ay) * aby) / std::max(1e-12f, abx * abx + aby * aby));
            distance = std::min(distance, std::hypot(X - (ax + abx * u), Y - (ay + aby * u)));
            const float jx = p[j * 2], jy = p[j * 2 + 1];
            if ((ay > Y) != (jy > Y) && X < (jx - ax) * (Y - ay) / (jy - ay) + ax) inside = !inside;
        }
        result = inside ? (softness > 0.f ? smooth(0.f, softness, distance) : 1.f) : 0.f;
    } else {
        const float d = shape == mask::Rectangle ? std::max(std::abs(X), std::abs(Y))
                      : shape == mask::Diamond   ? std::abs(X) + std::abs(Y)
                                                 : std::hypot(X, Y);
        result = 1.f - smooth(std::max(0.f, 1.f - softness), 1.f, d);
    }
    return w[9] != 0.f ? 1.f - result : result;
}
// A node's key at a pixel: its qualifier on the picture's colour, times its
// windows, before cleanup (grow, blur) and the key controls.
inline float maskAt(const float *t, const float rgba[4], int x, int y, int w, int h) {
    float key = 1.f;
    if (t[mask::Qualifier] != 0.f) {
        float d[3];
        if (t[mask::Rgb] != 0.f) {
            // The picture's values as they are (a float picture's colour is not clipped to 0..1).
            for (int c = 0; c < 3; ++c) d[c] = std::abs(rgba[c] - t[mask::Picked + c]);
        } else {
            float hue, sat, light;
            hsl(rgba, &hue, &sat, &light);
            const float dh = std::abs(std::max(hue, -1.f) - t[mask::PickedHsl]);
            d[0] = std::min(dh, 1.f - dh);
            d[1] = std::abs(sat - t[mask::PickedHsl + 1]);
            d[2] = std::abs(light - t[mask::PickedHsl + 2]);
        }
        for (int c = 0; c < 3; ++c) key *= 1.f - smooth(t[mask::Bounds + c], t[mask::Bounds + c] + t[mask::Softness], d[c]);
        if (t[mask::Invert] != 0.f) key = 1.f - key;
    }
    const int windows = int(t[mask::WindowCount]);
    float combined = 1.f;
    const float aspect = float(w) / float(h);
    for (int i = 0; i < windows; ++i) {
        const float a = window(t, i, (x + 0.5f) / w, (y + 0.5f) / h, aspect);
        const int combine = int(t[mask::Windows + i * mask::WindowStride + 1]);
        combined = i == 0 ? a : combine == mask::Add ? std::max(combined, a) : combine == mask::Subtract ? combined * (1.f - a) : combined * a;
    }
    return unit(key * combined);
}

}  // namespace grade
}  // namespace engine
