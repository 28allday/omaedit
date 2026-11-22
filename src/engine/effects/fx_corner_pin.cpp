#include "fx.h"
namespace engine::fx {
void fx_corner_pin(const Pass &p, int y0, int y1) {
    const float *m = p.q.p + 8;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            float *o = p.out.px(x, y);
            const float qx = (x + 0.5f) / p.in.width, qy = (y + 0.5f) / p.in.height;
            const float w = m[6] * qx + m[7] * qy + m[8];
            if (std::fabs(w) < 1e-8f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            const float u = (m[0] * qx + m[1] * qy + m[2]) / w, v = (m[3] * qx + m[4] * qy + m[5]) / w;
            if (u < 0.f || v < 0.f || u > 1.f || v > 1.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            sampleAt(p.in, u * p.in.width, v * p.in.height, o);
        }
}
}
