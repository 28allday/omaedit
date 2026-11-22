#pragma once

#include <QVariantMap>
#include <memory>
#include <vector>
#include <QtGlobal>

namespace AudioEngine {
// A synchronous sound read. Completed track inserts may survive a failed mix
// attempt, but later transport cancellation must not turn a successful read
// into a retry. Nested timelines share the outer attempt.
class ReadScope {
public:
  struct Attempt { bool failed = false; };
  ReadScope();
  ~ReadScope();
  ReadScope(const ReadScope &) = delete;
  ReadScope &operator=(const ReadScope &) = delete;
  static std::shared_ptr<Attempt> current() { return s_current; }
private:
  std::shared_ptr<Attempt> m_previous;
  inline static thread_local std::shared_ptr<Attempt> s_current;
};
class MeterCapture;
// Meter only a timeline interval; effects still process every decoded sample.
// Bounds are absolute samples on `rate`, with an exclusive last sample. An
// unset bound includes that side of the frame. The default measures it all.
struct MeterWindow {
  int position = -1;
  double fps = 25;
  qint64 first = -1, last = -1;
  int rate = 48000;
  std::shared_ptr<MeterCapture> capture = {};
  struct Range { int first = 0, last = 0; };
  Range samples(int frequency, int count) const;
};
// Optional per-decoded-block measurements. The playback queue publishes a const
// capture and can measure a remaining interval without reading the graph again.
// Only the channels used by each meter are retained, independently of PCM/DSP.
class MeterCapture {
public:
  using Reduction = std::vector<double>;
  struct Measurements { QList<float> tracks; QVariantList routing; };
  explicit MeterCapture(int tracks);
  ~MeterCapture();
  void clear();
  void clearRouting();
  void addTrack(int track, int stage, int rate, std::vector<float> stereoPeaks);
  void appendTracks(const MeterCapture &other);
  void addRouting(const std::vector<float> &data, int channels, int rate,
                  const QVariantMap &meter, const QVariantMap &mix,
                  std::shared_ptr<const Reduction> reduction, int reductionRate);
  Measurements measure(const MeterWindow &window) const;
private:
  struct State;
  std::unique_ptr<State> m_state;
};
QVariantMap serviceCapabilities();
QString projectResource(const QString &path);
QByteArray monitorAudio(QByteArray pcm, int channels, bool mono);
// Device-level channel mapping (Preferences ▸ Audio): "stereo" (as is),
// "mono" (fold to the centre), "swap" (L↔R), "left" / "right" (one side on
// both). Interleaved s16; anything but 2 channels passes through untouched.
QByteArray mapChannels(QByteArray pcm, int channels, const QString &mode);
QStringList channelModes();
double automated(const QVariantMap &mix, const QString &key, double frame,
                 double fallback);
QString validateRouting(const QVariantList &tracks, const QVariantMap &master);
// A route's meter as the routing filter makes it: levels within the meter
// window, the automation's values there, the inserts' gain reduction.
QVariantMap routeMeter(const std::vector<float> &post, int channels, int rate, const MeterWindow &window,
                       const QVariantMap &mix, double reduction, double meterPosition);

// A timeline's routing meters, as its mixer measured them.
class Routing {
public:
  struct State;
  Routing();
  QVariantList meters();
  // A meter from elsewhere (the editor's own mixer), after the ones there.
  void addMeter(const QVariantMap &meter);
  void setMeterWindow(const MeterWindow &window);
  void clearMeters();

private:
  std::shared_ptr<State> m_state;
};
} // namespace AudioEngine
