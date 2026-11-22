#include "fx.h"
namespace engine::fx {
void fx_letterbox(const Pass &p, int y0, int y1) {
    const float bar = clamp01(p.P(0)) * float(p.in.height) * 0.5f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            const float fy = y + 0.5f;
            if (fy >= bar && fy <= float(p.in.height) - bar) { at(p.in, x, y, o); continue; }
            o[0] = o[1] = o[2] = 0.f;
            o[3] = p.P(1) > 0.5f ? 0.f : 1.f;
        }
}
}
