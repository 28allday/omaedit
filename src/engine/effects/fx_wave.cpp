#include "fx.h"
namespace engine::fx {
void fx_wave(const Pass &p, int y0, int y1) {
    const float amp = p.P(0) * float(p.in.height) / 1080.f, phase = p.P(1) * float(p.frame) * 0.1f;
    const float period = std::max(amp * 4.f, 8.f * p.unit());
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < p.out.width; ++x) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            const float dx = p.P(2) > 0.5f ? amp * std::sin(qy / period + phase) : 0.f;
            const float dy = p.P(3) > 0.5f ? amp * std::sin(qx / period + phase) : 0.f;
            sampleAt(p.in, qx + dx, qy + dy, p.out.px(x, y));
        }
}
}
