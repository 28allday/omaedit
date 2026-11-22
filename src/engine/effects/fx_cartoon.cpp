#include "fx.h"
namespace engine::fx {
static float lumaAt(const FrameF &f, int x, int y) {
    float c[4];
    at(f, x, y, c);
    return 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
}
void fx_cartoon(const Pass &p, int y0, int y1) {
    const float n = 3.f + std::floor(clamp01(p.P(1)) * 12.f + 0.5f);
    const float edge = std::max(0.02f, p.P(0) * 0.4f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            auto l = [&](int dx, int dy) { return lumaAt(p.in, x + dx, y + dy); };
            const float gx = l(1, -1) + 2.f * l(1, 0) + l(1, 1) - l(-1, -1) - 2.f * l(-1, 0) - l(-1, 1);
            const float gy = l(-1, 1) + 2.f * l(0, 1) + l(1, 1) - l(-1, -1) - 2.f * l(0, -1) - l(1, -1);
            const float m = std::sqrt(gx * gx + gy * gy) * 0.25f * p.unit();
            float c[4];
            at(p.in, x, y, c);
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = m > edge ? 0.f : light(std::floor(display(c[k]) * n + 0.5f) / n);
            o[3] = c[3];
        }
}
}
