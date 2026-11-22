#include "fx.h"
namespace engine::fx {
void fx_fisheye(const Pass &p, int y0, int y1) {
    const float cx = p.in.width * 0.5f, cy = p.in.height * 0.5f, half = std::sqrt(cx * cx + cy * cy);
    const float k = p.P(0) * (p.P(1) > 0.5f ? -0.5f : 0.8f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float dx = (x + 0.5f - cx) / half, dy = (y + 0.5f - cy) / half;
            float scale = 1.f + k * (dx * dx + dy * dy);
            if (p.P(2) > 0.5f) scale /= 1.f + std::max(k, 0.f);
            sampleAt(p.in, cx + dx * scale * half, cy + dy * scale * half, p.out.px(x, y));
        }
}
}
