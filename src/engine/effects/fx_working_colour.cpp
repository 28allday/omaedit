#include "fx.h"
namespace engine::fx {
void fx_working_colour(const Pass &p, int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = ((lum + p.P(2) * (c[k] - lum)) - 0.5f) * p.P(1) + 0.5f + p.P(0);
            o[3] = c[3];
        }
}
}
