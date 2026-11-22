#include "fx.h"
namespace engine::fx {
void fx_scratches(const Pass &p, int y0, int y1) {
    const int count = int(std::min(p.P(1), 100.f) + 0.5f);
    const float width = std::max(0.5f, p.P(0) * float(p.in.height) / 1080.f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float e[3] = {display(c[0]), display(c[1]), display(c[2])};
            const float fx = x + 0.5f;
            for (int i = 0; i < count; ++i) {
                const float ax = hash(p.frame / 2, i, 23) * float(p.in.width);
                const float cover = clamp01(width * 0.5f - std::fabs(fx - ax) + 0.5f) * (0.4f + 0.6f * hash(y / 8, i, p.frame + 29));
                const bool lighten = hash(p.frame / 2, i, 31) < 0.5f;
                for (int k = 0; k < 3; ++k) e[k] = lighten ? e[k] + (1.f - e[k]) * cover * 0.5f : e[k] * (1.f - cover * 0.6f);
            }
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(e[k]);
            o[3] = c[3];
        }
}
}
