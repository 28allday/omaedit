#pragma once
#include <QString>
#include <QVariantMap>
#include <functional>
#include <memory>
struct AVFrame;
struct AVBufferRef;
namespace SuiteVideo {
struct DecodeOptions {
    QString backend = "software"; // software | vulkan | vaapi | nvdec
    QString device;
    int ioTimeoutMs = 8000;
    // Vulkan decoding on this FFmpeg device (borrowed) rather than one of its
    // own: a render engine's, so pictures pass to it without copies.
    AVBufferRef *vulkanDevice = nullptr;
    // Vulkan pictures handed over as they are (AV_PIX_FMT_VULKAN frames on
    // that device), not read back to memory.
    bool keepVulkanFrames = false;
    // Asked while a picture is being reached: true stops the read (it fails
    // with "Cancelled."), and the same request made again carries on from
    // where it stopped. Called on the reading thread.
    std::function<bool()> cancelled;
    // Software decoding threads; 0 for the usual share of the machine.
    int threads = 0;
};
DecodeOptions defaultDecodeOptions();
QString hardwareRestriction(const DecodeOptions &options);
// Vulkan devices (their index, as FFmpeg names them) that offer a video decode
// queue; CPU implementations are left out.
QStringList vulkanDecodeDevices();
// Asks the GPU drivers that keep their video queues behind a switch to show
// them (Intel's Vulkan driver: ANV_DEBUG video-decode and video-encode),
// keeping whatever else was asked. Only that driver reads the switch. Call
// first thing in main, before anything starts Vulkan; children inherit it.
void enableDriverVideoQueues();
// Startup only: set before creating media workers. Each decoder owns a copy.
void setDefaultDecodeOptions(const DecodeOptions &options);
// Counts over every decoder of the process since it started: hardware
// pictures brought down by interface, software replays, and (for tests and
// measurements) "seeks" and "decodedPictures".
QVariantMap runtimeInformation();
// A picture that does not say which matrix its Y'CbCr is in most likely
// follows the standard of its size: BT.601 below 720 lines, BT.709 from
// there up. One rule for every reader (the engine's sources, the source
// viewer's and the media probe), so a clip does not change colour with
// whichever of them draws it.
inline bool untaggedIsBt601(int height) { return height < 720; }
// A video file's picture as its container says, nothing decoded: "width",
// "height", "pixelAspect" and "alpha" (true when it may hold transparency or
// cannot tell), as it is shown: a recording that carries a quarter turn has
// its width and height the way a decoder hands its pictures over. Empty when
// it cannot be read.
QVariantMap pictureFacts(const QString &path);
// One instance per media thread. Returned frames belong to the decoder and
// remain valid until the next call; callers retain outputs by copying/ref'ing.
// Pictures come upright: a recording's display rotation or mirroring (a phone
// held on its side) is applied, so every reader sees what a player shows.
class Decoder {
  public:
    Decoder();
    ~Decoder();
    bool open(const QString &path, const DecodeOptions &options, QString *error);
    const AVFrame *frame(double seconds, QString *error);
    QVariantMap information() const;

  private:
    struct State;
    std::unique_ptr<State> d;
};
} // namespace SuiteVideo
