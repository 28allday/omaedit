#include "fx.h"
namespace engine::fx {
void fx_vignette(const Pass &p, int y0, int y1) {
    const float shape = std::clamp(p.P(2), 0.01f, 0.99f);
    const float sx = 2.f * shape * 1.41421356f, sy = 2.f * (1.f - shape) * 1.41421356f;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float c[4];
            at(p.in, x, y, c);
            const float dx = ((x + 0.5f) / p.in.width - 0.5f) * sx, dy = ((y + 0.5f) / p.in.height - 0.5f) * sy;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float t = clamp01((r - p.P(0)) / std::max(p.P(1), 1e-3f));
            const float keep = 1.f - t * t * (3.f - 2.f * t);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = c[k] * keep;
            o[3] = c[3];
        }
}
}
