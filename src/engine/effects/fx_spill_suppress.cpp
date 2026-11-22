#include "fx.h"
namespace engine::fx {
void fx_spill_suppress(const Pass &p, int y0, int y1) {
    const bool green = p.P(0) < 0.5f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float e[3] = {display(c[0]), display(c[1]), display(c[2])};
            if (green) e[1] = std::min(e[1], std::max(e[0], e[2]));
            else e[2] = std::min(e[2], std::max(e[0], e[1]));
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(e[k]);
            o[3] = c[3];
        }
}
}
