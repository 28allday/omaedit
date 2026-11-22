#include "mediapool.h"
#include "engines.h"

#include "decoder.h"
#include "renderrequest.h"
#include "stagetimer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <shared_mutex>

namespace render {

Picture::Picture() = default;
Picture::~Picture() { reset(); }

void Picture::reset() {
    if (frame) av_frame_free(&frame);
    converted.clear();
    kind = Kind::None;
    yuv = {};
    rgb8 = {};
    width = height = 0;
    reducedBy = 1;
}

QImage Picture::rgba64(SwsContext **converter, QString *error) const {
    if (!frame) { if (error) *error = QStringLiteral("No decoded picture."); return {}; }
    if (kind == Kind::Gpu) {
        Picture inMemory;
        AVFrame *cpu = av_frame_alloc();
        const bool ok = cpu && av_hwframe_transfer_data(cpu, frame, 0) >= 0 && av_frame_copy_props(cpu, frame) >= 0
                        && pictureFromFrame(cpu, inMemory, converter, error);
        av_frame_free(&cpu);
        return ok ? inMemory.rgba64(converter, error) : QImage();
    }
    const auto *desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    *converter = sws_getCachedContext(*converter, frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                                      frame->height, AV_PIX_FMT_RGBA64LE, SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND,
                                      nullptr, nullptr, nullptr);
    if (!*converter) { if (error) *error = QStringLiteral("This picture cannot be converted to 16-bit RGB."); return {}; }
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        const int space = yuv.matrix == engine::YuvMatrix::BT601 ? SWS_CS_ITU601
                        : yuv.matrix == engine::YuvMatrix::BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU709;
        const int *coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(*converter, coefficients, yuv.fullRange, sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);
    }
    QImage image(frame->width, frame->height, QImage::Format_RGBA64);
    uint8_t *dst[4] = {image.bits(), nullptr, nullptr, nullptr};
    int strides[4] = {int(image.bytesPerLine()), 0, 0, 0};
    if (sws_scale(*converter, frame->data, frame->linesize, 0, frame->height, dst, strides) != frame->height) {
        if (error) *error = QStringLiteral("Converting the picture to 16-bit RGB failed.");
        return {};
    }
    return image;
}

namespace {
// For one output sample along one axis: the chroma samples that go into it
// and how much of each, the mean over the block's luma positions of the two
// chroma samples the conversion mixes for each (yuv_in.comp pixelAt).
struct Taps {
    int first = 0, count = 0;
    float weight[4] = {};
    bool operator==(const Taps &o) const { return count == o.count && std::equal(weight, weight + 4, o.weight); }
};
// `size` luma positions in blocks of `by`; chroma on a grid of `chroma`
// samples at `at(position)` (a whole number where there is one a position).
template <typename At>
std::vector<Taps> chromaTaps(int size, int by, int chroma, bool subsampled, At at) {
    std::vector<Taps> out(size_t((size + by - 1) / by));
    for (size_t o = 0; o < out.size(); ++o) {
        const int begin = int(o) * by, end = std::min(size, begin + by);
        int low = chroma, high = -1, base = -1;
        float sums[16] = {};
        const auto add = [&](int index, float w) {
            index = std::clamp(index, 0, chroma - 1);
            if (base < 0) base = std::max(0, index - 4);
            sums[std::clamp(index - base, 0, 15)] += w;
            low = std::min(low, index);
            high = std::max(high, index);
        };
        for (int p = begin; p < end; ++p) {
            if (!subsampled) { add(p, 1.f); continue; }
            const float f = at(p);
            const int i = int(std::floor(f));
            add(i, 1.f - (f - float(i)));
            add(i + 1, f - float(i));
        }
        Taps &taps = out[o];
        taps.first = low;
        taps.count = std::min(4, high - low + 1);
        for (int k = 0; k < taps.count; ++k) taps.weight[k] = sums[low - base + k] / float(end - begin);
    }
    return out;
}
// The loops below are written so that the compiler makes vector code of them
// at the project's ordinary optimisation level: one plain stride, no branches.
template <typename Sample>
void addRow(uint32_t *sums, const Sample *row, int step, int offset, int count) {
    if (step == 1) for (int x = 0; x < count; ++x) sums[x] += row[x + offset];
    else for (int x = 0; x < count; ++x) sums[x] += row[x * step + offset];
}
void addWeighted(float *into, const uint32_t *row, float weight, int count) {
    for (int x = 0; x < count; ++x) into[x] += weight * float(row[x]);
}
void lumaAcross2(uint16_t *out, const uint32_t *sums, int count, float scale) {
    for (int x = 0; x < count; ++x) out[x] = uint16_t(int(float(sums[2 * x] + sums[2 * x + 1]) * scale + 0.5f));
}
void lumaAcross4(uint16_t *out, const uint32_t *sums, int count, float scale) {
    for (int x = 0; x < count; ++x)
        out[x] = uint16_t(int(float(sums[4 * x] + sums[4 * x + 1] + sums[4 * x + 2] + sums[4 * x + 3]) * scale + 0.5f));
}
// out[x] = (the four taps from `from + x * step`) * scale + offset, as a 16-bit code.
template <int Step>
void chromaAcross(uint16_t *out, const float *from, int count, const float *w, float scale, float offset) {
    const float w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
    for (int x = 0; x < count; ++x) {
        const float *at = from + x * Step;
        const float v = (w0 * at[0] + w1 * at[1] + w2 * at[2] + w3 * at[3]) * scale + offset;
        out[x] = uint16_t(int(std::min(std::max(v, 0.f), 65535.f)));
    }
}
}

bool Picture::makeSmaller(int by) {
    const engine::YuvInput in = yuv;
    if (kind != Kind::Yuv || (by != 2 && by != 4) || in.alpha || in.interlaced || in.depth < 8 || in.depth > 16 || (in.depth > 8 && in.msb))
        return false;
    const bool nv12 = in.layout == engine::YuvLayout::NV12;
    if (!nv12 && in.layout != engine::YuvLayout::I420 && in.layout != engine::YuvLayout::I422 && in.layout != engine::YuvLayout::I444) return false;
    if (!in.plane[0] || !in.plane[1] || (!nv12 && !in.plane[2]) || in.width < by * 2 || in.height < by * 2) return false;
    StageTimer::Scope timed("decode.reduce");
    const int w = in.width, h = in.height, ow = (w + by - 1) / by, oh = (h + by - 1) / by;
    const bool wide = in.depth > 8;
    const bool halfAcross = in.layout != engine::YuvLayout::I444;
    const bool halfDown = nv12 || in.layout == engine::YuvLayout::I420;
    const int cw = halfAcross ? (w + 1) / 2 : w, ch = halfDown ? (h + 1) / 2 : h;
    // Codes as 16-bit ones the conversion reads to the same values: a
    // limited-range code keeps its place between its black and white, a
    // full-range one its share of the whole range.
    const double top = double((1 << in.depth) - 1), mid = double(1 << (in.depth - 1));
    const float scale = float(in.fullRange ? 65535.0 / top : double(1 << (16 - in.depth)));
    const float chromaOffset = float(in.fullRange ? 32768.0 - mid * 65535.0 / top : 0.0) + 0.5f;
    std::vector<uint8_t> made(size_t(ow) * oh * 2 * 3);
    auto *outY = reinterpret_cast<uint16_t *>(made.data());
    uint16_t *outC[2] = {outY + size_t(ow) * oh, outY + size_t(ow) * oh * 2};

    // Luma: the block's sum, a row of column sums at a time. (Whole blocks
    // across in one loop; a last, narrower one by itself.)
    std::vector<uint32_t> columns(size_t(w) + 8);
    const int wholeAcross = w / by;
    for (int oy = 0; oy < oh; ++oy) {
        std::fill(columns.begin(), columns.end(), 0u);
        const int y0 = oy * by, y1 = std::min(h, y0 + by);
        for (int y = y0; y < y1; ++y) {
            const uint8_t *row = in.plane[0] + size_t(y) * in.stride[0];
            if (wide) addRow(columns.data(), reinterpret_cast<const uint16_t *>(row), 1, 0, w);
            else addRow(columns.data(), row, 1, 0, w);
        }
        uint16_t *out = outY + size_t(oy) * ow;
        const float each = scale / float((y1 - y0) * by);
        if (by == 2) lumaAcross2(out, columns.data(), wholeAcross, each);
        else lumaAcross4(out, columns.data(), wholeAcross, each);
        if (wholeAcross < ow) {
            uint32_t sum = 0;
            for (int x = wholeAcross * by; x < w; ++x) sum += columns[size_t(x)];
            out[wholeAcross] = uint16_t(int(float(sum) * scale / float((y1 - y0) * (w - wholeAcross * by)) + 0.5f));
        }
    }

    // Chroma: down its own grid first (a row of weighted sums), then across.
    const bool left = in.chromaLeft;
    const auto across = chromaTaps(w, by, cw, halfAcross, [&](int x) { return engine::chromaColumn(x, left); });
    const auto down = chromaTaps(h, by, ch, halfDown, [](int y) { return (float(y) + 0.5f) * 0.5f - 0.5f; });
    // The columns between the edges take their samples the same way, each
    // `step` further along: those in one loop, the edges one by one.
    const int step = halfAcross ? by / 2 : by;
    int run0 = ow, run1 = ow;
    if (ow >= 4) {
        const Taps &typical = across[size_t(ow / 2)];
        const int origin = typical.first - (ow / 2) * step;
        run0 = 0;
        while (run0 < ow && !(across[size_t(run0)] == typical && across[size_t(run0)].first == origin + run0 * step)) ++run0;
        run1 = run0;
        while (run1 < ow && across[size_t(run1)] == typical && across[size_t(run1)].first == origin + run1 * step) ++run1;
    }
    std::vector<uint32_t> samples(size_t(cw) + 8);
    std::vector<float> sums[2] = {std::vector<float>(size_t(cw) + 8), std::vector<float>(size_t(cw) + 8)};
    for (int oy = 0; oy < oh; ++oy) {
        const Taps &rows = down[size_t(oy)];
        for (int c = 0; c < 2; ++c) {
            std::fill(sums[c].begin(), sums[c].end(), 0.f);
            for (int k = 0; k < rows.count; ++k) {
                const int cy = rows.first + k;
                // (NV12: the two side by side in one plane.)
                const uint8_t *row = nv12 ? in.plane[1] + size_t(cy) * in.stride[1] : in.plane[1 + c] + size_t(cy) * in.stride[1 + c];
                std::fill(samples.begin(), samples.end(), 0u);
                if (wide) addRow(samples.data(), reinterpret_cast<const uint16_t *>(row), nv12 ? 2 : 1, nv12 ? c : 0, cw);
                else addRow(samples.data(), row, nv12 ? 2 : 1, nv12 ? c : 0, cw);
                addWeighted(sums[c].data(), samples.data(), rows.weight[k], cw);
            }
            const float *from = sums[c].data();
            uint16_t *out = outC[c] + size_t(oy) * ow;
            const auto one = [&](int ox) {
                const Taps &taps = across[size_t(ox)];
                float value = 0;
                for (int k = 0; k < taps.count; ++k) value += taps.weight[k] * from[taps.first + k];
                out[ox] = uint16_t(int(std::min(std::max(value * scale + chromaOffset, 0.f), 65535.f)));
            };
            for (int ox = 0; ox < run0; ++ox) one(ox);
            if (run1 > run0) {
                const Taps &typical = across[size_t(run0)];
                const float *start = from + typical.first;
                if (step == 1) chromaAcross<1>(out + run0, start, run1 - run0, typical.weight, scale, chromaOffset);
                else if (step == 2) chromaAcross<2>(out + run0, start, run1 - run0, typical.weight, scale, chromaOffset);
                else chromaAcross<4>(out + run0, start, run1 - run0, typical.weight, scale, chromaOffset);
            }
            for (int ox = run1; ox < ow; ++ox) one(ox);
        }
    }

    converted = std::move(made);
    if (frame) av_frame_free(&frame);  // its planes are not read again
    engine::YuvInput out = in;
    out.width = ow;
    out.height = oh;
    out.layout = engine::YuvLayout::I444;
    out.depth = 16;
    out.msb = false;
    out.reduce = 1;
    auto *base = converted.data();
    for (int p = 0; p < 3; ++p) { out.plane[p] = base + size_t(ow) * oh * 2 * size_t(p); out.stride[p] = ow * 2; }
    yuv = out;
    reducedBy = by;
    return true;
}

bool Picture::download(QString *error) {
    if (kind != Kind::Gpu) return true;
    AVFrame *cpu = av_frame_alloc();
    int code = cpu ? av_hwframe_transfer_data(cpu, frame, 0) : AVERROR(ENOMEM);
    if (code >= 0) code = av_frame_copy_props(cpu, frame);
    if (code < 0) {
        av_frame_free(&cpu);
        if (error) *error = QStringLiteral("A picture could not be read back from the GPU.");
        return false;
    }
    const double at = seconds;
    SwsContext *converter = nullptr;
    const bool ok = pictureFromFrame(cpu, *this, &converter, error);
    sws_freeContext(converter);
    av_frame_free(&cpu);
    seconds = at;
    return ok;
}

bool Picture::upload(engine::Engine &e, int source, bool encoded) const {
    switch (kind) {
    case Kind::Gpu: {
        engine::YuvInput y = yuv;
        if (encoded) y.transfer = engine::Transfer::Encoded;
        if (e.updateSource(source, frame, y)) return true;
        // This engine will not take it from the GPU: through memory then.
        Picture inMemory;
        AVFrame *cpu = av_frame_alloc();
        SwsContext *converter = nullptr;
        QString why;
        const bool ok = cpu && av_hwframe_transfer_data(cpu, frame, 0) >= 0 && av_frame_copy_props(cpu, frame) >= 0
                        && pictureFromFrame(cpu, inMemory, &converter, &why) && inMemory.upload(e, source, encoded);
        sws_freeContext(converter);
        av_frame_free(&cpu);
        return ok;
    }
    case Kind::Yuv: {
        engine::YuvInput y = yuv;
        if (encoded) y.transfer = engine::Transfer::Encoded;
        return e.updateSource(source, y);
    }
    case Kind::Rgb8:
    case Kind::Float: {
        engine::CpuFrame c = kind == Kind::Float ? floats : rgb8;
        c.encoded = encoded;
        return e.updateSource(source, c);
    }
    case Kind::None: break;
    }
    return false;
}

namespace {

engine::YuvMatrix matrixOf(const AVFrame *f) {
    switch (f->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M: return engine::YuvMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return engine::YuvMatrix::BT2020;
    case AVCOL_SPC_BT709: return engine::YuvMatrix::BT709;
    default: break;
    }
    // Untagged: the size says which standard the picture most likely follows
    // (the one rule every reader shares).
    return SuiteVideo::untaggedIsBt601(f->height) ? engine::YuvMatrix::BT601 : engine::YuvMatrix::BT709;
}

void describeColour(const AVFrame *f, const AVPixFmtDescriptor *desc, engine::YuvInput &y) {
    y.matrix = matrixOf(f);
    // The "J" formats are full range whatever the tag says.
    const bool jpeg = desc->name && std::strncmp(desc->name, "yuvj", 4) == 0;
    y.fullRange = jpeg || f->color_range == AVCOL_RANGE_JPEG;
    y.transfer = f->color_trc == AVCOL_TRC_SMPTE2084      ? engine::Transfer::Pq
               : f->color_trc == AVCOL_TRC_ARIB_STD_B67 ? engine::Transfer::Hlg
                                                         : engine::Transfer::Sdr;
    y.wideGamut = f->color_primaries == AVCOL_PRI_BT2020;
    y.interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    // Untagged video chroma is MPEG's: level with the left luma sample. JPEG
    // ("J" formats) puts it midway.
    y.chromaLeft = f->chroma_location == AVCHROMA_LOC_LEFT || f->chroma_location == AVCHROMA_LOC_TOPLEFT
        || f->chroma_location == AVCHROMA_LOC_BOTTOMLEFT || (f->chroma_location == AVCHROMA_LOC_UNSPECIFIED && !jpeg);
}

// Planes the engine reads directly: planar 4:2:0, 4:2:2 or 4:4:4 at 8 to 16
// bits little-endian (with an optional alpha plane), NV12/P010-style
// semi-planar, and packed YUYV.
bool takeDirect(const AVFrame *f, const AVPixFmtDescriptor *desc, engine::YuvInput &y) {
    if (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BE |
                       AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BAYER))
        return false;
    const int depth = desc->comp[0].depth;
    if (depth < 8 || depth > 16) return false;
    for (int c = 1; c < desc->nb_components; ++c)
        if (desc->comp[c].depth != depth) return false;
    y.width = f->width;
    y.height = f->height;
    y.depth = depth;
    const int wordBits = depth > 8 ? 16 : 8;
    if (f->format == AV_PIX_FMT_YUYV422) {
        y.layout = engine::YuvLayout::YUY2;
        y.plane[0] = f->data[0];
        y.stride[0] = f->linesize[0];
        return true;
    }
    if (desc->flags & AV_PIX_FMT_FLAG_PLANAR) {
        if (desc->nb_components == 2) return false;  // grey with alpha
        if (desc->nb_components < 3) return false;    // grey: converted
        // Semi-planar: chroma interleaved in plane 1 (NV12, P010, P016...).
        if (desc->comp[1].plane == desc->comp[2].plane) {
            if (desc->log2_chroma_w != 1 || desc->log2_chroma_h != 1 || desc->comp[1].step != 2 * (wordBits / 8))
                return false;  // NV16/NV24 and the swapped NV21: converted
            if (desc->comp[1].offset > desc->comp[2].offset) return false;
            y.layout = engine::YuvLayout::NV12;
            y.msb = desc->comp[0].shift > 0;
            if (y.msb && desc->comp[0].shift + depth != 16) return false;
            for (int p = 0; p < 2; ++p) { y.plane[p] = f->data[p]; y.stride[p] = f->linesize[p]; }
            return true;
        }
        if (desc->comp[0].shift != 0) return false;
        if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) y.layout = engine::YuvLayout::I420;
        else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) y.layout = engine::YuvLayout::I422;
        else if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) y.layout = engine::YuvLayout::I444;
        else return false;
        for (int c = 0; c < 3; ++c) {
            const int plane = desc->comp[c].plane;
            y.plane[c] = f->data[plane];
            y.stride[c] = f->linesize[plane];
        }
        if (desc->nb_components == 4) {
            y.alpha = f->data[desc->comp[3].plane];
            y.alphaStride = f->linesize[desc->comp[3].plane];
        }
        return true;
    }
    return false;
}

bool rgb8Layout(int format, engine::PixelFormat *out) {
    switch (format) {
    case AV_PIX_FMT_RGBA: *out = engine::PixelFormat::RGBA; return true;
    case AV_PIX_FMT_BGRA: *out = engine::PixelFormat::BGRA; return true;
    case AV_PIX_FMT_RGB0: *out = engine::PixelFormat::RGBx; return true;
    case AV_PIX_FMT_BGR0: *out = engine::PixelFormat::BGRx; return true;
    default: return false;
    }
}

}  // namespace

bool pictureFromFrame(const AVFrame *f, Picture &out, SwsContext **converter, QString *error) {
    out.reset();
    const auto *desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!desc || f->width <= 0 || f->height <= 0) {
        *error = QStringLiteral("The decoded picture has no known pixel format.");
        return false;
    }
    out.width = f->width;
    out.height = f->height;
    out.pixelAspect = f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0
        ? double(f->sample_aspect_ratio.num) / f->sample_aspect_ratio.den : 1.0;
    if (f->format == AV_PIX_FMT_VULKAN && f->hw_frames_ctx) {
        // Left on the GPU: its planes described from the frames' own layout.
        const auto sw = reinterpret_cast<const AVHWFramesContext *>(f->hw_frames_ctx->data)->sw_format;
        const auto *swDesc = av_pix_fmt_desc_get(sw);
        AVFrame shape{};
        shape.format = sw;
        shape.width = f->width;
        shape.height = f->height;
        static uint8_t placeholder[1];
        for (int p = 0; p < 4; ++p) { shape.data[p] = placeholder; shape.linesize[p] = 1; }
        engine::YuvInput y;
        if (swDesc && takeDirect(&shape, swDesc, y) && !y.alpha) {
            describeColour(f, swDesc, y);
            for (auto &plane : y.plane) plane = nullptr;
            out.frame = av_frame_clone(f);
            if (!out.frame) { *error = QStringLiteral("Out of memory for a decoded picture."); return false; }
            out.kind = Picture::Kind::Gpu;
            out.yuv = y;
            return true;
        }
        // A layout the engine does not read: through memory.
        AVFrame *cpu = av_frame_alloc();
        const bool ok = cpu && av_hwframe_transfer_data(cpu, f, 0) >= 0 && av_frame_copy_props(cpu, f) >= 0
                        && pictureFromFrame(cpu, out, converter, error);
        av_frame_free(&cpu);
        if (!ok && error->isEmpty()) *error = QStringLiteral("A picture could not be read back from the GPU.");
        return ok;
    }
    engine::PixelFormat packed;
    if (rgb8Layout(f->format, &packed)) {
        out.frame = av_frame_clone(f);
        if (!out.frame) { *error = QStringLiteral("Out of memory for a decoded picture."); return false; }
        out.kind = Picture::Kind::Rgb8;
        out.rgb8 = {f->width, f->height, out.frame->linesize[0], packed, out.frame->data[0]};
        out.rgb8.limitedRange = f->color_range == AVCOL_RANGE_MPEG;
        return true;
    }
    engine::YuvInput y;
    describeColour(f, desc, y);
    if (takeDirect(f, desc, y)) {
        // The frame is referenced, not copied: the planes stay put.
        out.frame = av_frame_clone(f);
        if (!out.frame) { *error = QStringLiteral("Out of memory for a decoded picture."); return false; }
        for (int p = 0; p < 3; ++p)
            if (y.plane[p]) y.plane[p] = out.frame->data[p];
        if (y.alpha) y.alpha = out.frame->data[desc->comp[3].plane];
        out.kind = Picture::Kind::Yuv;
        out.yuv = y;
        return true;
    }
    out.frame = av_frame_clone(f);  // kept for rgba64(), whatever the layout
    if (!out.frame) { *error = QStringLiteral("Out of memory for a decoded picture."); return false; }
    if (desc->flags & AV_PIX_FMT_FLAG_FLOAT) {
        // Float samples stay float: straight RGBA floats, nothing clipped.
        // (swscale writes float planes, not packed float RGBA: G, B, R, A
        // planes, then interleaved here.)
        *converter = sws_getCachedContext(*converter, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                                          AV_PIX_FMT_GBRAPF32LE, SWS_POINT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!*converter) {
            *error = QStringLiteral("This picture's pixel format cannot be converted for the engine.");
            return false;
        }
        const size_t pixels = size_t(f->width) * f->height;
        std::vector<float> planes(pixels * 4);
        uint8_t *dst[4];
        int strides[4];
        for (int k = 0; k < 4; ++k) { dst[k] = reinterpret_cast<uint8_t *>(planes.data() + pixels * size_t(k)); strides[k] = f->width * 4; }
        if (sws_scale(*converter, f->data, f->linesize, 0, f->height, dst, strides) != f->height) {
            *error = QStringLiteral("Converting the decoded picture failed.");
            return false;
        }
        const bool alpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;
        out.converted.assign(pixels * 16, 0);
        auto *rgba = reinterpret_cast<float *>(out.converted.data());
        for (size_t i = 0; i < pixels; ++i) {
            rgba[i * 4] = planes[pixels * 2 + i];      // R
            rgba[i * 4 + 1] = planes[i];               // G
            rgba[i * 4 + 2] = planes[pixels + i];      // B
            rgba[i * 4 + 3] = alpha ? planes[pixels * 3 + i] : 1.f;
        }
        out.floats = engine::CpuFrame{f->width, f->height, f->width * 16, engine::PixelFormat::RGBAF32, out.converted.data()};
        out.kind = Picture::Kind::Float;
        return true;
    }
    // Anything else (RGB at other depths, grey, palette, big-endian, other
    // subsamplings) once through swscale to 16-bit 4:4:4 Y'CbCr, alpha kept.
    // RGB sources become BT.709 full range on the way; their own range stays.
    const bool alpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;
    const AVPixelFormat target = alpha ? AV_PIX_FMT_YUVA444P16LE : AV_PIX_FMT_YUV444P16LE;
    const bool rgb = desc->flags & AV_PIX_FMT_FLAG_RGB;
    *converter = sws_getCachedContext(*converter, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                                      target, SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!*converter) {
        *error = QStringLiteral("This picture's pixel format cannot be converted for the engine.");
        return false;
    }
    if (rgb) {
        const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(*converter, coefficients, f->color_range != AVCOL_RANGE_MPEG, coefficients, 1, 0, 1 << 16, 1 << 16);
        y.matrix = engine::YuvMatrix::BT709;
        y.fullRange = true;
    } else {
        // Y'CbCr (or grey) laid out afresh: the same matrix and range on both
        // sides, so its codes stay as they are and the range the engine is
        // told (describeColour) is the range they are in. Left to itself the
        // converter squeezes a full-range "J" picture into limited range.
        const int *coefficients = sws_getCoefficients(y.matrix == engine::YuvMatrix::BT601 ? SWS_CS_ITU601
                                                      : y.matrix == engine::YuvMatrix::BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU709);
        sws_setColorspaceDetails(*converter, coefficients, y.fullRange, coefficients, y.fullRange, 0, 1 << 16, 1 << 16);
    }
    const size_t plane = size_t(f->width) * 2 * f->height;
    const int planes = alpha ? 4 : 3;
    out.converted.assign(plane * planes, 0);
    uint8_t *dst[4] = {};
    int strides[4] = {};
    for (int p = 0; p < planes; ++p) { dst[p] = out.converted.data() + plane * p; strides[p] = f->width * 2; }
    if (sws_scale(*converter, f->data, f->linesize, 0, f->height, dst, strides) != f->height) {
        *error = QStringLiteral("Converting the decoded picture failed.");
        out.converted.clear();
        return false;
    }
    y.width = f->width;
    y.height = f->height;
    y.layout = engine::YuvLayout::I444;
    y.depth = 16;
    y.msb = false;
    for (int p = 0; p < 3; ++p) { y.plane[p] = dst[p]; y.stride[p] = strides[p]; }
    if (alpha) { y.alpha = dst[3]; y.alphaStride = strides[3]; }
    out.kind = Picture::Kind::Yuv;
    out.yuv = y;
    return true;
}

struct MediaSource::State {
    QString path;
    SuiteVideo::Decoder decoder;
    SwsContext *converter = nullptr;
    QVariantMap info;
    double position = -1;
    ~State() { sws_freeContext(converter); }
};

MediaSource::MediaSource() : d(std::make_unique<State>()) {}
MediaSource::~MediaSource() = default;

bool MediaSource::open(const QString &path, QString *error, int threads) {
    d->path = path;
    d->position = -1;
    auto options = SuiteVideo::defaultDecodeOptions();
    options.threads = threads;
    // A long way to a picture stops when a newer request has taken over, and
    // carries on if this one is asked for again.
    options.cancelled = [] { return RenderScope::cancelled(); };
    // Vulkan decoding on the engines' own GPU: its pictures stay there.
    int index = -1;
    if (options.backend == QLatin1String("vulkan"))
        if (AVBufferRef *shared = sharedDevice(&index);
            shared && (options.device.isEmpty() || options.device == QString::number(index))) {
            options.vulkanDevice = shared;
            options.keepVulkanFrames = true;
        }
    if (!d->decoder.open(path, options, error)) return false;
    d->info = d->decoder.information();
    return true;
}

namespace {
// Pictures are read on several threads at once (a frame's layers side by
// side), and a test may set or clear the hook while they are: it is copied
// under its lock and called outside it, so a slow hook holds nobody up.
std::shared_mutex hookLock;
std::function<void(const QString &)> &readHook() {
    static std::function<void(const QString &)> hook;
    return hook;
}
}

void setReadHook(std::function<void(const QString &path)> hook) {
    std::unique_lock lock(hookLock);
    readHook() = std::move(hook);
}
void announceRead(const QString &path) {
    std::function<void(const QString &)> hook;
    {
        std::shared_lock lock(hookLock);
        if (!readHook()) return;
        hook = readHook();
    }
    hook(path);
}

bool MediaSource::read(double seconds, Picture &out, QString *error) {
    announceRead(path());
    // A newer request took over: stop before decoding (decodeAt reports it).
    if (RenderScope::cancelled()) {
        *error = QStringLiteral("Cancelled.");
        return false;
    }
    const AVFrame *f = nullptr;
    {
        StageTimer::Scope timed("decode.wait");
        f = d->decoder.frame(seconds, error);
    }
    if (!f) return false;
    {
        StageTimer::Scope timed("decode.wrap");
        if (!pictureFromFrame(f, out, &d->converter, error)) return false;
    }
    out.seconds = seconds;
    d->position = seconds;
    return true;
}

QVariantMap MediaSource::information() const { return d->decoder.information(); }
QString MediaSource::path() const { return d->path; }
double MediaSource::position() const { return d->position; }

namespace {
// Frames a source may go unread before it closes.
constexpr quint64 kIdleFrames = 300;
// And one opened for pictures away from the playhead: sooner, its work is done in bursts.
constexpr quint64 kAsideIdleFrames = 50;
}

MediaPool::MediaPool(int capacity) : m_capacity(qMax(1, capacity)) {}
MediaPool::~MediaPool() = default;

void MediaPool::setLimits(int capacity, qint64 bytes, int threads, int perFile) {
    m_capacity = qMax(1, capacity);
    m_bytes = qMax<qint64>(0, bytes);
    m_threads = qMax(0, threads);
    m_perFile = qBound(1, perFile, 8);
    makeRoom();
}

QString MediaPool::stamp(const QString &path) {
    const QFileInfo file(path);
    return QString::number(file.size()) + QLatin1Char('.') + QString::number(file.lastModified().toMSecsSinceEpoch());
}

// Closes the least recently used sources until the pool is within its
// bounds; never one the frame being drawn has read from. The playhead's
// sources are held to the bounds first, and those for pictures away from it
// to what the playhead's leave (one of them at least): a filmstrip's tiles
// never close a decoder that playing needs, and the two together keep to the
// bounds rather than to twice them.
void MediaPool::makeRoom() {
    qint64 playing = 0;
    int playingCount = 0;
    for (const bool aside : {false, true}) {
        const int capacity = aside ? qMax(1, m_capacity - playingCount) : m_capacity;
        const qint64 bytes = aside && m_bytes > 0 ? qMax<qint64>(0, m_bytes - playing) : m_bytes;
        for (;;) {
            qint64 held = 0;
            int count = 0;
            for (const Entry &e : m_sources)
                if (e.aside == aside) { held += e.bytes; ++count; }
            if (!aside) { playing = held; playingCount = count; }
            if (count <= capacity && (m_bytes <= 0 || held <= bytes || count <= (aside ? 1 : 2))) break;
            auto oldest = m_sources.end();
            for (auto i = m_sources.begin(); i != m_sources.end(); ++i)
                if (i->aside == aside && i->frame != m_frame && (oldest == m_sources.end() || i->used < oldest->used)) oldest = i;
            if (oldest == m_sources.end()) break;
            m_sources.erase(oldest);
        }
    }
}

MediaPool::Entry *MediaPool::opened(const QString &path, const QString &stamp, QString *error) {
    auto source = std::make_unique<MediaSource>();
    if (!source->open(path, error, m_threads)) return nullptr;
    // What its decoder may hold: a dozen or so pictures (its threads' and
    // its references), three bytes a pixel.
    const QVariantMap facts = source->information();
    const qint64 picture = qint64(facts.value("width").toInt()) * facts.value("height").toInt() * 3;
    // Room for it: the newcomer (held as this frame's while room is made)
    // is not the one to go.
    m_sources.push_back(Entry{path, stamp, std::move(source), ++m_clock, m_frame, picture * 14, m_aside});
    const quint64 mine = m_sources.back().used;
    makeRoom();
    for (Entry &e : m_sources)
        if (e.used == mine) { e.frame = m_frame - 1; return &e; }
    return nullptr;
}

// A file that is not as it was when opened: its sources read what is gone.
void MediaPool::dropChanged(const QString &path, const QString &stamp) {
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(), [&](const Entry &e) { return e.path == path && e.stamp != stamp; }),
                    m_sources.end());
}

MediaSource *MediaPool::source(const QString &path, QString *error) {
    const QString now = stamp(path);
    dropChanged(path, now);
    Entry *found = nullptr;
    for (Entry &e : m_sources)
        if (e.path == path && (!found || e.used > found->used)) found = &e;
    if (!found) found = opened(path, now, error);
    if (!found) return nullptr;
    found->used = ++m_clock;
    return found->source.get();
}

MediaSource *MediaPool::sourceAt(const QString &path, double seconds, QString *error) {
    const QString now = stamp(path);
    dropChanged(path, now);
    Entry *ahead = nullptr, *idle = nullptr, *oldest = nullptr;
    int count = 0;
    for (Entry &e : m_sources) {
        // Pictures away from the playhead have sources of their own: the
        // playhead's stay where playing left them.
        if (e.path != path || e.aside != m_aside) continue;
        ++count;
        const double at = e.source->position();
        // Already there, or (one this frame has not read from) playing up to
        // it: no seek. One this frame has read from stays where that part of
        // the file is, so two parts close together do not share it.
        const bool mine = e.frame == m_frame;
        if (at >= 0 && (mine ? seconds == at : seconds >= at && seconds - at <= 1.0) && (!ahead || at > ahead->source->position())) ahead = &e;
        // Not read by this frame: free to go where it is asked (the nearest
        // one first: a step back finds the pictures its decoder kept).
        if (e.frame != m_frame && (!idle || std::abs(seconds - at) < std::abs(seconds - idle->source->position()))) idle = &e;
        if (!oldest || e.used < oldest->used) oldest = &e;
    }
    Entry *chosen = ahead ? ahead : idle;
    if (!chosen && count < m_perFile) chosen = opened(path, now, error);
    if (!chosen) chosen = oldest;
    if (!chosen) return nullptr;
    chosen->used = ++m_clock;
    chosen->frame = m_frame;
    return chosen->source.get();
}

void MediaPool::frameDone() {
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                   [&](const Entry &e) { return m_frame > e.frame + (e.aside ? kAsideIdleFrames : kIdleFrames); }),
                    m_sources.end());
    ++m_frame;
    // What a frame needed all at once may have been more than the pool keeps.
    makeRoom();
}

void MediaPool::unclaim() {
    for (Entry &e : m_sources)
        if (e.frame == m_frame) e.frame = m_frame - 1;
}

void MediaPool::clear() { m_sources.clear(); }
void MediaPool::setAside(bool aside) { m_aside = aside; }
int MediaPool::open() const { return int(m_sources.size()); }
int MediaPool::open(const QString &path) const {
    return int(std::count_if(m_sources.begin(), m_sources.end(), [&](const Entry &e) { return e.path == path; }));
}

}  // namespace render
