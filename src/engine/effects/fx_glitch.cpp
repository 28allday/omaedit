#include "fx.h"
namespace engine::fx {
void fx_glitch(const Pass &p, int y0, int y1) {
    const int band = std::max(2, int(p.P(1) * 0.1f * float(p.in.height) / p.unit() + 0.5f));
    for (int y = y0; y < y1; ++y) {
        const int b = int((float(y) + 0.5f) / p.unit()) / band;
        const bool moved = hash(b, p.frame, 29) < p.P(0);
        const float shift = moved ? (hash(b, p.frame, 31) * 2.f - 1.f) * p.P(2) * 0.2f * float(p.in.width) : 0.f;
        const bool swap = moved && hash(b, p.frame, 37) < p.P(3);
        for (int x = 0; x < p.out.width; ++x) {
            float qx = x + 0.5f + shift;
            qx = qx - std::floor(qx / float(p.in.width)) * float(p.in.width);
            float *o = p.out.px(x, y);
            sampleAt(p.in, qx, y + 0.5f, o);
            if (swap) std::swap(o[0], o[2]);
        }
    }
}
}
