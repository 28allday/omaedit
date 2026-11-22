#include "fx.h"
namespace engine::fx {
static float lumaAt(const FrameF &f, int x, int y) {
    float c[4];
    at(f, x, y, c);
    return 0.2126f * display(c[0]) + 0.7152f * display(c[1]) + 0.0722f * display(c[2]);
}
void fx_edge_glow(const Pass &p, int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            auto l = [&](int dx, int dy) { return lumaAt(p.in, x + dx, y + dy); };
            const float gx = l(1, -1) + 2.f * l(1, 0) + l(1, 1) - l(-1, -1) - 2.f * l(-1, 0) - l(-1, 1);
            const float gy = l(-1, 1) + 2.f * l(0, 1) + l(1, 1) - l(-1, -1) - 2.f * l(0, -1) - l(1, -1);
            const float m = std::sqrt(gx * gx + gy * gy) * 0.25f * p.unit();
            const float t = clamp01((m - p.P(0)) / 0.1f);
            float c[4];
            at(p.in, x, y, c);
            float e[3] = {display(c[0]), display(c[1]), display(c[2])};
            const float le = 0.2126f * e[0] + 0.7152f * e[1] + 0.0722f * e[2];
            float *o = p.out.px(x, y);
            for (int k = 0; k < 3; ++k) o[k] = light(e[k] + t * (p.P(1) * e[k] + p.P(2) * (e[k] - le)));
            o[3] = c[3];
        }
}
}
