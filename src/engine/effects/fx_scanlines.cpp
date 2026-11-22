#include "fx.h"
namespace engine::fx {
void fx_scanlines(const Pass &p, int y0, int y1) {
    const int pitch = std::max(2, p.in.height / 270);
    for (int y = y0; y < y1; ++y) {
        const bool dark = (y / (pitch / 2)) % 2 == 1;
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = dark ? c[k] * 0.25f : c[k];
            o[3] = c[3];
        }
    }
}
}
