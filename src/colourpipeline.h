#pragma once
#include <QImage>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace engine { struct ColourProgram; }

// Suite interchange: straight linear Rec.709 D65, with ACES2065-1 bridge.
// A space may be named with a view, "rec709/rec709@ACES 2.0", wherever a
// transform's target is taken: that display through that tone map.
// All OCIO operations return straight RGBA32F and leave coverage untouched.
namespace ColourPipeline {
// Worker-local opt-in. Nested scopes restore the previous converter; reentrant
// calls inside a converter use the CPU reference, including GPU error replay.
class ScopedConverter {
public:
    using Function = std::function<QImage(const QImage &,const QString &,const QString &,QString *)>;
    explicit ScopedConverter(Function converter);
    ~ScopedConverter();
    ScopedConverter(const ScopedConverter &) = delete;
    ScopedConverter &operator=(const ScopedConverter &) = delete;
private:
    Function m_converter;
    const Function *m_previous;
};
QVariantMap info();
QVariantMap defaults();
// LUT files may have changed on disk (a rescan): everything made from one is
// read again, even a file of the same size and time.
void forgetLuts();
// Counts those rescans, for whoever keeps something made from a LUT.
quint64 lutGeneration();
// A clip's colour management as every timeline renders it: its grade's when
// that is enabled OpenColorIO, otherwise the defaults (a title's input sRGB).
// Every timeline is colour-managed; there is no other engine.
QVariantMap effective(const QVariantMap &grade, bool title = false);
// GPU shader extraction is optional and does not enable GPU execution.
QVariantMap gpuShader(const QString &from,const QString &to,const QString &view = {});
// A transform as the render engine runs it: OCIO's Vulkan shader text with its
// tables and settings for the GPU, OCIO's CPU processor for the CPU twin.
// Straight float RGB in and out, alpha untouched. Kept per transform; null,
// with `error`, when the configuration has no such transform.
std::shared_ptr<const engine::ColourProgram> engineProgram(const QString &from, const QString &to,
                                                           const QString &view = {}, QString *error = nullptr);
// The playback cache's storage curve: working light into 0..1 code values
// (encode) and back. ACEScct's log as OCIO's LogCameraTransform, so HDR
// highlights (to about 222) and a little below black survive 10 bits.
std::shared_ptr<const engine::ColourProgram> engineCacheProgram(bool encode, QString *error = nullptr);
// A clip's input stage as one program: its source LUT (when it has one, on
// the encoded values, as applyLut), then its colour space into `to`.
std::shared_ptr<const engine::ColourProgram> engineInputProgram(const QString &lut, const QString &from, const QString &to,
                                                                QString *error = nullptr);
// Borrowed straight RGBA32F span; OCIO modifies RGB only, without allocation.
bool convertInPlace(float *rgba,qsizetype pixels,const QString &from,const QString &to,
                    const QString &view = {},QString *error = nullptr);
QString validate(const QVariantMap &management);
QString spaceName(const QString &gamut, const QString &gamma);
QImage convert(const QImage &image, const QString &from, const QString &to,
               QString *error = nullptr);
QImage stage(const QImage &image, const QVariantMap &management,
             const QString &from, const QString &to, QString *error = nullptr);
// The display image is packed straight into `target` (per band, the same
// pixels as converting the float result afterwards).
QImage display(const QImage &image, const QVariantMap &options,
               QString *error = nullptr,
               QImage::Format target = QImage::Format_RGBA32FPx4);
QImage applyLut(const QImage &image, const QString &path,
                QString *error = nullptr);
} // namespace ColourPipeline
