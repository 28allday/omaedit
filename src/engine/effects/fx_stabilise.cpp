#include "fx.h"
namespace engine::fx {
void fx_stabilise(const Pass &p, int y0, int y1) {
    const float z = 1.f - p.P(3) / 100.f, zc = z * std::cos(-p.P(2)), zs = z * std::sin(-p.P(2));
    const float cx = p.in.width * 0.5f, cy = p.in.height * 0.5f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            sampleAt(p.in, zc * dx + zs * dy + cx - p.P(0), -zs * dx + zc * dy + cy - p.P(1), p.out.px(x, y));
        }
}
}
