// The CPU twin. Everything the Vulkan engine does, in plain loops, so there
// is always a working path and always a reference to test the GPU against.

#include <algorithm>
#include <cstring>
#include <chrono>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <type_traits>
#include <unordered_map>

#include "engine.h"
#include "matting.h"
#include "parallel.h"
#include "effects/fx.h"

namespace engine {
namespace {

struct Sampled {
    float r, g, b, a;
};

// As common.glsl's: colour below zero kept.
Sampled straightSample(Sampled s) {
    if (s.a <= 0.f) return {0, 0, 0, 0};
    return {s.r / s.a, s.g / s.a, s.b / s.a, std::clamp(s.a, 0.f, 1.f)};
}

// Bilinear sample with clamp-to-edge, texel centres at +0.5, like a Vulkan
// sampler with VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE. Return associated colour:
// transparent texels have no influence on the colour of their neighbours.
Sampled sampleBilinear(const FrameF &src, float sx, float sy) {
    const float fx = sx - 0.5f;
    const float fy = sy - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const int x1 = std::clamp(x0 + 1, 0, src.width - 1);
    const int y1 = std::clamp(y0 + 1, 0, src.height - 1);
    x0 = std::clamp(x0, 0, src.width - 1);
    y0 = std::clamp(y0, 0, src.height - 1);
    const float *p00 = src.px(x0, y0);
    const float *p10 = src.px(x1, y0);
    const float *p01 = src.px(x0, y1);
    const float *p11 = src.px(x1, y1);
    Sampled out;
    float *o = &out.r;
    for (int c = 0; c < 4; ++c) {
        const float a = p00[c] * (c == 3 ? 1.f : p00[3]), b = p10[c] * (c == 3 ? 1.f : p10[3]);
        const float d = p01[c] * (c == 3 ? 1.f : p01[3]), e = p11[c] * (c == 3 ? 1.f : p11[3]);
        const float top = a + (b - a) * tx;
        const float bottom = d + (e - d) * tx;
        o[c] = top + (bottom - top) * ty;
    }
    return out;
}

// Bicubic (radius 2) or Lanczos (radius 3) at source pixel position (sx, sy),
// taps clamped to the crop; the same order of sums as kernel() in composite.comp.
Sampled sampleKernel(const FrameF &src, float sx, float sy, int radius, bool lanczos, int loX, int loY, int hiX, int hiY) {
    const float px = sx - 0.5f, py = sy - 0.5f;
    const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
    const float fx = px - ix, fy = py - iy;
    float sum[4] = {0, 0, 0, 0}, wsum = 0;
    for (int ky = 1 - radius; ky <= radius; ++ky) {
        const float wy = lanczos ? lanczosWeight(fy - ky) : cubicWeight(fy - ky);
        for (int kx = 1 - radius; kx <= radius; ++kx) {
            const float w = wy * (lanczos ? lanczosWeight(fx - kx) : cubicWeight(fx - kx));
            const float *t = src.px(std::clamp(ix + kx, loX, hiX), std::clamp(iy + ky, loY, hiY));
            for (int c = 0; c < 4; ++c) sum[c] += w * t[c] * (c == 3 ? 1.f : t[3]);
            wsum += w;
        }
    }
    return {sum[0] / wsum, sum[1] / wsum, sum[2] / wsum, sum[3] / wsum};
}

inline uint8_t to8(float v) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
}

// A whole frame copied or filled, its rows shared over the cores: a 2160p
// frame is 130 MB of floats, and one thread moving it holds up every other
// (measured: about 40 % of a CPU frame's time went to copies and fills made
// by the render thread alone).
void sizeFrame(FrameF &f, int w, int h) {
    if (f.width == w && f.height == h && f.rgba.size() == size_t(w) * h * 4) return;
    f.width = w;
    f.height = h;
    f.rgba.resize(size_t(w) * h * 4);
}
void copyFrame(FrameF &to, const FrameF &from) {
    if (&to == &from) return;
    sizeFrame(to, from.width, from.height);
    const size_t row = size_t(from.width) * 4;
    const float *in = from.rgba.data();
    float *out = to.rgba.data();
    parallelRows(0, from.height, [&](int first, int last) {
        std::copy(in + first * row, in + last * row, out + first * row);
    }, 64);
}
void fillFrame(FrameF &f, float r, float g, float b, float a) {
    const int w = f.width;
    parallelRows(0, f.height, [&](int first, int last) {
        float *p = f.rgba.data() + size_t(first) * w * 4;
        for (const float *end = f.rgba.data() + size_t(last) * w * 4; p < end; p += 4) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    }, 64);
}

// A grade plan (grade.h) on the CPU: the same operations as grade.comp.
void runPlan(const GradePlan &plan, const FrameF &input, FrameF &out, std::vector<FrameF> &regs,
             const std::function<bool(const FrameF &, const GradeOp &, size_t, FrameF &)> &findPeople) {
    const int w = input.width, h = input.height;
    if (int(regs.size()) < plan.registers) regs.resize(size_t(plan.registers));
    for (auto &r : regs)
        if (r.width != w || r.height != h) r.resize(w, h);
    copyFrame(regs[0], input);
    int last = 0;
    size_t people = 0;
    for (const GradeOp &op : plan.ops) {
        FrameF &to = regs[size_t(op.target)];
        if (op.kind == GradeOp::Kind::People) {
            // `found` is the picture cut out by the network: its alpha is the matte.
            FrameF found;
            const bool matte = findPeople(regs[size_t(op.a)], op, people++, found);
            parallelRows(0, h, [&](int first, int lastRow) {
                for (int y = first; y < lastRow; ++y)
                    for (int x = 0; x < w; ++x) {
                        float *o = to.px(x, y);
                        o[0] = matte ? found.px(x, y)[3] : 1.f;
                        o[1] = o[2] = 0.f;
                        o[3] = 1.f;
                    }
            }, 16);
            last = op.target;
            continue;
        }
        const FrameF *A = op.a >= 0 ? &regs[size_t(op.a)] : nullptr, *B = op.b >= 0 ? &regs[size_t(op.b)] : nullptr;
        const FrameF *M = op.mask >= 0 ? &regs[size_t(op.mask)] : nullptr;
        // An operation reading its own target works on a copy of it.
        FrameF copyA, copyB, copyM;
        if (A == &to) { copyFrame(copyA, *A); A = &copyA; }
        if (B == &to) { copyFrame(copyB, *B); B = &copyB; }
        if (M == &to) { copyFrame(copyM, *M); M = &copyM; }
        const float *t = op.table ? op.table->data() : nullptr;
        const int radius = int(std::lround(std::abs(op.amount)));
        parallelRows(0, h, [&](int first, int lastRow) {
            for (int y = first; y < lastRow; ++y)
                for (int x = 0; x < w; ++x) {
                    float *o = to.px(x, y);
                    const float *a = A ? &A->rgba[(size_t(y) * w + x) * 4] : nullptr;
                    const float *b = B ? &B->rgba[(size_t(y) * w + x) * 4] : nullptr;
                    switch (op.kind) {
                    case GradeOp::Kind::Correct: {
                        float rgb[3] = {a[0], a[1], a[2]};
                        grade::correct(t, rgb);
                        o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2]; o[3] = a[3];
                        break;
                    }
                    case GradeOp::Kind::Box: {
                        double sum[3] = {};
                        for (int k = -radius; k <= radius; ++k) {
                            const int sx = op.channel ? x : std::clamp(x + k, 0, w - 1), sy = op.channel ? std::clamp(y + k, 0, h - 1) : y;
                            const float *s = &A->rgba[(size_t(sy) * w + sx) * 4];
                            for (int c = 0; c < 3; ++c) sum[c] += s[c];
                        }
                        for (int c = 0; c < 3; ++c) o[c] = float(sum[c] / (2 * radius + 1));
                        o[3] = a[3];
                        break;
                    }
                    case GradeOp::Kind::Detail:
                        for (int c = 0; c < 3; ++c) o[c] = a[c] + op.amount * (a[c] - b[c]);
                        o[3] = a[3];
                        break;
                    case GradeOp::Kind::Program:  // the program itself runs after, on the whole picture
                        std::copy_n(a, 4, o);
                        break;
                    case GradeOp::Kind::Mask:
                        o[0] = grade::maskAt(t, a, x, y, w, h);
                        o[1] = o[2] = 0.f;
                        o[3] = 1.f;
                        break;
                    case GradeOp::Kind::Grow: {
                        float v = a[0];
                        for (int k = -radius; k <= radius; ++k) {
                            const int sx = op.channel ? x : x + k, sy = op.channel ? y + k : y;
                            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                            const float s = A->rgba[(size_t(sy) * w + sx) * 4];
                            v = op.amount > 0 ? std::max(v, s) : std::min(v, s);
                        }
                        o[0] = v; o[1] = o[2] = 0.f; o[3] = 1.f;
                        break;
                    }
                    case GradeOp::Kind::Blur: {
                        double sum = 0;
                        int count = 0;
                        for (int k = -radius; k <= radius; ++k) {
                            const int sx = op.channel ? x : x + k, sy = op.channel ? y + k : y;
                            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                            sum += A->rgba[(size_t(sy) * w + sx) * 4];
                            ++count;
                        }
                        o[0] = float(sum / std::max(1, count)); o[1] = o[2] = 0.f; o[3] = 1.f;
                        break;
                    }
                    case GradeOp::Kind::Key: {
                        const float k = (a ? a[0] : 1.f) * (b ? b[0] : 1.f);
                        o[0] = grade::unit((op.channel ? 1.f - k : k) * op.amount); o[1] = o[2] = 0.f; o[3] = 1.f;
                        break;
                    }
                    case GradeOp::Kind::Mix: {
                        const float m = grade::unit((M ? M->rgba[(size_t(y) * w + x) * 4] : 1.f) * op.amount);
                        for (int c = 0; c < 4; ++c) o[c] = a[c] + (b[c] - a[c]) * m;
                        break;
                    }
                    case GradeOp::Kind::Sum:
                        for (int c = 0; c < 3; ++c) o[c] = a[c] + b[c];
                        o[3] = M ? M->rgba[(size_t(y) * w + x) * 4 + 3] : a[3];
                        break;
                    case GradeOp::Kind::Split:
                        for (int c = 0; c < 3; ++c) o[c] = c == op.channel ? a[c] : 0.f;
                        o[3] = a[3];
                        break;
                    case GradeOp::Kind::Copy: std::copy_n(a, 4, o); break;
                    case GradeOp::Kind::Clear: o[0] = o[1] = o[2] = o[3] = 0.f; break;
                    case GradeOp::Kind::MatteView: o[0] = o[1] = o[2] = a ? a[0] : 1.f; o[3] = 1.f; break;
                    case GradeOp::Kind::People: break;  // above
                    }
                }
        }, 16);
        if (op.kind == GradeOp::Kind::Program && op.program && op.program->cpu) {
            // The program on the copy, then mixed back in by its amount (RGB).
            // (Each block copies its rows, runs the program on them and mixes
            // them back: the arithmetic per pixel is as it was.)
            const size_t row = size_t(w) * 4;
            parallelRows(0, h, [&](int first, int lastRow) {
                const size_t count = size_t(lastRow - first) * row;
                float *mine = to.rgba.data() + size_t(first) * row;
                std::vector<float> looked(mine, mine + count);
                op.program->cpu(looked.data(), size_t(lastRow - first) * w);
                for (size_t i = 0; i < count; ++i)
                    if (i % 4 != 3) mine[i] += (looked[i] - mine[i]) * op.amount;
            }, 16);
        }
        last = op.target;
    }
    copyFrame(out, regs[size_t(last)]);
}

class CpuEngine final : public Engine {
public:
    bool isGpu() const override { return false; }
    std::string deviceName() const override { return "CPU"; }

    int createSource() override {
        const int id = m_nextId++;
        m_sources.emplace(id, CpuSource{});
        return id;
    }
    void destroySource(int id) override { m_sources.erase(id); m_filterMs.erase(id); }
    double filterMs(int id) const override {
        const auto it = m_filterMs.find(id);
        return it == m_filterMs.end() ? -1 : it->second;
    }
    void setSourceDenoise(int id, float strength) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        if ((strength > 0.f) != (it->second.denoise > 0.f)) it->second.historyValid = false;
        it->second.denoise = strength;
        it->second.pendingProcess = true;
    }

    void setSourceDeinterlace(int id, DeinterlaceMode mode) override {
        const auto it = m_sources.find(id);
        if (it == m_sources.end() || it->second.deinterlace == mode) return;
        it->second.deinterlace = mode;
        resetSourceHistory(id);
    }
    void resetSourceHistory(int id) override {
        const auto it = m_sources.find(id); if (it == m_sources.end()) return;
        auto &s = it->second;
        s.previousValid = false; s.resetNext = true; s.fieldReady = false;
        s.historyValid = false; s.pendingProcess = true;
    }
    void renderSourceField(int id, int field, bool firstInFrame) override {
        const auto it = m_sources.find(id); if (it == m_sources.end()) return;
        auto &s = it->second;
        if (!s.hasFrame) return;
        field = field == 1 ? 1 : 0;
        if (s.deinterlace == DeinterlaceMode::Off) {
            if (!s.pendingProcess) return;
            denoise(s);
        } else {
            if (s.fieldReady && s.field == field && s.firstField == firstInFrame && !s.pendingProcess) return;
            deinterlace(s, field, firstInFrame);
            denoise(s);
        }
        s.pendingProcess = false; ++s.generation;
    }
    bool prepareProgram(const ColourProgram &program, std::string *error) override {
        if (program.cpu) return true;
        if (error) *error = "the colour transform has no CPU form";
        return false;
    }
    void setSourceFilters(int id, const std::vector<VideoFilter> &filters) override {
        const auto it = m_sources.find(id);
        if (it == m_sources.end() || it->second.filters == filters) return;
        auto &s = it->second;
        std::vector<std::remove_reference_t<decltype(s.steps)>::value_type> next(filters.size());
        for (size_t i = 0; i < filters.size(); ++i)
            if (filterStepKept(s.filters, filters, i)) next[i] = std::move(s.steps[i]);  // as in vk_engine.cpp
        s.filters = filters;
        s.steps = std::move(next);
        s.filtered = false;
        m_filterMs.erase(id);  // another chain: measured afresh
    }
    void renderSourceFilters(int id, double seconds) override;

    bool updateSource(int id, const CpuFrame &f) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end() || !f.data || f.width <= 0 || f.height <= 0)
            return false;
        beginFrame(it->second, f.width, f.height);
        FrameF &dst = it->second.work;
        if (dst.width != f.width || dst.height != f.height)
            dst.resize(f.width, f.height);
        const bool alpha = hasAlpha(f.format);
        const bool bgr = f.format == PixelFormat::BGRx || f.format == PixelFormat::BGRA;
        auto encoded = [&](uint8_t value) {
            return f.limitedRange ? std::clamp((value - 16.f) / 219.f, 0.f, 1.f) : value / 255.f;
        };
        // Straight alpha in linear light: each channel is a function of its
        // byte alone, so a table of the same values replaces the curve.
        float table[256];
        const bool wide = f.format == PixelFormat::RGBA64, floats = f.format == PixelFormat::RGBAF32;
        const bool tabled = !f.premultiplied && f.linearAlpha && !wide && !floats;
        if (tabled)
            for (int v = 0; v < 256; ++v) {
                const float e = std::min(1.f, encoded(uint8_t(v)));
                table[v] = f.encoded ? e : srgbToLinear(e);  // kept encoded: a colour program does the transfer
            }
        parallelRows(0, f.height, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const uint8_t *row = f.data + static_cast<size_t>(y) * f.stride;
            float *out = dst.px(0, y);
            if (tabled) {
                for (int x = 0; x < f.width; ++x, row += 4, out += 4) {
                    out[0] = table[bgr ? row[2] : row[0]];
                    out[1] = table[row[1]];
                    out[2] = table[bgr ? row[0] : row[2]];
                    out[3] = alpha ? row[3] / 255.f : 1.f;
                }
                continue;
            }
            if (floats) {
                // As the GPU's float texture through convert.comp: the values
                // as they are (nothing clipped at one), the flags in its order.
                for (int x = 0; x < f.width; ++x, row += 16, out += 4) {
                    float in[4];
                    std::memcpy(in, row, 16);
                    const float a = in[3];
                    const float factor = f.linearAlpha ? 1.f : a;
                    for (int k = 0; k < 3; ++k) {
                        float c = in[k];
                        if (f.limitedRange) c = std::clamp((c * 255.f - 16.f) / 219.f, 0.f, 1.f);
                        if (f.premultiplied) c = a > 0.f ? std::min(1.f, c / a) : 0.f;
                        out[k] = f.encoded ? c : factor > 0.f ? srgbToLinear(c * factor) / factor : 0.f;
                    }
                    out[3] = a;
                }
                continue;
            }
            for (int x = 0; x < f.width; ++x, row += pixelBytes(f.format), out += 4) {
                // 16-bit words are read as fractions of 65535 (the GPU's UNORM16).
                auto channel = [&](int i) -> float {
                    if (!wide) return encoded(row[i]);
                    const float v = float(row[i * 2] | (row[i * 2 + 1] << 8)) / 65535.f;
                    return f.limitedRange ? std::clamp((v * 255.f - 16.f) / 219.f, 0.f, 1.f) : v;
                };
                const float a = !alpha ? 1.f : wide ? float(row[6] | (row[7] << 8)) / 65535.f : row[3] / 255.f;
                const float scale = f.premultiplied ? (a > 0.f ? 1.f / a : 0.f) : 1.f;
                const float r = std::min(1.f, channel(bgr ? 2 : 0) * scale);
                const float g = std::min(1.f, channel(1) * scale);
                const float b = std::min(1.f, channel(bgr ? 0 : 2) * scale);
                const float factor = f.linearAlpha ? 1.f : a;
                if (f.encoded) {
                    out[0] = r; out[1] = g; out[2] = b;
                } else {
                    out[0] = factor > 0.f ? srgbToLinear(r * factor) / factor : 0.f;
                    out[1] = factor > 0.f ? srgbToLinear(g * factor) / factor : 0.f;
                    out[2] = factor > 0.f ? srgbToLinear(b * factor) / factor : 0.f;
                }
                out[3] = a;
            }
        }
        });
        finishFrame(it->second);
        return true;
    }

    bool updateSource(int, const DmaBufFrame &) override { return false; }

    // The same arithmetic as yuv_in.comp, read straight from the planes.
    bool updateSource(int id, const YuvInput &f) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end() || !f.plane[0] || f.width <= 0 || f.height <= 0) return false;
        if (f.layout != YuvLayout::YUY2 &&
            (!f.plane[1] || (f.layout != YuvLayout::NV12 && !f.plane[2])))
            return false;
        if (f.depth < 8 || f.depth > 16) return false;
        const int reduce = yuvReduce(f, it->second.deinterlace != DeinterlaceMode::Off);
        const int workW = (f.width + reduce - 1) / reduce, workH = (f.height + reduce - 1) / reduce;
        beginFrame(it->second, workW, workH);
        FrameF &dst = it->second.work;
        if (dst.width != workW || dst.height != workH) dst.resize(workW, workH);
        float kr, kb;
        yuvCoefficients(f.matrix, &kr, &kb);
        const InputParams light = inputParams(f, m_levels);
        const int cw = f.layout == YuvLayout::I444 ? f.width : (f.width + 1) / 2, ch = (f.height + 1) / 2;
        const bool yuy2 = f.layout == YuvLayout::YUY2;
        const bool everyRow = chromaEveryRow(f.layout);
        const bool wide = f.depth > 8;
        const int shift = wide && f.msb ? 16 - f.depth : 0;
        const float top = float((1 << f.depth) - 1);
        // Sample i of a row: a byte, or a little-endian word.
        auto word = [&](const uint8_t *row, int i) -> float {
            if (!wide) return row[i];
            return float((row[i * 2] | (row[i * 2 + 1] << 8)) >> shift);
        };
        auto chroma = [&](int cx, int cy, float *cb, float *cr) {
            cx = std::clamp(cx, 0, cw - 1);
            if (yuy2) {
                cy = std::clamp(cy, 0, f.height - 1);
                const uint8_t *row = f.plane[0] + static_cast<size_t>(cy) * f.stride[0];
                *cb = word(row, cx * 4 + 1);
                *cr = word(row, cx * 4 + 3);
                return;
            }
            cy = std::clamp(cy, 0, everyRow ? f.height - 1 : ch - 1);
            if (f.layout == YuvLayout::NV12) {
                const uint8_t *row = f.plane[1] + static_cast<size_t>(cy) * f.stride[1];
                *cb = word(row, cx * 2);
                *cr = word(row, cx * 2 + 1);
            } else {
                *cb = word(f.plane[1] + static_cast<size_t>(cy) * f.stride[1], cx);
                *cr = word(f.plane[2] + static_cast<size_t>(cy) * f.stride[2], cx);
            }
        };
        auto mix = [](float a, float b, float t) { return a * (1.f - t) + b * t; };
        // Rows [first, last) of the picture converted, into `to` (four floats a pixel, `step` floats a row).
        const auto convert = [&](int first, int last, float *to, size_t step) {
        for (int y = first; y < last; ++y) {
            const uint8_t *yrow = f.plane[0] + static_cast<size_t>(y) * f.stride[0];
            const uint8_t *arow = f.alpha ? f.alpha + static_cast<size_t>(y) * f.alphaStride : nullptr;
            float *out = to + static_cast<size_t>(y - first) * step;
            const bool fields = !everyRow && ch > 1 && (f.interlaced || it->second.deinterlace != DeinterlaceMode::Off);
            const float fy = everyRow ? float(y) : (float(fields ? y / 2 : y) + 0.5f) * 0.5f - 0.5f;
            const int localY = static_cast<int>(std::floor(fy));
            const float ty = fy - float(localY);
            const int parity = y & 1;
            const int last = (ch - 1 - parity) / 2;
            const int y0 = fields ? 2 * std::clamp(localY, 0, last) + parity : localY;
            const int y1 = fields ? 2 * std::clamp(localY + 1, 0, last) + parity : localY + 1;
            for (int x = 0; x < f.width; ++x, out += 4) {
                const float luma = yuy2 ? word(yrow, x * 2) : word(yrow, x);
                const float fx = f.layout == YuvLayout::I444 ? float(x) : chromaColumn(x, f.chromaLeft);
                const int x0 = static_cast<int>(std::floor(fx));
                const float tx = fx - float(x0);
                float b00, r00, b10, r10, b01, r01, b11, r11;
                chroma(x0, y0, &b00, &r00);
                chroma(x0 + 1, y0, &b10, &r10);
                chroma(x0, y1, &b01, &r01);
                chroma(x0 + 1, y1, &b11, &r11);
                const float cb = mix(mix(b00, b10, tx), mix(b01, b11, tx), ty);
                const float cr = mix(mix(r00, r10, tx), mix(r01, r11, tx), ty);
                float r, g, b;
                yuvToRgb(luma, cb, cr, f.fullRange, kr, kb, &r, &g, &b, f.depth, light.kind != 3);
                const float a = arow ? word(arow, x) / top : 1.f;
                if (f.premultiplied) {
                    const float scale = a > 0.f ? 1.f / a : 0.f;
                    r = std::min(1.f, r * scale); g = std::min(1.f, g * scale); b = std::min(1.f, b * scale);
                }
                const float factor = f.linearAlpha ? 1.f : a;
                if (factor > 0.f) {
                    const float e[3] = {r * factor, g * factor, b * factor};
                    inputLinear(light, e, out);
                    for (int k = 0; k < 3; ++k) out[k] /= factor;
                } else {
                    out[0] = out[1] = out[2] = 0.f;
                }
                out[3] = a;
            }
        }
        };
        if (reduce == 1) {
            parallelRows(0, f.height, [&](int first, int last) { convert(first, last, dst.px(0, first), static_cast<size_t>(dst.width) * 4); });
        } else {
            // Reduced: each row of the smaller picture from its block of
            // rows, colour weighted by alpha, added up in the shader's order.
            parallelRows(0, workH, [&](int first, int last) {
                std::vector<float> rows(static_cast<size_t>(f.width) * 4 * reduce);
                const size_t step = static_cast<size_t>(f.width) * 4;
                for (int y = first; y < last; ++y) {
                    const int top = y * reduce, bottom = std::min(f.height, top + reduce);
                    convert(top, bottom, rows.data(), step);
                    float *out = dst.px(0, y);
                    for (int x = 0; x < workW; ++x, out += 4) {
                        float colour[3] = {0.f, 0.f, 0.f}, alpha = 0.f, count = 0.f;
                        for (int j = 0; j < bottom - top; ++j)
                            for (int i = 0; i < reduce; ++i) {
                                const int sx = x * reduce + i;
                                if (sx >= f.width) continue;
                                const float *v = rows.data() + static_cast<size_t>(j) * step + static_cast<size_t>(sx) * 4;
                                for (int k = 0; k < 3; ++k) colour[k] += v[k] * v[3];
                                alpha += v[3];
                                count += 1.f;
                            }
                        for (int k = 0; k < 3; ++k) out[k] = alpha > 0.f ? colour[k] / alpha : 0.f;
                        out[3] = alpha / count;
                    }
                }
            }, 4);
        }
        finishFrame(it->second);
        return true;
    }

    void setHdrLevels(const HdrLevels &levels) override {
        if (levels == m_levels) return;
        m_levels = levels;
        for (auto &kv : m_sources) kv.second.filtered = false;  // tone maps read the white level
    }
    HdrLevels hdrLevels() const override { return m_levels; }

    void setCanvasSize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        if (m_canvas.width != w || m_canvas.height != h)
            m_canvas.resize(w, h);
    }
    int canvasWidth() const override { return m_canvas.width; }
    int canvasHeight() const override { return m_canvas.height; }

    void composite(const std::vector<Layer> &layers, const Color &bg) override { compositeTo(m_canvas, -1, layers, bg); }

    void compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) override {
        auto it = m_sources.find(target);
        if (it == m_sources.end() || width <= 0 || height <= 0) return;
        // Into a scratch frame first: a layer may not read what is being drawn.
        // The frame it replaces is the next call's scratch: no fresh 130 MB
        // to allocate and clear for every picture (compositeTo fills it all).
        FrameF &out = m_scratch;
        sizeFrame(out, width, height);
        compositeTo(out, target, layers, bg);
        it = m_sources.find(target);
        std::swap(it->second.work, out);
        it->second.denoise = 0.f;  // a drawn studio is not noisy
        ++it->second.generation;
    }

    void transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style,
                        float progress) override {
        if (target == from || target == to || width <= 0 || height <= 0) return;
        auto dst = m_sources.find(target), a = m_sources.find(from), b = m_sources.find(to);
        if (dst == m_sources.end() || a == m_sources.end() || b == m_sources.end()) return;
        const auto &old = a->second.shown(), &next = b->second.shown();
        if (old.width <= 0 || next.width <= 0) return;
        const float t = std::clamp(progress, 0.f, 1.f), cp = std::clamp(style.colourPoint, 0.f, 1.f);
        const Sampled colour{style.colour.r * style.colour.a, style.colour.g * style.colour.a,
                             style.colour.b * style.colour.a, style.colour.a};
        const bool across = style.side == Side::Left || style.side == Side::Right;
        const float length = across ? float(width) : float(height), edge = t * length;
        auto mix = [](const Sampled &p, const Sampled &q, float k) {
            return Sampled{p.r + (q.r - p.r) * k, p.g + (q.g - p.g) * k, p.b + (q.b - p.b) * k, p.a + (q.a - p.a) * k};
        };
        FrameF &out = m_scratch;  // every pixel is written below; swapped in, as compositeInto
        sizeFrame(out, width, height);
        // Each pixel from the two pictures alone: rows shared over the cores.
        parallelRows(0, height, [&](int firstRow, int lastRow) {
        for (int y = firstRow; y < lastRow; ++y) for (int x = 0; x < width; ++x) {
            const float u = (x + .5f) / width, v = (y + .5f) / height;
            auto at = [&](const FrameF &f, float su, float sv) { return sampleBilinear(f, su * f.width, sv * f.height); };
            Sampled c;
            switch (style.type) {
            case Transition::Fade: c = mix(at(old, u, v), at(next, u, v), t); break;
            case Transition::FadeColour:
                c = t < cp ? mix(at(old, u, v), colour, t / cp)
                           : mix(colour, at(next, u, v), cp >= 1.f ? 1.f : (t - cp) / (1.f - cp));
                break;
            case Transition::Swipe:
                c = sideDistance(style.side, x, y, width, height) < edge ? at(next, u, v) : at(old, u, v);
                break;
            case Transition::Cover: case Transition::Uncover: case Transition::Slide: {
                const float along = sideDistance(style.side, x, y, width, height);
                float su = u, sv = v;
                if (along < edge) {
                    if (style.type != Transition::Uncover) sidePoint(style.side, along + (length - edge), x, y, width, height, &su, &sv);
                    c = at(next, su, sv);
                } else {
                    if (style.type != Transition::Cover) sidePoint(style.side, along - edge, x, y, width, height, &su, &sv);
                    c = at(old, su, sv);
                }
                break;
            }
            case Transition::Wipe:
                c = mix(at(old, u, v), at(next, u, v),
                        wipeCoverage(wipeMaskValue(style.mask, x, y, width, height), t, style.softness, style.invert));
                break;
            case Transition::Matte: {
                const Sampled m = at(next, style.matteRect[0] + (style.matteRect[2] - style.matteRect[0]) * u,
                                     style.matteRect[1] + (style.matteRect[3] - style.matteRect[1]) * v);
                const float k = matteCoverage(m.r, m.g, m.b, style.invert);
                c = at(old, u, v);
                c = {c.r * k, c.g * k, c.b * k, c.a * k};
                break;
            }
            case Transition::Cut: c = at(t > 0.f ? next : old, u, v); break;
            }
            c = straightSample(c);
            float *p = out.px(x, y); p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=c.a;
        }
        });
        std::swap(dst->second.work, out); dst->second.denoise = 0.f; ++dst->second.generation;
    }

    void compositeTo(FrameF &into, int skip, const std::vector<Layer> &layers, const Color &bg) {
        if (into.width == 0) return;
        fillFrame(into, bg.r, bg.g, bg.b, bg.a);
        for (const Layer &l : layers) {
            auto it = m_sources.find(l.source);
            if (l.source == skip || it == m_sources.end() || it->second.shown().width == 0 || l.w <= 0 || l.h <= 0)
                continue;
            const VideoFilter *wrap = it->second.filtered ? lightWrapOf(it->second.filters) : nullptr;
            if (wrap) wrapLayer(into, it->second.shown(), l, *wrap);
            else drawLayer(into, it->second.shown(), l);
        }
    }
    // A layer with light wrap (lightWrapParams in frame.h), as wrapLayer in vk_engine.cpp.
    void wrapLayer(FrameF &into, const FrameF &src, const Layer &l, const VideoFilter &f) {
        const PixelBounds b = lightWrapRegion(f, l, into.width, into.height);
        if (b.empty()) return;
        const int scale = lightWrapScale(f, into.height);
        const int bw = b.x1 - b.x0, bh = b.y1 - b.y0, hw = (bw + scale - 1) / scale, hh = (bh + scale - 1) / scale;
        FrameF drawn, lit, light, across;
        const bool whole = layerIsWhole(l, src.width, src.height, into.width, into.height);
        if (!whole) {
            drawn.resize(bw, bh);
            Layer plain = l;
            plain.x -= float(b.x0); plain.y -= float(b.y0);
            plain.blend = BlendMode::Normal;
            plain.blendSpace = BlendSpace::Linear;
            drawLayer(drawn, src, plain);
        }
        const FrameF &alone = whole ? src : drawn;
        lit.resize(bw, bh);
        light.resize(hw, hh);
        across.resize(hw, hh);
        runStep(into, light, f, lightWrapParams(f, 0, into.height, b.x0, b.y0, hw, hh), &alone);
        runStep(light, across, f, lightWrapParams(f, 1, into.height, b.x0, b.y0, hw, hh));
        runStep(across, light, f, lightWrapParams(f, 2, into.height, b.x0, b.y0, hw, hh));
        runStep(alone, lit, f, lightWrapParams(f, 3, into.height, b.x0, b.y0, hw, hh), &light);
        Layer region;
        region.x = float(b.x0); region.y = float(b.y0); region.w = float(bw); region.h = float(bh);
        region.filter = ScaleFilter::Point;
        region.blend = l.blend;
        region.blendSpace = l.blendSpace;
        drawLayer(into, lit, region);
    }

    static void drawLayer(FrameF &into, const FrameF &src, const Layer &l) {
        {
            const float fx = std::fabs(l.u1 - l.u0) * src.width / l.w, fy = std::fabs(l.v1 - l.v0) * src.height / l.h;
            const ScaleFilter filter = layerFilter(l, src.width, src.height);
            // Taps stay inside the crop, as in composite.comp.
            // And inside the picture, whatever the layer asks for.
            const int tapLoX = std::clamp(static_cast<int>(std::floor(std::min(l.u0, l.u1) * src.width + 0.001f)), 0, src.width - 1);
            const int tapLoY = std::clamp(static_cast<int>(std::floor(std::min(l.v0, l.v1) * src.height + 0.001f)), 0, src.height - 1);
            const int tapHiX = std::clamp(static_cast<int>(std::ceil(std::max(l.u0, l.u1) * src.width - 0.001f)) - 1, tapLoX, src.width - 1);
            const int tapHiY = std::clamp(static_cast<int>(std::ceil(std::max(l.v0, l.v1) * src.height - 0.001f)) - 1, tapLoY, src.height - 1);
            const float stepU = (l.u1 > l.u0 ? 1.f : -1.f) * fx / src.width;
            const float stepV = (l.v1 > l.v0 ? 1.f : -1.f) * fy / src.height;
            const PixelBounds b = layerBounds(l, into.width, into.height);
            const float cosR = std::cos(l.rotation), sinR = std::sin(l.rotation);
            const float hw = l.w * 0.5f, hh = l.h * 0.5f;
            const float cx = l.x + hw, cy = l.y + hh;
            const float tu = 0.5f / src.width, tv = 0.5f / src.height;
            const float loU = std::clamp(std::min(l.u0, l.u1), 0.f, 1.f) + tu, loV = std::clamp(std::min(l.v0, l.v1), 0.f, 1.f) + tv;
            const float hiU = std::max(loU, std::clamp(std::max(l.u0, l.u1), 0.f, 1.f) - tu);
            const float hiV = std::max(loV, std::clamp(std::max(l.v0, l.v1), 0.f, 1.f) - tv);
            parallelRows(b.y0, b.y1, [&](int first, int last) {
            for (int y = first; y < last; ++y) {
                for (int x = b.x0; x < b.x1; ++x) {
                    const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    const float qx = cosR * dx + sinR * dy + hw;
                    const float qy = -sinR * dx + cosR * dy + hh;
                    const float edge = std::min(std::min(qx, l.w - qx), std::min(qy, l.h - qy));
                    const float cover = std::clamp(edge + 0.5f, 0.f, 1.f);
                    if (cover <= 0.f)
                        continue;
                    const float tx = qx / l.w, ty = qy / l.h;
                    const float ru = l.u0 + (l.u1 - l.u0) * tx, rv = l.v0 + (l.v1 - l.v0) * ty;
                    Sampled s{};
                    if (filter == ScaleFilter::Bicubic || filter == ScaleFilter::Lanczos) {
                        s = sampleKernel(src, ru * src.width, rv * src.height, filter == ScaleFilter::Lanczos ? 3 : 2,
                                         filter == ScaleFilter::Lanczos, tapLoX, tapLoY, tapHiX, tapHiY);
                    } else if (filter == ScaleFilter::Area) {
                        const int nx = areaTaps(fx), ny = areaTaps(fy);
                        for (int ay = 0; ay < ny; ++ay)
                            for (int ax = 0; ax < nx; ++ax) {
                                const float ox = (ax + 0.5f) / nx - 0.5f, oy = (ay + 0.5f) / ny - 0.5f;
                                const float u = std::clamp(ru + ox * stepU, loU, hiU);
                                const float v = std::clamp(rv + oy * stepV, loV, hiV);
                                const Sampled t = sampleBilinear(src, u * src.width, v * src.height);
                                s.r += t.r; s.g += t.g; s.b += t.b; s.a += t.a;
                            }
                        const float inv = 1.f / float(nx * ny);
                        s.r *= inv; s.g *= inv; s.b *= inv; s.a *= inv;
                    } else if (filter == ScaleFilter::Point) {
                        const auto *p = src.px(std::clamp(int(std::floor(ru * src.width)), tapLoX, tapHiX),
                                              std::clamp(int(std::floor(rv * src.height)), tapLoY, tapHiY));
                        s = {p[0] * p[3], p[1] * p[3], p[2] * p[3], p[3]};
                    } else {
                        s = sampleBilinear(src, std::clamp(ru, loU, hiU) * src.width, std::clamp(rv, loV, hiV) * src.height);
                    }
                    s = straightSample(s);
                    const float a = s.a * l.opacity * cover;
                    if (a <= 0.f) continue;  // as the shader: the canvas stays exactly as it is
                    float *d = into.px(x, y);
                    const bool srgb = l.blendSpace == BlendSpace::Srgb;
                    float top[3] = {s.r, s.g, s.b}, backdrop[3] = {d[0], d[1], d[2]};
                    if (srgb) for (int k = 0; k < 3; ++k) { top[k] = linearToSrgb(top[k]); backdrop[k] = linearToSrgb(backdrop[k]); }
                    if (l.blend != BlendMode::Normal) {
                        // As composite.comp: on the timeline's codes, then back to linear.
                        for (int k = 0; k < 3; ++k) {
                            const float mixed = srgb ? blendChannel(static_cast<int>(l.blend), backdrop[k], top[k])
                                : rec709Decode(blendChannel(static_cast<int>(l.blend), rec709Encode(backdrop[k]), rec709Encode(top[k])));
                            top[k] = top[k] * (1.f - d[3]) + mixed * d[3];
                        }
                    }
                    const float remaining = d[3] * (1.f - a);
                    const float outAlpha = a + remaining;
                    // Working frames are straight-alpha, including nested
                    // studios. Store the unassociated colour so another layer
                    // does not multiply it by alpha a second time.
                    for (int k = 0; k < 3; ++k) {
                        const float result = outAlpha > 0.f ? (top[k] * a + backdrop[k] * remaining) / outAlpha : 0.f;
                        d[k] = srgb ? srgbToLinear(result) : result;
                    }
                    d[3] = outAlpha;
                }
            }
            });
        }
    }

    bool readPreview(int w, int h, Bgra8Frame &out) override { return previewOf(m_canvas, w, h, out); }
    bool readSourcePreview(int source, int w, int h, Bgra8Frame &out) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && previewOf(it->second.shown(), w, h, out);
    }
    bool readLinear(int source, FrameF &out) override {
        const FrameF *picture = &m_canvas;
        if (source >= 0) {
            const auto it = m_sources.find(source);
            if (it == m_sources.end()) return false;
            picture = &it->second.shown();
        }
        if (picture->width == 0) return false;
        copyFrame(out, *picture);
        return true;
    }
    bool readSourceEncoded(int source, int w, int h, Bgra8Frame &out) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && previewOf(it->second.shown(), w, h, out, true);
    }
    bool readSourceShown(int source, int w, int h, uint8_t *bgra) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && bgra && previewInto(it->second.shown(), w, h, bgra, 2);
    }
    bool readSourceInput(int source, int w, int h, Bgra8Frame &out) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && previewOf(it->second.base(), w, h, out);
    }
    static bool previewOf(const FrameF &picture, int w, int h, Bgra8Frame &out, bool encoded = false) {
        if (picture.width == 0 || w <= 0 || h <= 0) return false;
        if (out.width != w || out.height != h) out.resize(w, h);
        return previewInto(picture, w, h, out.data.data(), encoded ? 1 : 0);
    }
    // encoded: as preview.comp's (2 multiplies colour by its alpha).
    static bool previewInto(const FrameF &picture, int w, int h, uint8_t *to, int encoded) {
        if (picture.width == 0 || w <= 0 || h <= 0) return false;
        parallelRows(0, h, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            uint8_t *row = to + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x, row += 4) {
                const Sampled s = straightSample(sampleBilinear(picture, (x + 0.5f) / w * picture.width,
                                                 (y + 0.5f) / h * picture.height));
                // Encoded: already display R'G'B' (as preview.comp).
                const float cover = encoded == 2 ? (s.a < 0.f ? 0.f : s.a > 1.f ? 1.f : s.a) : 1.f;
                auto shown = [&](float v) { return (encoded ? (v < 0.f ? 0.f : v > 1.f ? 1.f : v) : linearToSrgb(v)) * cover; };
                row[0] = to8(shown(s.b) * 255.f);
                row[1] = to8(shown(s.g) * 255.f);
                row[2] = to8(shown(s.r) * 255.f);
                row[3] = to8(s.a * 255.f);
            }
        }
        });
        return true;
    }

    // As kernelSample in yuv.comp: bicubic or Lanczos about canvas position
    // (cx, cy), stretched over the footprint when shrinking.
    static void outputKernel(const FrameF &pic, float cx, float cy, float fx, float fy, bool lanczos, float out[3]) {
        const float radius = lanczos ? 3.f : 2.f;
        const int loX = int(std::floor(cx - radius * fx)) + 1, hiX = int(std::floor(cx + radius * fx));
        const int loY = int(std::floor(cy - radius * fy)) + 1, hiY = int(std::floor(cy + radius * fy));
        float sum[3] = {0, 0, 0}, wsum = 0;
        for (int ty = loY; ty <= hiY; ++ty) {
            const float dy = (float(ty) - cy) / fy;
            const float wy = lanczos ? lanczosWeight(dy) : cubicWeight(dy);
            for (int tx = loX; tx <= hiX; ++tx) {
                const float dx = (float(tx) - cx) / fx;
                const float w = wy * (lanczos ? lanczosWeight(dx) : cubicWeight(dx));
                const float *t = pic.px(std::clamp(tx, 0, pic.width - 1), std::clamp(ty, 0, pic.height - 1));
                for (int k = 0; k < 3; ++k) sum[k] += w * t[k];
                wsum += w;
            }
        }
        for (int k = 0; k < 3; ++k) out[k] = std::max(sum[k] / wsum, 0.f);
    }

    // As yuv.comp: the texel at the picture's own size, else by the chosen
    // filter, by default the mean of ceil(scale)^2 bilinear samples, in
    // linear light, then encoded.
    static void outputEncoded(const FrameF &pic, const OutputParams &q, int x, int y, int w, int h, float rgb[3]) {
        const int W = pic.width, H = pic.height;
        x = std::min(x, w - 1);
        y = std::min(y, h - 1);
        if (w == W && h == H) {
            const float *p = pic.px(x, y);
            outputEncode(q, p, rgb);
            return;
        }
        const float sx = float(W) / float(w), sy = float(H) / float(h);
        const float cx = (float(x) + 0.5f) * sx - 0.5f, cy = (float(y) + 0.5f) * sy - 0.5f;
        if (q.filter == int32_t(ScaleFilter::Bilinear)) {
            const float fx = std::clamp(cx, 0.f, float(W - 1)), fy = std::clamp(cy, 0.f, float(H - 1));
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float ax = fx - float(x0), ay = fy - float(y0);
            float c[3];
            for (int k = 0; k < 3; ++k) {
                const float top = pic.px(x0, y0)[k] + (pic.px(x1, y0)[k] - pic.px(x0, y0)[k]) * ax;
                const float bottom = pic.px(x0, y1)[k] + (pic.px(x1, y1)[k] - pic.px(x0, y1)[k]) * ax;
                c[k] = top + (bottom - top) * ay;
            }
            outputEncode(q, c, rgb);
            return;
        }
        if (q.filter == int32_t(ScaleFilter::Bicubic) || q.filter == int32_t(ScaleFilter::Lanczos)) {
            float c[3];
            outputKernel(pic, cx, cy, std::max(sx, 1.f), std::max(sy, 1.f), q.filter == int32_t(ScaleFilter::Lanczos), c);
            outputEncode(q, c, rgb);
            return;
        }
        const int kx = std::max(1, int(std::ceil(sx - 1e-4f))), ky = std::max(1, int(std::ceil(sy - 1e-4f)));
        float sum[3] = {0, 0, 0};
        for (int j = 0; j < ky; ++j)
            for (int i = 0; i < kx; ++i) {
                const float fx = std::clamp((float(x) + (float(i) + 0.5f) / float(kx)) * sx - 0.5f, 0.f, float(W - 1));
                const float fy = std::clamp((float(y) + (float(j) + 0.5f) / float(ky)) * sy - 0.5f, 0.f, float(H - 1));
                const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
                const float ax = fx - float(x0), ay = fy - float(y0);
                for (int k = 0; k < 3; ++k) {
                    const float top = pic.px(x0, y0)[k] + (pic.px(x1, y0)[k] - pic.px(x0, y0)[k]) * ax;
                    const float bottom = pic.px(x0, y1)[k] + (pic.px(x1, y1)[k] - pic.px(x0, y1)[k]) * ax;
                    sum[k] += top + (bottom - top) * ay;
                }
            }
        const float mean[3] = {sum[0] / float(kx * ky), sum[1] / float(kx * ky), sum[2] / float(kx * ky)};
        outputEncode(q, mean, rgb);
    }

    bool readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) override {
        return outputOf(m_canvas, colour, out, width, height);
    }
    bool readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) override {
        const auto it = m_sources.find(source);
        return it != m_sources.end() && outputOf(it->second.shown(), colour, out, width, height);
    }
    static bool outputOf(const FrameF &pic, const OutputColour &colour, YuvFrame &out, int width, int height) {
        if (pic.width == 0) return false;
        const int w = width > 0 ? width : pic.width, h = height > 0 ? height : pic.height;
        const ColourFormat format = colour.format;
        if (out.width != w || out.height != h || out.format != format) out.resize(w, h, format);
        const OutputParams q = outputParams(colour);
        const int top = maxCode(format), bytes = out.sampleBytes();
        const int shift = format == ColourFormat::P010 ? 6 : 0;
        auto put = [&](std::vector<uint8_t> &plane, int rowBytes, int x, int y, int code) {
            const size_t at = size_t(y) * rowBytes + size_t(x) * bytes;
            if (bytes == 1) { plane[at] = uint8_t(code); return; }
            const int v = code << shift;
            plane[at] = uint8_t(v & 255);
            plane[at + 1] = uint8_t(v >> 8);
        };
        const int yRow = out.rowBytes(0), cRow = out.rowBytes(1), vRow = out.rowBytes(2);
        const bool rgb = formatIsRgb(format), halfW = chromaHalfWidth(format), halfH = chromaHalfHeight(format);
        const bool interleaved = chromaInterleaved(format);
        const int bw = halfW ? 2 : 1, bh = halfH ? 2 : 1;
        parallelRows(0, (h + bh - 1) / bh, [&](int firstBlock, int lastBlock) {
        for (int by = firstBlock * bh; by < lastBlock * bh && by < h; by += bh) {
            for (int bx = 0; bx < w; bx += bw) {
                float sr = 0, sg = 0, sb = 0;
                for (int dy = 0; dy < bh; ++dy) {
                    for (int dx = 0; dx < bw; ++dx) {
                        float c[3];
                        outputEncoded(pic, q, bx + dx, by + dy, w, h, c);
                        const float r = c[0], g = c[1], b = c[2];
                        sr += r; sg += g; sb += b;
                        if (bx + dx >= w || by + dy >= h) continue;
                        if (rgb) {
                            uint8_t *px = &out.y[size_t(by + dy) * yRow + size_t(bx + dx) * 4];
                            px[0] = uint8_t(outputCode(0.f, q.yScale, b, 255));
                            px[1] = uint8_t(outputCode(0.f, q.yScale, g, 255));
                            px[2] = uint8_t(outputCode(0.f, q.yScale, r, 255));
                            px[3] = 255;
                            continue;
                        }
                        const float yy = q.kr * r + q.kg * g + q.kb * b;
                        put(out.y, yRow, bx + dx, by + dy, outputCode(q.yOffset, q.yScale, yy, top));
                    }
                }
                if (rgb) continue;
                const float inv = 1.f / float(bw * bh);
                const float r = sr * inv, g = sg * inv, b = sb * inv;
                const float yy = q.kr * r + q.kg * g + q.kb * b;
                const float cb = (b - yy) / q.cbDiv;
                const float cr = (r - yy) / q.crDiv;
                const int cx = bx / bw, cy = by / bh;
                if (interleaved) {
                    put(out.u, cRow, cx * 2, cy, outputCode(q.cOffset, q.cScale, cb, top));
                    put(out.u, cRow, cx * 2 + 1, cy, outputCode(q.cOffset, q.cScale, cr, top));
                } else {
                    put(out.u, cRow, cx, cy, outputCode(q.cOffset, q.cScale, cb, top));
                    put(out.v, vRow, cx, cy, outputCode(q.cOffset, q.cScale, cr, top));
                }
            }
        }
        });
        return true;
    }

private:
    // A source: its converted frame and, with noise reduction on, two cleaned
    // frames used in turn (the other is the history), as in vk_engine.cpp.
    struct CpuSource {
        FrameF work, previous, fieldFrame;
        DeinterlaceMode deinterlace = DeinterlaceMode::Off;
        bool hasFrame = false, previousValid = false, resetNext = false, fieldReady = false, pendingProcess = false;
        int field = 0;
        bool firstField = true;
        FrameF clean[2];
        int cleanIndex = 0;
        float denoise = 0.f;
        bool historyValid = false;
        const FrameF &processed() const { return deinterlace != DeinterlaceMode::Off && fieldReady ? fieldFrame : work; }
        const FrameF &base() const { return denoise > 0.f && historyValid ? clean[cleanIndex] : processed(); }
        // Video filters: one picture per step, or a delay's held pictures,
        // oldest first, the shown one at the front.
        struct Step {
            FrameF out, mid, mid2;  // mid: a two-pass step's first pass, or the matte; mid2: a third pass's
            FrameF shaped;           // the step kept within its shape (VideoFilter::within)
            bool within = false;
            matting::Plan plan;      // RemoveBackground's network and its working space,
            std::vector<float> arena;
            FrameF guide[4], back[2], cut;  // after the network, as recordGuided in vk_engine.cpp
            std::vector<FrameF> regs;  // a grade plan's pictures
            std::vector<Step> people;  // its People operations', each a RemoveBackground of its own
            std::deque<std::pair<FrameF, double>> held;
            double origin = 0;
            bool started = false;
            uint64_t pushed = ~0ull;
            const FrameF &output() const { return !held.empty() ? held.front().first : within ? shaped : out; }
        };
        std::vector<VideoFilter> filters;
        std::vector<Step> steps;
        bool filtered = false;
        uint64_t generation = 0, filteredGeneration = ~0ull;
        const FrameF &shown() const { return filtered && !steps.empty() ? steps.back().output() : base(); }
    };

    static void beginFrame(CpuSource &s, int w, int h) {
        s.previousValid = s.deinterlace != DeinterlaceMode::Off && s.hasFrame && !s.resetNext && s.work.width == w && s.work.height == h;
        if (s.deinterlace != DeinterlaceMode::Off) std::swap(s.work, s.previous);
        s.resetNext = false; s.fieldReady = false;
    }
    static void finishFrame(CpuSource &s) {
        s.hasFrame = true; s.pendingProcess = true; ++s.generation;
        if (s.deinterlace == DeinterlaceMode::Off) { denoise(s); s.pendingProcess = false; }
    }

    // Independently authored spatial/temporal filter; deinterlace.comp follows
    // this arithmetic. The retained field is exact. Motion uses interpolation
    // along the least-changing local edge; still areas retain both fields.
    static void deinterlace(CpuSource &s, int parity, bool first) {
        const auto &cur = s.work;
        auto &out = s.fieldFrame;
        if (out.width != cur.width || out.height != cur.height) out.resize(cur.width, cur.height);
        auto rows = [](int y, int height, int field) {
            field = std::min(field, height-1);
            const int last = field + 2*((height-1-field)/2);
            return std::array<int,2>{std::clamp(y-1, field, last), std::clamp(y+1, field, last)};
        };
        auto fieldSample = [&](const FrameF &frame, int x, int y, int field) {
            std::array<float,4> result{};
            field = std::min(field, frame.height-1);
            if ((y & 1) == field) std::copy_n(frame.px(x,y),4,result.begin());
            else {
                const auto r = rows(y,frame.height,field);
                for (int c=0;c<4;++c) result[c] = .5f*(frame.px(x,r[0])[c]+frame.px(x,r[1])[c]);
            }
            return result;
        };
        parallelRows(0, cur.height, [&](int firstRow, int lastRow) {
        for (int y=firstRow;y<lastRow;++y) for (int x=0;x<cur.width;++x) {
            auto spatial = fieldSample(cur,x,y,parity);
            const auto r = rows(y,cur.height,parity);
            if (s.deinterlace == DeinterlaceMode::Discard) {
                const int selected = std::min(parity,cur.height-1);
                const int last = selected+2*((cur.height-1-selected)/2);
                const int row = std::clamp((y/2)*2+selected,selected,last);
                std::copy_n(cur.px(x,row),4,spatial.begin());
            } else if (s.deinterlace == DeinterlaceMode::Blend) {
                const auto &neighbour = first && s.previousValid ? s.previous : cur;
                const auto other = fieldSample(neighbour,x,y,1-parity);
                for (int c=0;c<4;++c) spatial[c] = .5f*(spatial[c]+other[c]);
            } else if (s.deinterlace == DeinterlaceMode::Adaptive && (y&1) != parity) {
                float best = 1e10f;
                // Zero slope wins ties; a small slope penalty avoids jitter.
                for (int dx : {0,-1,1,-2,2}) {
                    float score = .001f * std::abs(dx);
                    for (int tap=-1;tap<=1;++tap) for (int c=0;c<3;++c)
                        score += std::abs(cur.px(std::clamp(x+tap-dx,0,cur.width-1),r[0])[c] -
                                          cur.px(std::clamp(x+tap+dx,0,cur.width-1),r[1])[c]);
                    if (score < best) {
                        best = score;
                        for (int c=0;c<4;++c) spatial[c] = .5f*(cur.px(std::clamp(x-dx,0,cur.width-1),r[0])[c] +
                                                                             cur.px(std::clamp(x+dx,0,cur.width-1),r[1])[c]);
                    }
                }
                if (s.previousValid) {
                    float change = 0;
                    for (int dy=-2;dy<=2;++dy) for (int dx=-1;dx<=1;++dx) for (int c=0;c<4;++c) {
                        const int xx=std::clamp(x+dx,0,cur.width-1), yy=std::clamp(y+dy,0,cur.height-1);
                        change = std::max(change,std::abs(cur.px(xx,yy)[c]-s.previous.px(xx,yy)[c]));
                    }
                    const float motion = std::clamp((change-.01f)/.03f,0.f,1.f);
                    for (int c=0;c<4;++c) spatial[c] = cur.px(x,y)[c] + motion*(spatial[c]-cur.px(x,y)[c]);
                }
            }
            std::copy(spatial.begin(),spatial.end(),out.px(x,y));
        }
        });
        s.fieldReady = true; s.field = parity; s.firstField = first;
    }

    // The same arithmetic as denoise.comp.
    static void denoise(CpuSource &s) {
        if (s.denoise <= 0.f) {
            s.historyValid = false;
            return;
        }
        const FrameF &cur = s.processed();
        const int next = 1 - s.cleanIndex;
        for (FrameF &f : s.clean)
            if (f.width != cur.width || f.height != cur.height) {
                f.resize(cur.width, cur.height);
                s.historyValid = false;
            }
        const DenoiseParams dp = denoiseParams(s.denoise);
        const FrameF &hist = s.clean[s.cleanIndex];
        FrameF &out = s.clean[next];
        auto luma = [](const float *c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
        parallelRows(0, cur.height, [&](int firstRow, int lastRow) {
        for (int y = firstRow; y < lastRow; ++y)
            for (int x = 0; x < cur.width; ++x) {
                const float *c = cur.px(x, y);
                const float yc = luma(c), gc = std::sqrt(std::max(yc, 0.f));
                float cs[3] = {0, 0, 0}, ws = 0, change = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int qx = std::clamp(x + dx, 0, cur.width - 1), qy = std::clamp(y + dy, 0, cur.height - 1);
                        const float *n = cur.px(qx, qy);
                        const float yn = luma(n);
                        const float gn = std::sqrt(std::max(yn, 0.f));
                        const float dg = (gn - gc) / dp.sigma;
                        const float w = std::exp(-dg * dg);
                        for (int k = 0; k < 3; ++k) cs[k] += w * (n[k] - yn);
                        ws += w;
                        if (s.historyValid && std::abs(dx) <= 1 && std::abs(dy) <= 1)
                            change += gn - std::sqrt(std::max(luma(hist.px(qx, qy)), 0.f));
                    }
                float clean[3];
                for (int k = 0; k < 3; ++k) clean[k] = yc + cs[k] / ws;
                if (s.historyValid) {
                    const float *h = hist.px(x, y);
                    const float keep = std::clamp(std::fabs(change / 9.f) / dp.threshold, dp.keepMin, 1.f);
                    for (int k = 0; k < 3; ++k) clean[k] = h[k] + keep * (clean[k] - h[k]);
                }
                float *o = out.px(x, y);
                for (int k = 0; k < 3; ++k) o[k] = std::max(clean[k], 0.f);
                o[3] = c[3];
            }
        });
        s.cleanIndex = next;
        s.historyValid = true;
    }

    static void runStep(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux = nullptr);
    static bool findPeople(const FrameF &cur, CpuSource::Step &st, const VideoFilter &f, bool fresh);
    // One pass of an effect, by its CPU twin (effects/*.cpp), over the cores.
    static void runEffect(const FrameF &in, FrameF &out, const FrameF &aux, const VideoFilter &f, const FilterParams &q) {
        const fx::Pass pass{in, aux, out, q, q.mode, q.ix};
        parallelRows(0, out.height, [&](int y0, int y1) {
            switch (f.effect) {
#define EFFECT(Name, stem, passes) case Effect::Name: fx::fx_##stem(pass, y0, y1); break;
#include "effects/effects.def"
#undef EFFECT
            case Effect::Count: break;
            }
        });
    }

    std::unordered_map<int, CpuSource> m_sources;
    FrameF m_scratch;  // compositeInto / transitionInto draw here, then swap it with the target's picture
    std::unordered_map<int, double> m_filterMs;  // each chain's time, smoothed
    HdrLevels m_levels;
    FrameF m_canvas;
    int m_nextId = 1;
};

// The same arithmetic as filter.comp, on the same FilterParams.
void CpuEngine::runStep(const FrameF &in, FrameF &out, const VideoFilter &f, const FilterParams &q, const FrameF *aux) {
    const float *P = q.p;
    const int W = in.width, H = in.height;
    auto at = [&](int x, int y) { return in.px(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); };
    auto assoc = [](const float *c) { return Sampled{c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]}; };
    auto ramp = [](float t) { return t * t * (3.f - 2.f * t); };
    auto gauss = [&](int k) { const float x = float(k) / P[0]; return std::exp(-0.5f * x * x); };
    auto boxTap = [&](int i) { return std::clamp(float(q.iy) * 0.5f + 0.5f - std::abs(float(i)), 0.f, 1.f); };
    auto clamp01 = [](float v) { return std::clamp(v, 0.f, 1.f); };
    auto lutAt = [&](int r, int g, int b, int k) { return f.lut->rgb[((size_t(b) * q.ix + g) * q.ix + r) * 3 + k]; };
    auto scrollTap = [&](int x, int y) {
        if (q.mode == 1) { x = (x + 2 * W) % W; y = (y + 2 * H) % H; }
        else if (x < 0 || y < 0 || x >= W || y >= H) return Sampled{0, 0, 0, 0};
        return assoc(in.px(x, y));
    };
    parallelRows(0, out.height, [&](int first, int last) {
    for (int y = first; y < last; ++y)
        for (int x = 0; x < out.width; ++x) {
            const float *c = in.px(std::min(x, W - 1), std::min(y, H - 1));
            float o[4] = {c[0], c[1], c[2], c[3]};
            switch (q.kind) {
            case 1: {
                float e[3];
                for (int k = 0; k < 3; ++k) e[k] = std::pow(linearToSrgb(c[k]), P[3]);
                for (int k = 0; k < 3; ++k)
                    o[k] = srgbToLinear(clamp01(P[k * 4] * e[0] + P[k * 4 + 1] * e[1] + P[k * 4 + 2] * e[2] + P[12 + k]));
                o[3] = c[3] * P[7];
                break;
            }
            case 2: {
                const float k = P[0];
                const Sampled n = assoc(at(x, y - 1)), s = assoc(at(x, y + 1)), w = assoc(at(x - 1, y)), e = assoc(at(x + 1, y)), m = assoc(c);
                Sampled r{m.r * (1.f + 4.f * k) - (((n.r + s.r) + w.r) + e.r) * k, m.g * (1.f + 4.f * k) - (((n.g + s.g) + w.g) + e.g) * k,
                          m.b * (1.f + 4.f * k) - (((n.b + s.b) + w.b) + e.b) * k, m.a * (1.f + 4.f * k) - (((n.a + s.a) + w.a) + e.a) * k};
                r.a = clamp01(r.a);
                r.r = std::clamp(r.r, 0.f, r.a); r.g = std::clamp(r.g, 0.f, r.a); r.b = std::clamp(r.b, 0.f, r.a);
                r = straightSample(r);
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 3: {
                // chromaKeyPixel: word for word as kind 3 in filter.comp.
                auto lumaOf = [](const float *v) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; };
                const float yp = std::sqrt(std::max(lumaOf(c), 0.f));
                float sum[3] = {0, 0, 0}, wsum = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const float *n = at(x + i * 2, y + j * 2);
                        const float dyp = (std::sqrt(std::max(lumaOf(n), 0.f)) - yp) * P[14];
                        const float w = std::exp(-dyp * dyp);
                        for (int k = 0; k < 3; ++k) sum[k] += w * n[k];
                        wsum += w;
                    }
                float ea[3], e[3];
                for (int k = 0; k < 3; ++k) { ea[k] = linearToSrgb(sum[k] / wsum); e[k] = linearToSrgb(c[k]); }
                const float yy = lumaOf(e), ya = lumaOf(ea);
                const float cb = (ea[2] - ya) / 1.8556f, cr = (ea[0] - ya) / 1.5748f;
                const float d = cb * P[0] + cr * P[1], q = std::fabs(cr * P[0] - cb * P[1]);
                const float a = clamp01((P[3] - (d - P[5] * q) * P[2]) * P[4]);
                const float w = std::min((1.f - a) * 4.f, 1.f);
                const float ob = (e[2] - yy) / 1.8556f, orr = (e[0] - yy) / 1.5748f;
                const float mb = ob + (cb - ob) * w, mr = orr + (cr - orr) * w;
                float m[3] = {yy + 1.5748f * mr, 0, yy + 1.8556f * mb};
                m[1] = (yy - 0.2126f * m[0] - 0.0722f * m[2]) / 0.7152f;
                float fe[3];
                for (int k = 0; k < 3; ++k)
                    fe[k] = linearToSrgb(clamp01((srgbToLinear(clamp01(m[k])) - (1.f - a) * P[8 + k]) / std::max(a, 1e-3f)));
                const float fy = lumaOf(fe);
                float fb = (fe[2] - fy) / 1.8556f, fr = (fe[0] - fy) / 1.5748f;
                const float df = fb * P[0] + fr * P[1];
                const float nd = std::max(df, std::min(mb * P[0] + mr * P[1], 0.f));
                fb += P[0] * (nd - df);
                fr += P[1] * (nd - df);
                const float qf = std::fabs(fr * P[0] - fb * P[1]);
                const float s = std::max(nd - P[7] * a * a * qf, 0.f) * P[2] * P[6];
                float r[3] = {fy + 1.5748f * fr, 0, fy + 1.8556f * fb};
                r[1] = (fy - 0.2126f * r[0] - 0.0722f * r[2]) / 0.7152f;
                for (int k = 0; k < 3; ++k) o[k] = srgbToLinear(clamp01(r[k] - s * P[11 + k]));
                o[3] = c[3] * a;
                if (P[15] > 0.5f) std::copy_n(c, 3, o);  // the matte only: the picture's own colour
                break;
            }
            case 4: {
                const float dr = linearToSrgb(c[0]) - P[0], dg = linearToSrgb(c[1]) - P[1], db = linearToSrgb(c[2]) - P[2];
                o[3] = c[3] * ramp(clamp01((std::sqrt(dr * dr + dg * dg + db * db) * 0.577350269f - P[3]) / P[4]));
                break;
            }
            case 5: {
                const float yy = 0.2126f * linearToSrgb(c[0]) + 0.7152f * linearToSrgb(c[1]) + 0.0722f * linearToSrgb(c[2]);
                o[3] = c[3] * clamp01((yy - P[0] + P[2]) / P[2]) * clamp01((P[1] + P[3] - yy) / P[3]);
                break;
            }
            case 6: {
                float e[3], xx[3], fr[3], l[3];
                int i[3];
                const int n = q.ix;
                for (int k = 0; k < 3; ++k) {
                    e[k] = linearToSrgb(c[k]);
                    // A place that is not a number (a table whose domain has
                    // no end) is the table's start, never an index outside it.
                    const float placed = e[k] * P[k] + P[4 + k];
                    xx[k] = placed > 0.f ? std::min(placed, 1.f) : 0.f;
                    const float pos = xx[k] * float(n - 1);
                    i[k] = std::clamp(int(std::floor(pos)), 0, n - 2);
                    fr[k] = pos - float(i[k]);
                }
                for (int k = 0; k < 3; ++k) {
                    if (q.mode == 2) {
                        const float a = f.lut->rgb[size_t(i[k]) * 3 + k], b = f.lut->rgb[size_t(i[k] + 1) * 3 + k];
                        l[k] = a + (b - a) * fr[k];
                        continue;
                    }
                    const int r = i[0], g = i[1], b = i[2];
                    const float fR = fr[0], fG = fr[1], fB = fr[2];
                    const float c000 = lutAt(r, g, b, k), c111 = lutAt(r + 1, g + 1, b + 1, k);
                    if (q.mode == 1) {
                        if (fR > fG) {
                            if (fG > fB) l[k] = (1.f - fR) * c000 + (fR - fG) * lutAt(r + 1, g, b, k) + (fG - fB) * lutAt(r + 1, g + 1, b, k) + fB * c111;
                            else if (fR > fB) l[k] = (1.f - fR) * c000 + (fR - fB) * lutAt(r + 1, g, b, k) + (fB - fG) * lutAt(r + 1, g, b + 1, k) + fG * c111;
                            else l[k] = (1.f - fB) * c000 + (fB - fR) * lutAt(r, g, b + 1, k) + (fR - fG) * lutAt(r + 1, g, b + 1, k) + fG * c111;
                        } else {
                            if (fB > fG) l[k] = (1.f - fB) * c000 + (fB - fG) * lutAt(r, g, b + 1, k) + (fG - fR) * lutAt(r, g + 1, b + 1, k) + fR * c111;
                            else if (fB > fR) l[k] = (1.f - fG) * c000 + (fG - fB) * lutAt(r, g + 1, b, k) + (fB - fR) * lutAt(r, g + 1, b + 1, k) + fR * c111;
                            else l[k] = (1.f - fG) * c000 + (fG - fR) * lutAt(r, g + 1, b, k) + (fR - fB) * lutAt(r + 1, g + 1, b, k) + fB * c111;
                        }
                    } else {
                        const float c100 = lutAt(r + 1, g, b, k), c010 = lutAt(r, g + 1, b, k), c110 = lutAt(r + 1, g + 1, b, k);
                        const float c001 = lutAt(r, g, b + 1, k), c101 = lutAt(r + 1, g, b + 1, k), c011 = lutAt(r, g + 1, b + 1, k);
                        const float x00 = c000 + (c100 - c000) * fR, x10 = c010 + (c110 - c010) * fR;
                        const float x01 = c001 + (c101 - c001) * fR, x11 = c011 + (c111 - c011) * fR;
                        const float y0 = x00 + (x10 - x00) * fG, y1 = x01 + (x11 - x01) * fG;
                        l[k] = y0 + (y1 - y0) * fB;
                    }
                }
                for (int k = 0; k < 3; ++k) o[k] = srgbToLinear(clamp01(e[k] + (l[k] - e[k]) * P[3]));
                break;
            }
            case 7: {
                const float mx = (x + 0.5f) / float(out.width) * float(q.ix) - 0.5f, my = (y + 0.5f) / float(out.height) * float(q.iy) - 0.5f;
                const int ix = int(std::floor(mx)), iy = int(std::floor(my));
                const float fx = mx - float(ix), fy = my - float(iy);
                auto tap = [&](int tx, int ty, int k) {
                    tx = std::clamp(tx, 0, q.ix - 1); ty = std::clamp(ty, 0, q.iy - 1);
                    return f.image->rgba[(size_t(ty) * q.ix + tx) * 4 + k] / 255.f;
                };
                float v[4];
                for (int k = 0; k < 4; ++k) {
                    const float a = tap(ix, iy, k), b = tap(ix + 1, iy, k), d = tap(ix, iy + 1, k), e = tap(ix + 1, iy + 1, k);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    v[k] = top + (bottom - top) * fy;
                }
                const float k = P[3];
                if (q.mode == 0) o[3] = c[3] * (1.f + (v[3] - 1.f) * k);
                else if (q.mode == 1) o[3] = c[3] * (1.f + ((0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]) - 1.f) * k);
                else if (q.mode == 5) {
                    const float y = v[2];
                    const float still = std::clamp(1.f - (std::abs(v[0] - y) + std::abs(v[1] - y)) * 8.f, 0.f, 1.f);
                    o[3] = c[3] + (v[3] - c[3]) * (k * still * (2.f / 3.f));
                } else {
                    const float w = v[3] * k;
                    for (int j = 0; j < 3; ++j) {
                        const float mk = srgbToLinear(v[j]) * P[j];
                        o[j] = q.mode == 2 ? c[j] + (c[j] * mk - c[j]) * w : q.mode == 3 ? c[j] + mk * w : std::max(c[j] - mk * w, 0.f);
                    }
                }
                break;
            }
            case 8: {
                const int sx = x + q.ix, sy = y + q.iy;
                if (sx >= 0 && sy >= 0 && sx < W && sy < H) std::copy_n(in.px(sx, sy), 4, o);
                else std::fill_n(o, 4, 0.f);
                break;
            }
            case 9: {
                const float sx = float(x) - P[0], sy = float(y) - P[1];
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const float fx = sx - float(ix), fy = sy - float(iy);
                const Sampled a = scrollTap(ix, iy), b = scrollTap(ix + 1, iy), d = scrollTap(ix, iy + 1), e = scrollTap(ix + 1, iy + 1);
                auto mix = [&](float pa, float pb, float pd, float pe) {
                    const float top = pa + (pb - pa) * fx, bottom = pd + (pe - pd) * fx;
                    return top + (bottom - top) * fy;
                };
                const Sampled r = straightSample({mix(a.r, b.r, d.r, e.r), mix(a.g, b.g, d.g, e.g), mix(a.b, b.b, d.b, e.b), mix(a.a, b.a, d.a, e.a)});
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 10: {
                float sum[4] = {0, 0, 0, 0}, wsum = 0;
                if (q.mode == 4) {
                    const int h = q.iy / 2;
                    for (int j = -h; j <= h; ++j)
                        for (int i = -h; i <= h; ++i) {
                            const float w = boxTap(i) * boxTap(j);
                            const Sampled s = assoc(at(x + i, y + j));
                            sum[0] += w * s.r; sum[1] += w * s.g; sum[2] += w * s.b; sum[3] += w * s.a;
                            wsum += w;
                        }
                } else for (int k = -q.ix; k <= q.ix; ++k) {
                    const float w = gauss(k);
                    const float *t = q.mode == 0 || q.mode == 2 ? at(x + k * q.iy, y) : at(x, y + k * q.iy);
                    const Sampled s = q.mode == 0 ? assoc(t) : Sampled{t[0], t[1], t[2], t[3]};
                    sum[0] += w * s.r; sum[1] += w * s.g; sum[2] += w * s.b; sum[3] += w * s.a;
                    wsum += w;
                }
                Sampled r{sum[0] / wsum, sum[1] / wsum, sum[2] / wsum, sum[3] / wsum};
                if (q.mode == 1 || q.mode == 4) r = straightSample(r);
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 11: {
                const int dx = int(P[1]), dy = int(P[2]);
                float sum = 0, wsum = 0;
                if (q.mode == 2) {
                    const int h = q.iy / 2;
                    for (int j = -h; j <= h; ++j)
                        for (int i = -h; i <= h; ++i) {
                            const float w = boxTap(i) * boxTap(j);
                            const int qx = x - dx + i, qy = y - dy + j;
                            const bool inside = qx >= 0 && qy >= 0 && qx < W && qy < H;
                            sum += w * (inside ? in.px(qx, qy)[3] : 0.f);
                            wsum += w;
                        }
                } else for (int k = -q.ix; k <= q.ix; ++k) {
                    const float w = gauss(k);
                    const int qx = q.mode == 0 ? x - dx + k * q.iy : x, qy = q.mode == 0 ? y - dy : y + k * q.iy;
                    const bool inside = qx >= 0 && qy >= 0 && qx < W && qy < H;
                    sum += w * (inside ? (q.mode == 0 ? in.px(qx, qy)[3] : aux->px(qx, qy)[3]) : 0.f);
                    wsum += w;
                }
                const float s = sum / wsum;
                if (q.mode != 1) { o[0] = o[1] = o[2] = 0.f; o[3] = s; break; }
                const float sh = s * P[3], a = c[3], oa = a + sh * (1.f - a);
                for (int k = 0; k < 3; ++k) o[k] = oa > 0.f ? (c[k] * a + P[4 + k] * (sh * (1.f - a))) / oa : 0.f;
                o[3] = oa > 0.f ? oa : 0.f;
                break;
            }
            case 12: {
                // Hue kept: the brightest channel follows the curve, the others in proportion.
                const float m = std::max(std::max(c[0], c[1]), c[2]);
                const float k = m > 0.f ? toneMapValue(q.mode, P, m) / m : 0.f;
                for (int j = 0; j < 3; ++j) o[j] = std::max(c[j], 0.f) * k;
                break;
            }
            case 13:
                for (int j = 0; j < 3; ++j) o[j] = c[j] * P[0];
                break;
            case 15: {
                float e[3];
                for (int k = 0; k < 3; ++k) e[k] = clamp01(linearToSrgb(c[k]));
                if (q.mode & 1)
                    for (int k = 0; k < 3; ++k) e[k] = gradeTableAt((*f.grade->channels)[size_t(k)], e[k]);
                if (q.mode & 2) {
                    const float r = e[0], g = e[1], b = e[2];
                    for (int k = 0; k < 3; ++k) e[k] = P[k * 3] * r + P[k * 3 + 1] * g + P[k * 3 + 2] * b;
                }
                for (int k = 0; k < 3; ++k) o[k] = srgbToLinear(clamp01(e[k]));
                break;
            }
            case 34: {
                const float m = shapeCover(q, x + 0.5f, y + 0.5f);
                const Sampled was = assoc(aux->px(std::min(x, W - 1), std::min(y, H - 1))), now = assoc(c);
                const Sampled r = straightSample({was.r + (now.r - was.r) * m, was.g + (now.g - was.g) * m, was.b + (now.b - was.b) * m, was.a + (now.a - was.a) * m});
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 14: {
                const float qx = x + 0.5f, qy = y + 0.5f;
                const float d = std::min(std::min(qx - P[0], P[2] - qx), std::min(qy - P[1], P[3] - qy));
                const float a = P[4] > 0.f ? clamp01(d / P[4]) : (d >= 0.f ? 1.f : 0.f);
                o[3] = c[3] * (q.mode == 1 ? 1.f - a : a);
                break;
            }
            case 24: {
                const float mx = (x + 0.5f) / float(out.width) * float(q.ix) - 0.5f, my = (y + 0.5f) / float(out.height) * float(q.iy) - 0.5f;
                const int ix = int(std::floor(mx)), iy = int(std::floor(my));
                const float fx = mx - float(ix), fy = my - float(iy);
                float ab[3];
                for (int k = 0; k < 3; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    ab[k] = top + (bottom - top) * fy;
                }
                const float l = 0.2126f * linearToSrgb(c[0]) + 0.7152f * linearToSrgb(c[1]) + 0.0722f * linearToSrgb(c[2]);
                const float guided = clamp01(ab[0] * l + ab[1]);
                const float v = ab[2] + (guided - ab[2]) * clamp01(P[2] * ab[2] * (1.f - ab[2]));
                o[3] = c[3] * clamp01((v - P[0]) * P[1]);
                break;
            }
            case 27: {
                const int x0 = x * W / out.width, x1 = std::max(x0 + 1, ((x + 1) * W + out.width - 1) / out.width);
                const int y0 = y * H / out.height, y1 = std::max(y0 + 1, ((y + 1) * H + out.height - 1) / out.height);
                float sum = 0, squares = 0;
                for (int j = y0; j < y1; ++j)
                    for (int i = x0; i < x1; ++i) {
                        const float *t = in.px(std::min(i, W - 1), std::min(j, H - 1));
                        const float l = 0.2126f * linearToSrgb(t[0]) + 0.7152f * linearToSrgb(t[1]) + 0.0722f * linearToSrgb(t[2]);
                        sum += l;
                        squares += l * l;
                    }
                const float n = float((x1 - x0) * (y1 - y0)), bright = sum / n, mt = aux->px(x, y)[0];
                o[0] = bright; o[1] = mt; o[2] = squares / n; o[3] = bright * mt;
                break;
            }
            case 28: {
                const float a = (c[3] - c[0] * c[1]) / (c[2] - c[0] * c[0] + P[0]);
                o[0] = a; o[1] = c[1] - a * c[0]; o[2] = 0; o[3] = 1;
                break;
            }
            case 32: {
                o[0] = c[0]; o[1] = c[1]; o[2] = aux->px(x, y)[1]; o[3] = 1;
                break;
            }
            case 30: {
                Sampled sum{0, 0, 0, 0};
                for (int j = 0; j < q.ix; ++j)
                    for (int i = 0; i < q.ix; ++i) {
                        const Sampled s = assoc(in.px(std::min(x * q.ix + i, W - 1), std::min(y * q.ix + j, H - 1)));
                        sum.r += s.r; sum.g += s.g; sum.b += s.b; sum.a += s.a;
                    }
                const float n = float(q.ix * q.ix);
                const Sampled r = straightSample({sum.r / n, sum.g / n, sum.b / n, sum.a / n});
                o[0] = r.r; o[1] = r.g; o[2] = r.b; o[3] = r.a;
                break;
            }
            case 31: {
                const float mx = (x + 0.5f) * P[0] - 0.5f, my = (y + 0.5f) * P[0] - 0.5f;
                const int ix = int(std::floor(mx)), iy = int(std::floor(my));
                const float fx = mx - float(ix), fy = my - float(iy);
                float back[4];
                for (int k = 0; k < 4; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    back[k] = top + (bottom - top) * fy;
                }
                const float oa = c[3] + back[3] * (1.f - c[3]);
                for (int k = 0; k < 3; ++k) o[k] = oa > 0.f ? (c[k] * c[3] + back[k] * back[3] * (1.f - c[3])) / oa : 0.f;
                o[3] = oa > 0.f ? oa : 0.f;
                break;
            }
            case 25: {
                const int n = int(P[0]);
                float light[3] = {0, 0, 0}, open = 0;
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i) {
                        const int tx = x * n + i, ty = y * n + j;
                        const float u = 1.f - aux->px(std::min(tx, aux->width - 1), std::min(ty, aux->height - 1))[3];
                        const float *t = in.px(std::clamp(tx + q.ix, 0, W - 1), std::clamp(ty + q.iy, 0, H - 1));
                        for (int k = 0; k < 3; ++k) light[k] += t[k] * t[3] * u;
                        open += u;
                    }
                for (int k = 0; k < 3; ++k) o[k] = open > 0.f ? light[k] / open : 0.f;
                o[3] = open / float(n * n);
                break;
            }
            case 26: {
                const float mx = (x + 0.5f) * P[1] - 0.5f, my = (y + 0.5f) * P[1] - 0.5f;
                const int ix = int(std::floor(mx)), iy = int(std::floor(my));
                const float fx = mx - float(ix), fy = my - float(iy);
                float v[4];
                for (int k = 0; k < 4; ++k) {
                    auto tap = [&](int tx, int ty) { return aux->px(std::clamp(tx, 0, q.ix - 1), std::clamp(ty, 0, q.iy - 1))[k]; };
                    const float a = tap(ix, iy), b = tap(ix + 1, iy), d = tap(ix, iy + 1), e = tap(ix + 1, iy + 1);
                    const float top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                    v[k] = top + (bottom - top) * fy;
                }
                const float edge = std::min(v[3] * 2.f, 1.f);
                for (int k = 0; k < 3; ++k)
                    o[k] = q.mode == 1 ? c[k] + (v[k] - c[k]) * clamp01(edge * P[0]) : c[k] + v[k] * edge * P[0];
                break;
            }
            case 33: {
                for (int k = 0; k < 3; ++k) {
                    const float m = std::fabs(c[k]);
                    const float v = q.mode == 0 ? srgbToLinear(m) : m <= 0.0031308f ? m * 12.92f : 1.055f * std::pow(m, 1.f / 2.4f) - 0.055f;
                    o[k] = c[k] < 0.f ? -v : v;
                }
                break;
            }
            default: break;
            }
            std::copy_n(o, 4, out.px(x, y));
        }
    });
}

// RemoveBackground on `cur` into the step's out. False, with nothing drawn,
// when there is no model: the caller decides what that means.
bool CpuEngine::findPeople(const FrameF &cur, CpuSource::Step &st, const VideoFilter &f, bool fresh) {
    // The network runs on new pictures only; a missing model leaves the picture as it was.
    const FilterParams q = filterParams(f, cur.width, cur.height, 0);
    const matting::Model &m = matting::model();
    const bool samePlan = st.plan.ok && st.plan.width == q.ix && st.plan.height == q.iy &&
                          st.plan.passes.front().inW == cur.width && st.plan.passes.front().inH == cur.height;
    if (!samePlan) { st.plan = matting::plan(m, q.ix, q.iy, cur.width, cur.height, 1); st.mid = {}; }
    if (!st.plan.ok) return false;
    if (fresh || st.mid.width != q.ix || st.mid.height != q.iy) {
        matting::run(st.plan, m, cur, st.arena);
        if (st.mid.width != q.ix || st.mid.height != q.iy) st.mid.resize(q.ix, q.iy);
        for (int i = 0; i < q.ix * q.iy; ++i) std::fill_n(&st.mid.rgba[size_t(i) * 4], 4, st.arena[size_t(st.plan.matte) + i]);
    }
    // As recordGuided in vk_engine.cpp.
    const int w = cur.width, h = cur.height;
    for (FrameF &img : st.guide)
        if (img.width != q.ix || img.height != q.iy) img.resize(q.ix, q.iy);
    FrameF &now = st.guide[0], &spare = st.guide[1], &blurred = st.guide[2], &fit = st.guide[3];
    auto pass = [&](MattePass p) { return matteParams(f, p, w, h); };
    runStep(cur, now, f, pass(MattePass::Stats), &st.mid);
    runStep(now, spare, f, pass(MattePass::Across));
    runStep(spare, blurred, f, pass(MattePass::Down));
    runStep(blurred, fit, f, pass(MattePass::Coefficients));
    runStep(fit, spare, f, pass(MattePass::Across));
    runStep(spare, blurred, f, pass(MattePass::Down));
    runStep(blurred, fit, f, pass(MattePass::Pack), &now);
    if (st.out.width != w || st.out.height != h) st.out.resize(w, h);
    if (f.blurBackground) {
        if (st.cut.width != w || st.cut.height != h) st.cut.resize(w, h);
        for (FrameF &img : st.back)
            if (img.width != (w + 3) / 4 || img.height != (h + 3) / 4) img.resize((w + 3) / 4, (h + 3) / 4);
        runStep(cur, st.cut, f, q, &fit);
        runStep(cur, st.back[0], f, pass(MattePass::BackDown));
        runStep(st.back[0], st.back[1], f, pass(MattePass::BackAcross));
        runStep(st.back[1], st.back[0], f, pass(MattePass::BackDownBlur));
        runStep(st.cut, st.out, f, pass(MattePass::BackOver), &st.back[0]);
    } else {
        runStep(cur, st.out, f, q, &fit);
    }
    return true;
}

void CpuEngine::renderSourceFilters(int id, double seconds) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end()) return;
    CpuSource &s = it->second;
    if (s.filters.empty() || s.base().width == 0) { s.filtered = false; return; }
    bool timed = false;
    for (const VideoFilter &f : s.filters) timed |= filterIsTimed(f);
    if (s.filtered && !timed && s.filteredGeneration == s.generation) return;
    const FrameF *cur = &s.base();
    bool fresh = s.generation != s.filteredGeneration || !s.filtered;  // the step's input is new
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < s.filters.size(); ++i) {
        const VideoFilter &f = s.filters[i];
        auto &st = s.steps[i];
        st.within = false;
        if (!st.started) { st.started = true; st.origin = seconds; }
        if (filterIsDelay(f)) {
            const double due = seconds - f.delayMs / 1000.0;
            if (f.type == VideoFilterType::RenderDelay || fresh || st.held.empty()) {
                while (st.held.size() >= 2 && st.held[1].second <= due) st.held.pop_front();
                while (st.held.size() >= filterDelayCapacity(cur->width, cur->height)) st.held.pop_front();
                st.held.emplace_back(*cur, seconds);
            }
            while (st.held.size() >= 2 && st.held[1].second <= due) st.held.pop_front();
            cur = &st.held.front().first;
            fresh = true;
            continue;
        }
        int ow, oh;
        filterSize(f, cur->width, cur->height, &ow, &oh);
        if (st.out.width != ow || st.out.height != oh) st.out.resize(ow, oh);
        if (f.type == VideoFilterType::Scale) {
            fillFrame(st.out, 0.f, 0.f, 0.f, 0.f);
            drawLayer(st.out, *cur, scaleLayer(f, cur->width, cur->height, ow, oh));
        } else if (f.type == VideoFilterType::Effect) {
            // As vk_engine.cpp: the passes in turn, every one also seeing the step's input.
            const int passes = effectPasses(f.effect);
            const FrameF *from = cur;
            for (int pass = 0; pass < passes; ++pass) {
                FrameF *to = &st.out;
                if (pass + 1 < passes) {
                    to = pass % 2 ? &st.mid2 : &st.mid;
                    if (to->width != cur->width || to->height != cur->height) to->resize(cur->width, cur->height);
                }
                runEffect(*from, *to, *cur, f, filterParams(f, cur->width, cur->height, 0, pass));
                from = to;
            }
        } else if (f.type == VideoFilterType::RemoveBackground) {
            if (!findPeople(*cur, st, f, fresh)) { runStep(*cur, st.out, f, FilterParams{}); cur = &st.out; continue; }
        } else if (f.type == VideoFilterType::Blur || f.type == VideoFilterType::Shadow) {
            const bool blur = f.type == VideoFilterType::Blur;
            if (st.mid.width != cur->width || st.mid.height != cur->height) st.mid.resize(cur->width, cur->height);
            // A wide one reads the picture averaged over its taps' spacing.
            const FrameF *first = cur;
            if (blurStep(f.radius) > 1) {
                if (st.mid2.width != cur->width || st.mid2.height != cur->height) st.mid2.resize(cur->width, cur->height);
                runStep(*cur, st.mid2, f, filterParams(f, cur->width, cur->height, 0, 2));
                first = &st.mid2;
            }
            runStep(*first, st.mid, f, filterParams(f, cur->width, cur->height, 0, 0));
            runStep(blur ? st.mid : *cur, st.out, f, filterParams(f, cur->width, cur->height, 0, 1), blur ? nullptr : &st.mid);
        } else if (f.type == VideoFilterType::GradePlan) {
            if (f.plan) runPlan(*f.plan, *cur, st.out, st.regs, [&st](const FrameF &from, const GradeOp &op, size_t index, FrameF &found) {
                if (st.people.size() <= index) st.people.resize(index + 1);
                CpuSource::Step &own = st.people[index];
                VideoFilter person;
                person.type = VideoFilterType::RemoveBackground;
                person.quality = op.channel;
                person.tighten = op.amount;
                if (!findPeople(from, own, person, true)) return false;
                found = own.out;
                return true;
            });
            else copyFrame(st.out, *cur);
        } else if (f.type == VideoFilterType::Program) {
            // The program's own CPU maths on a copy, rows shared out (the
            // copy too: each block copies its rows, then works on them).
            const bool run = f.program && f.program->cpu;
            const float *in = cur->rgba.data();
            float *pixels = st.out.rgba.data();
            const size_t row = size_t(st.out.width) * 4;
            parallelRows(0, st.out.height, [&](int first, int last) {
                std::copy(in + first * row, in + last * row, pixels + first * row);
                if (run) f.program->cpu(pixels + first * row, size_t(last - first) * st.out.width);
            }, 16);
        } else {
            runStep(*cur, st.out, f, filterParams(f, cur->width, cur->height, seconds - st.origin, 0, m_levels.sdrWhite));
        }
        // Kept within its shape: outside it, the picture the step was given.
        st.within = f.within.kind != 0 && ow == cur->width && oh == cur->height;
        if (st.within) {
            if (st.shaped.width != ow || st.shaped.height != oh) st.shaped.resize(ow, oh);
            runStep(st.out, st.shaped, f, shapeParams(f.within, ow, oh), cur);
            cur = &st.shaped;
            fresh |= filterIsTimed(f);
            continue;
        }
        cur = &st.out;
        fresh |= filterIsTimed(f);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto known = m_filterMs.find(id);
    if (known == m_filterMs.end()) m_filterMs[id] = ms;
    else known->second += (ms - known->second) * 0.1;
    s.filtered = true;
    s.filteredGeneration = s.generation;
}

}  // namespace

std::unique_ptr<Engine> createCpuEngine() { return std::make_unique<CpuEngine>(); }

std::unique_ptr<Engine> Engine::create(Mode mode, int deviceIndex) {
    if (mode != Mode::Cpu) {
        std::string why;
        if (auto gpu = createVulkanEngine(deviceIndex, &why))
            return gpu;
        if (mode == Mode::Vulkan)
            return nullptr;
    }
    return createCpuEngine();
}

}  // namespace engine
