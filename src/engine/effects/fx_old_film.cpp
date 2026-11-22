#include "fx.h"
namespace engine::fx {
void fx_old_film(const Pass &p, int y0, int y1) {
    float jump = 0.f;
    if (hash(p.frame, 1, 3) * 100.f < p.P(1))
        jump = std::floor((hash(p.frame, 2, 3) * 2.f - 1.f) * p.P(0) * float(p.in.height) / 1080.f + 0.5f);
    const float flicker = 1.f + (hash(p.frame, 4, 3) - 0.5f) * 0.08f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y + int(jump), c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(display(c[k]) * flicker);
            o[3] = c[3];
        }
}
}
