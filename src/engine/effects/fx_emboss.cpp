#include "fx.h"
namespace engine::fx {
static float lumaAt(const FrameF &f, int x, int y) {
    float c[4];
    at(f, x, y, c);
    return 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
}
void fx_emboss(const Pass &p, int y0, int y1) {
    const float az = p.P(0) * 6.28318531f, el = clamp01(p.P(1)) * 1.57079633f;
    const float lx = std::cos(el) * std::cos(az), ly = std::cos(el) * std::sin(az), lz = std::sin(el);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float gx = lumaAt(p.in, x + 1, y) - lumaAt(p.in, x - 1, y), gy = lumaAt(p.in, x, y + 1) - lumaAt(p.in, x, y - 1);
            float nx = -gx * p.P(2) * 8.f * p.unit(), ny = -gy * p.P(2) * 8.f * p.unit(), nz = 1.f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            const float v = clamp01(0.5f + (nx * lx + ny * ly + nz * lz) - lz);
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            o[0] = o[1] = o[2] = light(v);
            o[3] = c[3];
        }
}
}
