#include "fx.h"
namespace engine::fx {
void fx_luma_band(const Pass &p, int y0, int y1) {
    const float lo = p.P(0), hi = p.P(1), soft = std::max(p.P(2), 4.f / 255.f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float l = 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
            // The nearer edge's ramp (the two may cross, when the softness is
            // wider than the band).
            const float keep = std::max(clamp01((lo - l) / soft), clamp01((l - hi) / soft));
            float *o = p.out.px(x, y);
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3] * keep;
        }
}
}
