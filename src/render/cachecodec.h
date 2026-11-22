#pragma once

// The background render cache's pictures as hardware video: each frame one
// intra-coded 10-bit picture, 4:4:4 or 4:2:0, encoded and decoded by
// whichever GPU video engine the machine has.
//
// Nothing is assumed about the GPU. Which encoder and decoder is found at run
// time: every hardware HEVC and AV1 encoder FFmpeg offers (Vulkan Video, VA-API
// on each render node, NVENC, AMF) is tried in a child process on a test
// picture, in full colour and in 4:2:0, each with every decoder that can read
// it back (the same interface, Vulkan Video, software). Of the pairs that
// give the picture back faithfully, the quickest in full colour is used
// unless it costs half as much again as the quickest in 4:2:0 (the cache is
// there for smooth playback); OMAEDIT_CACHE_CHROMA=444 or 420 takes that one. The answer is kept for a day, and again until
// the machine's devices or encoders change. None: the cache keeps its
// lossless float frames.

#include "frame.h"

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <vector>

namespace render::cachecodec {

struct Choice {
    QString encoder;        // FFmpeg's encoder name ("av1_vaapi", "hevc_nvenc", ...)
    QString encodeDevice;   // its device: a render node, a Vulkan or CUDA index; empty: the default
    QString decodeType;     // the decoder's interface: "vaapi", "vulkan", "cuda" or "software"
    QString decodeDevice;
    bool full = false;      // 4:4:4, else 4:2:0
    // The encoder's own picture layout (FFmpeg's name) that the probe found it
    // takes; an encoder can list layouts it then refuses. Empty: the first.
    QString picture;
    double msPerFrame = 0;  // a 1080p picture encoded and decoded again, as measured
    bool valid() const { return !encoder.isEmpty(); }
    // The picture the engine makes for it: P416 (4:4:4) or P010.
    engine::ColourFormat layout() const { return full ? engine::ColourFormat::P416 : engine::ColourFormat::P010; }
    QVariantMap toMap() const;
    static Choice fromMap(const QVariantMap &map);
};

// This machine's choice, qualified when there is none yet (or it is stale):
// seconds of child processes, so not on the interface thread. `report` (when
// given) says what was tried and why the choice fell as it did.
Choice choose(QVariantMap *report = nullptr);
// The child's side: `--omaedit-cache-probe ENCODER DEVICE`. -1 when the
// arguments are something else.
int probeMain(int argc, char **argv);

// Pictures into packets. Each packet stands alone given the codec's
// parameters (codecId and extradata), so frames are read back in any order.
class Writer {
public:
    // A finished packet and the frame it is; false stops the encoder.
    using Packet = std::function<bool(int frame, const QByteArray &packet)>;
    Writer();
    ~Writer();
    bool open(const Choice &choice, int width, int height, Packet packet, QString *error);
    // One picture (the choice's layout() at the opened size, full range,
    // BT.709 matrix) as `frame`. An encoder may hand packets over a picture
    // or two later.
    bool encode(const engine::YuvFrame &picture, int frame, QString *error);
    // Every picture still held, handed over.
    bool finish(QString *error);
    int codecId() const;
    QByteArray extradata() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

// A picture read back: 10-bit codes in 16-bit words. 4:2:0 as P010 (luma
// in `y`, Cb Cr interleaved in `u`, in the high bits); 4:4:4 planar (`y`,
// `u`, `v`, in the low bits).
struct Planes {
    int width = 0, height = 0;
    bool full = false;
    std::vector<uint16_t> y, u, v;
};

class Reader {
public:
    Reader();
    ~Reader();
    bool open(const Choice &choice, int codecId, const QByteArray &extradata, QString *error);
    bool decode(const QByteArray &packet, Planes &picture, QString *error);

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace render::cachecodec
