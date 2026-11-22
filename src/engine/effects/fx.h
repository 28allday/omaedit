#pragma once

// The CPU twins of the effect shaders (shaders/fx_*.comp), with the helpers
// fx_common.glsl gives them, word for word.

#include "frame.h"

#include <algorithm>
#include <cmath>

namespace engine::fx {

// One pass of an effect: `in` is this pass's input, `aux` the step's input,
// `out` already sized. Each twin writes every pixel of `out` in [y0, y1).
struct Pass {
    const FrameF &in;
    const FrameF &aux;
    FrameF &out;
    const FilterParams &q;
    int pass = 0, frame = 0;
    float P(int i) const { return q.p[i]; }
    // As unit() in fx_common.glsl.
    float unit() const { return q.p[23] > 0.f ? q.p[23] : 1.f; }
};

inline void at(const FrameF &f, int x, int y, float o[4]) {
    const float *c = f.px(std::clamp(x, 0, f.width - 1), std::clamp(y, 0, f.height - 1));
    std::copy_n(c, 4, o);
}
// As sampleAt in fx_common.glsl.
inline void sampleAt(const FrameF &f, float qx, float qy, float o[4]) {
    const float fx = qx - 0.5f, fy = qy - 0.5f;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = fx - float(ix), ty = fy - float(iy);
    float s[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            const int cx = ix + k, cy = iy + j;
            if (cx < 0 || cy < 0 || cx >= f.width || cy >= f.height) continue;
            const float *c = f.px(cx, cy);
            const float w = (k ? tx : 1.f - tx) * (j ? ty : 1.f - ty);
            for (int n = 0; n < 3; ++n) s[n] += c[n] * c[3] * w;
            s[3] += c[3] * w;
        }
    if (s[3] <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    // Colour below zero is kept, as straightSample in common.glsl keeps it.
    for (int n = 0; n < 3; ++n) o[n] = s[n] / s[3];
    o[3] = std::clamp(s[3], 0.f, 1.f);
}
// As gaussLine in fx_common.glsl: associated colour out.
inline void gaussLine(const FrameF &f, float qx, float qy, float dx, float dy, float sigma, float o[4]) {
    auto tap = [&](float x, float y, float v[4]) {
        sampleAt(f, std::clamp(x, 0.5f, f.width - 0.5f), std::clamp(y, 0.5f, f.height - 0.5f), v);
        for (int k = 0; k < 3; ++k) v[k] *= v[3];
    };
    if (sigma < 0.25f) { tap(qx, qy, o); return; }
    const int n = std::min(32, int(std::ceil(3.f * sigma)));
    const float step = 3.f * sigma / float(n);
    float s[4] = {0, 0, 0, 0}, w = 0.f;
    for (int i = -n; i <= n; ++i) {
        const float x = float(i) * step, k = std::exp(-0.5f * (x / sigma) * (x / sigma));
        float v[4];
        tap(qx + dx * x, qy + dy * x, v);
        for (int c = 0; c < 4; ++c) s[c] += k * v[c];
        w += k;
    }
    for (int c = 0; c < 4; ++c) o[c] = s[c] / w;
}
// Associated colour back to straight, as straightSample.
inline void straight(const float s[4], float o[4]) {
    if (s[3] <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    for (int k = 0; k < 3; ++k) o[k] = s[k] / s[3];
    o[3] = std::clamp(s[3], 0.f, 1.f);
}
inline float hash(int x, int y, int seed) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ uint32_t(seed) * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return float(h >> 8) * (1.f / 16777216.f);
}
inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float display(float c) { return clamp01(linearToSrgb(c)); }
inline float light(float e) { return srgbToLinear(clamp01(e)); }

#define EFFECT(Name, stem, passes) void fx_##stem(const Pass &p, int y0, int y1);
#include "effects.def"
#undef EFFECT

}  // namespace engine::fx
