#include "fx.h"
namespace engine::fx {
void fx_rgb_split(const Pass &p, int y0, int y1) {
    const float dx = p.P(1) * 0.1f * p.in.width, dy = p.P(0) * 0.1f * p.in.height;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            float r[4], g[4], b[4];
            sampleAt(p.in, qx - dx, qy - dy, r);
            sampleAt(p.in, qx, qy, g);
            sampleAt(p.in, qx + dx, qy + dy, b);
            float *o = p.out.px(x, y);
            o[0] = r[0]; o[1] = g[1]; o[2] = b[2];
            o[3] = std::max(std::max(r[3], g[3]), b[3]);
        }
}
}
