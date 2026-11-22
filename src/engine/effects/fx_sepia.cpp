#include "fx.h"
namespace engine::fx {
void fx_sepia(const Pass &p, int y0, int y1) {
    const float pb = (p.P(0) - 128.f) / 255.f, pr = (p.P(1) - 128.f) / 255.f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float l = 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
            const float r = l + 1.5748f * pr, b = l + 1.8556f * pb, g = (l - 0.2126f * r - 0.0722f * b) / 0.7152f;
            float *o = p.out.px(x, y);
            o[0] = light(r); o[1] = light(g); o[2] = light(b);
            o[3] = c[3];
        }
}
}
