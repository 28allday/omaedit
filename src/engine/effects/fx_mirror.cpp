#include "fx.h"
namespace engine::fx {
void fx_mirror(const Pass &p, int y0, int y1) {
    const int mode = int(p.P(0) + 0.5f);
    const bool other = p.P(1) > 0.5f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float fx = (x + 0.5f) / p.in.width, fy = (y + 0.5f) / p.in.height;
            if (mode == 0) { if ((fx > 0.5f) != other) fx = 1.f - fx; }
            else if (mode == 1) { if ((fy > 0.5f) != other) fy = 1.f - fy; }
            else if (mode == 2) { if ((fy > fx) != other) std::swap(fx, fy); }
            else if ((fx + fy > 1.f) != other) { const float t = fx; fx = 1.f - fy; fy = 1.f - t; }
            sampleAt(p.in, fx * p.in.width, fy * p.in.height, p.out.px(x, y));
        }
}
}
