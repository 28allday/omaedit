#include "fx.h"
namespace engine::fx {
void fx_posterize(const Pass &p, int y0, int y1) {
    const float n = std::max(2.f, std::floor(p.P(0) + 0.5f)) - 1.f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(std::floor(display(c[k]) * n + 0.5f) / n);
            o[3] = c[3];
        }
}
}
