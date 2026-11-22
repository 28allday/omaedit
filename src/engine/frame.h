#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "grade.h"

// Plain frame descriptions shared by the capture layer, both engine backends
// and the recorder. No Qt types: the engine and its CPU twin stay testable
// without a GUI and portable to a worker thread.
namespace engine {

// Packed 8-bit formats as PipeWire and DRM describe them, byte order in memory.
// RGBA64: 16 bits per channel, little-endian words, R G B A (Qt's RGBA64), for
// pictures worked on the CPU at more than 8 bits (a clip's grade).
// RGBAF32: four native floats a pixel (high dynamic range and float sources).
enum class PixelFormat { BGRx, BGRA, RGBx, RGBA, RGBA64, RGBAF32 };
inline int pixelBytes(PixelFormat f) { return f == PixelFormat::RGBAF32 ? 16 : f == PixelFormat::RGBA64 ? 8 : 4; }

// Source processing; field rate and ordering belong to the presentation clock.
enum class DeinterlaceMode { Off, Discard, Linear, Blend, Adaptive };

inline bool hasAlpha(PixelFormat f) { return f == PixelFormat::BGRA || f == PixelFormat::RGBA || f == PixelFormat::RGBA64 || f == PixelFormat::RGBAF32; }

// A CPU-visible frame the engine reads but does not own.
struct CpuFrame {
    int width = 0;
    int height = 0;
    int stride = 0;          // bytes per row
    PixelFormat format = PixelFormat::BGRx;
    const uint8_t *data = nullptr;
    bool premultiplied = false; // encoded RGB already multiplied by alpha
    bool limitedRange = false; // RGB code values 16..235; alpha stays 0..255
    bool linearAlpha = true; // false applies source alpha to encoded RGB before conversion
    bool encoded = false;    // keep the encoded values (a colour program after it does the transfer)
};

// An 8-bit Y'CbCr frame as decoders, cameras and capture cards deliver it.
// NV12: Y plane, then interleaved Cb/Cr at half size. I420: Y, Cb, Cr planes,
// chroma at half size. YUY2: packed 4:2:2, Y0 Cb Y1 Cr per two pixels. I422:
// Y, Cb, Cr planes, chroma half width and full height (what MJPEG decodes to).
// I444: three full-size planes, retaining all chroma detail.
enum class YuvLayout { NV12, I420, YUY2, I422, I444 };
// Chroma on every row (4:2:2) rather than every other (4:2:0).
inline bool chromaEveryRow(YuvLayout l) { return l == YuvLayout::YUY2 || l == YuvLayout::I422 || l == YuvLayout::I444; }
enum class YuvMatrix { BT601, BT709, BT2020 };
// How a source's R'G'B' becomes light. Sdr is the sRGB curve every 8-bit
// source has always been read with; Pq is SMPTE ST 2084 (absolute, up to
// 10 000 nits) and Hlg ARIB STD-B67 (relative, through the BT.2100 OOTF at
// the HDR nominal peak), both as BT.2100 defines them.
// How an upload's encoded R'G'B' becomes canvas light. Encoded keeps the
// encoded values as they are: a colour program after it (the clip's OCIO
// input transform) does the transfer and the primaries.
enum class Transfer { Sdr, Pq, Hlg, Encoded };
struct YuvInput {
    int width = 0;
    int height = 0;
    YuvLayout layout = YuvLayout::NV12;
    YuvMatrix matrix = YuvMatrix::BT709;
    bool fullRange = false;
    const uint8_t *plane[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};       // bytes
    bool interlaced = false; // 4:2:0 chroma rows alternate between fields
    const uint8_t *alpha = nullptr; // optional full-size, full-range plane, samples as the others
    int alphaStride = 0;
    bool premultiplied = false;
    bool linearAlpha = true;
    // Above 8 bits every sample is a little-endian 16-bit word holding a
    // `depth`-bit code, in its low bits or (msb, as P010) its high bits.
    int depth = 8;
    bool msb = false;
    Transfer transfer = Transfer::Sdr;
    bool wideGamut = false;  // BT.2020 primaries, turned into the canvas's BT.709
    // Where subsampled chroma sits across: midway between its two luma
    // samples (JPEG, most webcams) or level with the left one (H.264, HEVC,
    // MPEG-2 and 4:2:2 production codecs). Down, 4:2:0 chroma is midway.
    bool chromaLeft = false;
    // Wanted at a half or a quarter of its size each way (2 or 4; anything
    // else, the whole picture): each pixel the mean of that block of the
    // picture's own, as they come out of this conversion, weighted by their
    // alpha. For a picture that is only drawn that small, so that nothing
    // after the conversion works on pixels no one sees. A picture in fields
    // is always taken whole. See yuvReduce.
    int reduce = 1;
};
// How many pixels each way go into one of the converted picture's.
inline int yuvReduce(const YuvInput &f, bool fields) { return (f.reduce == 2 || f.reduce == 4) && !f.interlaced && !fields ? f.reduce : 1; }
// The horizontal chroma-grid position of luma column x (half-width chroma).
inline float chromaColumn(int x, bool left) { return left ? float(x) * 0.5f : (float(x) + 0.5f) * 0.5f - 0.5f; }
inline int sampleBytes(const YuvInput &f) { return f.depth > 8 ? 2 : 1; }

// The two coefficients each matrix is built from.
inline void yuvCoefficients(YuvMatrix m, float *kr, float *kb) {
    switch (m) {
    case YuvMatrix::BT601: *kr = 0.299f; *kb = 0.114f; return;
    case YuvMatrix::BT2020: *kr = 0.2627f; *kb = 0.0593f; return;
    case YuvMatrix::BT709: break;
    }
    *kr = 0.2126f;
    *kb = 0.0722f;
}

// `depth`-bit code values (as floats, chroma already interpolated) to R'G'B'
// in 0..1, as BT.2100 quantises them: limited range scales 8-bit 16/219/224
// by 2^(depth−8), full range divides by 2^depth − 1 about 2^(depth−1). The
// shader in yuv_in.comp does the same, in the same order.
// `clamped`: R'G'B' kept to 0..1; a picture kept encoded for its colour
// program keeps what the code values say (above white, below black), as the
// colour-managed decoder does.
inline void yuvToRgb(float y, float cb, float cr, bool full, float kr, float kb, float *r, float *g, float *b, int depth = 8,
                     bool clamped = true) {
    const float s = float(1 << (depth - 8)), top = float((1 << depth) - 1), mid = float(1 << (depth - 1));
    const float Y = full ? y / top : (y - 16.f * s) / (219.f * s);
    const float Pb = full ? (cb - mid) / top : (cb - 128.f * s) / (224.f * s);
    const float Pr = full ? (cr - mid) / top : (cr - 128.f * s) / (224.f * s);
    const float R = Y + 2.f * (1.f - kr) * Pr;
    const float B = Y + 2.f * (1.f - kb) * Pb;
    const float G = (Y - kr * R - kb * B) / (1.f - kr - kb);
    if (!clamped) { *r = R; *g = G; *b = B; return; }
    *r = R < 0.f ? 0.f : R > 1.f ? 1.f : R;
    *g = G < 0.f ? 0.f : G > 1.f ? 1.f : G;
    *b = B < 0.f ? 0.f : B > 1.f ? 1.f : B;
}

// How a YUV frame is laid out as rows of bytes for the GPU upload: Y rows
// first, then chroma rows (NV12: interleaved; planar: Cb then Cr), then
// optional full-size alpha rows.
struct YuvRows {
    int rowBytes = 0;   // widest row
    int rows = 0;
};
inline YuvRows yuvRows(const YuvInput &f) {
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2, b = sampleBytes(f);
    const int a = f.alpha ? f.height : 0;
    switch (f.layout) {
    case YuvLayout::NV12: return {(f.width > cw * 2 ? f.width : cw * 2) * b, f.height + ch + a};
    case YuvLayout::I420: return {f.width * b, f.height + ch * 2 + a};
    case YuvLayout::I422:
    case YuvLayout::I444: return {f.width * b, f.height * 3 + a};
    case YuvLayout::YUY2: break;
    }
    return {cw * 4 * b, f.height + a};
}

// Display transform carried by a capture buffer (Wayland enum ordering).
// Capture transports normalize this to upright output before compositing.
// Flipped variants reverse upright x before undoing the buffer rotation.
enum class BufferTransform : uint32_t { Normal, Rotate90, Rotate180, Rotate270, Flipped, Flipped90, Flipped180, Flipped270 };
inline bool swapsAxes(BufferTransform t) { return (uint32_t(t) & 1) != 0; }
inline void bufferPixel(BufferTransform t, int rawWidth, int rawHeight, int x, int y, bool yInvert, int &sx, int &sy) {
    if(uint32_t(t) & 4) x=(swapsAxes(t) ? rawHeight : rawWidth)-1-x;
    switch(uint32_t(t) & 3) {
    case 1: sx=y; sy=rawHeight-1-x; break;
    case 2: sx=rawWidth-1-x; sy=rawHeight-1-y; break;
    case 3: sx=rawWidth-1-y; sy=x; break;
    default: sx=x; sy=y; break;
    }
    if(yInvert) sy=rawHeight-1-sy;
}

struct DmaBufPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

// A frame living in GPU memory, as handed over by the compositor through
// PipeWire. Single-plane RGB only for now.
struct DmaBufFrame {
    int width = 0;
    int height = 0;
    uint32_t fourcc = 0;       // DRM_FORMAT_*
    uint64_t modifier = 0;     // DRM_FORMAT_MOD_*
    std::vector<DmaBufPlane> planes;
    // Stable identity of the underlying buffer so an engine can cache its import.
    const void *token = nullptr;
    // Optional allocation ownership for cached imports; never a write lease.
    std::shared_ptr<const void> owner;
    bool premultiplied = false;
    bool yInvert = false;
    BufferTransform transform = BufferTransform::Normal;
};

// Linear-light float RGBA, straight alpha. The CPU working frame.
struct FrameF {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;   // width*height*4

    void resize(int w, int h) {
        width = w;
        height = h;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0.f);
    }
    float *px(int x, int y) { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
    const float *px(int x, int y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

// 8-bit BGRA, sRGB encoded, for the preview.
struct Bgra8Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;  // width*height*4, stride = width*4

    void resize(int w, int h) {
        width = w;
        height = h;
        data.assign(static_cast<size_t>(w) * h * 4, 0);
    }
};

// Colour ------------------------------------------------------------------
// What the encoders are handed, as FFmpeg names the layouts: NV12 and I420
// 8-bit 4:2:0, I444 8-bit 4:4:4, P010 (10 bits in the high bits of 16) and
// I010 (in the low bits) 4:2:0, P216 and P416 16-bit 4:2:2 and 4:4:4 with
// interleaved chroma, BGRA 8-bit R'G'B' with opaque alpha, I210 10-bit
// planar 4:2:2 (in the low bits), what APV takes.
enum class ColourFormat { NV12, I420, I444, P010, I010, P216, P416, BGRA, I210 };
constexpr int kColourFormats = 9;
// sRGB, Rec. 601 and Rec. 709 carry the picture's sRGB-encoded values (the
// curve the canvas has always been read out with) with their own matrix and
// primaries; Rec. 2100 PQ and HLG carry BT.2020 primaries, the BT.2020
// matrix and the BT.2100 transfer, SDR white placed at the SDR white level.
enum class ColourSpace { Srgb, Bt601, Bt709, Bt2100Pq, Bt2100Hlg };
inline bool isHdr(ColourSpace s) { return s == ColourSpace::Bt2100Pq || s == ColourSpace::Bt2100Hlg; }
inline int formatBits(ColourFormat f) {
    return f == ColourFormat::P010 || f == ColourFormat::I010 || f == ColourFormat::I210 ? 10
         : f == ColourFormat::P216 || f == ColourFormat::P416 ? 16 : 8;
}
inline bool formatIsRgb(ColourFormat f) { return f == ColourFormat::BGRA; }
// Chroma halved across (4:2:0, 4:2:2) and down (4:2:0).
inline bool chromaHalfWidth(ColourFormat f) { return f != ColourFormat::I444 && f != ColourFormat::P416 && f != ColourFormat::BGRA; }
inline bool chromaHalfHeight(ColourFormat f) {
    return chromaHalfWidth(f) && f != ColourFormat::P216 && f != ColourFormat::I210;
}
// Cb and Cr interleaved in one plane rather than two.
inline bool chromaInterleaved(ColourFormat f) {
    return f == ColourFormat::NV12 || f == ColourFormat::P010 || f == ColourFormat::P216 || f == ColourFormat::P416;
}

// How a layer's source is resampled onto the canvas. Auto: Lanczos when the
// layer enlarges its source (or shows it 1:1), Area when it shrinks it. An
// output rescaled from the canvas takes Area, Bilinear, Bicubic or Lanczos.
enum class ScaleFilter { Auto, Bilinear, Bicubic, Lanczos, Area, Point };

// An output's whole colour description. Defaults are the 8-bit SDR picture
// every output has always carried.
struct OutputColour {
    ColourFormat format = ColourFormat::NV12;
    ColourSpace space = ColourSpace::Bt709;
    bool fullRange = false;     // BGRA is always full
    float sdrWhite = 203.f;     // nits SDR white (the canvas's 1.0) shows at in HDR (BT.2408)
    float hdrPeak = 1000.f;     // nits: the HDR nominal peak
    ScaleFilter downscale = ScaleFilter::Area; // to an output size other than the canvas's
    // The picture already holds the output's R'G'B' (a colour program made
    // it, the OCIO output transform): only the matrix and levels remain.
    bool encoded = false;
    bool operator==(const OutputColour &) const = default;
};

// Encoder-ready pictures in any ColourFormat. `y` holds luma (or the packed
// BGRA rows), `u` the interleaved chroma or Cb, `v` Cr for the planar
// layouts; above 8 bits every sample is a little-endian 16-bit word. Rows are
// packed: rowBytes(plane) apart.
struct YuvFrame {
    int width = 0;
    int height = 0;
    ColourFormat format = ColourFormat::NV12;
    bool nv12 = true;         // format == NV12, kept for 8-bit callers
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;   // planar layouts only

    int chromaWidth() const { return chromaHalfWidth(format) ? (width + 1) / 2 : width; }
    int chromaHeight() const { return chromaHalfHeight(format) ? (height + 1) / 2 : height; }
    int sampleBytes() const { return formatBits(format) > 8 ? 2 : 1; }
    int rowBytes(int plane) const {
        if (formatIsRgb(format)) return plane == 0 ? width * 4 : 0;
        if (plane == 0) return width * sampleBytes();
        if (plane == 2 && chromaInterleaved(format)) return 0;
        return chromaWidth() * sampleBytes() * (chromaInterleaved(format) ? 2 : 1);
    }
    int rows(int plane) const { return plane == 0 ? height : rowBytes(plane) ? chromaHeight() : 0; }
    void resize(int w, int h, ColourFormat f) {
        width = w;
        height = h;
        format = f;
        nv12 = f == ColourFormat::NV12;
        y.assign(size_t(rowBytes(0)) * rows(0), 0);
        u.assign(size_t(rowBytes(1)) * rows(1), 0);
        v.assign(size_t(rowBytes(2)) * rows(2), 0);
    }
    void resize(int w, int h, bool interleaved) { resize(w, h, interleaved ? ColourFormat::NV12 : ColourFormat::I420); }
    // A sample of a plane (0..2) as a code value.
    int sample(int plane, int x, int y_) const {
        const std::vector<uint8_t> &p = plane == 0 ? y : plane == 1 ? u : v;
        const size_t at = size_t(y_) * rowBytes(plane) + size_t(x) * sampleBytes();
        const int word = sampleBytes() == 2 ? p[at] | (p[at + 1] << 8) : p[at];
        return format == ColourFormat::P010 ? word >> 6 : word;
    }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;  // linear
    bool operator==(const Color &) const = default;
};

// How one full picture gives way to another. Cut changes at any progress
// above zero. Fade mixes associated linear colour and alpha; FadeColour goes
// through `colour`, reaching it at `colourPoint`. Swipe sweeps an edge in
// from `side`, revealing the next picture in place; Cover moves the next
// picture in from `side` over the old one; Uncover moves the old one away
// from `side`, uncovering the next; Slide moves both. Wipe reveals the next
// picture where a generated mask is below the progress, over a soft edge.
// Matte keeps the first picture where the second is bright (its `matteRect`
// part, display-encoded brightness), for drawing over the other studio: a
// stinger's track matte. Progress plays no part.
enum class Transition { Cut, Fade, Swipe, Slide, FadeColour, Cover, Uncover, Wipe, Matte };
enum class Side { Left, Right, Top, Bottom };
// Masks start where the name says: Left reveals from the left edge, Iris
// from the centre outwards, Clock from twelve o'clock clockwise.
enum class WipeMask {
    Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight,
    BarnVertical, BarnHorizontal, BarnDiagonal, Iris, Box, Diamond, Clock, Fan,
    BlindsHorizontal, BlindsVertical, Stripes, Checkerboard, Squares, Circles, Spiral,
    ZigZag, Sinus, Curtain, Burst, Cloud, Fractal, Count
};
struct TransitionStyle {
    Transition type = Transition::Fade;
    Side side = Side::Left;
    Color colour;              // FadeColour
    float colourPoint = 0.5f;  // FadeColour: the progress at which only colour shows
    WipeMask mask = WipeMask::Left;
    float softness = 0.1f;     // Wipe: edge width as a share of the mask's range
    bool invert = false;       // Wipe: run the mask backwards; Matte: keep where it is dark
    float matteRect[4] = {0.f, 0.f, 1.f, 1.f};  // Matte: u0, v0, u1, v1 of the second picture
};

// Wipe masks are worked from pixel coordinates with these shared helpers,
// word for word as transition.comp: GPU atan and sin are too loose to agree
// with the CPU, and cell edges come from integer sums, not floored ratios.

// atan2(y, x) in turns, 0..1 anticlockwise from +x.
inline float wipeTurns(float x, float y) {
    const float ax = __builtin_fabsf(x), ay = __builtin_fabsf(y);
    if (ax == 0.f && ay == 0.f) return 0.f;
    const float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0.f) r = 3.14159274f - r;
    if (y < 0.f) r = 6.28318548f - r;
    return r / 6.28318548f;
}
// sin of t turns.
inline float wipeSin(float t) {
    t -= __builtin_floorf(t);
    float s = t < 0.5f ? t : t - 1.f;
    if (s > 0.25f) s = 0.5f - s;
    else if (s < -0.25f) s = -0.5f - s;
    const float z = s * 6.28318548f, z2 = z * z;
    return z * (1.f + z2 * (-0.166666672f + z2 * (0.00833333377f + z2 * (-0.000198412701f + z2 * 2.75573188e-06f))));
}
// Where pixel `p` falls among `cells` equal cells across `size` pixels: the
// cell, and the position inside it (0..1).
inline int wipeCell(int p, int cells, int size, float *inside) {
    const int q = (2 * p + 1) * cells, range = 2 * size;
    *inside = float(q % range) / float(range);
    return q / range;
}
inline float wipeLattice(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return float(h >> 8) * (1.f / 16777216.f);
}
// Smooth value noise, `cells` lattice cells across `size` pixels.
inline float wipeNoise(int px, int py, int size, int cells, uint32_t seed) {
    float fx, fy;
    const int cx = wipeCell(px, cells, size, &fx), cy = wipeCell(py, cells, size, &fy);
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    const float a = wipeLattice(cx, cy, seed), b = wipeLattice(cx + 1, cy, seed);
    const float c = wipeLattice(cx, cy + 1, seed), d = wipeLattice(cx + 1, cy + 1, seed);
    const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}
inline float wipeClamp(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
// The mask at pixel (px, py) of a width × height picture, 0..1: the next
// picture shows there once the progress passes it.
inline float wipeMaskValue(WipeMask mask, int px, int py, int width, int height) {
    const float fx = (px + 0.5f) / width, fy = (py + 0.5f) / height;
    const float hw = width * 0.5f, hh = height * 0.5f;
    const float dx = px + 0.5f - hw, dy = py + 0.5f - hh;
    const float radius = __builtin_sqrtf(hw * hw + hh * hh), r = __builtin_sqrtf(dx * dx + dy * dy) / radius;
    const int columns = 8, rows = (8 * height + width / 2) / width < 1 ? 1 : (8 * height + width / 2) / width;
    const int shorter = width < height ? width : height;
    float ix, iy;
    switch (mask) {
    case WipeMask::Left: return fx;
    case WipeMask::Right: return 1.f - fx;
    case WipeMask::Top: return fy;
    case WipeMask::Bottom: return 1.f - fy;
    case WipeMask::TopLeft: return (fx + fy) * 0.5f;
    case WipeMask::TopRight: return (1.f - fx + fy) * 0.5f;
    case WipeMask::BottomLeft: return (fx + 1.f - fy) * 0.5f;
    case WipeMask::BottomRight: return (2.f - fx - fy) * 0.5f;
    case WipeMask::BarnVertical: return wipeClamp(__builtin_fabsf(dx) / hw);
    case WipeMask::BarnHorizontal: return wipeClamp(__builtin_fabsf(dy) / hh);
    case WipeMask::BarnDiagonal: return wipeClamp(__builtin_fabsf(fx - fy));
    case WipeMask::Iris: return wipeClamp(r);
    case WipeMask::Box: {
        const float a = __builtin_fabsf(dx) / hw, b = __builtin_fabsf(dy) / hh;
        return wipeClamp(a > b ? a : b);
    }
    case WipeMask::Diamond: return wipeClamp((__builtin_fabsf(dx) / hw + __builtin_fabsf(dy) / hh) * 0.5f);
    case WipeMask::Clock: return wipeTurns(-dy, dx);
    case WipeMask::Fan: return wipeClamp(1.f - 2.f * wipeTurns(dx, py + 0.5f));
    case WipeMask::BlindsHorizontal: wipeCell(py, 10, height, &iy); return iy;
    case WipeMask::BlindsVertical: wipeCell(px, 10, width, &ix); return ix;
    case WipeMask::Stripes: {
        const int period = (width + height) / 12 < 4 ? 4 : (width + height) / 12;
        return float((2 * (px + py) + 2) % (2 * period)) / float(2 * period);
    }
    case WipeMask::Checkerboard: {
        const int cx = wipeCell(px, columns, width, &ix), cy = wipeCell(py, rows, height, &iy);
        return (float((cx + cy) & 1) + ix) * 0.5f;
    }
    case WipeMask::Squares: {
        wipeCell(px, columns, width, &ix); wipeCell(py, rows, height, &iy);
        const float a = __builtin_fabsf(ix - 0.5f), b = __builtin_fabsf(iy - 0.5f);
        return wipeClamp((a > b ? a : b) * 2.f);
    }
    case WipeMask::Circles: {
        wipeCell(px, columns, width, &ix); wipeCell(py, rows, height, &iy);
        return wipeClamp(__builtin_sqrtf((ix - 0.5f) * (ix - 0.5f) + (iy - 0.5f) * (iy - 0.5f)) * 1.41421354f);
    }
    case WipeMask::Spiral: return wipeClamp((wipeClamp(r) * 3.f + wipeTurns(dx, dy)) * 0.25f);
    case WipeMask::ZigZag: {
        const float z = fx * 6.f, tri = __builtin_fabsf(2.f * (z - __builtin_floorf(z)) - 1.f);
        return wipeClamp((fy + 0.1f * tri) / 1.1f);
    }
    case WipeMask::Sinus: return wipeClamp((fy + 0.06f * (1.f + wipeSin(fx * 2.f))) / 1.12f);
    case WipeMask::Curtain: return wipeClamp((1.f - wipeClamp(__builtin_fabsf(dx) / hw) + 0.05f * (1.f + wipeSin(fy * 3.f))) / 1.1f);
    case WipeMask::Burst: return wipeClamp(wipeClamp(r) * (0.7f + 0.3f * wipeSin(wipeTurns(dx, dy) * 8.f + 0.25f)));
    case WipeMask::Cloud: {
        float n = 0.f, total = 0.f, amp = 1.f;
        for (int o = 0; o < 4; ++o, amp *= 0.5f) { n += amp * wipeNoise(px, py, shorter, 3 << o, uint32_t(o + 1)); total += amp; }
        return wipeClamp((n / total - 0.2f) / 0.6f);
    }
    case WipeMask::Fractal: {
        float n = 0.f, total = 0.f, amp = 1.f;
        for (int o = 0; o < 6; ++o, amp *= 0.55f) {
            const float v = wipeNoise(px, py, shorter, 4 << o, uint32_t(o + 11));
            n += amp * (1.f - __builtin_fabsf(2.f * v - 1.f)); total += amp;
        }
        return wipeClamp((n / total - 0.3f) / 0.5f);
    }
    case WipeMask::Count: break;
    }
    return fx;
}
// How much of the next picture shows over a mask value at this progress.
// The edge never gets harder than a quarter of a percent, so the two engines'
// last-bit differences cannot flip whole pixels.
inline float wipeCoverage(float maskValue, float progress, float softness, bool invert) {
    const float m = invert ? 1.f - maskValue : maskValue;
    const float s = softness < 0.004f ? 0.004f : softness > 1.f ? 1.f : softness;
    return wipeClamp((progress * (1.f + s) - m) / s);
}
// Along-axis distance in pixels of a pixel's centre from `side`.
inline float sideDistance(Side side, int px, int py, int width, int height) {
    switch (side) {
    case Side::Right: return float(width) - (px + 0.5f);
    case Side::Top: return py + 0.5f;
    case Side::Bottom: return float(height) - (py + 0.5f);
    case Side::Left: break;
    }
    return px + 0.5f;
}
// A point `along` pixels from `side`, on the pixel's other axis, as 0..1
// texture coordinates.
inline void sidePoint(Side side, float along, int px, int py, int width, int height, float *u, float *v) {
    switch (side) {
    case Side::Right: *u = 1.f - along / width; *v = (py + 0.5f) / height; return;
    case Side::Top: *u = (px + 0.5f) / width; *v = along / height; return;
    case Side::Bottom: *u = (px + 0.5f) / width; *v = 1.f - along / height; return;
    case Side::Left: break;
    }
    *u = along / width;
    *v = (py + 0.5f) / height;
}

// Where a source lands on the canvas, in canvas pixels.
// How a layer combines with what is under it. Normal is source-over (linear
// light by default); the others are worked on display-encoded values, as in image editors
// (Overlay or Multiply in linear light look wrong to the eye).
enum class BlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, Add, Subtract, Difference,
                       ColourDodge, ColourBurn, HardLight, SoftLight, Exclusion };
// Space used for source-over opacity mixing; blend functions remain display-encoded.
enum class BlendSpace { Linear, Srgb };

// One channel of a blend, on display-encoded values in 0..1. Shared word for
// word with blendChannel in common.glsl.
inline float blendChannel(int mode, float d, float s) {
    switch (mode) {
    case 1: return d * s;
    case 2: return 1.f - (1.f - d) * (1.f - s);
    case 3: return d < 0.5f ? 2.f * d * s : 1.f - 2.f * (1.f - d) * (1.f - s);
    case 4: return d < s ? d : s;
    case 5: return d > s ? d : s;
    case 6: { const float v = d + s; return v > 1.f ? 1.f : v; }
    case 7: { const float v = d - s; return v < 0.f ? 0.f : v; }
    case 8: { const float v = d - s; return v < 0.f ? -v : v; }
    case 9: return d <= 0.f ? 0.f : s >= 1.f ? 1.f : (d / (1.f - s) < 1.f ? d / (1.f - s) : 1.f);
    case 10: return d >= 1.f ? 1.f : s <= 0.f ? 0.f : 1.f - ((1.f - d) / s < 1.f ? (1.f - d) / s : 1.f);
    case 11: return s < 0.5f ? 2.f * d * s : 1.f - 2.f * (1.f - d) * (1.f - s);
    case 12: {
        if (s <= 0.5f) return d - (1.f - 2.f * s) * d * (1.f - d);
        const float e = d <= 0.25f ? ((16.f * d - 12.f) * d + 4.f) * d : __builtin_sqrtf(d);
        return d + (2.f * s - 1.f) * (e - d);
    }
    case 13: return d + s - 2.f * d * s;
    default: return s;
    }
}

// `footprint`: source texels per canvas pixel along each source axis.
inline ScaleFilter resolveFilter(ScaleFilter f, float footprintX, float footprintY) {
    if (f != ScaleFilter::Auto) return f;
    return (footprintX > 1.001f || footprintY > 1.001f) ? ScaleFilter::Area : ScaleFilter::Lanczos;
}

// Catmull-Rom: sharp, little ringing. Shared word for word with common.glsl.
inline float cubicWeight(float x) {
    x = x < 0.f ? -x : x;
    if (x < 1.f) return 1.5f * x * x * x - 2.5f * x * x + 1.f;
    if (x < 2.f) return -0.5f * x * x * x + 2.5f * x * x - 4.f * x + 2.f;
    return 0.f;
}

// Lanczos, three lobes.
inline float lanczosWeight(float x) {
    x = x < 0.f ? -x : x;
    if (x < 1e-5f) return 1.f;
    if (x >= 3.f) return 0.f;
    const float px = 3.14159265358979f * x;
    return 3.f * __builtin_sinf(px) * __builtin_sinf(px / 3.f) / (px * px);
}

// The area filter's taps per axis: enough to cover the footprint, at most 8.
inline int areaTaps(float footprint) {
    const int n = static_cast<int>(__builtin_ceilf(footprint - 0.001f));
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

// Noise reduction from a strength in 0..1 (0 = off). sigma: how alike two
// pixels' brightness must be for their colour to be shared (spatial chroma);
// threshold: how much a 3x3 patch's mean brightness may change before it is
// taken as motion (grain, being random, mostly averages out of the patch;
// movement does not); keepMin: the least of the new frame kept where nothing
// moves.
struct DenoiseParams {
    float sigma, threshold, keepMin;
};
inline DenoiseParams denoiseParams(float strength) {
    const float s = strength < 0.f ? 0.f : strength > 1.f ? 1.f : strength;
    return {0.02f + 0.06f * s, 0.015f + 0.065f * s, 1.f - 0.85f * s};
}

// One source drawn onto the canvas: a w×h box at (x, y) in canvas pixels,
// rotated by `rotation` radians (clockwise on screen) about its centre,
// showing the part of the source between (u0, v0) and (u1, v1) in 0..1. A
// crop that runs backwards (u1 < u0) mirrors. Edges get one pixel of coverage
// antialiasing, so rotated boxes do not stair-step.
struct Layer {
    int source = -1;
    float x = 0, y = 0, w = 0, h = 0;
    float opacity = 1.f;
    float rotation = 0.f;
    float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
    ScaleFilter filter = ScaleFilter::Auto;
    BlendMode blend = BlendMode::Normal;
    BlendSpace blendSpace = BlendSpace::Linear;
};

// The canvas pixels a layer can touch: its rotated box, a pixel wider for the
// antialiased edge, clipped to the canvas. x1/y1 are exclusive.
struct PixelBounds {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
inline PixelBounds layerBounds(const Layer &l, int canvasW, int canvasH) {
    const float c = __builtin_cosf(l.rotation), s = __builtin_sinf(l.rotation);
    const float cx = l.x + l.w * 0.5f, cy = l.y + l.h * 0.5f;
    const float hw = l.w * 0.5f, hh = l.h * 0.5f;
    const float ex = __builtin_fabsf(c) * hw + __builtin_fabsf(s) * hh;
    const float ey = __builtin_fabsf(s) * hw + __builtin_fabsf(c) * hh;
    PixelBounds b;
    b.x0 = static_cast<int>(__builtin_floorf(cx - ex)) - 1;
    b.y0 = static_cast<int>(__builtin_floorf(cy - ey)) - 1;
    b.x1 = static_cast<int>(__builtin_ceilf(cx + ex)) + 1;
    b.y1 = static_cast<int>(__builtin_ceilf(cy + ey)) + 1;
    b.x0 = b.x0 < 0 ? 0 : b.x0;
    b.y0 = b.y0 < 0 ? 0 : b.y0;
    b.x1 = b.x1 > canvasW ? canvasW : b.x1;
    b.y1 = b.y1 > canvasH ? canvasH : b.y1;
    return b;
}

// The filter a layer is drawn with. Unturned, one source texel per canvas
// pixel, and texel centres on pixel centres: every filter gives exactly the
// texel there, so the cheapest (point) is used. A full-frame clip shown as it
// is costs one read per pixel rather than Lanczos' thirty-six.
inline ScaleFilter layerFilter(const Layer &l, int srcW, int srcH) {
    const float fx = __builtin_fabsf(l.u1 - l.u0) * srcW / l.w, fy = __builtin_fabsf(l.v1 - l.v0) * srcH / l.h;
    const auto whole = [](float v) { return __builtin_fabsf(v - __builtin_roundf(v)) < 1e-3f; };
    if (l.rotation == 0.f && __builtin_fabsf(fx - 1.f) < 1e-5f && __builtin_fabsf(fy - 1.f) < 1e-5f
        && whole(l.u0 * srcW + (l.u1 > l.u0 ? -l.x : l.x)) && whole(l.v0 * srcH + (l.v1 > l.v0 ? -l.y : l.y)))
        return ScaleFilter::Point;
    return resolveFilter(l.filter, fx, fy);
}

// Shared colour maths so both backends and the tests agree to the digit.
inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : __builtin_powf((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    if (c <= 0.f) return 0.f;
    if (c >= 1.f) return 1.f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * __builtin_powf(c, 1.f / 2.4f) - 0.055f;
}
// The Rec. 709 curve, as the timeline's codes have it (colour/oma-video-v1).
inline float rec709Encode(float c) {
    if (c <= 0.f) return 0.f;
    if (c >= 1.f) return 1.f;
    return c < 0.018f ? c * 4.5f : 1.099f * __builtin_powf(c, 0.45f) - 0.099f;
}
inline float rec709Decode(float c) {
    return c < 0.081f ? c / 4.5f : __builtin_powf((c + 0.099f) / 1.099f, 1.f / 0.45f);
}
// BT.2100's two HDR transfers, shared word for word with common.glsl.
// PQ (SMPTE ST 2084): display light as a share of 10 000 nits <-> signal.
inline float pqEncode(float y) {
    y = y < 0.f ? 0.f : y > 1.f ? 1.f : y;
    const float p = __builtin_powf(y, 0.1593017578125f);
    return __builtin_powf((0.8359375f + 18.8515625f * p) / (1.f + 18.6875f * p), 78.84375f);
}
inline float pqDecode(float e) {
    e = e < 0.f ? 0.f : e > 1.f ? 1.f : e;
    const float p = __builtin_powf(e, 1.f / 78.84375f), n = p - 0.8359375f;
    return __builtin_powf((n > 0.f ? n : 0.f) / (18.8515625f - 18.6875f * p), 1.f / 0.1593017578125f);
}
// HLG (ARIB STD-B67): scene light 0..1 <-> signal.
inline float hlgEncode(float e) {
    e = e < 0.f ? 0.f : e > 1.f ? 1.f : e;
    return e <= 1.f / 12.f ? __builtin_sqrtf(3.f * e) : 0.17883277f * __builtin_logf(12.f * e - 0.28466892f) + 0.55991073f;
}
inline float hlgDecode(float v) {
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    return v <= 0.5f ? v * v / 3.f : (__builtin_expf((v - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.f;
}
// The HLG system gamma for a display of this peak (BT.2100 note 5f), kept
// at 1 or more so the OOTF never brightens the shadows.
inline float hlgSystemGamma(float peakNits) {
    const float g = 1.2f + 0.42f * __builtin_log10f((peakNits > 1.f ? peakNits : 1.f) / 1000.f);
    return g < 1.f ? 1.f : g;
}

// Linear RGB from one set of primaries to another (both D65), from the
// primaries' chromaticities: BT.709 (and sRGB), BT.2020, SMPTE 170M.
enum class Primaries { BT709, BT2020, SMPTE170M };
inline void primariesToXyz(Primaries p, double m[9]) {
    const double table[3][6] = {{0.64, 0.33, 0.30, 0.60, 0.15, 0.06},
                                {0.708, 0.292, 0.170, 0.797, 0.131, 0.046},
                                {0.630, 0.340, 0.310, 0.595, 0.155, 0.070}};
    const double *c = table[int(p)];
    const double wx = 0.3127, wy = 0.3290, W[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
    double P[9];  // columns: each primary's XYZ at Y = 1
    for (int i = 0; i < 3; ++i) {
        const double x = c[i * 2], y = c[i * 2 + 1];
        P[i] = x / y; P[3 + i] = 1.0; P[6 + i] = (1.0 - x - y) / y;
    }
    // Scale the columns so the three add up to the white point.
    const double det = P[0] * (P[4] * P[8] - P[5] * P[7]) - P[1] * (P[3] * P[8] - P[5] * P[6]) + P[2] * (P[3] * P[7] - P[4] * P[6]);
    const double inv[9] = {(P[4] * P[8] - P[5] * P[7]) / det, (P[2] * P[7] - P[1] * P[8]) / det, (P[1] * P[5] - P[2] * P[4]) / det,
                           (P[5] * P[6] - P[3] * P[8]) / det, (P[0] * P[8] - P[2] * P[6]) / det, (P[2] * P[3] - P[0] * P[5]) / det,
                           (P[3] * P[7] - P[4] * P[6]) / det, (P[1] * P[6] - P[0] * P[7]) / det, (P[0] * P[4] - P[1] * P[3]) / det};
    double S[3];
    for (int i = 0; i < 3; ++i) S[i] = inv[i * 3] * W[0] + inv[i * 3 + 1] * W[1] + inv[i * 3 + 2] * W[2];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) m[r * 3 + k] = P[r * 3 + k] * S[k];
}
inline void primariesMatrix(Primaries from, Primaries to, float out[9]) {
    double a[9], b[9];
    primariesToXyz(from, a);
    primariesToXyz(to, b);
    const double det = b[0] * (b[4] * b[8] - b[5] * b[7]) - b[1] * (b[3] * b[8] - b[5] * b[6]) + b[2] * (b[3] * b[7] - b[4] * b[6]);
    const double inv[9] = {(b[4] * b[8] - b[5] * b[7]) / det, (b[2] * b[7] - b[1] * b[8]) / det, (b[1] * b[5] - b[2] * b[4]) / det,
                           (b[5] * b[6] - b[3] * b[8]) / det, (b[0] * b[8] - b[2] * b[6]) / det, (b[2] * b[3] - b[0] * b[5]) / det,
                           (b[3] * b[7] - b[4] * b[6]) / det, (b[1] * b[6] - b[0] * b[7]) / det, (b[0] * b[4] - b[1] * b[3]) / det};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            double v = 0;
            for (int j = 0; j < 3; ++j) v += inv[r * 3 + j] * a[j * 3 + k];
            out[r * 3 + k] = float(v);
        }
}

// The levels HDR sources and filters are read against: what the canvas's
// 1.0 means in nits, and the HDR nominal peak (HLG's display).
struct HdrLevels {
    float sdrWhite = 203.f, hdrPeak = 1000.f;
    bool operator==(const HdrLevels &) const = default;
};

// A Y'CbCr source's light, as the push block of yuv_in.comp takes it and the
// CPU twin reads it: kind 0 the sRGB curve, 1 PQ, 2 HLG; `scale` turns
// display light into canvas units (nits / SDR white); `gamma1` is the HLG
// system gamma less one; `gamut` turns BT.2020 into BT.709 when `wide`.
struct InputParams {
    int32_t kind = 0, wide = 0;
    float scale = 1.f, gamma1 = 0.f;
    float gamut[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
inline InputParams inputParams(const YuvInput &f, const HdrLevels &levels) {
    InputParams q;
    const float white = levels.sdrWhite > 1.f ? levels.sdrWhite : 1.f;
    q.kind = f.transfer == Transfer::Pq ? 1 : f.transfer == Transfer::Hlg ? 2 : f.transfer == Transfer::Encoded ? 3 : 0;
    q.scale = q.kind == 1 ? 10000.f / white : q.kind == 2 ? levels.hdrPeak / white : 1.f;
    q.gamma1 = hlgSystemGamma(levels.hdrPeak) - 1.f;
    q.wide = f.wideGamut && q.kind != 3 ? 1 : 0;
    if (q.wide) primariesMatrix(Primaries::BT2020, Primaries::BT709, q.gamut);
    return q;
}
// Encoded R'G'B' (0..1) to canvas light, as yuv_in.comp's toLinear.
inline void inputLinear(const InputParams &q, const float e[3], float o[3]) {
    if (q.kind == 3) {
        for (int k = 0; k < 3; ++k) o[k] = e[k];
        return;
    }
    if (q.kind == 0) {
        for (int k = 0; k < 3; ++k) o[k] = srgbToLinear(e[k]);
    } else if (q.kind == 1) {
        for (int k = 0; k < 3; ++k) o[k] = pqDecode(e[k]) * q.scale;
    } else {
        // Scene light through the OOTF: Fd = Lw · Ys^(γ−1) · E.
        float s[3];
        for (int k = 0; k < 3; ++k) s[k] = hlgDecode(e[k]);
        const float ys = 0.2627f * s[0] + 0.678f * s[1] + 0.0593f * s[2];
        const float f = ys > 0.f ? __builtin_powf(ys, q.gamma1) * q.scale : 0.f;
        for (int k = 0; k < 3; ++k) o[k] = s[k] * f;
    }
    if (q.wide) {
        const float r = o[0], g = o[1], b = o[2];
        for (int k = 0; k < 3; ++k) o[k] = q.gamut[k * 3] * r + q.gamut[k * 3 + 1] * g + q.gamut[k * 3 + 2] * b;
    }
}

// One output conversion as yuv.comp's push block takes it and the CPU twin
// reads it. transfer: 0 the sRGB curve, 1 PQ, 2 HLG, plus 4 when the
// primaries change first (`gamut`, from the canvas's BT.709). PQ takes
// canvas × scale (SDR white / 10 000) up to `limit` (peak / 10 000); HLG
// takes display light canvas × scale (SDR white / peak), at most 1, back
// through the OOTF (Ys = Yd^(1/γ): `hlgExponent` is (1 − γ)/γ). Then
// Y' = kr R' + kg G' + kb B', Cb = (B' − Y')/cbDiv, Cr = (R' − Y')/crDiv,
// each code offset + scale × value, rounded and held in 0..maxCode.
struct OutputParams {
    int32_t format = 0, transfer = 0;
    float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f, cbDiv = 1.8556f, crDiv = 1.5748f;
    float yScale = 219.f, yOffset = 16.f, cScale = 224.f, cOffset = 128.f;
    float scale = 1.f, limit = 1.f, hlgExponent = 0.f;
    float gamut[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int32_t filter = int32_t(ScaleFilter::Area); // rescaling: Bilinear, Bicubic, Lanczos, else Area
};
inline int maxCode(ColourFormat f) { return (1 << formatBits(f)) - 1; }
inline OutputParams outputParams(const OutputColour &c) {
    OutputParams q;
    q.format = int(c.format);
    q.filter = int32_t(c.downscale);
    const bool hdr = isHdr(c.space);
    if (hdr) { q.kr = 0.2627f; q.kg = 0.678f; q.kb = 0.0593f; q.cbDiv = 1.8814f; q.crDiv = 1.4746f; }
    else if (c.space == ColourSpace::Bt601) { q.kr = 0.299f; q.kg = 0.587f; q.kb = 0.114f; q.cbDiv = 1.772f; q.crDiv = 1.402f; }
    const int bits = formatBits(c.format);
    const float s = float(1 << (bits - 8)), top = float((1 << bits) - 1);
    if (formatIsRgb(c.format) || c.fullRange) {
        q.yScale = top; q.yOffset = 0.f; q.cScale = top; q.cOffset = float(1 << (bits - 1));
    } else {
        q.yScale = 219.f * s; q.yOffset = 16.f * s; q.cScale = 224.f * s; q.cOffset = 128.f * s;
    }
    const float white = c.sdrWhite > 1.f ? c.sdrWhite : 1.f, peak = c.hdrPeak > white ? c.hdrPeak : white;
    if (c.space == ColourSpace::Bt2100Pq) {
        q.transfer = 1; q.scale = white / 10000.f; q.limit = peak / 10000.f;
    } else if (c.space == ColourSpace::Bt2100Hlg) {
        const float g = hlgSystemGamma(peak);
        q.transfer = 2; q.scale = white / peak; q.hlgExponent = (1.f - g) / g;
    }
    if (c.encoded) {
        q.transfer = 3;  // as it is: the colour program did the transfer and primaries
        return q;
    }
    if (hdr || c.space == ColourSpace::Bt601) {
        q.transfer |= 4;
        primariesMatrix(Primaries::BT709, hdr ? Primaries::BT2020 : Primaries::SMPTE170M, q.gamut);
    }
    return q;
}
// Canvas light to the output's R'G'B' in 0..1, as yuv.comp's encode().
inline void outputEncode(const OutputParams &q, const float in[3], float e[3]) {
    float c[3] = {in[0], in[1], in[2]};
    if (q.transfer & 4)
        for (int k = 0; k < 3; ++k) c[k] = q.gamut[k * 3] * in[0] + q.gamut[k * 3 + 1] * in[1] + q.gamut[k * 3 + 2] * in[2];
    const int t = q.transfer & 3;
    if (t == 3) {
        for (int k = 0; k < 3; ++k) e[k] = c[k] < 0.f ? 0.f : c[k] > 1.f ? 1.f : c[k];
    } else if (t == 0) {
        for (int k = 0; k < 3; ++k) e[k] = linearToSrgb(c[k]);
    } else if (t == 1) {
        for (int k = 0; k < 3; ++k) { const float v = c[k] * q.scale; e[k] = pqEncode(v < q.limit ? v : q.limit); }
    } else {
        float d[3];
        for (int k = 0; k < 3; ++k) { const float v = c[k] * q.scale; d[k] = v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
        const float yd = q.kr * d[0] + q.kg * d[1] + q.kb * d[2];
        const float f = __builtin_powf(yd > 1e-6f ? yd : 1e-6f, q.hlgExponent);
        for (int k = 0; k < 3; ++k) e[k] = hlgEncode(d[k] * f);
    }
}
inline int outputCode(float offset, float scale, float value, int top) {
    const int v = int(__builtin_roundf(offset + scale * value));
    return v < 0 ? 0 : v > top ? top : v;
}

// How much a matte's display-encoded brightness keeps, from an associated
// linear sample. Shared word for word with transition.comp.
inline float matteCoverage(float r, float g, float b, bool invert) {
    const float l = rec709Encode(0.2126f * r + 0.7152f * g + 0.0722f * b);
    return invert ? 1.f - l : l;
}

// Video filters ------------------------------------------------------------
// A source's filters run in order on its picture after deinterlacing and
// noise reduction, before compositing (Engine::setSourceFilters). Pictures
// stay linear-light straight alpha between steps; the colour controls work on
// display-encoded values, as people expect of them. Colours here are
// display-encoded 0..1.
enum class VideoFilterType { Colour, Sharpen, ChromaKey, ColourKey, LumaKey, Lut, Mask, Crop, Scale, Scroll, RenderDelay, VideoDelay, Blur, Shadow, ToneMap, SdrOnHdr, BoxMask, Grade, Effect, Program, GradePlan, RemoveBackground, LightWrap, Filmed };
// How ToneMap brings HDR light into SDR's 0..1: the BT.2390 EETF (a knee in
// PQ space, highlights rolled into the space left), extended Reinhard, or a
// plain clip.
enum class ToneCurve { Bt2390, Reinhard, Clip };
// What a mask picture does: limit alpha by its alpha or its brightness, or
// multiply, add or subtract its colour. TemporalAlpha: the picture holds the
// display brightness of the frame before (red), after (green) and this one
// (blue), as filmed, and the neighbours' mean alpha; where the picture barely
// changes across them, alpha moves towards the three frames' mean, by the
// opacity.
enum class MaskMode { AlphaFromAlpha, AlphaFromColour, Multiply, Add, Subtract, TemporalAlpha };
// How Scale meets another shape: stretch, fit inside (transparent bars) or fill (cropped).
enum class ScaleFit { Stretch, Fit, Fill };

// A colour look-up table on display-encoded values: `size` entries per axis
// of a cube (red fastest, then green, then blue), or, when not `cube`, three
// curves of `size` entries. Inputs are mapped from the domain to 0..1 first.
struct LookupTable {
    int size = 0;
    bool cube = true;
    float domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    std::vector<float> rgb;  // size^3 or size entries, three floats each
};
// A picture a mask filter stretches over the source: 8-bit display-encoded
// RGBA, straight alpha, rows of width * 4 bytes.
struct FilterImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// The editor's video effects the engine draws (effects/effects.def), each a
// shader and a CPU twin of its own.
enum class Effect : int32_t {
#define EFFECT(Name, stem, passes) Name,
#include "effects/effects.def"
#undef EFFECT
    Count
};
inline int effectPasses(Effect e) {
    switch (e) {
#define EFFECT(Name, stem, passes) case Effect::Name: return passes;
#include "effects/effects.def"
#undef EFFECT
    case Effect::Count: break;
    }
    return 1;
}
constexpr int kEffectKind = 100;  // FilterParams::kind of the first effect

// Corner pin: the perspective map taking the unit square to the quadrilateral
// (top left, top right, bottom right, bottom left; x, y as shares of the
// picture), inverted, row-major: a picture position (x, y, 1) times it gives
// (u w, v w, w). False when the corners make no quadrilateral.
inline bool cornerPinInverse(const float c[8], float out[9]) {
    const double x0 = c[0], y0 = c[1], x1 = c[2], y1 = c[3], x2 = c[4], y2 = c[5], x3 = c[6], y3 = c[7];
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double a, b, cc, d, e, f, g = 0, h = 0;
    if (sx == 0.0 && sy == 0.0) {
        a = x1 - x0; b = x3 - x0; cc = x0; d = y1 - y0; e = y3 - y0; f = y0;
    } else {
        const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2, det = dx1 * dy2 - dx2 * dy1;
        if (det == 0.0) return false;
        g = (sx * dy2 - dx2 * sy) / det;
        h = (dx1 * sy - sx * dy1) / det;
        a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; cc = x0;
        d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }
    const double m[9] = {a, b, cc, d, e, f, g, h, 1};
    const double inv[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                           m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                           m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    const double det = m[0] * inv[0] + m[1] * inv[3] + m[2] * inv[6];
    if (det == 0.0) return false;
    for (int i = 0; i < 9; ++i) out[i] = float(inv[i] / det);
    return true;
}

// A display-referred grade's tonal tables and colour matrix (the editor's
// one-node grade): each channel's value read from its own table of 65536
// entries at the display-encoded level (between entries, interpolated), then
// a 3x3 matrix (hue and saturation about luma), clamped to 0..1.
// A colour transform the engine runs without knowing its maths (an OCIO
// processor, made by the editor). For the GPU: GLSL text declaring
// `vec4 <function>(vec4)`, written for Vulkan, its lookup textures at set 1
// from binding 1 and its uniform block at set 1 binding 0. For the CPU twin:
// the same transform on straight RGBA floats, in place. Both are given the
// working RGB and leave alpha alone.
struct ColourTexture {
    uint32_t binding = 1;
    int dims = 1;                       // 1, 2 or 3
    int width = 1, height = 1, depth = 1;
    int channels = 3;                   // 1 (red only) or 3 (RGB)
    bool linear = true;                 // interpolated, or the nearest entry
    std::vector<float> values;          // width·height·depth·channels, x fastest
};
struct ColourProgram {
    std::string id;                     // the same maths: the same id
    std::string shader, function;
    std::vector<ColourTexture> textures;
    std::vector<uint8_t> uniforms;      // the uniform block's bytes; empty: none
    std::function<void(float *rgba, size_t pixels)> cpu;
};
// What the scopes count of a picture (Engine::countScopes), and where each
// count lies in the table it fills. The numbers are those src/scopes.cpp
// measures with: the engine's pass and that file's CPU counting are twins and
// must give the same counts. Every plane is `hits` then `clipped`, row by row.
struct ScopeLayout {
    int32_t width = 256, height = 200;  // the waveform's raster; the histogram has `width` bins
    int32_t third = 85;                 // one parade column's width
    int32_t vector = 200;               // the vectorscope's side
    bool wave = true, parade = true, histogram = true, vectorscope = true;
    bool lumaOnly = true;               // the waveform draws luma (plane 3), not the chosen channels
    bool selected[4] = {true, true, true, false};  // R, G, B, Y: waveform channels and histogram rows
    // Plotted range, luma weights, chroma gains and the bit depth's steps (0: none).
    double low = -0.1, high = 1.1, inverseRange = 1 / 1.2;
    double kr = 0.2126, kb = 0.0722, cbGain = 1 / 1.8556, crGain = 1 / 1.5748, steps = 0;
    size_t wavePlane() const { return size_t(width) * size_t(height) * 2; }
    size_t paradePlane() const { return size_t(third) * size_t(height) * 2; }
    size_t waveAt(int c) const { return size_t(c) * wavePlane(); }
    size_t paradeAt(int c) const { return 4 * wavePlane() + size_t(c) * paradePlane(); }
    size_t vectorAt() const { return paradeAt(3); }
    size_t histogramAt(int c) const { return vectorAt() + size_t(vector) * size_t(vector) * 2 + size_t(c) * size_t(width) * 2; }
    // How many pixels were counted (all but those whose colour, as measured,
    // was not a number).
    size_t pixelsAt() const { return histogramAt(4); }
    size_t cells() const { return pixelsAt() + 1; }
};
struct GradeTables {
    std::shared_ptr<const std::array<std::vector<float>, 3>> channels;  // 65536 each; null when only the matrix acts
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool chroma = false;
};
// One channel's table at display value v, word for word with filter.comp:
// the table laid out 256 wide, channel c's 256 rows below the previous one's.
inline float gradeTableAt(const std::vector<float> &table, float v) {
    // (A value that is not a number reads the table's first entry.)
    const float f = (v > 0.f ? (v > 1.f ? 1.f : v) : 0.f) * 65535.f;
    const int i0 = int(f), i1 = i0 < 65535 ? i0 + 1 : 65535;
    const float t = f - float(i0);
    return table[size_t(i0)] + (table[size_t(i1)] - table[size_t(i0)]) * t;
}

// A shape a step can be kept within: inside it the step's picture, outside it
// the picture the step was given (the other way round with `invert`), fading
// across the edge over `feather` (a share of the picture's shorter side,
// centred on the edge). Places are fractions of the picture. kind: 0 none,
// 1 rectangle, 2 ellipse (both: centre, half width, half height, turned by
// degrees), 3 polygon (`count` corners in `points`, x then y, 3 to 8).
struct StepShape {
    int32_t kind = 0;
    float box[5] = {0.5f, 0.5f, 0.25f, 0.25f, 0.f};
    float points[16] = {};
    int32_t count = 0;
    float feather = 0;
    bool invert = false;
    bool operator==(const StepShape &) const = default;
};

struct VideoFilter {
    VideoFilterType type = VideoFilterType::Colour;
    StepShape within;  // any step: only inside this shape (see StepShape)
    // Colour: gamma −3..3 (positive lifts the mid-tones), contrast and
    // brightness −1..1, saturation −1 (grey)..4, hue turned by degrees,
    // opacity 0..1; then multiply by one colour and add another.
    float gamma = 0, contrast = 0, brightness = 0, saturation = 0, hue = 0, opacity = 1;
    Color multiply{1, 1, 1, 1}, add{0, 0, 0, 0};
    float amount = 1;  // Sharpen strength 0..1; Lut mix 0..1
    // Chroma key: `key` is the backdrop as filmed (display values). A pixel
    // whose hue dominates by at least 1 − similarity of the backdrop's own
    // strength is clear, fading to solid over `smoothness` of it; spill 0..1
    // takes that share of the key colour out of what stays.
    // Colour key: RGB distance as a share of black to white.
    Color key{0, 1, 0, 1};
    float similarity = 0.2f, smoothness = 0.04f, spill = 0.1f;
    bool matteOnly = false;  // chroma key: the picture's own colour, only alpha keyed
    // Filmed: a kept-encoded picture read through the sRGB curve, so what comes
    // after it sees the codes as filmed as its display values (a key's numbers);
    // with `filmedBack`, back to those codes. Neither loses what is beyond 0..1.
    bool filmedBack = false;
    // RemoveBackground: the people found (matting.h) at quality 0, 1 or 2;
    // tighten 0..1 narrows the matte's soft edge towards its middle. Every
    // frame's matte stands by itself, as a frame of a timeline must.
    int quality = 0;
    float tighten = 0;
    // RemoveBackground: the share of its quality's size the people are found
    // at: below 1 while playback draws smaller than the timeline, where a
    // smaller picture needs no more (see matteSize).
    float matteShare = 1;
    // RemoveBackground: blur the background (by radius, a share of the
    // picture's height) instead of taking it away.
    bool blurBackground = false;
    // LightWrap (see lightWrapParams) uses radius, its width as a share of
    // the height of what it is drawn into, and opacity, its strength 0..3;
    // blendColour takes the edges towards the colours behind instead of adding light.
    bool blendColour = false;
    // Luma key: brightness kept between min and max, faded over the smooth ranges.
    float lumaMin = 0, lumaMax = 1, lumaMinSmooth = 0, lumaMaxSmooth = 0;
    std::shared_ptr<const LookupTable> lut;
    bool tetrahedral = false;
    std::shared_ptr<const FilterImage> image;  // Mask
    MaskMode mask = MaskMode::AlphaFromAlpha;
    Color tint{1, 1, 1, 1};                    // Mask colour; opacity sets its strength
    // Crop: pixels off each edge (negative pads with transparency), or with
    // `absolute` the box at left, top of width × height. Scale: the new size
    // (0 keeps that side, or follows the shape when the other is set).
    // Scroll: the size shown (0 keeps the picture's).
    int left = 0, top = 0, right = 0, bottom = 0;
    bool absolute = false;
    int width = 0, height = 0;
    ScaleFilter sampling = ScaleFilter::Bilinear;
    ScaleFit fit = ScaleFit::Stretch;
    // Scroll: pixels per second, positive to the right and down. Looping
    // repeats the picture edge to edge; otherwise it crosses and comes back.
    float speedX = 0, speedY = 0;
    bool loop = true;
    // RenderDelay keeps every rendered picture; VideoDelay only new ones from
    // the source. Either shows the newest at least this old.
    float delayMs = 0;
    // Blur and Shadow: a Gaussian reaching this many pixels; Shadow is the
    // picture's alpha moved by the offset, in `shadow` at `opacity`, under it.
    float radius = 0;
    int offsetX = 0, offsetY = 0;
    Color shadow{0, 0, 0, 1};
    // ToneMap: the source's brightest light, in nits, brought down to SDR
    // white (the canvas's 1.0) along `tone`. SdrOnHdr: an SDR source's white
    // shown at `whiteNits`, or with `useMultiplier` its light times `multiplier`.
    ToneCurve tone = ToneCurve::Bt2390;
    float peakNits = 1000, whiteNits = 203, multiplier = 1;
    bool useMultiplier = false;
    // BoxMask: alpha kept inside the box left, top, right, bottom (pixels,
    // right and bottom exclusive), fading over `feather` pixels inside its
    // edges (0: a hard edge); with `invert`, kept outside it instead.
    float box[4] = {0, 0, 0, 0};
    float feather = 0;
    bool invert = false;
    std::shared_ptr<const GradeTables> grade;  // Grade
    // Effect: which, its controls as the editor stores them (in the order of
    // its catalogue entry, a choice as its index, a switch as 0 or 1), and the
    // clip's frame, for effects that change as it plays.
    Effect effect = Effect::Invert;
    float values[16] = {};
    int32_t frame = 0;
    // The picture drawn against the one it stands for (below 1 when playback
    // draws the timeline at a viewer's size): distances in pixels and
    // brightness changes between neighbours scale by it, so the effect
    // looks the same at any size.
    float scale = 1.f;
    std::shared_ptr<const ColourProgram> program;  // Program
    std::shared_ptr<const GradePlan> plan;         // GradePlan (grade.h)
    bool operator==(const VideoFilter &) const = default;
};

inline int filterClampSize(int v) { return v < 1 ? 1 : v > 16384 ? 16384 : v; }
// The picture size one filter makes of a width × height one.
inline void filterSize(const VideoFilter &f, int w, int h, int *ow, int *oh) {
    *ow = w;
    *oh = h;
    switch (f.type) {
    case VideoFilterType::Crop:
        *ow = filterClampSize(f.absolute ? f.width : w - f.left - f.right);
        *oh = filterClampSize(f.absolute ? f.height : h - f.top - f.bottom);
        return;
    case VideoFilterType::Scale:
        if (f.width > 0 && f.height > 0) { *ow = filterClampSize(f.width); *oh = filterClampSize(f.height); }
        else if (f.width > 0) { *ow = filterClampSize(f.width); *oh = filterClampSize(int(double(h) * f.width / w + 0.5)); }
        else if (f.height > 0) { *oh = filterClampSize(f.height); *ow = filterClampSize(int(double(w) * f.height / h + 0.5)); }
        return;
    case VideoFilterType::Scroll:
        if (f.width > 0) *ow = filterClampSize(f.width);
        if (f.height > 0) *oh = filterClampSize(f.height);
        return;
    default: return;
    }
}
inline void filteredSize(const std::vector<VideoFilter> &chain, int w, int h, int *ow, int *oh) {
    for (const VideoFilter &f : chain) filterSize(f, w, h, &w, &h);
    *ow = w;
    *oh = h;
}
inline bool filterIsDelay(const VideoFilter &f) {
    return f.type == VideoFilterType::RenderDelay || f.type == VideoFilterType::VideoDelay;
}
// Changes with time alone, so the chain runs every frame.
inline bool filterIsTimed(const VideoFilter &f) {
    return filterIsDelay(f) || (f.type == VideoFilterType::Scroll && (f.speedX != 0.f || f.speedY != 0.f));
}
// Whether step `i` keeps what it holds (a delay's pictures, a scroll's place)
// when the chain goes from `was` to `now`: it is the same step, and nothing
// before it changes the size of what reaches it. A colour slider moving
// elsewhere in the chain then leaves a delay where it was.
inline bool filterStepKept(const std::vector<VideoFilter> &was, const std::vector<VideoFilter> &now, size_t i) {
    if (i >= was.size() || i >= now.size() || !(was[i] == now[i])) return false;
    for (size_t j = 0; j < i; ++j) {
        if (was[j] == now[j]) continue;
        const VideoFilterType t = now[j].type;
        if (was[j].type != t || t == VideoFilterType::Crop || t == VideoFilterType::Scale || t == VideoFilterType::Scroll) return false;
    }
    return true;
}

// The size RemoveBackground finds people at for a w × h picture: its shape,
// about 384 × 224, 512 × 288 or 672 × 384 pixels for quality 0, 1 or 2, each
// side a multiple of 32 (as the network needs). `share` below 1 takes that
// much of the area, down to about 256 × 160: the network's cost follows it.
inline void matteSize(int quality, int w, int h, int *mw, int *mh, float share = 1.f) {
    static const double area[3] = {384.0 * 224.0, 512.0 * 288.0, 672.0 * 384.0};
    const double whole = area[quality < 0 ? 0 : quality > 2 ? 2 : quality], least = 256.0 * 160.0;
    const double want = share < 1.f ? (whole * share > least ? whole * share : least) : whole;
    const double s = __builtin_sqrt(want / (double(w > 1 ? w : 1) * double(h > 1 ? h : 1)));
    const int sw = int(__builtin_lround(w * s / 32.0)) * 32, sh = int(__builtin_lround(h * s / 32.0)) * 32;
    *mw = sw > 32 ? sw : 32;
    *mh = sh > 32 ? sh : 32;
}

// One step as the shader's push block takes it, and the CPU twin too: both
// read the same derived numbers. Kind 0 copies; 1 colour, 2 sharpen,
// 3 chroma key, 4 colour key, 5 luma key, 6 LUT, 7 mask, 8 crop, 9 scroll,
// 10 blur, 11 shadow, 12 tone map, 13 gain (SDR on HDR), 14 box mask, 15 grade,
// 34 a step kept within its shape (shapeParams).
// (Scale draws a layer instead.)
struct FilterParams {
    int32_t kind = 0, mode = 0, ix = 0, iy = 0;
    float p[24] = {};
};
inline double filterPositiveMod(double v, double m) {
    const double r = v - m * __builtin_floor(v / m);
    return r >= m ? 0.0 : r;
}
// The tone map's new brightest channel for one of `m` (canvas light), from
// its FilterParams: mode 0 BT.2390 (p0 SDR white in nits, p1 the source
// peak as a PQ signal, p2 SDR white over it, p3 the knee), 1 Reinhard (p0
// the peak in canvas units, p1 its inverse square), 2 clip. Shared word for
// word with filter.comp.
inline float toneMapValue(int mode, const float *p, float m) {
    if (m <= 0.f) return 0.f;
    if (mode == 1) { const float v = m * (1.f + m * p[1]) / (1.f + m); return v < 1.f ? v : 1.f; }
    if (mode != 0) return m < 1.f ? m : 1.f;
    float e = pqEncode(m * p[0] / 10000.f) / p[1];
    e = e > 1.f ? 1.f : e;
    if (e > p[3]) {
        const float t = (e - p[3]) / (1.f - p[3]), t2 = t * t, t3 = t2 * t;
        e = (2.f * t3 - 3.f * t2 + 1.f) * p[3] + (t3 - 2.f * t2 + t) * (1.f - p[3]) + (-2.f * t3 + 3.f * t2) * p[2];
    }
    const float v = pqDecode(e * p[1]) * 10000.f / p[0];
    return v < 1.f ? v : 1.f;
}

// A Gaussian's taps: at most kBlurTaps a side, a pixel apart up to that
// reach. A wider one (to kBlurReach) puts them blurStep pixels apart, over the
// picture first averaged over that many pixels each way so that nothing finer
// than the step comes through between the taps.
constexpr float kBlurTaps = 64.f, kBlurReach = 512.f;
inline int blurStep(float radius) {
    const float r = radius > kBlurReach ? kBlurReach : radius;
    return r > kBlurTaps ? int(__builtin_ceilf(r / kBlurTaps)) : 1;
}
// `elapsed`: seconds the step has been running (Scroll). Blur and Shadow take
// two passes: 0 across, 1 down (Shadow: down, then under the picture); with
// blurStep above 1, pass 2 comes first: the averaged picture the other two read.
// `sdrWhite`: the nits the canvas's 1.0 stands for (ToneMap, SdrOnHdr).
inline FilterParams filterParams(const VideoFilter &f, int w, int h, double elapsed, int pass = 0, float sdrWhite = 203.f) {
    FilterParams q;
    float *p = q.p;
    // Key edges are never harder than 2 %: GPU pictures are 16-bit floats, and a
    // harder edge would let their last bits flip whole pixels between the engines.
    auto soft = [](float v) { return v < 0.02f ? 0.02f : v; };
    switch (f.type) {
    case VideoFilterType::Colour: {
        // pow(gamma), then one matrix and offset: contrast about mid-grey and
        // brightness, saturation about BT.709 luma, a turn of hue about the
        // grey axis, the multiply colour; then the add colour.
        const double g = f.gamma >= 0.f ? 1.0 / (1.0 + f.gamma) : 1.0 - f.gamma;
        const double k = f.contrast >= 0.f ? 1.0 + 3.0 * f.contrast : 1.0 + f.contrast;
        const double s = f.saturation, wl[3] = {0.2126, 0.7152, 0.0722};
        const double th = f.hue * 3.14159265358979323846 / 180.0, c = __builtin_cos(th), sn = __builtin_sin(th);
        const double third = (1.0 - c) / 3.0, r3 = sn / 1.7320508075688772;
        const double hm[3][3] = {{c + third, third - r3, third + r3}, {third + r3, c + third, third - r3}, {third - r3, third + r3, c + third}};
        const double mul[3] = {f.multiply.r, f.multiply.g, f.multiply.b}, add[3] = {f.add.r, f.add.g, f.add.b};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double v = 0;  // (H · S)[i][j], S = (1 + s) I − s · 1 wᵀ
                for (int m = 0; m < 3; ++m) v += hm[i][m] * ((m == j ? 1.0 + s : 0.0) - s * wl[j]);
                p[i * 4 + j] = float(mul[i] * v * k);
            }
            p[12 + i] = float(mul[i] * (0.5 - 0.5 * k + f.brightness) + add[i]);
        }
        p[3] = float(g);
        p[7] = f.opacity < 0.f ? 0.f : f.opacity > 1.f ? 1.f : f.opacity;
        q.kind = 1;
        break;
    }
    case VideoFilterType::Sharpen: q.kind = 2; p[0] = f.amount; break;
    case VideoFilterType::ChromaKey: {
        // The backdrop's hue direction and strength (Cb/Cr of its display
        // values), the ramp, and the backdrop itself for unmixing and spill.
        const float y = 0.2126f * f.key.r + 0.7152f * f.key.g + 0.0722f * f.key.b;
        const float cb = (f.key.b - y) / 1.8556f, cr = (f.key.r - y) / 1.5748f, len = __builtin_sqrtf(cb * cb + cr * cr);
        if (len < 0.02f) break;  // a grey backdrop has no hue to key
        p[0] = cb / len; p[1] = cr / len; p[2] = 1.f / len;
        p[3] = 1.f - f.similarity; p[4] = 1.f / soft(f.smoothness);
        p[5] = 0.5f;   // other hues count against the key: a cyan or yellow subject stays solid
        p[6] = f.spill;
        p[7] = 0.3f;   // in solid parts, a hue leaning towards the key is partly its own
        p[8] = srgbToLinear(f.key.r); p[9] = srgbToLinear(f.key.g); p[10] = srgbToLinear(f.key.b);
        p[11] = f.key.r; p[12] = f.key.g; p[13] = f.key.b;
        p[14] = 25.f;  // neighbours within ~0.04 of brightness lend their chroma
        p[15] = f.matteOnly ? 1.f : 0.f;
        q.kind = 3;
        break;
    }
    case VideoFilterType::ColourKey:
        p[0] = f.key.r; p[1] = f.key.g; p[2] = f.key.b; p[3] = f.similarity; p[4] = soft(f.smoothness);
        q.kind = 4;
        break;
    case VideoFilterType::LumaKey:
        p[0] = f.lumaMin; p[1] = f.lumaMax; p[2] = soft(f.lumaMinSmooth); p[3] = soft(f.lumaMaxSmooth);
        q.kind = 5;
        break;
    case VideoFilterType::Lut:
        if (!f.lut || f.lut->size < 2) break;
        q.kind = 6;
        q.mode = !f.lut->cube ? 2 : f.tetrahedral ? 1 : 0;
        q.ix = f.lut->size;
        for (int i = 0; i < 3; ++i) {
            const float span = f.lut->domainMax[i] - f.lut->domainMin[i];
            p[i] = span > 1e-6f ? 1.f / span : 1.f;
            p[4 + i] = -f.lut->domainMin[i] * p[i];
        }
        p[3] = f.amount;
        break;
    case VideoFilterType::Mask:
        if (!f.image || f.image->width <= 0 || f.image->height <= 0) break;
        q.kind = 7;
        q.mode = int(f.mask); q.ix = f.image->width; q.iy = f.image->height;
        p[0] = srgbToLinear(f.tint.r); p[1] = srgbToLinear(f.tint.g); p[2] = srgbToLinear(f.tint.b);
        p[3] = f.opacity < 0.f ? 0.f : f.opacity > 1.f ? 1.f : f.opacity;
        break;
    case VideoFilterType::Crop: q.kind = 8; q.ix = f.left; q.iy = f.top; break;
    case VideoFilterType::Blur:
    case VideoFilterType::Shadow: {
        // Taps reach ceil(radius) pixels, at a sigma of a third of it: ix
        // taps a side, iy pixels apart, the sigma (p0) counted in taps.
        const bool blur = f.type == VideoFilterType::Blur;
        const float reach = f.radius < 0.f ? 0.f : f.radius > kBlurReach ? kBlurReach : f.radius;
        const int step = blurStep(reach);
        const float r = reach / float(step);
        q.kind = blur ? 10 : 11;
        // The averaging pass: after the blur's four modes, the shadow's two.
        q.mode = pass == 2 ? (blur ? 4 : 2) : pass;
        q.ix = int(__builtin_ceilf(r - 0.001f));
        q.iy = step;
        p[0] = r > 0.f ? r / 3.f : 1.f;
        // The averaging pass has moved the shadow already.
        if (step == 1 || pass != 0) { p[1] = float(f.offsetX); p[2] = float(f.offsetY); }
        p[3] = f.opacity < 0.f ? 0.f : f.opacity > 1.f ? 1.f : f.opacity;
        p[4] = srgbToLinear(f.shadow.r); p[5] = srgbToLinear(f.shadow.g); p[6] = srgbToLinear(f.shadow.b);
        break;
    }
    case VideoFilterType::Scroll: {
        // Where the picture's left/top edge sits: wrapped over its own size
        // when looping, else over the crossing (shown size plus its own).
        int ow, oh;
        filterSize(f, w, h, &ow, &oh);
        const double speed[2] = {f.speedX, f.speedY}, size[2] = {double(w), double(h)}, shown[2] = {double(ow), double(oh)};
        for (int a = 0; a < 2; ++a) {
            if (speed[a] == 0.0) continue;
            const double period = f.loop ? size[a] : size[a] + shown[a];
            p[a] = float(filterPositiveMod(speed[a] * elapsed, period) - (f.loop ? 0.0 : size[a]));
        }
        q.kind = 9;
        q.mode = f.loop ? 1 : 0;
        break;
    }
    case VideoFilterType::ToneMap: {
        const float white = sdrWhite > 1.f ? sdrWhite : 1.f, peak = f.peakNits > 1.f ? f.peakNits : 1.f;
        q.kind = 12;
        // Nothing above white to bring down: a clip is all there is.
        q.mode = peak <= white ? 2 : f.tone == ToneCurve::Reinhard ? 1 : f.tone == ToneCurve::Clip ? 2 : 0;
        if (q.mode == 0) {
            p[0] = white;
            p[1] = pqEncode(peak / 10000.f);
            p[2] = pqEncode(white / 10000.f) / p[1];
            p[3] = 1.5f * p[2] - 0.5f;
        } else if (q.mode == 1) {
            p[0] = peak / white;
            p[1] = 1.f / (p[0] * p[0]);
        }
        break;
    }
    case VideoFilterType::Effect:
        q.kind = kEffectKind + int(f.effect);
        q.mode = pass;
        q.ix = f.frame;
        for (int i = 0; i < 16; ++i) p[i] = f.values[i];
        p[23] = f.scale;
        // Corner pin's map back from the picture, after its eight corners.
        if (f.effect == Effect::CornerPin && !cornerPinInverse(f.values, p + 8))
            for (int i = 8; i < 17; ++i) p[i] = 0.f;
        break;
    case VideoFilterType::Grade:
        if (!f.grade) break;
        q.kind = 15;
        q.mode = (f.grade->channels ? 1 : 0) | (f.grade->chroma ? 2 : 0);
        for (int i = 0; i < 9; ++i) p[i] = f.grade->matrix[i];
        break;
    case VideoFilterType::BoxMask:
        q.kind = 14;
        q.mode = f.invert ? 1 : 0;
        for (int i = 0; i < 4; ++i) p[i] = f.box[i];
        p[4] = f.feather > 0.f ? f.feather : 0.f;
        break;
    case VideoFilterType::RemoveBackground: {
        // The guided matte's lo..hi becomes clear..solid (see matteParams).
        const float t = f.tighten < 0.f ? 0.f : f.tighten > 1.f ? 1.f : f.tighten;
        q.kind = 24;
        matteSize(f.quality, w, h, &q.ix, &q.iy, f.matteShare);
        p[0] = 0.45f * t;
        p[1] = 1.f / (1.f - 0.9f * t);
        // How wide the band near the matte's edges where the fit takes over: at
        // Fast only, where it sharpens the coarse matte's edges (measured: by
        // about a tenth); Balanced's and Best's finer ones gain nothing.
        p[2] = f.quality == 0 ? 8.f : 0.f;
        break;
    }
    case VideoFilterType::Filmed:
        q.kind = 33;
        q.mode = f.filmedBack ? 1 : 0;
        break;
    case VideoFilterType::SdrOnHdr:
        q.kind = 13;
        p[0] = f.useMultiplier ? (f.multiplier > 0.f ? f.multiplier : 0.f) : (f.whiteNits > 0.f ? f.whiteNits : 0.f) / (sdrWhite > 1.f ? sdrWhite : 1.f);
        break;
    default: break;
    }
    return q;
}

// A step kept within its shape (kind 34): the numbers for a w × h picture, in
// pixels. mode: the shape's kind, + 16 inverted; ix: a polygon's corners.
// Rectangle and ellipse: p0,1 the centre, p2,3 the half sizes, p4,5 the
// turn's cosine and sine. Polygon: p0..15 its corners. p16: the feather.
inline FilterParams shapeParams(const StepShape &s, int w, int h) {
    FilterParams q;
    q.kind = 34;
    q.mode = s.kind | (s.invert ? 16 : 0);
    if (s.kind == 3) {
        q.ix = s.count < 3 ? 3 : s.count > 8 ? 8 : s.count;
        for (int i = 0; i < q.ix; ++i) { q.p[2 * i] = s.points[2 * i] * float(w); q.p[2 * i + 1] = s.points[2 * i + 1] * float(h); }
    } else {
        const float turn = s.box[4] * 3.14159265358979f / 180.f;
        q.p[0] = s.box[0] * float(w); q.p[1] = s.box[1] * float(h);
        q.p[2] = (s.box[2] > 0.f ? s.box[2] : 0.f) * float(w) + 1e-3f; q.p[3] = (s.box[3] > 0.f ? s.box[3] : 0.f) * float(h) + 1e-3f;
        q.p[4] = __builtin_cosf(turn); q.p[5] = __builtin_sinf(turn);
    }
    q.p[16] = (s.feather > 0.f ? s.feather : 0.f) * float(w < h ? w : h);
    return q;
}
// How much of the step shows at a pixel centre (x, y), 0..1: the signed
// distance to the shape's edge over the feather. Word for word as filter.comp's kind 34.
inline float shapeCover(const FilterParams &q, float x, float y) {
    const float *P = q.p;
    const int shape = q.mode & 15;
    float d;
    if (shape == 3) {
        d = (x - P[0]) * (x - P[0]) + (y - P[1]) * (y - P[1]);
        float s = 1.f;
        for (int i = 0, j = q.ix - 1; i < q.ix; j = i, ++i) {
            const float ix = P[2 * i], iy = P[2 * i + 1], ex = P[2 * j] - ix, ey = P[2 * j + 1] - iy, wx = x - ix, wy = y - iy;
            const float ee = ex * ex + ey * ey;
            float t = (wx * ex + wy * ey) / (ee > 1e-12f ? ee : 1e-12f);
            t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            const float bx = wx - ex * t, by = wy - ey * t, bb = bx * bx + by * by;
            if (bb < d) d = bb;
            const bool c0 = y >= iy, c1 = y < P[2 * j + 1], c2 = ex * wy > ey * wx;
            if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
        }
        d = s * __builtin_sqrtf(d);
    } else {
        const float tx = x - P[0], ty = y - P[1];
        const float rx = tx * P[4] + ty * P[5], ry = ty * P[4] - tx * P[5];
        if (shape == 2) {
            const float ax = rx / P[2], ay = ry / P[3], bx = rx / (P[2] * P[2]), by = ry / (P[3] * P[3]);
            const float k0 = __builtin_sqrtf(ax * ax + ay * ay), k1 = __builtin_sqrtf(bx * bx + by * by);
            d = k1 > 1e-12f ? k0 * (k0 - 1.f) / k1 : -(P[2] < P[3] ? P[2] : P[3]);
        } else {
            const float dx = __builtin_fabsf(rx) - P[2], dy = __builtin_fabsf(ry) - P[3];
            const float ox = dx > 0.f ? dx : 0.f, oy = dy > 0.f ? dy : 0.f, in = dx > dy ? dx : dy;
            d = __builtin_sqrtf(ox * ox + oy * oy) + (in < 0.f ? in : 0.f);
        }
    }
    float m = P[16] > 0.f ? 0.5f - d / P[16] : (d <= 0.f ? 1.f : 0.f);
    m = m < 0.f ? 0.f : m > 1.f ? 1.f : m;
    m = m * m * (3.f - 2.f * m);
    return (q.mode & 16) != 0 ? 1.f - m : m;
}

// RemoveBackground after its network, every frame, at the matte's size (mw ×
// mh, see matteSize) unless said: the network's matte brought to the picture's
// own edges with a guided filter (He, Sun and Tang),
// on display-encoded brightness.
//  Stats (kind 27, from the w × h picture, aux the network's matte): each
//    pixel's mean brightness I and I² over the picture pixels it covers, and
//    the matte: (I, m, I², I·m).
//  Across, Down: a plain Gaussian, kind 10 modes 2 and 3, of radius kGuide.
//  Coefficients (kind 28): from the blurred stats, the local fit m ≈ a·I + b.
//  (Across and Down again on a, b.) Pack (kind 32): those beside the
//  matte (aux, the Stats). Then filterParams' kind 24 at w × h makes alpha
//  a·I + b near the matte's edges, the matte elsewhere, all bilinear, I its
//  own brightness. Stats to Pack are 32-bit pictures: I² − Ī² cancels.
//  BackDown (kind 30), BackAcross, BackDown2 (kind 10 0/1), BackOver
//  (kind 31): blurBackground's picture: the picture at a quarter size,
//  blurred, then drawn under the cut-out at w × h.
enum class MattePass { Stats, Across, Down, Coefficients, Pack, BackDown, BackAcross, BackDownBlur, BackOver };
inline FilterParams matteParams(const VideoFilter &f, MattePass pass, int w, int h) {
    constexpr float kGuide = 4.f, kEpsilon = 1e-3f;
    FilterParams q;
    int mw, mh;
    matteSize(f.quality, w, h, &mw, &mh, f.matteShare);
    VideoFilter blur;
    blur.type = VideoFilterType::Blur;
    switch (pass) {
    case MattePass::Stats: q.kind = 27; break;
    case MattePass::Across:
    case MattePass::Down:
        blur.radius = kGuide;
        q = filterParams(blur, mw, mh, 0, pass == MattePass::Across ? 0 : 1);
        q.mode += 2;  // plain values, not colours
        break;
    case MattePass::Coefficients: q.kind = 28; q.p[0] = kEpsilon; break;
    case MattePass::Pack: q.kind = 32; break;
    case MattePass::BackDown: q.kind = 30; q.ix = 4; break;
    case MattePass::BackAcross:
    case MattePass::BackDownBlur:
        // Two passes, so within the reach of taps a pixel apart.
        blur.radius = f.radius * float(h) * 0.25f;
        if (blur.radius > kBlurTaps) blur.radius = kBlurTaps;
        q = filterParams(blur, (w + 3) / 4, (h + 3) / 4, 0, pass == MattePass::BackAcross ? 0 : 1);
        break;
    case MattePass::BackOver: q.kind = 31; q.ix = (w + 3) / 4; q.iy = (h + 3) / 4; q.p[0] = 0.25f; break;
    }
    return q;
}

// Light wrap, as its source is drawn into a picture: the colours of what is
// there already, where the source leaves them uncovered, blurred and brought
// onto the source's edges, so a keyed subject looks lit by what is put behind
// it. It works in the region lightWrapRegion gives, the source drawn alone
// there on the picture's grid, at 1/lightWrapScale of its size. Pass 0 (kind
// 25) takes the uncovered colour under each scale × scale pixels (aux the
// source alone, ix, iy where the region starts, p0 the scale) with how much is
// uncovered as alpha; 1 and 2 blur it across and down over the width; 3 (kind
// 26, from its hw × hh, p1 the scale) brings it onto the source: full at the
// very edge (where half is uncovered), fading inwards.
inline int lightWrapScale(const VideoFilter &f, int pictureHeight) {
    return f.radius * float(pictureHeight) > 192.f ? 8 : 4;
}
inline FilterParams lightWrapParams(const VideoFilter &f, int pass, int pictureHeight, int x0, int y0, int hw, int hh) {
    const int scale = lightWrapScale(f, pictureHeight);
    VideoFilter blur;
    blur.type = VideoFilterType::Blur;
    blur.radius = f.radius * float(pictureHeight) / float(scale);
    if (blur.radius > kBlurTaps) blur.radius = kBlurTaps;  // two passes: taps a pixel apart
    FilterParams q;
    switch (pass) {
    case 0: q.kind = 25; q.ix = x0; q.iy = y0; q.p[0] = float(scale); break;
    case 1:
    case 2: return filterParams(blur, hw, hh, 0, pass - 1);
    default:
        q.kind = 26;
        q.mode = f.blendColour ? 1 : 0;
        q.ix = hw; q.iy = hh;
        q.p[0] = f.opacity < 0.f ? 0.f : f.opacity > 3.f ? 3.f : f.opacity;
        q.p[1] = 1.f / float(scale);
        break;
    }
    return q;
}
// Where light wrap works for a layer drawn into a w × h picture: the layer's
// pixels and as far round them as the light reaches.
inline PixelBounds lightWrapRegion(const VideoFilter &f, const Layer &l, int w, int h) {
    PixelBounds b = layerBounds(l, w, h);
    const int reach = int(__builtin_ceilf(f.radius * float(h)));
    b.x0 = b.x0 - reach < 0 ? 0 : b.x0 - reach;
    b.y0 = b.y0 - reach < 0 ? 0 : b.y0 - reach;
    b.x1 = b.x1 + reach > w ? w : b.x1 + reach;
    b.y1 = b.y1 + reach > h ? h : b.y1 + reach;
    return b;
}
// The layer is the source's own picture, pixel for pixel over the whole of
// the w × h picture: then it is its own "alone".
inline bool layerIsWhole(const Layer &l, int sw, int sh, int w, int h) {
    return l.x == 0.f && l.y == 0.f && l.w == float(w) && l.h == float(h) && sw == w && sh == h && l.rotation == 0.f &&
           l.u0 == 0.f && l.v0 == 0.f && l.u1 == 1.f && l.v1 == 1.f && l.opacity >= 1.f;
}
// The light wrap a source's chain asks for (its last), or none.
inline const VideoFilter *lightWrapOf(const std::vector<VideoFilter> &chain) {
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (it->type == VideoFilterType::LightWrap && it->radius > 0.f && it->opacity > 0.f) return &*it;
    return nullptr;
}

// The layer Scale draws its w × h input as, onto an ow × oh picture.
inline Layer scaleLayer(const VideoFilter &f, int w, int h, int ow, int oh) {
    Layer l;
    l.w = float(ow);
    l.h = float(oh);
    l.filter = f.sampling;
    const float in = float(w) / float(h), out = float(ow) / float(oh);
    if (f.fit == ScaleFit::Fit) {
        if (in > out) { l.h = float(ow) / in; l.y = (float(oh) - l.h) * 0.5f; }
        else { l.w = float(oh) * in; l.x = (float(ow) - l.w) * 0.5f; }
    } else if (f.fit == ScaleFit::Fill) {
        if (in > out) { const float keep = out / in; l.u0 = (1.f - keep) * 0.5f; l.u1 = 1.f - l.u0; }
        else { const float keep = in / out; l.v0 = (1.f - keep) * 0.5f; l.v1 = 1.f - l.v0; }
    }
    return l;
}
// Pictures a delay holds at most: 1 GiB of them, at least two. `pixelBytes`
// is what the engine holding them keeps a pixel in (the GPU's working
// pictures are 16 bytes a pixel).
inline size_t filterDelayCapacity(int w, int h, int pixelBytes = 8) {
    const size_t bytes = size_t(w > 0 ? w : 1) * size_t(h > 0 ? h : 1) * size_t(pixelBytes > 0 ? pixelBytes : 8);
    const size_t n = (size_t(1) << 30) / bytes;
    return n < 2 ? 2 : n > 1200 ? 1200 : n;
}

}  // namespace engine
