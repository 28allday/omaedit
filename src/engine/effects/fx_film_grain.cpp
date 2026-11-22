#include "fx.h"
namespace engine::fx {
void fx_film_grain(const Pass &p, int y0, int y1) {
    const float flicker = 1.f + (hash(0, 0, p.frame * 3 + 1) - 0.5f) * p.P(3) * 0.4f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float grain = (hash(x, y, p.frame) - 0.5f) * p.P(0) * 0.5f;
            const bool speck = hash(x / 3, y / 3, p.frame * 7 + 5) < p.P(2) * 0.002f;
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) {
                float e = (display(c[k]) + grain) * flicker;
                if (speck) e *= 0.2f;
                o[k] = light(e);
            }
            o[3] = c[3];
        }
}
}
