#include "playerworker.h"
#include <QScopedValueRollback>
#include "mediaprobe.h"
#include "mixer.h"
#include "audiotimeline.h"
#include "renderrequest.h"
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
namespace {
constexpr int kFrequency = 48000, kChannels = 2;
}

bool PlayerWorker::fineAudioSample(qint64 *sample) const {
  AudioTimeline::Time time;
  if (m_fineAudioFrame!=m_position || !AudioTimeline::readTime(m_fineAudioPosition,&time)
      || !AudioTimeline::sameEditContext(m_fineAudioContext,audioEditContext(),false)) return false;
  *sample=qint64((__int128(time.ticks)*kFrequency*2+time.rate)/(2*time.rate));
  return true;
}

bool PlayerWorker::parkFinePlayback() {
  qint64 sample=0;
  if (!m_playAnchored) return fineAudioSample(&sample); // Play/Pause before the first pump
  if (!m_playFineAudio || !m_audioOutput->isOpen()) return false;
  const qint64 played=qBound<qint64>(0,m_audioOutput->suspend(),m_audioWritten);
  if (m_audioSpans.isEmpty()) {
    // The first picture may have been interrupted before any completed sound
    // reached the sink. Its prepared samples, if any, still begin at this cursor.
    if (played!=0 || !fineAudioSample(&sample)) return false;
    if (!m_pausedAudio.isEmpty()) retainPausedAudio(0);
    return true;
  }
  if (played==0 && m_audioSpans.first().offset==0 && m_fineAudioFrame>=0) {
    // Nothing was consumed: retain the original rational cursor, which may
    // lie between output samples on a different project sample grid.
    m_position=m_fineAudioFrame;
    retainPausedAudio(played);
    return true;
  }
  const AudioSpan *span=&m_audioSpans.first();
  for (const auto &candidate:m_audioSpans) {
    if (candidate.offset>played) break;
    span=&candidate;
  }
  sample=span->firstSample+qBound<qint64>(0,played-span->offset,span->bytes)/(kChannels*int(sizeof(qint16)));
  const int total=m_timeline.frameCount();
  if (m_loop && total>0) {
    const int lo=m_loopIn>=0?qMin(m_loopIn,total-1):0;
    const int hi=m_loopOut>=lo?qMin(m_loopOut,total-1):total-1;
    const qint64 end=sound::Mixer::frameStart(hi+1,m_timeline.fps(),kFrequency);
    if (sample==end) sample=sound::Mixer::frameStart(lo,m_timeline.fps(),kFrequency);
  }
  const auto *profile=m_timeline.profile();
  const auto frame=__int128(sample)*profile->frame_rate_num()/(__int128(kFrequency)*profile->frame_rate_den());
  if (frame<0 || frame>=total) return false; // natural end retains the last picture
  m_position=int(frame);m_fineAudioFrame=m_position;
  m_fineAudioPosition=AudioTimeline::writeTime({sample,kFrequency});
  m_fineAudioContext=audioEditContext();
  retainPausedAudio(played);
  return true;
}

void PlayerWorker::discardPausedAudio() {
  m_pausedAudio.clear();m_pausedContext.clear();m_audioHistory.clear();m_playContext.clear();
}

void PlayerWorker::retainPausedAudio(qint64 played) {
  // An edit may have rebuilt the graph before it stops playback. Never combine
  // old processed sound with the new graph, even if its frame count is unchanged.
  if (m_playContext!=audioEditContext()) {discardPausedAudio();return;}
  QList<QueuedFrame> remaining;
  for (auto frame:std::as_const(m_audioHistory)) {
    const qint64 drop=qBound<qint64>(0,played-frame.audioOffset,frame.decoded.audio.size());
    frame.decoded.audio.remove(0,drop);
    if (frame.decoded.audio.isEmpty()) continue;
    frame.firstSample+=drop/(kChannels*int(sizeof(qint16)));
    if (drop > 0 && frame.meterCapture) {
      const auto measured=frame.meterCapture->measure({frame.position,m_timeline.fps(),frame.firstSample,-1,kFrequency});
      frame.trackPeaks=measured.tracks;frame.routing=measured.routing;
    }
    frame.durationNs=qint64((__int128(frame.decoded.audio.size())*1000000000+
        kFrequency*kChannels*sizeof(qint16)/2)/(kFrequency*kChannels*sizeof(qint16)));
    remaining.append(std::move(frame));
  }
  // A second pause can happen before all previously retained blocks have been
  // resubmitted. They still precede the next frame the effects must process.
  remaining.append(m_pausedAudio);
  // (Where the sound will go on: with sound made ahead, that is past the pictures.)
  if (m_pausedContext.isEmpty()) m_pausedNext=m_soundAhead?m_soundNext:m_playNext;
  m_pausedAudio=std::move(remaining);m_pausedContext=m_playContext;
}

int PlayerWorker::nextAudioPosition(int from, bool retained) {
  if (!retained) return nextPlayPosition(from);
  if (!m_pausedAudio.isEmpty()) return m_pausedAudio.first().position;
  m_pausedContext.clear();
  return m_pausedNext;
}

QVariantMap PlayerWorker::parkedAudioPosition() const {
  qint64 sample=0;
  return fineAudioSample(&sample) ? m_fineAudioPosition : QVariantMap{};
}

void PlayerWorker::restoreAudioPosition(const QVariantMap &position, bool resetGang) {
  m_fineAudioPosition.clear(); m_fineAudioContext.clear(); m_fineAudioFrame=-1;
  AudioTimeline::Time time;
  const auto *profile=m_timeline.profile();
  if (profile && profile->frame_rate_num()>0 && profile->frame_rate_den()>0
      && AudioTimeline::readTime(position,&time) && time.ticks>=0) {
    const auto frame=__int128(time.ticks)*profile->frame_rate_num()/(__int128(time.rate)*profile->frame_rate_den());
    if (frame<m_timeline.frameCount()) {
      m_position=int(frame); m_fineAudioFrame=m_position;
      m_fineAudioPosition=AudioTimeline::writeTime(time); m_fineAudioContext=audioEditContext();
    }
  }
  // Invalid or out-of-range saved cursors fall back to the clamped picture
  // position. A valid rational cursor survives rate changes without rounding.
  m_position=qBound(0,m_position,qMax(0,m_timeline.frameCount()-1));
  emit audioCursorRestored(audioEditContext(),m_fineAudioPosition,m_position);
  if (resetGang) resetMediaGangAnchors();
}

void PlayerWorker::trimAudioStart(QueuedFrame &frame, qint64 sample) {
  const qint64 first=sound::Mixer::frameStart(frame.position,m_timeline.fps(),kFrequency);
  const qint64 end=sound::Mixer::frameStart(frame.position+1,m_timeline.fps(),kFrequency);
  const qint64 start=qBound<qint64>(first,sample,end);
  frame.decoded.audio.remove(0,qMin<qint64>(frame.decoded.audio.size(),(start-first)*kChannels*sizeof(qint16)));
  frame.durationNs=qint64((__int128(end-start)*1000000000+kFrequency/2)/kFrequency);
}

void PlayerWorker::seekAudioPosition(quint64 request, const QVariantMap &context,
                                     const QVariantMap &position, bool audition) {
  const auto refuse=[&](const QString &message) { emit audioPositionReady(request,audioEditContext(),{},m_position,message); };
  if (exportActive() || m_sourceMode || !AudioTimeline::sameEditContext(context,audioEditContext(),false)) {
    refuse(tr("The project or timeline changed. Choose the sound position again.")); return;
  }
  AudioTimeline::Time time;
  const auto *profile=m_timeline.profile();
  if (!profile || profile->frame_rate_num()<1 || profile->frame_rate_den()<1
      || !AudioTimeline::readTime(position,&time) || time.ticks<0
      || __int128(time.ticks)*kFrequency/time.rate>std::numeric_limits<qint64>::max()-1) {
    refuse(tr("Choose a sound position inside the timeline.")); return;
  }
  const auto picture=__int128(time.ticks)*profile->frame_rate_num()/(__int128(time.rate)*profile->frame_rate_den());
  if (picture<0 || picture>=m_timeline.frameCount()) {
    refuse(tr("Choose a sound position inside the timeline.")); return;
  }
  setSpeed(0.);
  mediaGangReady(); // rebase a changed timeline before moving either cursor
  const auto previousPosition=m_position, previousFineFrame=m_fineAudioFrame;
  const auto previousFinePosition=m_fineAudioPosition, previousFineContext=m_fineAudioContext;
  const auto failedSeek=[&](const QString &message) {
    m_position=previousPosition; m_fineAudioFrame=previousFineFrame;
    m_fineAudioPosition=previousFinePosition; m_fineAudioContext=previousFineContext;
    refuse(message);
  };
  RenderScope render(frameRequest());
  ViewerDraw viewerDraw(this);  // filmstrip tiles wait
  m_position=int(picture); m_fineAudioFrame=m_position;
  m_fineAudioPosition=AudioTimeline::writeTime(time); m_fineAudioContext=audioEditContext();
  flushAudio();
  emit linearPlaybackChanged(m_timeline.originalsOnly());
  const auto pictureResult=decodePlaybackFrame(m_position,false);
  if (pictureResult.cancelled || RenderScope::cancelled()) {
    failedSeek(tr("The sound position request was superseded.")); return;
  }
  if (!pictureResult.error.isEmpty()) { failedSeek(pictureResult.error); return; }
  const auto linked=decodeGangedSource();
  if (linked.decoded.cancelled || RenderScope::cancelled()) {
    failedSeek(tr("The sound position request was superseded.")); return;
  }
  if (!linked.decoded.error.isEmpty()) { failedSeek(linked.decoded.error); return; }
  if (audition) {
    QueuedFrame frame; frame.position=m_position;
    qint64 sample=0; fineAudioSample(&sample);
    Timeline::MeterScope meters(m_timeline,m_position,sample,kFrequency);
    frame.decoded=m_timeline.decodeAt(m_position,true,kFrequency,kChannels,false);
    if (frame.decoded.cancelled || RenderScope::cancelled()) {
      failedSeek(tr("The sound position request was superseded.")); return;
    }
    if (!frame.decoded.error.isEmpty()) { failedSeek(frame.decoded.error); return; }
    frame.trackPeaks=m_timeline.takeTrackPeaks();
    frame.routing=m_timeline.audioRoutingMeters();
    trimAudioStart(frame,sample);
    if (ensureAudio()) queueAudition(frame.decoded.audio);
    const auto peaks=levelsFor(frame.decoded.audio,kChannels);
    emit audioLevels(peaks.value(0,-100.f),peaks.value(1,-100.f));
    emit trackLevels(frame.trackPeaks); emit audioRoutingLevels(frame.routing); feedMeters(frame.decoded.audio);
  }
  // Fine seeks carry their picture in the tagged reply. An untagged frameReady
  // could otherwise overwrite a newer cursor while an older seek is decoding.
  if (linked.position>=0) m_previewPos=linked.position;
  emit audioPositionReady(request,m_fineAudioContext,m_fineAudioPosition,m_position,{},pictureResult.image,
                          linked.decoded.image,linked.position,linked.position>=0?m_previewPath:QString());
  if (m_compare) emit beforeFrameReady(m_timeline.decodeBefore(m_position).image);
}

// ── Audio page ────────────────────────────────────────────────────────────

void PlayerWorker::announceMix() {
  // A level, a pan, a route: sound alone, so the kept pictures are not asked about.
  QScopedValueRollback<bool> mixOnly(m_mixAnnounce, true);
  emit tracksChanged(m_timeline.tracks());
  emit masterChanged(m_timeline.masterMix());
  announceTimelineState(true);
}
void PlayerWorker::recordMixEdit(const QVariantList &before,
                                 const QString &key) {
  if (m_undoStack.isEmpty() || m_mixUndoKey != key || !m_mixUndoClock.isValid() ||
      (m_mixUndoClock.elapsed() > 500 && !m_mixGestures.contains(key)) || m_mixRevision != m_editRevision) {
    recordEdit(before, false, tr("Adjust audio mix"));
    m_mixRevision = m_editRevision;
  }
  m_mixUndoKey = key;
  m_mixUndoClock.restart();
}
// `written`, when given, holds the frames of the last two keys this pass
// wrote, and is brought up to date.
static void putAutomationKey(QVariantMap *mix, const QString &key, int frame,
                             double value, int *written = nullptr) {
  auto all = mix->value("automationKeys").toMap();
  auto keys = all.value(key).toList();
  for (int i = keys.size() - 1; i >= 0; --i)
    if (keys[i].toMap().value("frame").toInt() == frame)
      keys.removeAt(i);
  keys.append(QVariantMap{
      {"frame", frame}, {"value", value}, {"interpolation", "linear"}});
  std::sort(keys.begin(), keys.end(), [](const QVariant &a, const QVariant &b) {
    return a.toMap().value("frame").toInt() < b.toMap().value("frame").toInt();
  });
  if (!written) {
    all[key] = keys;
    (*mix)["automationKeys"] = all;
    return;
  }
  // A pass writes a key on every frame played. The key it wrote last says
  // nothing when the line from the one before it to this one already passes
  // through it (a fader held still, or moved evenly): it is left out, and the
  // curve is the same with a fraction of the keys. Only the pass's own keys
  // are thinned, never one that was there before it.
  for (int i = keys.size() - 1; i >= 2 && written[0] >= 0 && written[1] > written[0] && frame > written[1]; --i) {
    if (keys[i].toMap().value("frame").toInt() != frame)
      continue;
    const auto middle = keys[i - 1].toMap(), first = keys[i - 2].toMap();
    if (middle.value("frame").toInt() == written[1] &&
        first.value("frame").toInt() == written[0] &&
        middle.value("interpolation") == "linear" &&
        first.value("interpolation") == "linear") {
      const double from = first.value("value").toDouble();
      const double line = from + (value - from) * double(written[1] - written[0]) / double(frame - written[0]);
      if (std::abs(middle.value("value").toDouble() - line) < 1e-9) {
        keys.removeAt(i - 1);
        written[1] = written[0];  // the key before is again the one before this
      }
    }
    break;
  }
  if (written[1] != frame) {
    written[0] = written[1];
    written[1] = frame;
  }
  all[key] = keys;
  (*mix)["automationKeys"] = all;
}
void PlayerWorker::writeAutomation(int frame) {
  if (m_automationWrites.isEmpty())
    return;
  // The whole pass is one step of undo: the project as it was is needed only
  // before the pass's first key, not on every frame played.
  const auto before = m_automationRecorded ? QVariantList() : snapshotNow();
  bool changed = false;
  for (auto &write : m_automationWrites) {
    if (m_timeline.trackLocked(write.track))
      continue;
    auto mix = m_timeline.trackMix(write.track);
    putAutomationKey(&mix, write.key, frame, write.value, write.written);
    changed = m_timeline.setTrackMixMap(write.track, mix) || changed;
  }
  if (changed) {
    // The pass's first key is the edit: it goes out in full, with the undo
    // step it made. Every later frame of the pass only moves a fader's
    // keys, so only the tracks go out; the timeline is announced once more
    // when the pass ends (setSpeed). A full announce a frame would have the
    // window rebuild its clips, and the worker drop its paused sound, at
    // the playback rate.
    if (!m_automationRecorded) {
      recordEdit(before, false, tr("Write automation"));
      m_automationRecorded = true;
      announceMix();
    } else {
      m_automationUnannounced = true;
      QScopedValueRollback<bool> mixOnly(m_mixAnnounce, true);
      emit tracksChanged(m_timeline.tracks());
    }
      soundMayHaveChanged(); // the key is heard as soon as a fader moved by hand is
  }
}
void PlayerWorker::beginAudioGesture(int track, const QString &key) {
  const QString id = (track == -2 ? QStringLiteral("master") : QString::number(track)) + ":" + key;
  m_mixGestures.insert(id);
  m_mixUndoKey.clear();
}
void PlayerWorker::endAudioGesture(int track, const QString &key) {
  const QString mixId = (track == -2 ? QStringLiteral("master") : QString::number(track)) + ":" + key;
  m_mixGestures.remove(mixId);
  if (m_mixUndoKey == mixId) m_mixUndoKey.clear();
  const QString id = QString::number(track) + ":" + key;
  if (!m_automationWrites.contains(id))
    return;
  const auto write = m_automationWrites.value(id);
  auto mix = m_timeline.trackMix(track);
  if (mix.value("automation") == "touch") {
    putAutomationKey(
        &mix, key, m_position + 1,
        AudioEngine::automated(write.original, key, m_position + 1,
                               write.original.value(key).toDouble()));
    m_timeline.setTrackMixMap(track, mix);
    m_automationWrites.remove(id);
    announceMix();
  }
}
void PlayerWorker::setTrackMix(int track, const QString &key,
                               const QVariant &value) {
  if (track < 0 || track >= m_timeline.trackCount() ||
      (m_timeline.trackLocked(track) && key != "editHeight" && key != "audioHeight")) // a locked track's header still resizes, on either page
    return;
  // A track's own height on the Audio page is how the editor looks at the
  // track, not part of the edit. It is kept with the track, as the header
  // flags are (setTrackFlag): saved with the project, no undo step, and the
  // timeline is not announced, so the project is not marked changed.
  if (key == "audioHeight") {
    if (m_timeline.setTrackMix(track, key, value)) {
      QScopedValueRollback<bool> noPicture(m_mixAnnounce, true); // the kept pictures are not asked about
      emit tracksChanged(m_timeline.tracks());
    }
    return;
  }
  const auto before = snapshotNow();
  auto mix = m_timeline.trackMix(track);
  const QString mode = mix.value("automation", "read").toString();
  if ((key == "volume" || key == "pan") && m_speed != 0 && !m_sourceMode &&
      (mode == "touch" || mode == "latch" || mode == "write")) {
    const QString id = QString::number(track) + ":" + key;
    if (!m_automationWrites.contains(id))
      m_automationWrites[id] = {track, key, value.toDouble(), mix};
    m_automationWrites[id].value = value.toDouble();
    writeAutomation(m_position);
    return;
  }
  if (!m_timeline.setTrackMix(track, key, value)) {
    if (mix.value(key) != value)
      emit audioOperationFinished("The mixer change is invalid; check audio "
                                  "routing and bus destinations.");
    return;
  }
  recordMixEdit(before, QString::number(track) + ":" + key);
  announceMix();
  if (m_speed == 0.)
    refreshFrame();
}

void PlayerWorker::setTrackMixMap(int track, const QVariantMap &mix) {
  if (m_timeline.trackLocked(track))
    return;
  const auto before = snapshotNow();
  if (!m_timeline.setTrackMixMap(track, mix))
    return;
  recordMixEdit(before, QString::number(track) + ":map");
  announceMix();
  if (m_speed == 0.0)
    refreshFrame();
}

void PlayerWorker::setMasterMix(const QString &key, const QVariant &value) {
  const auto before = snapshotNow();
  if (!m_timeline.setMasterMix(key, value)) {
    auto candidate = m_timeline.masterMix();
    candidate[key] = value;
    const auto error =
        AudioEngine::validateRouting(m_timeline.tracks(), candidate);
    if (!error.isEmpty())
      emit audioOperationFinished(error);
    return;
  }
  recordMixEdit(before, "master:" + key);
  announceMix();
}

void PlayerWorker::setMasterMixMap(const QVariantMap &mix) {
  if (mix!=m_timeline.masterMix()) discardPausedAudio();
  m_timeline.setMasterMixMap(mix);
  soundMayHaveChanged();
  emit masterChanged(m_timeline.masterMix());
}

void PlayerWorker::setAdrCues(const QVariantList &cues) {
  if (cues == m_timeline.adrCues())
    return;
  const auto before = snapshotNow();
  m_timeline.setAdrCues(cues);
  recordEdit(before, false, tr("Edit ADR cues"));
  announceTimeline();
  emit adrCuesChanged(cues);
}

void PlayerWorker::setLoop(bool on, int in, int out) {
  if (on!=m_loop || in!=m_loopIn || out!=m_loopOut) {
    if (m_speed==0.) flushAudio();
    else discardPausedAudio();
  }
  const bool changed = on!=m_loop || in!=m_loopIn || out!=m_loopOut;
  m_loop = on;
  m_loopIn = in;
  m_loopOut = out;
  if (changed) soundChanged(); // sound made ahead ran on past the old marks
}

void PlayerWorker::scrubTo(int frame) {
  seek(frame);
  if (exportActive() || activeEmpty() || m_speed != 0.0)
    return;
  // Meters follow a scrub even with no output device (the offscreen demo
  // recorder scrubs through the edit to picture them).
  const bool sink = ensureAudio();
  const MediaPlayer::Decoded d =
      m_sourceMode
          ? m_preview.decodeAt(m_previewPos, true, kFrequency, kChannels, false)
          : m_timeline.decodeAt(m_position, true, kFrequency, kChannels, false);
  if (sink && !d.audio.isEmpty()) queueAudition(d.audio);
  const QList<float> levels = levelsFor(d.audio, kChannels);
  emit audioLevels(levels.value(0, -100.0f), levels.value(1, -100.0f));
  if (!m_sourceMode) {
    emit trackLevels(m_timeline.takeTrackPeaks());
    emit audioRoutingLevels(m_timeline.audioRoutingMeters());
    feedMeters(d.audio);
  }
}

void PlayerWorker::resetLoudness() {
  m_analysis.reset();
  m_lastLoudnessAnalysis.clear();
  m_loudnessFingerprint.clear();
  emit loudnessAnalyzed(100, {}, {});
  m_loudness.reset(kFrequency, kChannels);
  emit loudnessChanged(m_loudness.snapshot(), m_loudness.history());
}

void PlayerWorker::feedMeters(const QByteArray &audio) {
  if (audio.isEmpty())
    return;
  const auto *samples = reinterpret_cast<const int16_t *>(audio.constData());
  const int frames = audio.size() / (kChannels * int(sizeof(int16_t)));
  m_loudness.push(samples, frames);
  if (++m_loudnessTick % 3 == 0)
    emit loudnessChanged(m_loudness.snapshot(), m_loudness.history());
  emit spectrumChanged(spectrumOf(audio, kChannels, kFrequency));
}

// 48 log-spaced bins, 40 Hz–16 kHz, from a Hann-windowed mono fold of the
// frame's first 1024 samples. A naive DFT at 48 bins is ~50k multiplies —
// nothing, and no FFT dependency.
QList<float> PlayerWorker::spectrumOf(const QByteArray &audio, int channels,
                                      int rate) {
  QList<float> bins;
  const auto *samples = reinterpret_cast<const int16_t *>(audio.constData());
  const int frames =
      channels > 0 ? audio.size() / (channels * int(sizeof(int16_t))) : 0;
  const int n = qMin(1024, frames);
  if (n < 64)
    return bins;
  std::vector<double> mono(static_cast<size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) {
    double v = 0.0;
    for (int c = 0; c < channels; ++c)
      v += samples[i * channels + c];
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
    mono[size_t(i)] = v / channels / 32768.0 * w;
  }
  constexpr int kBins = 48;
  const double fLo = 40.0, fHi = 16000.0;
  bins.reserve(kBins);
  for (int b = 0; b < kBins; ++b) {
    const double f = fLo * std::pow(fHi / fLo, double(b) / (kBins - 1));
    const double w = 2.0 * M_PI * f / rate;
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
      re += mono[size_t(i)] * std::cos(w * i);
      im -= mono[size_t(i)] * std::sin(w * i);
    }
    const double mag =
        2.0 * std::sqrt(re * re + im * im) / (n * 0.5); // Hann gain
    bins.append(mag > 1e-6 ? float(qMax(-100.0, 20.0 * std::log10(mag)))
                           : -100.0f);
  }
  return bins;
}

void PlayerWorker::analyzeLoudness(int in, int out) {
  if (m_analysis || m_timeline.frameCount() <= 0 || exportActive())
    return;
  m_analysis = std::make_unique<Analysis>();
  m_analysis->scratch = std::make_unique<Timeline>();
  m_analysis->scratch->setProjectSettings(m_timeline.projectSettings());
  m_analysis->scratch->setMediaSources(m_timeline.mediaSources(),
                                       m_timeline.proxyMode());
  m_analysis->scratch->restore(
      m_timeline.entries(),
      Timeline::trackFlagsFromVariant(m_timeline.trackFlagsVariant()));
  m_analysis->scratch->setMasterMixMap(m_timeline.masterMix());
  m_analysis->meter.reset(kFrequency, kChannels);
  m_analysis->total = m_analysis->scratch->frameCount();
  m_analysis->start = qBound(0, in, m_analysis->total - 1);
  if (out >= 0)
    m_analysis->total = qMin(m_analysis->total, out + 1);
  if (m_analysis->total <= m_analysis->start) {
    m_analysis.reset();
    return;
  }
  m_analysis->position = m_analysis->start;
  m_analysis->fingerprint =
      QJsonDocument::fromVariant(snapshotNow()).toJson(QJsonDocument::Compact);
  emit loudnessAnalyzed(0, QVariantMap(), QList<float>());
  QTimer::singleShot(0, this, &PlayerWorker::continueAnalysis);
}

void PlayerWorker::continueAnalysis() {
  if (!m_analysis)
    return;
  Analysis &a = *m_analysis;
  const int end = qMin(a.total, a.position + 25);
  for (; a.position < end; ++a.position) {
    const QByteArray audio =
        a.scratch->audioAt(a.position, kFrequency, kChannels);
    const auto *samples = reinterpret_cast<const int16_t *>(audio.constData());
    a.meter.push(samples, audio.size() / (kChannels * int(sizeof(int16_t))));
  }
  if (a.position < a.total) {
    emit loudnessAnalyzed(
        qBound(0,
               int(100.0 * (a.position - a.start) / qMax(1, a.total - a.start)),
               99),
        QVariantMap(), QList<float>());
    QTimer::singleShot(0, this, &PlayerWorker::continueAnalysis);
    return;
  }
  QVariantMap values = a.meter.snapshot();
  values["rangeIn"] = a.start;
  values["rangeOut"] = a.total - 1;
  m_loudnessFingerprint = a.fingerprint;
  if (a.fingerprint !=
      QJsonDocument::fromVariant(snapshotNow()).toJson(QJsonDocument::Compact))
    values = QVariantMap{
        {"error", "The mix changed during analysis. Analyse again."}};
  m_lastLoudnessAnalysis = values;
  const QList<float> history = a.meter.history();
  m_analysis.reset();
  emit loudnessAnalyzed(100, values, history);
}

void PlayerWorker::cancelLoudnessAnalysis() {
  m_analysis.reset();
  m_lastLoudnessAnalysis.clear();
  emit loudnessAnalyzed(100, {{"error", "Loudness analysis cancelled."}}, {});
}

void PlayerWorker::normalizeLoudness(double target, double peakCeiling) {
  if (!std::isfinite(target) || target < -60 || target > -1 || m_analysis)
    return;
  if (m_loudnessFingerprint != QJsonDocument::fromVariant(snapshotNow())
                                   .toJson(QJsonDocument::Compact) ||
      m_lastLoudnessAnalysis.value("integrated", -100.).toDouble() <= -99) {
    emit audioOperationFinished("Analyse the current mix before normalising.");
    return;
  }
  const double delta =
      target - m_lastLoudnessAnalysis.value("integrated").toDouble();
  const auto before = snapshotNow();
  auto master = m_timeline.masterMix();
  const int in = m_lastLoudnessAnalysis.value("rangeIn").toInt(),
            out = m_lastLoudnessAnalysis.value("rangeOut").toInt();
  // The master fader's range bounds the gain either way: a near-silent mix
  // cannot be raised all the way, and the result says so.
  constexpr double floorDb = -60., ceilingDb = 24.;
  double applied = 0;
  if (in == 0 && out == m_timeline.frameCount() - 1) {
    const double volume = master.value("volume").toDouble();
    master["volume"] = qBound(floorDb, volume + delta, ceilingDb);
    applied = master.value("volume").toDouble() - volume;
  } else {
    applied = qBound(floorDb, delta, ceilingDb);
    auto regions = master.value("normalizations").toList();
    regions.append(QVariantMap{{"in", in}, {"out", out}, {"gain", applied}});
    master["normalizations"] = regions;
  }
  if (!m_timeline.setMasterMixMap(master))
    return;
  recordEdit(before, false, tr("Normalise loudness"));
  announceTimeline();
  // Gain moves the true peak by the same amount; normalizing never limits it.
  const double peak =
      m_lastLoudnessAnalysis.value("truePeak", -100.).toDouble() + applied;
  m_lastLoudnessAnalysis.clear();
  QStringList message{
      tr("Normalisation applied: %1 dB.").arg(applied, 0, 'f', 1)};
  if (std::abs(delta - applied) >= 0.05)
    message << tr("That is the gain limit; the mix stays about %1 LU %2 "
                  "%3 LUFS.")
                   .arg(std::abs(delta - applied), 0, 'f', 1)
                   .arg(delta > applied ? tr("under") : tr("over"))
                   .arg(target);
  // Without a target's ceiling, the one most delivery targets share.
  if (!std::isfinite(peakCeiling))
    peakCeiling = -1.;
  if (peak > peakCeiling + 0.05)
    message << tr("True peak will be about %1 dBTP, above %2 dBTP: add a "
                  "master limiter.")
                   .arg(peak, 0, 'f', 1)
                   .arg(peakCeiling);
  message << tr("Analyse again to check loudness and true peak.");
  emit loudnessAnalyzed(100, {{"error", message.join(QLatin1Char(' '))}}, {});
}

void PlayerWorker::setAudioFadeCurve(int track, int index,
                                     const QString &curve) {
    if (staleEdit())
        return;
  auto before = snapshotNow();
  if (!m_timeline.setAudioFadeCurveAt(track, index, curve))
    return;
  recordEdit(before, false, tr("Set audio fade curve"));
  announceTimeline();
  refreshFrame();
}

void PlayerWorker::prepareAdrRecording(int cueIndex) {
  const auto cues = m_timeline.adrCues();
  if (cueIndex < 0 || cueIndex >= cues.size())
    return;
  auto cue = cues[cueIndex].toMap();
  const int track = cue.value("track", -1).toInt();
  if (track >= 0 && m_timeline.trackLocked(track)) {
    emit audioOperationFinished("The ADR track is locked.");
    return;
  }
  if (m_sourceMode)
    closePreview();
  setSpeed(0.);
  const int start =
      qMax(0, cue.value("in").toInt() - cue.value("preRoll").toInt());
  seek(start);
  emit adrRecordReady(cue.value("id").toString(),
                      track < 0 ? m_timeline.trackCount() : track, start,
                      cue.value("out").toInt() + cue.value("postRoll").toInt(),
                      cue.value("character", "ADR").toString());
}
void PlayerWorker::promoteAdrTake(int cueIndex, int takeIndex, int in,
                                  int out) {
  auto cues = m_timeline.adrCues();
  if (cueIndex < 0 || cueIndex >= cues.size())
    return;
  auto cue = cues[cueIndex].toMap();
  auto takes = cue.value("takes").toList();
  if (takeIndex < 0 || takeIndex >= takes.size())
    return;
  const auto take = takes[takeIndex].toMap();
  const QString path = take.value("path").toString();
  const int track = cue.value("track", -1).toInt() < 0
                        ? m_timeline.trackCount()
                        : cue.value("track").toInt();
  if (track < m_timeline.trackCount() &&
      (m_timeline.trackLocked(track) ||
       m_timeline.trackKind(track) != Timeline::Track::Audio)) {
    emit audioOperationFinished(
        "Choose an unlocked audio track for the ADR cue.");
    return;
  }
  const int a =
      qMax(cue.value("in").toInt(), in < 0 ? cue.value("in").toInt() : in);
  const int b =
      qMin(cue.value("out").toInt(), out < 0 ? cue.value("out").toInt() : out);
  const int source =
      a - take.value("start", qMax(0, cue.value("in").toInt() -
                                          cue.value("preRoll").toInt()))
              .toInt();
  if (!QFileInfo::exists(path)) {
    emit audioOperationFinished("The recording is missing.");
    return;
  }
  // A timeline takes its clock from its first clip: before that there is no
  // rate to measure the take against, and nothing for it to replace.
  if (!m_timeline.profile()) {
    emit audioOperationFinished(
        "Add a clip to the timeline before promoting an ADR recording.");
    return;
  }
  // Its length on the timeline's clock.
  const media::Probe probe = media::probeCached(path);
  if (!probe.valid || a > b || source < 0 ||
      source + b - a >= media::framesOn(probe, m_timeline.profile()->fps())) {
    emit audioOperationFinished(
        "This recording does not cover the selected cue range.");
    return;
  }
  const auto before = snapshotNow();
  if (!m_timeline.overwriteOn(track, path, a, source, source + b - a, true))
    return;
  QVariantList comp;
  for (const auto &v : cue.value("comp").toList()) {
    auto segment = v.toMap();
    int lo = segment.value("in").toInt(), hi = segment.value("out").toInt();
    if (hi < a || lo > b)
      comp.append(segment);
    else {
      if (lo < a) {
        auto left = segment;
        left["out"] = a - 1;
        comp.append(left);
      }
      if (hi > b) {
        auto right = segment;
        right["in"] = b + 1;
        comp.append(right);
      }
    }
  }
  comp.append(QVariantMap{{"in", a}, {"out", b}, {"take", takeIndex}});
  std::sort(comp.begin(), comp.end(), [](const QVariant &x, const QVariant &y) {
    return x.toMap().value("in").toInt() < y.toMap().value("in").toInt();
  });
  cue["comp"] = comp;
  cue["track"] = track;
  cue["status"] = "approved";
  for (int i = 0; i < takes.size(); ++i) {
    auto t = takes[i].toMap();
    t["promoted"] = a == cue.value("in").toInt() &&
                    b == cue.value("out").toInt() && i == takeIndex;
    takes[i] = t;
  }
  cue["takes"] = takes;
  cues[cueIndex] = cue;
  m_timeline.setAdrCues(cues);
  recordEdit(before, false, tr("Promote ADR recording"));
  announceTimeline();
  refreshFrame();
}
