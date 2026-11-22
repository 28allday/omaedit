#include "fx.h"
namespace engine::fx {
namespace {
// As keyed() in fx_matte_refine.comp.
float keyed(const Pass &p, int x, int y) {
    x = std::clamp(x, 0, p.in.width - 1);
    y = std::clamp(y, 0, p.in.height - 1);
    const float a = p.in.px(x, y)[3];
    if (p.P(2) < 0.5f) return a;
    const float fx = (float(x) + 0.5f) / float(p.in.width), fy = (float(y) + 0.5f) / float(p.in.height);
    const bool inside = fx >= p.P(3) && fx < p.P(5) && fy >= p.P(4) && fy < p.P(6);
    return inside != (p.P(7) > 0.5f) ? a : 0.f;
}
// As grow() there.
void grow(const Pass &p, int x, int y, int dx, int dy, bool first, float o[4]) {
    const float g = p.P(0) * (float(p.in.height) / 1080.f);
    const int n = std::min(64, int(std::floor(std::abs(g) + 0.5f)));
    at(p.in, x, y, o);
    if (first) o[3] = keyed(p, x, y);
    for (int i = -n; i <= n; ++i) {
        if (i == 0) continue;
        const int qx = x + dx * i, qy = y + dy * i;
        float c[4];
        at(p.in, qx, qy, c);
        const float a = first ? keyed(p, qx, qy) : c[3];
        if (g > 0.f ? a > o[3] : a < o[3]) {
            if (g > 0.f) std::copy_n(c, 3, o);
            o[3] = a;
        }
    }
}
}  // namespace

void fx_matte_refine(const Pass &p, int y0, int y1) {
    const float sigma = p.P(1) * (float(p.in.height) / 1080.f);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            if (p.pass == 0) { grow(p, x, y, 1, 0, true, o); continue; }
            if (p.pass == 1) { grow(p, x, y, 0, 1, false, o); continue; }
            float s[4];
            gaussLine(p.in, x + 0.5f, y + 0.5f, p.pass == 2 ? 1.f : 0.f, p.pass == 2 ? 0.f : 1.f, sigma, s);
            if (p.pass == 2) { straight(s, o); continue; }
            float b[4];
            straight(s, b);
            const float *c = p.aux.px(x, y);
            if (p.P(8) > 0.5f) {
                o[0] = o[1] = o[2] = light(b[3]);
                o[3] = 1.f;
                continue;
            }
            for (int k = 0; k < 3; ++k) o[k] = b[k] + (c[k] - b[k]) * c[3];
            o[3] = b[3];
        }
}
}  // namespace engine::fx
