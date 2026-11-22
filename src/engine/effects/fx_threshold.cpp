#include "fx.h"
namespace engine::fx {
void fx_threshold(const Pass &p, int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float l = 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
            float *o = p.out.px(x, y);
            o[0] = o[1] = o[2] = l >= p.P(0) ? 1.f : 0.f;
            o[3] = c[3];
        }
}
}
