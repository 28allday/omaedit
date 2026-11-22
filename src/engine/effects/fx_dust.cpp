#include "fx.h"
namespace engine::fx {
void fx_dust(const Pass &p, int y0, int y1) {
    const int count = int(hash(p.frame, 0, 11) * std::min(p.P(1), 100.f) + 0.5f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            float e[3] = {display(c[0]), display(c[1]), display(c[2])};
            const float qx = x + 0.5f, qy = y + 0.5f;
            for (int i = 0; i < count; ++i) {
                const float ax = hash(p.frame, i, 13) * float(p.in.width), ay = hash(p.frame, i, 17) * float(p.in.height);
                const float r = (0.5f + hash(p.frame, i, 19)) * p.P(0) * float(p.in.height) / 1080.f;
                const float cover = clamp01(r - std::sqrt((qx - ax) * (qx - ax) + (qy - ay) * (qy - ay)) + 0.5f);
                for (int k = 0; k < 3; ++k) e[k] *= 1.f - 0.8f * cover;
            }
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(e[k]);
            o[3] = c[3];
        }
}
}
