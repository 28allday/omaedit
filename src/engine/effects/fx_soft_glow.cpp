#include "fx.h"
namespace engine::fx {
void fx_soft_glow(const Pass &p, int y0, int y1) {
    const float sigma = p.P(0) * 0.04f * p.in.height;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            if (p.pass == 0) {
                float c[4];
                at(p.in, x, y, c);
                const float e[3] = {display(c[0]), display(c[1]), display(c[2])};
                const float l = 0.2126f * e[0] + 0.7152f * e[1] + 0.0722f * e[2];
                const float t = clamp01((l - (1.f - p.P(2))) / std::max(p.P(2), 0.01f));
                for (int k = 0; k < 3; ++k) o[k] = light(e[k] * t);
                o[3] = c[3];
                continue;
            }
            if (p.pass < 3) {
                float s[4];
                gaussLine(p.in, x + 0.5f, y + 0.5f, p.pass == 1 ? 1.f : 0.f, p.pass == 1 ? 0.f : 1.f, sigma, s);
                straight(s, o);
                continue;
            }
            const float *c = p.aux.px(x, y), *g = p.in.px(x, y);
            for (int k = 0; k < 3; ++k) {
                const float ge = clamp01(display(g[k]) * p.P(1) * 2.f);
                o[k] = light(1.f - (1.f - display(c[k])) * (1.f - ge));
            }
            o[3] = c[3];
        }
}
}
