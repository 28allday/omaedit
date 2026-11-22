#include "fx.h"
namespace engine::fx {
void fx_pixelate(const Pass &p, int y0, int y1) {
    const float bw = std::max(1.f, std::floor(p.P(0) * p.in.width + 0.5f)), bh = std::max(1.f, std::floor(p.P(1) * p.in.height + 0.5f));
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float cx = std::floor((x + 0.5f) / bw) * bw, cy = std::floor((y + 0.5f) / bh) * bh;
            float s[4] = {0, 0, 0, 0};
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    float v[4];
                    sampleAt(p.in, std::clamp(cx + (i + 0.5f) / 4.f * bw, 0.5f, p.in.width - 0.5f),
                             std::clamp(cy + (j + 0.5f) / 4.f * bh, 0.5f, p.in.height - 0.5f), v);
                    for (int k = 0; k < 3; ++k) s[k] += v[k] * v[3];
                    s[3] += v[3];
                }
            float *o = p.out.px(x, y);
            for (int k = 0; k < 4; ++k) s[k] /= 16.f;
            if (s[3] <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            for (int k = 0; k < 3; ++k) o[k] = s[k] / s[3];
            o[3] = clamp01(s[3]);
        }
}
}
