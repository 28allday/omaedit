#include "fx.h"
namespace engine::fx {
void fx_strobe(const Pass &p, int y0, int y1) {
    const int interval = std::max(1, int(p.P(0) + 0.5f));
    const bool hidden = ((p.frame / interval) % 2 == 1) != (p.P(1) > 0.5f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            at(p.in, x, y, o);
            if (hidden) o[3] = 0.f;
        }
}
}
