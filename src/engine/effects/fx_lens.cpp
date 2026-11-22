#include "fx.h"
namespace engine::fx {
void fx_lens(const Pass &p, int y0, int y1) {
    const float w = float(p.in.width), h = float(p.in.height), cx = p.P(2) * w, cy = p.P(3) * h;
    const float scale = 4.f / (w * w + h * h);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float ox = x + 0.5f - cx, oy = y + 0.5f - cy, r2 = (ox * ox + oy * oy) * scale;
            const float m = 1.f + p.P(0) * r2 + p.P(1) * r2 * r2;
            sampleAt(p.in, cx + ox * m, cy + oy * m, p.out.px(x, y));
        }
}
}
