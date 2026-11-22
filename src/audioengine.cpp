#include "audioengine.h"
#include "mixer.h"
#include "timeline.h"
#include "renderrequest.h"
#include <QFile>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include "projectfile.h"
#include "projectsafety.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

AudioEngine::ReadScope::ReadScope() : m_previous(s_current) {
  if (!s_current) s_current = std::make_shared<Attempt>();
}
AudioEngine::ReadScope::~ReadScope() {
  if (RenderScope::cancelled() || (RenderScope::current() && !RenderScope::current()->error().isEmpty()))
    s_current->failed = true;
  s_current = std::move(m_previous);
}

AudioEngine::MeterWindow::Range AudioEngine::MeterWindow::samples(int frequency, int count) const {
  count = qMax(0, count);
  if (position < 0 || frequency <= 0 || rate <= 0 || !std::isfinite(fps) || fps <= 0)
    return {0, count};
  const qint64 origin = sound::Mixer::frameStart(position, fps, frequency);
  const auto offset = [&](qint64 sample, int fallback) {
    if (sample < 0) return fallback;
    const __int128 value = (__int128(sample) * frequency * 2 + rate) / (2 * __int128(rate)) - origin;
    return int(std::clamp<__int128>(value, 0, count));
  };
  const int begin = offset(first, 0);
  return {begin, qMax(begin, offset(last, count))};
}
namespace {
using Samples = std::vector<float>;
using Reduction = AudioEngine::MeterCapture::Reduction;
double number(const QVariantMap &m, const QString &key, double fallback = 0) {
  const double v = m.value(key, fallback).toDouble();
  return std::isfinite(v) ? v : fallback;
}
// The mixer's own list: the validator and the mixer must know the same buses.
QVariantList busesOf(const QVariantMap &m) { return sound::busList(m); }
// One automation lane read out of the mix once, so a per-sample evaluation
// is arithmetic on plain numbers rather than QVariant lookups (the fader runs
// once per sample, per track, per frame). Same result as reading the map.
struct Curve {
  struct Key {
    double frame = 0, value = 0;
    bool empty = false, hold = false, smooth = false;
  };
  std::vector<Key> keys;
  double fallback = 0;
  bool off = false;
  Curve(const QVariantMap &mix, const QString &key, double fallback)
      : fallback(fallback) {
    off = mix.value("automation").toString() == "off";
    if (off)
      return;
    for (const auto &v : mix.value("automationKeys").toMap().value(key).toList()) {
      const auto k = v.toMap();
      const QString interpolation = k.value("interpolation").toString();
      keys.push_back({number(k, "frame"), number(k, "value", fallback), k.isEmpty(),
                      interpolation == "hold", interpolation == "smooth"});
    }
  }
  double at(double frame) const {
    if (off)
      return fallback;
    const Key *a = nullptr, *b = nullptr;
    double af = -1e30, bf = 1e30;
    for (const Key &k : keys) {
      if (k.frame <= frame && k.frame >= af) {
        af = k.frame;
        a = &k;
      }
      if (k.frame >= frame && k.frame <= bf) {
        bf = k.frame;
        b = &k;
      }
    }
    if (!a || a->empty)
      return !b || b->empty ? fallback : b->value;
    if (!b || b->empty || af == bf || a->hold)
      return a->value;
    double t = (frame - af) / (bf - af);
    if (a->smooth)
      t = t * t * (3 - 2 * t);
    return a->value * (1 - t) + b->value * t;
  }
};
QVariantMap levels(const Samples &data, int ch, int rate, const AudioEngine::MeterWindow &window) {
  double peak[2] = {}, square[2] = {};
  const int n = ch > 0 ? data.size() / ch : 0;
  const auto range = window.samples(rate, n);
  for (int i = range.first; i < range.last; ++i)
    for (int c = 0; c < qMin(ch, 2); ++c) {
      const double v = data[i * ch + c];
      peak[c] = qMax(peak[c], std::abs(v));
      square[c] += v * v;
    }
  if (ch == 1) {
    peak[1] = peak[0];
    square[1] = square[0];
  }
  auto db = [](double v) { return v > 1e-5 ? 20 * std::log10(v) : -100.; };
  return {{"out", QVariantList{db(peak[0]), db(peak[1])}},
          {"rms", QVariantList{db(std::sqrt(square[0] / qMax(1, range.last - range.first))),
                               db(std::sqrt(square[1] / qMax(1, range.last - range.first)))}}};
}
} // namespace

struct AudioEngine::MeterCapture::State {
  struct Track { int index = 0, rate = 0; Samples peaks; };
  struct Route {
    int channels = 0, rate = 0, reductionRate = 0;
    Samples samples;
    QVariantMap meter, mix;
    std::shared_ptr<const Reduction> reduction;
  };
  QMutex mutex;
  int tracks = 0;
  std::vector<Track> stages;
  std::vector<Route> routes;
};
AudioEngine::MeterCapture::MeterCapture(int tracks) : m_state(std::make_unique<State>()) {
  m_state->tracks = qMax(0, tracks);
}
AudioEngine::MeterCapture::~MeterCapture() = default;
void AudioEngine::MeterCapture::clear() {
  QMutexLocker lock(&m_state->mutex);
  m_state->stages.clear(); m_state->routes.clear();
}
void AudioEngine::MeterCapture::clearRouting() {
  QMutexLocker lock(&m_state->mutex); m_state->routes.clear();
}
void AudioEngine::MeterCapture::addTrack(int track, int stage, int rate, Samples peaks) {
  if (track < 0 || track >= m_state->tracks || stage < 0 || stage > 2 || rate <= 0) return;
  QMutexLocker lock(&m_state->mutex);
  m_state->stages.push_back({track * 6 + stage * 2, rate, std::move(peaks)});
}
void AudioEngine::MeterCapture::appendTracks(const MeterCapture &other) {
  if (this == &other) return;
  std::vector<State::Track> stages;
  { QMutexLocker lock(&other.m_state->mutex); stages = other.m_state->stages; }
  QMutexLocker lock(&m_state->mutex);
  for (auto &stage : stages)
    if (stage.index >= 0 && stage.index + 1 < m_state->tracks * 6)
      m_state->stages.push_back(std::move(stage));
}
void AudioEngine::MeterCapture::addRouting(const Samples &data, int channels, int rate,
    const QVariantMap &meter, const QVariantMap &mix,
    std::shared_ptr<const Reduction> reduction, int reductionRate) {
  if (channels <= 0 || rate <= 0) return;
  State::Route route;
  route.channels = qMin(2, channels); route.rate = rate; route.reductionRate = reductionRate;
  route.meter = meter; route.mix = mix; route.reduction = std::move(reduction);
  const size_t count = data.size() / channels;
  route.samples.resize(count * route.channels);
  for (size_t i = 0; i < count; ++i)
    for (int c = 0; c < route.channels; ++c) route.samples[i * route.channels + c] = data[i * channels + c];
  QMutexLocker lock(&m_state->mutex); m_state->routes.push_back(std::move(route));
}
AudioEngine::MeterCapture::Measurements AudioEngine::MeterCapture::measure(const MeterWindow &window) const {
  QMutexLocker lock(&m_state->mutex);
  Measurements result; result.tracks = QList<float>(m_state->tracks * 6, 0.f);
  for (const auto &stage : m_state->stages) {
    const auto range = window.samples(stage.rate, int(stage.peaks.size() / 2));
    for (int i = range.first; i < range.last; ++i)
      for (int c = 0; c < 2; ++c)
        result.tracks[stage.index + c] = qMax(result.tracks[stage.index + c], stage.peaks[2 * i + c]);
  }
  for (const auto &route : m_state->routes) {
    auto meter = route.meter;
    const auto measured = levels(route.samples, route.channels, route.rate, window);
    for (auto i = measured.cbegin(); i != measured.cend(); ++i) meter[i.key()] = i.value();
    if (route.reduction) {
      const auto range = window.samples(route.reductionRate, int(route.reduction->size()));
      double maximum = 0;
      for (int i = range.first; i < range.last; ++i) maximum = qMax(maximum, (*route.reduction)[i]);
      meter["gainReduction"] = maximum;
    }
    if (!route.mix.value("automationKeys").toMap().isEmpty() && route.mix.value("automation") != "off") {
      const auto range = window.samples(route.rate, int(route.samples.size() / route.channels));
      const double position = window.position + double(range.first) * window.fps / route.rate;
      meter["volume"] = automated(route.mix, "volume", position, number(route.mix, "volume"));
      meter["pan"] = automated(route.mix, "pan", position, number(route.mix, "pan"));
    }
    result.routing.append(meter);
  }
  return result;
}

QString AudioEngine::projectResource(const QString &path) {
  return ProjectFile::isProject(path) && ProjectSafety::isDocument(path)
             ? "omaedit.project:" + path
             : path;
}

double AudioEngine::automated(const QVariantMap &mix, const QString &key,
                              double frame, double fallback) {
  return Curve(mix, key, fallback).at(frame);
}
QVariantMap AudioEngine::serviceCapabilities() {
  const QVariantList cleanup{
      QVariantMap{{"id", "highpass"},
                  {"group", "voice"},
                  {"params", QVariantMap{{"frequency", 80}}}},
      QVariantMap{
          {"id", "nr"},
          {"group", "voice"},
          {"params",
           QVariantMap{{"reduction", 10}, {"floor", -50}, {"track", true}}}},
      QVariantMap{
          {"id", "deesser"},
          {"group", "voice"},
          {"params",
           QVariantMap{{"frequency", 6500}, {"intensity", .55}, {"max", .5}}}},
      QVariantMap{{"id", "comp"},
                  {"group", "voice"},
                  {"params", QVariantMap{{"threshold", -24},
                                         {"ratio", 2.5},
                                         {"attack", 8},
                                         {"release", 150},
                                         {"makeup", 3},
                                         {"knee", 8}}}}};
  return {{"version", 1},
          {"cleanupProvider", "builtin-dsp"},
          {"learned", false},
          {"cleanupChain", cleanup},
          {"noiseReduction", "omaedit.denoise"},
          {"humRemoval", "parametric-notch"},
          {"pluginHost",
           QVariantMap{{"available", false},
                       {"reason", "Third-party plug-in host is not installed. "
                                  "Built-in effects remain available."},
                       {"interfaceVersion", 1}}}};
}
QString AudioEngine::validateRouting(const QVariantList &tracks,
                                     const QVariantMap &master) {
  const auto buses = busesOf(master);
  if (buses.size() > 32)
    return QStringLiteral("At most 32 audio buses are supported.");
  QMap<QString, QVariantMap> all;
  for (const auto &v : buses) {
    auto b = v.toMap();
    auto id = b.value("id").toString();
    if (id.isEmpty() || id == "master" || all.contains(id))
      return QStringLiteral("Audio buses need unique IDs.");
    all[id] = b.value("mix").toMap();
  }
  auto destinations = [](const QVariantMap &m) {
    QStringList ds;
    ds << m.value("output", "master").toString();
    for (const auto &v : m.value("sends").toList())
      ds << v.toMap().value("bus").toString();
    return ds;
  };
  QSet<QString> active, done;
  std::function<bool(QString)> visit = [&](const QString &id) {
    if (id.isEmpty() || id == "master")
      return true;
    if (!all.contains(id) || active.contains(id))
      return false;
    if (done.contains(id))
      return true;
    active.insert(id);
    for (const auto &d : destinations(all[id]))
      if (!visit(d))
        return false;
    active.remove(id);
    done.insert(id);
    return true;
  };
  for (auto it = all.cbegin(); it != all.cend(); ++it)
    if (!visit(it.key()))
      return QStringLiteral(
          "Audio routing contains a missing bus or feedback loop.");
  for (const auto &v : tracks)
    for (const auto &d : destinations(v.toMap().value("mix").toMap()))
      if (!d.isEmpty() && d != "master" && !all.contains(d))
        return QStringLiteral("A track routes to a missing audio bus.");
  return {};
}

struct AudioEngine::Routing::State {
  QMutex mutex;
  QVariantList meters;
  MeterWindow window;
};
AudioEngine::Routing::Routing() : m_state(std::make_shared<State>()) {}
void AudioEngine::Routing::setMeterWindow(const MeterWindow &window) {
  QMutexLocker lock(&m_state->mutex);
  m_state->window = window;
}
void AudioEngine::Routing::clearMeters() {
  QMutexLocker lock(&m_state->mutex);
  m_state->meters.clear();
}
QVariantList AudioEngine::Routing::meters() {
  QMutexLocker lock(&m_state->mutex);
  return m_state->meters;
}
void AudioEngine::Routing::addMeter(const QVariantMap &meter) {
  QMutexLocker lock(&m_state->mutex);
  m_state->meters.append(meter);
}
QVariantMap AudioEngine::routeMeter(const std::vector<float> &post, int channels, int rate, const MeterWindow &window,
                                    const QVariantMap &mix, double reduction, double meterPosition) {
  auto meter = levels(post, channels, rate, window);
  if (!mix.value("automationKeys").toMap().isEmpty() && mix.value("automation") != "off") {
    meter["volume"] = AudioEngine::automated(mix, "volume", meterPosition, number(mix, "volume"));
    meter["pan"] = AudioEngine::automated(mix, "pan", meterPosition, number(mix, "pan"));
  }
  meter["gainReduction"] = reduction;
  return meter;
}

QStringList AudioEngine::channelModes() { return {QStringLiteral("stereo"), QStringLiteral("mono"), QStringLiteral("swap"), QStringLiteral("left"), QStringLiteral("right")}; }
QByteArray AudioEngine::mapChannels(QByteArray pcm, int channels, const QString &mode) {
  if (channels != 2 || mode.isEmpty() || mode == QStringLiteral("stereo"))
    return pcm;
  if (mode == QStringLiteral("mono"))
    return monitorAudio(pcm, channels, true);
  auto *d = reinterpret_cast<qint16 *>(pcm.data());
  const int frames = pcm.size() / 4;
  for (int i = 0; i < frames; ++i) {
    qint16 &l = d[i * 2], &r = d[i * 2 + 1];
    if (mode == QStringLiteral("swap")) std::swap(l, r);
    else if (mode == QStringLiteral("left")) r = l;
    else if (mode == QStringLiteral("right")) l = r;
  }
  return pcm;
}
QByteArray AudioEngine::monitorAudio(QByteArray pcm, int channels, bool mono) {
  if (!mono || channels < 2)
    return pcm;
  auto *d = reinterpret_cast<qint16 *>(pcm.data());
  for (int i = 0; i + channels <= pcm.size() / 2; i += channels) {
    qint64 sum = 0;
    for (int c = 0; c < channels; ++c)
      sum += d[i + c];
    const qint16 value = qint16(sum / channels);
    for (int c = 0; c < channels; ++c)
      d[i + c] = value;
  }
  return pcm;
}
