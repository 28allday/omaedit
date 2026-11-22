#include "fx.h"
namespace engine::fx {
void fx_invert(const Pass &p, int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(1.f - display(c[k]));
            o[3] = c[3];
        }
}
}
