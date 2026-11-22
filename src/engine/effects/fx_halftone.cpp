#include "fx.h"
namespace engine::fx {
static float dotCover(float qx, float qy, float ink, float turn, float cell) {
    const float a = turn * 3.14159265f, ca = std::cos(a), sa = std::sin(a);
    const float rx = ca * qx + sa * qy, ry = -sa * qx + ca * qy;
    const float fx = rx / cell - std::floor(rx / cell) - 0.5f, fy = ry / cell - std::floor(ry / cell) - 0.5f;
    const float d = std::sqrt(fx * fx + fy * fy) * cell, radius = std::sqrt(clamp01(ink)) * cell * 0.70710678f;
    return clamp01(radius - d + 0.5f) * std::min(1.f, 2.f * radius);  // as fx_halftone.comp
}
void fx_halftone(const Pass &p, int y0, int y1) {
    const float cell = std::max(2.f, p.P(0) * 0.03f * p.in.height);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float qx = x + 0.5f, qy = y + 0.5f;
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(1.f - dotCover(qx, qy, 1.f - display(c[k]), p.P(1 + k), cell));
            o[3] = c[3];
        }
}
}
