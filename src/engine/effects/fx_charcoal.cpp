#include "fx.h"
namespace engine::fx {
static float lumaAt(const FrameF &f, int x, int y) {
    float c[4];
    at(f, x, y, c);
    return 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
}
void fx_charcoal(const Pass &p, int y0, int y1) {
    const int sx = std::max(1, int(p.P(0) * p.unit() + 0.5f)), sy = std::max(1, int(p.P(1) * p.unit() + 0.5f));
    const float colour = clamp01(p.P(3));
    // As fx_charcoal.comp: a reach rounded up at a smaller size, scaled back.
    const float fx = std::min(1.f, std::max(1.f, std::floor(p.P(0) + 0.5f)) * p.unit() / float(sx));
    const float fy = std::min(1.f, std::max(1.f, std::floor(p.P(1) + 0.5f)) * p.unit() / float(sy));
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float gx = (lumaAt(p.in, x + sx, y) - lumaAt(p.in, x - sx, y)) * fx,
                        gy = (lumaAt(p.in, x, y + sy) - lumaAt(p.in, x, y - sy)) * fy;
            float v = 1.f - clamp01(std::sqrt(gx * gx + gy * gy) * p.P(2));
            if (p.P(4) > 0.5f) v = 1.f - v;
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(v + (display(c[k]) * v - v) * colour);
            o[3] = c[3];
        }
}
}
