#include "fx.h"
namespace engine::fx {
void fx_directional_blur(const Pass &p, int y0, int y1) {
    const float reach = p.P(0) * float(p.in.height) / 1080.f;
    const float a = p.P(1) * 3.14159265358979f / 180.f, dx = std::cos(a), dy = -std::sin(a);
    const int n = std::min(32, int(std::ceil(reach)));
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            if (reach < 0.5f) { at(p.in, x, y, o); continue; }
            float s[4] = {0, 0, 0, 0};
            for (int i = -n; i <= n; ++i) {
                float v[4];
                const float t = reach * float(i) / float(n);
                sampleAt(p.in, x + 0.5f + dx * t, y + 0.5f + dy * t, v);
                for (int k = 0; k < 3; ++k) s[k] += v[k] * v[3];
                s[3] += v[3];
            }
            const float inv = 1.f / float(2 * n + 1);
            for (int k = 0; k < 4; ++k) s[k] *= inv;
            if (s[3] <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            for (int k = 0; k < 3; ++k) o[k] = s[k] / s[3];
            o[3] = clamp01(s[3]);
        }
}
}
