#include "fx.h"
namespace engine::fx {
void fx_glow(const Pass &p, int y0, int y1) {
    const float sigma = p.P(0) * 0.05f * p.in.height;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            if (p.pass < 2) {
                float s[4];
                gaussLine(p.in, x + 0.5f, y + 0.5f, p.pass == 0 ? 1.f : 0.f, p.pass == 0 ? 0.f : 1.f, sigma, s);
                straight(s, o);
                continue;
            }
            const float *c = p.aux.px(x, y), *g = p.in.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(1.f - (1.f - display(c[k])) * (1.f - display(g[k])));
            o[3] = c[3];
        }
}
}
