#include "matting.h"
#include "parallel.h"

#include <QFile>
#include <QFloat16>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::matting {

const Model::Tensor *Model::find(const std::string &name) const {
    for (const Tensor &t : tensors)
        if (t.name == name) return &t;
    return nullptr;
}

namespace {

// "OMNN", version 1, the count; then each tensor's name, shape and fp16
// values (written when the weights were converted).
Model load() {
    Model m;
    QFile f(QStringLiteral(":/models/modnet.omnn"));
    if (!f.open(QIODevice::ReadOnly)) return m;
    const QByteArray data = f.readAll();
    const char *p = data.constData(), *end = p + data.size();
    auto take = [&](void *to, size_t n) {
        if (size_t(end - p) < n) return false;
        std::memcpy(to, p, n);
        p += n;
        return true;
    };
    char magic[4];
    uint32_t version = 0, count = 0;
    if (!take(magic, 4) || std::memcmp(magic, "OMNN", 4) != 0 || !take(&version, 4) || version != 1 || !take(&count, 4)) return m;
    std::vector<qfloat16> halves;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t length = 0;
        uint8_t dims = 0;
        Model::Tensor t;
        if (!take(&length, 2)) return m;
        t.name.resize(length);
        if (!take(t.name.data(), length) || !take(&dims, 1) || dims == 0 || dims > 4) return m;
        size_t n = 1;
        for (uint8_t d = 0; d < dims; ++d) {
            uint32_t v = 0;
            if (!take(&v, 4) || v == 0 || v > 4096) return m;
            t.shape.push_back(int(v));
            n *= v;
        }
        halves.resize(n);
        if (!take(halves.data(), n * 2)) return m;
        std::vector<float> v(n);
        qFloatFromFloat16(v.data(), halves.data(), qsizetype(n));
        // Each tensor starts on a 16-byte boundary; a full convolution's
        // weights are laid out eight output channels at a time (see Model).
        t.offset = uint32_t((m.values.size() + 3) / 4 * 4);
        const bool dense = dims == 4 && t.name.size() > 2 && t.name.compare(t.name.size() - 2, 2, ".w") == 0 &&
                           t.name.find(".dw.") == std::string::npos;
        if (dense) {
            const int cout = t.shape[0], rest = t.shape[1] * t.shape[2] * t.shape[3], groups = (cout + 7) / 8;
            m.values.resize(t.offset + size_t(groups) * rest * 8, 0.f);
            for (int oc = 0; oc < cout; ++oc)
                for (int r = 0; r < rest; ++r) m.values[t.offset + (size_t(oc / 8) * rest + r) * 8 + oc % 8] = v[size_t(oc) * rest + r];
        } else {
            m.values.resize(t.offset + n);
            std::copy(v.begin(), v.end(), m.values.begin() + t.offset);
        }
        m.tensors.push_back(std::move(t));
    }
    m.ok = p == end;
    return m;
}

struct Tensor {
    int32_t off = 0;
    int c = 0, h = 0, w = 0;
    size_t size() const { return size_t(c) * h * w; }
};

// Lays the network out pass by pass, reusing the arena as tensors fall out of use.
struct Builder {
    const Model &m;
    Plan &p;
    std::vector<std::pair<size_t, size_t>> spare;  // offset, size; in order

    Tensor make(int c, int h, int w) {
        Tensor t{0, c, h, w};
        const size_t n = t.size();
        for (auto it = spare.begin(); it != spare.end(); ++it) {
            if (it->second < n) continue;
            t.off = int32_t(it->first);
            it->first += n;
            it->second -= n;
            if (it->second == 0) spare.erase(it);
            return t;
        }
        t.off = int32_t(p.arena);
        p.arena += n;
        return t;
    }
    void drop(const Tensor &t) {
        std::pair<size_t, size_t> block{size_t(t.off), t.size()};
        auto it = std::lower_bound(spare.begin(), spare.end(), block);
        it = spare.insert(it, block);
        if (it + 1 != spare.end() && it->first + it->second == (it + 1)->first) { it->second += (it + 1)->second; spare.erase(it + 1); }
        if (it != spare.begin() && (it - 1)->first + (it - 1)->second == it->first) { (it - 1)->second += it->second; spare.erase(it); }
    }

    // A convolution as MODNet has them: padding k / 2. `ibn`: the second
    // half of its channels is instance-normalised, then Relu.
    Tensor conv(const char *name, std::initializer_list<Tensor> in, int stride, int act, bool ibn = false,
                bool depthwise = false, const Tensor *residual = nullptr) {
        const Model::Tensor *w = m.find(std::string(name) + ".w"), *b = m.find(std::string(name) + ".b");
        int cin = 0;
        for (const Tensor &t : in) cin += t.c;
        const Tensor &first = *in.begin();
        if (!w || !b || w->shape.size() != 4 || w->shape[2] != w->shape[3] || b->shape[0] != w->shape[0] ||
            w->shape[1] != (depthwise ? 1 : cin) || (depthwise && w->shape[0] != cin)) {
            p.ok = false;
            return first;
        }
        Pass q;
        q.op = Conv;
        q.k = w->shape[2];
        q.pad = q.k / 2;
        q.stride = stride;
        q.inW = first.w; q.inH = first.h;
        q.outW = (first.w + 2 * q.pad - q.k) / stride + 1;
        q.outH = (first.h + 2 * q.pad - q.k) / stride + 1;
        const Tensor *v[3] = {nullptr, nullptr, nullptr};
        int n = 0;
        for (const Tensor &t : in) v[n++] = &t;
        q.in0 = v[0]->off; q.c0 = v[0]->c;
        if (v[1]) { q.in1 = v[1]->off; q.c1 = v[1]->c; }
        if (v[2]) { q.in2 = v[2]->off; q.c2 = v[2]->c; }
        q.cout = w->shape[0];
        q.depthwise = depthwise;
        q.act = act;
        q.actUntil = ibn ? q.cout / 2 : q.cout;
        q.weights = int32_t(w->offset);
        q.bias = int32_t(b->offset);
        q.residual = residual ? residual->off : -1;
        // Few outputs and long sums (the low-resolution layers): the input
        // channels shared out over more of the GPU, the shares summed after.
        const int64_t threads = int64_t((q.outW + 3) / 4) * q.outH * ((q.cout + 7) / 8);
        if (!depthwise && threads < 16384 && cin >= 32)
            q.splits = int(std::clamp<int64_t>((16384 + threads - 1) / threads, 1, std::min(32, cin / 16)));
        const Tensor out = make(q.cout, q.outH, q.outW);
        q.out = out.off;
        Tensor partial;
        if (q.splits > 1) { partial = make(q.splits * q.cout, q.outH, q.outW); q.partial = partial.off; }
        p.passes.push_back(q);
        if (q.splits > 1) {
            Pass s = q;
            s.op = Sum;
            p.passes.push_back(s);
            drop(partial);
        }
        if (ibn) {
            // Chunks of 4096 pixels each, so a big picture's statistics spread over the GPU.
            const int channels = q.cout - q.actUntil, chunks = (q.outW * q.outH + 4095) / 4096;
            const Tensor stats = make(2 * channels, 1, 1), parts = make(3 * channels * chunks, 1, 1);
            Pass s;
            s.outW = q.outW; s.outH = q.outH;
            s.out = out.off; s.cout = q.cout; s.actUntil = q.actUntil;
            s.partial = stats.off; s.in1 = parts.off; s.splits = chunks;
            for (int32_t op : {NormStats, NormMerge, NormApply}) {
                s.op = op;
                p.passes.push_back(s);
            }
            drop(parts);
            drop(stats);
        }
        return out;
    }
    Tensor resize(const Tensor &in, int w, int h) {
        Pass q;
        q.op = Resize;
        q.inW = in.w; q.inH = in.h; q.outW = w; q.outH = h;
        q.in0 = in.off; q.c0 = in.c;
        const Tensor out = make(in.c, h, w);
        q.out = out.off;
        p.passes.push_back(q);
        return out;
    }
    // MobileNetV2's inverted residual: widen, depthwise, narrow (plus the input when shapes allow).
    Tensor block(int i, const Tensor &in, int stride) {
        const std::string n = "b" + std::to_string(i);
        const Tensor e = conv((n + ".ex").c_str(), {in}, 1, Relu6);
        const Tensor d = conv((n + ".dw").c_str(), {e}, stride, Relu6, false, true);
        drop(e);
        const Model::Tensor *pw = m.find(n + ".pw.w");
        const bool residual = stride == 1 && pw && pw->shape[0] == in.c;
        const Tensor o = conv((n + ".pw").c_str(), {d}, 1, None, false, false, residual ? &in : nullptr);
        drop(d);
        return o;
    }
};

}  // namespace

const Model &model() {
    static const Model m = load();
    return m;
}

Plan plan(const Model &m, int width, int height, int srcW, int srcH, int parts) {
    Plan p;
    p.width = width;
    p.height = height;
    if (!m.ok || width < 32 || height < 32 || width % 32 || height % 32 || srcW < 1 || srcH < 1) return p;
    p.ok = true;
    Builder b{m, p, {}};
    const int W = width, H = height;

    Pass in;
    in.op = Input;
    in.inW = srcW; in.inH = srcH; in.outW = W; in.outH = H;
    const Tensor img = b.make(3, H, W);
    in.out = img.off;
    p.passes.push_back(in);

    // The backbone, MobileNetV2: features at 1/2, 1/4 and 1/32 kept.
    Tensor x = b.conv("b0", {img}, 2, Relu6);
    Tensor y = b.conv("b1.dw", {x}, 1, Relu6, false, true);
    b.drop(x);
    const Tensor enc2 = b.conv("b1.pw", {y}, 1, None);
    b.drop(y);
    const Tensor b2 = b.block(2, enc2, 2);
    const Tensor enc4 = b.block(3, b2, 1);
    b.drop(b2);
    static const int strides[] = {2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1};  // blocks 4 to 17
    x = enc4;
    for (int i = 4; i <= 17; ++i) {
        y = b.block(i, x, strides[i - 4]);
        if (i > 4) b.drop(x);
        x = y;
    }
    Tensor enc32 = b.conv("b18", {x}, 1, Relu6);
    b.drop(x);

    // Low resolution: the semantics, squeezed and excited, brought up to 1/8.
    {
        Pass q;
        q.op = Pool;
        q.inW = enc32.w; q.inH = enc32.h;
        q.in0 = enc32.off; q.c0 = enc32.c;
        const Tensor pooled = b.make(enc32.c, 1, 1);
        q.out = pooled.off;
        p.passes.push_back(q);
        const Tensor s1 = b.conv("se.fc1", {pooled}, 1, Relu);
        b.drop(pooled);
        const Tensor s2 = b.conv("se.fc2", {s1}, 1, Sigmoid);
        b.drop(s1);
        Pass s;
        s.op = Scale;
        s.outW = enc32.w; s.outH = enc32.h;
        s.in0 = enc32.off; s.c0 = enc32.c; s.in1 = s2.off; s.out = enc32.off;
        p.passes.push_back(s);
        b.drop(s2);
    }
    x = b.resize(enc32, W / 16, H / 16);
    b.drop(enc32);
    y = b.conv("lr16", {x}, 1, Relu, true);
    b.drop(x);
    x = b.resize(y, W / 8, H / 8);
    b.drop(y);
    const Tensor lr8 = b.conv("lr8", {x}, 1, Relu, true);
    b.drop(x);

    // High resolution: detail at 1/4, then 1/2.
    const Tensor img2 = b.resize(img, W / 2, H / 2), img4 = b.resize(img, W / 4, H / 4);
    const Tensor e2 = b.conv("hr.tohr2", {enc2}, 1, Relu, true);
    b.drop(enc2);
    const Tensor h4a = b.conv("hr.enc2", {img2, e2}, 2, Relu, true);
    b.drop(img2);
    const Tensor e4 = b.conv("hr.tohr4", {enc4}, 1, Relu, true);
    b.drop(enc4);
    const Tensor h4b = b.conv("hr.enc4", {h4a, e4}, 1, Relu, true);
    b.drop(h4a);
    b.drop(e4);
    const Tensor lr4 = b.resize(lr8, W / 4, H / 4);
    b.drop(lr8);
    x = b.conv("hr.hr4.0", {h4b, lr4, img4}, 1, Relu, true);
    b.drop(h4b);
    b.drop(img4);
    y = b.conv("hr.hr4.1", {x}, 1, Relu, true);
    b.drop(x);
    x = b.conv("hr.hr4.2", {y}, 1, Relu, true);
    b.drop(y);
    y = b.resize(x, W / 2, H / 2);
    b.drop(x);
    x = b.conv("hr.hr2.0", {y, e2}, 1, Relu, true);
    b.drop(y);
    b.drop(e2);
    for (const char *n : {"hr.hr2.1", "hr.hr2.2", "hr.hr2.3"}) {
        y = b.conv(n, {x}, 1, Relu, true);
        b.drop(x);
        x = y;
    }
    const Tensor hr2 = x;

    // Fusion: the two together at 1/2, then the full size with the picture.
    x = b.conv("f.lr4", {lr4}, 1, Relu, true);
    b.drop(lr4);
    y = b.resize(x, W / 2, H / 2);
    b.drop(x);
    x = b.conv("f.f2", {y, hr2}, 1, Relu, true);
    b.drop(y);
    b.drop(hr2);
    y = b.resize(x, W, H);
    b.drop(x);
    x = b.conv("f.f0", {y, img}, 1, Relu, true);
    b.drop(y);
    b.drop(img);
    const Tensor matte = b.conv("f.f1", {x}, 1, Sigmoid);
    b.drop(x);

    Pass out;
    out.op = Output;
    out.outW = W; out.outH = H;
    out.in0 = matte.off;
    p.passes.push_back(out);
    p.matte = matte.off;

    // Parts of about equal work, counting multiply-adds (the rest is small).
    std::vector<double> cost;
    double total = 0;
    for (const Pass &q : p.passes) {
        const double pixels = double(q.outW) * q.outH;
        const double c = q.op == Conv ? pixels * q.cout * (q.depthwise ? 1 : q.c0 + q.c1 + q.c2) * q.k * q.k : pixels * std::max(1, q.c0);
        cost.push_back(c);
        total += c;
    }
    parts = std::clamp(parts, 1, 8);
    double done = 0;
    for (size_t i = 0; i < p.passes.size(); ++i) {
        done += cost[i];
        if (int(p.ends.size()) < parts - 1 && done >= total * double(p.ends.size() + 1) / parts) p.ends.push_back(i + 1);
    }
    p.ends.push_back(p.passes.size());
    return p;
}

// The same arithmetic as nn.comp, pass by pass.
void run(const Plan &p, const Model &m, const FrameF &src, std::vector<float> &arena, size_t first, size_t last) {
    if (!p.ok || !m.ok) return;
    arena.resize(p.arena);
    float *A = arena.data();
    const float *Wt = m.values.data();
    auto activate = [](const Pass &q, int c, float v) {
        if (c >= q.actUntil) return v;
        switch (q.act) {
        case Relu: return std::max(v, 0.f);
        case Relu6: return std::clamp(v, 0.f, 6.f);
        case Sigmoid: return 1.f / (1.f + std::exp(-v));
        default: return v;
        }
    };
    for (size_t index = first; index < std::min(last, p.passes.size()); ++index) {
        const Pass &q = p.passes[index];
        const int P = q.outW * q.outH, Pin = q.inW * q.inH;
        switch (q.op) {
        case Input:
            for (int y = 0; y < q.outH; ++y) {
                const int y0 = y * q.inH / q.outH, y1 = std::max(y0 + 1, ((y + 1) * q.inH + q.outH - 1) / q.outH);
                for (int x = 0; x < q.outW; ++x) {
                    const int x0 = x * q.inW / q.outW, x1 = std::max(x0 + 1, ((x + 1) * q.inW + q.outW - 1) / q.outW);
                    float sum[3] = {0, 0, 0};
                    for (int j = y0; j < y1; ++j)
                        for (int i = x0; i < x1; ++i) {
                            const float *t = src.px(std::min(i, src.width - 1), std::min(j, src.height - 1));
                            for (int c = 0; c < 3; ++c) sum[c] += t[c] * t[3];
                        }
                    const float n = float((y1 - y0) * (x1 - x0));
                    for (int c = 0; c < 3; ++c) A[q.out + c * P + y * q.outW + x] = linearToSrgb(sum[c] / n) * 2.f - 1.f;
                }
            }
            break;
        case Conv: {
            const int kk = q.k * q.k, cin = q.c0 + q.c1 + q.c2;
            auto plane = [&](int c) {
                return c < q.c0 ? q.in0 + c * Pin : c < q.c0 + q.c1 ? q.in1 + (c - q.c0) * Pin : q.in2 + (c - q.c0 - q.c1) * Pin;
            };
            // Each output channel is a plane of its own, summed from the
            // inputs alone: the channels are shared over the cores.
            parallelRows(0, q.cout, [&](int firstChannel, int lastChannel) {
            std::vector<float> acc(static_cast<size_t>(P));
            for (int oc = firstChannel; oc < lastChannel; ++oc) {
                std::fill(acc.begin(), acc.end(), 0.f);
                const int first = q.depthwise ? oc : 0, last = q.depthwise ? oc + 1 : cin;
                for (int ic = first; ic < last; ++ic) {
                    const float *plane0 = A + plane(ic);
                    // Depthwise: [c][ky][kx]; full: [oc / 8][ic][ky][kx][oc % 8].
                    const float *w = Wt + q.weights + (q.depthwise ? oc * kk : ((oc / 8) * cin + ic) * kk * 8 + oc % 8);
                    const int step = q.depthwise ? 1 : 8;
                    for (int ky = 0; ky < q.k; ++ky)
                        for (int kx = 0; kx < q.k; ++kx) {
                            const float wv = w[(ky * q.k + kx) * step];
                            for (int oy = 0; oy < q.outH; ++oy) {
                                const int iy = oy * q.stride - q.pad + ky;
                                if (iy < 0 || iy >= q.inH) continue;
                                const float *row = plane0 + iy * q.inW;
                                float *o = acc.data() + oy * q.outW;
                                // The columns whose tap lands inside the picture.
                                const int lo = std::max(0, (q.pad - kx + q.stride - 1) / q.stride);
                                const int hi = std::min(q.outW, (q.inW - 1 + q.pad - kx) / q.stride + 1);
                                if (q.stride == 1)
                                    for (int ox = lo; ox < hi; ++ox) o[ox] += row[ox - q.pad + kx] * wv;
                                else
                                    for (int ox = lo; ox < hi; ++ox) o[ox] += row[ox * q.stride - q.pad + kx] * wv;
                            }
                        }
                }
                float *out = A + q.out + oc * P;
                const float *res = q.residual >= 0 ? A + q.residual + oc * P : nullptr;
                for (int i = 0; i < P; ++i) {
                    float v = acc[size_t(i)] + Wt[q.bias + oc];
                    if (res) v += res[i];
                    out[i] = activate(q, oc, v);
                }
            }
            }, 1);
            break;
        }
        case Sum:        // the CPU sums every input channel at once,
        case NormMerge:  // and each channel's statistics too
            break;
        case Resize:
            for (int c = 0; c < q.c0; ++c) {
                const float *in = A + q.in0 + c * Pin;
                float *out = A + q.out + c * P;
                const float sx = float(q.inW) / float(q.outW), sy = float(q.inH) / float(q.outH);
                for (int y = 0; y < q.outH; ++y) {
                    const float fy = std::max(sy * (float(y) + 0.5f) - 0.5f, 0.f);
                    const int y0 = int(fy), y1 = y0 + (y0 < q.inH - 1 ? 1 : 0);
                    const float ty = fy - float(y0);
                    for (int x = 0; x < q.outW; ++x) {
                        const float fx = std::max(sx * (float(x) + 0.5f) - 0.5f, 0.f);
                        const int x0 = int(fx), x1 = x0 + (x0 < q.inW - 1 ? 1 : 0);
                        const float tx = fx - float(x0);
                        const float top = in[y0 * q.inW + x0] + (in[y0 * q.inW + x1] - in[y0 * q.inW + x0]) * tx;
                        const float bottom = in[y1 * q.inW + x0] + (in[y1 * q.inW + x1] - in[y1 * q.inW + x0]) * tx;
                        out[y * q.outW + x] = top + (bottom - top) * ty;
                    }
                }
            }
            break;
        case NormStats:
            for (int c = q.actUntil; c < q.cout; ++c) {
                const float *v = A + q.out + c * P;
                float sum = 0;
                for (int i = 0; i < P; ++i) sum += v[i];
                const float mean = sum / float(P);
                float dev = 0;
                for (int i = 0; i < P; ++i) dev += (v[i] - mean) * (v[i] - mean);
                A[q.partial + 2 * (c - q.actUntil)] = mean;
                A[q.partial + 2 * (c - q.actUntil) + 1] = 1.f / std::sqrt(dev / float(P) + 1e-5f);
            }
            break;
        case NormApply:
            for (int c = q.actUntil; c < q.cout; ++c) {
                float *v = A + q.out + c * P;
                const float mean = A[q.partial + 2 * (c - q.actUntil)], inv = A[q.partial + 2 * (c - q.actUntil) + 1];
                for (int i = 0; i < P; ++i) v[i] = std::max((v[i] - mean) * inv, 0.f);
            }
            break;
        case Pool:
            for (int c = 0; c < q.c0; ++c) {
                float sum = 0;
                for (int i = 0; i < Pin; ++i) sum += A[q.in0 + c * Pin + i];
                A[q.out + c] = sum / float(Pin);
            }
            break;
        case Scale:
            for (int c = 0; c < q.c0; ++c)
                for (int i = 0; i < P; ++i) A[q.in0 + c * P + i] *= A[q.in1 + c];
            break;
        default: break;  // Output: the matte stays where it is
        }
    }
}

}  // namespace engine::matting
