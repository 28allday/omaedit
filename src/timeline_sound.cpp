#include "timeline_p.h"
#include "rendersnapshot.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}


// The meters' access to the timeline (the mixer's taps write through it).
struct TapAccess {
    static AudioEngine::MeterWindow &window(Timeline *owner) { return owner->m_meterWindow; }
    static AudioEngine::MeterWindow::Range range(Timeline *owner, int frequency, int count) {
        return owner ? owner->m_meterWindow.samples(frequency,count) : AudioEngine::MeterWindow::Range{0,count};
    }
    static std::shared_ptr<AudioEngine::MeterCapture> capture(Timeline *owner) {
        return owner ? owner->m_meterWindow.capture : nullptr;
    }
    static void write(Timeline *owner, int track, int stage, float l, float r) {
        if (!owner)
            return;
        const size_t base = size_t(track) * 6 + size_t(stage) * 2;
        if (base + 1 >= owner->m_trackPeaks.size())
            return;
        owner->m_trackPeaks[base] = qMax(owner->m_trackPeaks[base], l);
        owner->m_trackPeaks[base + 1] = qMax(owner->m_trackPeaks[base + 1], r);
    }
};

// ── Audio page: track mix state, the fx chain, level taps, the master ─────

QVariantMap Timeline::trackMix(int track) const {
    if (track < 0 || track >= trackCount())
        return QVariantMap();
    return m_tracks[size_t(track)].mix;
}

bool Timeline::setTrackMix(int track, const QString &key, const QVariant &value) {
    if (track < 0 || track >= trackCount() || key.isEmpty())
        return false;
    QVariantMap mix = m_tracks[size_t(track)].mix;
    if (value.isValid() && !value.isNull())
        mix.insert(key, value);
    else
        mix.remove(key);
    return setTrackMixMap(track, mix);
}

bool Timeline::setTrackMixMap(int track, const QVariantMap &mix) {
    if (track < 0 || track >= trackCount())
        return false;
    auto candidates = tracks(); auto candidate = candidates[track].toMap();
    candidate["mix"] = mix; candidates[track] = candidate;
    if (!AudioEngine::validateRouting(candidates, m_master).isEmpty()) return false;
    if (m_tracks[size_t(track)].mix == mix) return false;
    m_tracks[size_t(track)].mix = mix;
    refreshTrackFilters();
    return true;
}

bool Timeline::setMasterMix(const QString &key, const QVariant &value) {
    if (key.isEmpty())
        return false;
    QVariantMap mix = m_master;
    if (value.isValid() && !value.isNull())
        mix.insert(key, value);
    else
        mix.remove(key);
    return setMasterMixMap(mix);
}

bool Timeline::setMasterMixMap(const QVariantMap &mix) {
    if (!AudioEngine::validateRouting(tracks(), mix).isEmpty() || m_master == mix) return false;
    m_master = mix;
    return true;
}

bool Timeline::masterActive() const {
    if (!m_master.value("buses").toList().isEmpty() || !m_master.value("normalizations").toList().isEmpty()
        || !m_master.value("automationKeys").toMap().isEmpty()) return true;

    if (qAbs(m_master.value(QStringLiteral("volume")).toDouble()) > 1e-6)
        return true;
    if (m_master.value(QStringLiteral("muted")).toBool())
        return true;
    if (qAbs(m_master.value(QStringLiteral("pan")).toDouble()) > 1e-6)
        return true;
    const QVariantList fx = m_master.value(QStringLiteral("fx")).toList();
    for (const QVariant &v : fx)
        if (!v.toMap().value(QStringLiteral("bypass")).toBool())
            return true;
    return false;
}

Timeline::MeterScope::MeterScope(Timeline &timeline, int position, qint64 firstSample,
                                   int frequency, qint64 lastSample, bool retain)
    : m_timeline(timeline), m_previous(timeline.m_meterWindow),
      m_capture(retain ? std::make_shared<AudioEngine::MeterCapture>(timeline.trackCount()) : nullptr) {
    timeline.setMeterWindow({position, timeline.fps(), firstSample, lastSample, frequency, m_capture});
}
Timeline::MeterScope::~MeterScope() { m_timeline.setMeterWindow(m_previous); }
void Timeline::setMeterWindow(const AudioEngine::MeterWindow &window) {
    m_meterWindow=window; m_audioRouting.setMeterWindow(window);
}
void Timeline::clearAudioMeters() {
    if (m_meterWindow.capture) m_meterWindow.capture->clear();
    std::fill(m_trackPeaks.begin(), m_trackPeaks.end(), 0.0f);
    m_audioRouting.clearMeters();
}

QList<float> Timeline::takeTrackPeaks() {
    QList<float> out(m_trackPeaks.begin(), m_trackPeaks.end());
    std::fill(m_trackPeaks.begin(), m_trackPeaks.end(), 0.0f);
    return out;
}

QStringList Timeline::mixEffectIds() {
    return {QStringLiteral("eq"),      QStringLiteral("comp"),     QStringLiteral("gate"),
            QStringLiteral("limiter"), QStringLiteral("deesser"),  QStringLiteral("nr"),
            QStringLiteral("highpass"), QStringLiteral("lowpass"), QStringLiteral("hum")};
}

// Defaults in operator units: dB, ms, Hz, as the mixer reads them; what
// each avfilter wants (linear amplitudes, ratios of Nyquist).
QVariantList Timeline::mixEffectCatalogue() {
    auto entry = [](const QString &id, const QString &name, bool mixable,
                    const QVariantMap &params) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), id);
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("mixable"), mixable);
        m.insert(QStringLiteral("params"), params);
        return QVariant(m);
    };
    QVariantList bands;
    const double freqs[5] = {100, 350, 1000, 3500, 10000};
    const double qs[5] = {0.7, 1.0, 1.0, 1.0, 0.7};
    for (int i = 0; i < 5; ++i)
        bands.append(QVariant(QVariantMap{{QStringLiteral("f"), freqs[i]},
                                          {QStringLiteral("g"), 0.0},
                                          {QStringLiteral("q"), qs[i]}}));
    return {
        entry(QStringLiteral("eq"), QStringLiteral("Equaliser"), true,
              {{QStringLiteral("bands"), bands}}),
        entry(QStringLiteral("comp"), QStringLiteral("Compressor"), true,
              {{QStringLiteral("threshold"), -20.0}, {QStringLiteral("ratio"), 3.0},
               {QStringLiteral("attack"), 10.0},     {QStringLiteral("release"), 120.0},
               {QStringLiteral("knee"), 6.0},        {QStringLiteral("makeup"), 2.0}}),
        entry(QStringLiteral("gate"), QStringLiteral("Gate / Expander"), true,
              {{QStringLiteral("threshold"), -40.0}, {QStringLiteral("ratio"), 2.0},
               {QStringLiteral("attack"), 20.0},     {QStringLiteral("release"), 250.0},
               {QStringLiteral("range"), -60.0},     {QStringLiteral("knee"), 6.0}}),
        entry(QStringLiteral("limiter"), QStringLiteral("Limiter"), true,
              {{QStringLiteral("ceiling"), -1.0}, {QStringLiteral("attack"), 5.0},
               {QStringLiteral("release"), 50.0}}),
        entry(QStringLiteral("deesser"), QStringLiteral("De-Esser"), true,
              {{QStringLiteral("frequency"), 6500.0}, {QStringLiteral("intensity"), 0.5},
               {QStringLiteral("max"), 0.5}}),
        entry(QStringLiteral("nr"), QStringLiteral("Noise Reduction"), true,
              {{QStringLiteral("reduction"), 12.0}, {QStringLiteral("floor"), -50.0},
               {QStringLiteral("track"), true}}),
        entry(QStringLiteral("highpass"), QStringLiteral("High-pass"), true,
              {{QStringLiteral("frequency"), 80.0}}),
        entry(QStringLiteral("hum"), QStringLiteral("Hum Removal"), true, {{"frequency",50.0},{"reduction",36.0}}),
        entry(QStringLiteral("lowpass"), QStringLiteral("Low-pass"), true,
              {{QStringLiteral("frequency"), 12000.0}}),
    };
}

// One frame of the mixed programme's audio, no picture: the offline
// loudness pass runs this over a scratch timeline.
// One frame's sound from the editor's own mixer, with the meters the
// graph's taps and routing give. False when it does not mix this
// timeline (the caller reads the graph).
QString Timeline::soundUnsupported() {
    QVariantList flags = trackFlagsVariant();
    for (int i = 0; i < flags.size(); ++i) { auto f = flags[i].toMap(); f["index"] = i; flags[i] = f; }
    sound::Mixer mixer;
    mixer.configure({soundEntries(), flags, m_master, m_mediaSources, m_profile ? m_profile->frame_rate_num() : 25, m_profile ? m_profile->frame_rate_den() : 1});
    return mixer.unsupported();
}

// A file's first sound stream decoded straight through, start to end, as
// a player gives it (the codec's own start-up samples dropped), handed on
// as interleaved float at `rate` and `channels`. No seeking and no timing
// checks: only the samples, in order.
static bool decodeWholeSound(const QString &path, int rate, int channels, const std::function<void(const float *, int)> &take,
                             QString *error) {
    const auto failed = [&](const QString &why) { *error = QStringLiteral("The delivered sound could not be read back: %1").arg(why); return false; };
    AVFormatContext *file = nullptr;
    if (avformat_open_input(&file, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) return failed(QStringLiteral("it does not open"));
    AVCodecContext *codec = nullptr;
    SwrContext *convert = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const auto done = qScopeGuard([&] {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&convert);
        avcodec_free_context(&codec);
        avformat_close_input(&file);
    });
    if (avformat_find_stream_info(file, nullptr) < 0) return failed(QStringLiteral("its streams cannot be read"));
    const AVCodec *decoder = nullptr;
    const int stream = av_find_best_stream(file, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder || !packet || !frame) return failed(QStringLiteral("it has no sound to decode"));
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, file->streams[stream]->codecpar) < 0 || avcodec_open2(codec, decoder, nullptr) < 0)
        return failed(QStringLiteral("its decoder does not open"));
    AVChannelLayout out;
    av_channel_layout_default(&out, channels);
    std::vector<float> samples;
    const auto give = [&](const AVFrame *in) {
        if (!convert) {
            if (swr_alloc_set_opts2(&convert, &out, AV_SAMPLE_FMT_FLT, rate, &in->ch_layout, AVSampleFormat(in->format), in->sample_rate, 0, nullptr) < 0
                || swr_init(convert) < 0)
                return false;
        }
        const int room = swr_get_out_samples(convert, in ? in->nb_samples : 0);
        if (room <= 0) return true;
        samples.resize(size_t(room) * size_t(channels));
        uint8_t *to = reinterpret_cast<uint8_t *>(samples.data());
        const int made = swr_convert(convert, &to, room, in ? const_cast<const uint8_t **>(in->extended_data) : nullptr, in ? in->nb_samples : 0);
        if (made < 0) return false;
        if (made > 0) take(samples.data(), made);
        return true;
    };
    bool ending = false;
    while (true) {
        const int got = avcodec_receive_frame(codec, frame);
        if (got == 0) {
            if (!give(frame)) return failed(QStringLiteral("its samples cannot be converted"));
            av_frame_unref(frame);
            continue;
        }
        if (got == AVERROR_EOF) break;
        if (got != AVERROR(EAGAIN)) return failed(QStringLiteral("its samples cannot be decoded"));
        if (ending) return failed(QStringLiteral("its decoder does not finish"));
        int read;
        do {
            av_packet_unref(packet);
            read = av_read_frame(file, packet);
        } while (read >= 0 && packet->stream_index != stream);
        if (read < 0 && read != AVERROR_EOF) return failed(QStringLiteral("its packets cannot be read"));
        ending = read == AVERROR_EOF;
        if (avcodec_send_packet(codec, ending ? nullptr : packet) < 0) return failed(QStringLiteral("its packets cannot be decoded"));
    }
    // What the converter still holds.
    if (convert && !give(nullptr)) return failed(QStringLiteral("its samples cannot be converted"));
    return true;
}

bool Timeline::measureSound(int first, int last, int rate, int channels, const std::function<bool(int)> &progress,
                            QVariantMap *reading, QString *error, double gainDb, double ceilingDb) {
    if (!m_profile) { *error = QStringLiteral("The delivery was not prepared."); return false; }
    QVariantList flags = trackFlagsVariant();
    for (int i = 0; i < flags.size(); ++i) { auto f = flags[i].toMap(); f["index"] = i; flags[i] = f; }
    sound::Mixer mixer;
    mixer.configure({soundEntries(), flags, m_master, m_mediaSources, m_profile->frame_rate_num(), m_profile->frame_rate_den()});
    if (!mixer.unsupported().isEmpty()) { *error = mixer.unsupported(); return false; }
    const double fps = m_profile->fps();
    LoudnessMeter meter;
    meter.reset(rate, channels);
    sound::ReconstructedPeak reconstructed;
    reconstructed.reset(channels);
    const bool levelled = std::isfinite(gainDb);
    sound::TruePeakLimiter limiter;
    if (levelled) limiter.reset(rate, channels, gainDb, ceilingDb);
    sound::Samples samples, limited;
    for (int frame = first; frame <= last; ++frame) {
        const qint64 a = sound::Mixer::frameStart(frame, fps, rate), b = sound::Mixer::frameStart(frame + 1, fps, rate);
        if (!mixer.render(a, int(b - a), rate, channels, samples, error)) return false;
        if (levelled) {
            // (What comes out trails by the limiter's look-ahead: the last
            // few samples are let out at the end.)
            limited.resize(samples.size());
            limiter.process(samples.data(), int(b - a), limited.data());
            meter.push(limited.data(), int(b - a));
            reconstructed.push(limited.data(), int(b - a));
        } else {
            meter.push(samples.data(), int(b - a));
            reconstructed.push(samples.data(), int(b - a));
        }
        if (progress && !progress(frame)) { *error = QStringLiteral("Cancelled"); return false; }
    }
    if (levelled && limiter.latency() > 0) {
        samples.assign(size_t(limiter.latency()) * size_t(channels), 0.0f);
        limited.resize(samples.size());
        limiter.process(samples.data(), limiter.latency(), limited.data());
        meter.push(limited.data(), limiter.latency());
        reconstructed.push(limited.data(), limiter.latency());
    }
    *reading = meter.snapshot();
    reading->insert(QStringLiteral("reconstructedPeak"), qMax(reconstructed.peakDb(), LoudnessMeter::silence()));
    return true;
}

bool Timeline::hearDeliveredSound(const QString &path, const QVariantMap &spec, int first, int last, double gainDb, double ceilingDb,
                                  const std::function<bool(int)> &progress, QVariantMap *reading, QString *error,
                                  std::shared_ptr<RenderRequest> request) {
    const QVariantMap normal = ExportSpec::normalized(spec);
    const int rate = normal.value(QStringLiteral("arate"), 48000).toInt(), channels = normal.value(QStringLiteral("channels"), 2).toInt();
    // A lossless codec gives back what went in: the levelled sound is read as made.
    if (!ExportSpec::lossySound(normal))
        return measureSound(first, last, rate, channels, progress, reading, error, gainDb, ceilingDb);
    // A lossy one rebuilds it: the sound alone, encoded as the delivery
    // encodes it (the same samples to the same encoder give the same
    // packets), and decoded again as a player decodes the file.
    QVariantMap alone = spec;
    alone[QStringLiteral("video")] = false;
    alone[QStringLiteral("loudnessGain")] = gainDb;
    alone[QStringLiteral("loudnessCeiling")] = ceilingDb;
    alone.remove(QStringLiteral("chapters"));
    QFile::remove(path);
    const auto cleanUp = qScopeGuard([&] { QFile::remove(path); });
    // (Stopped part-way, the encoder still finishes its file: a pause, not a reading.)
    bool stopped = false;
    const auto going = [&](int frame) { return !(stopped = progress && !progress(frame)); };
    if (!writeDelivery(path, alone, false, first, last, going, error, nullptr, std::move(request))) return false;
    if (stopped) { *error = QStringLiteral("Cancelled"); return false; }
    LoudnessMeter meter;
    meter.reset(rate, channels);
    sound::ReconstructedPeak reconstructed;
    reconstructed.reset(channels);
    if (!decodeWholeSound(path, rate, channels, [&](const float *heard, int frames) {
            meter.push(heard, frames);
            reconstructed.push(heard, frames);
        }, error))
        return false;
    *reading = meter.snapshot();
    reading->insert(QStringLiteral("reconstructedPeak"), qMax(reconstructed.peakDb(), LoudnessMeter::silence()));
    return true;
}

bool Timeline::deliverySettings(const QVariantMap &spec, bool mezzanine, int first, ExportSpec::EncoderSettings *settings,
                                int *rateNum, int *rateDen, QString *error) const {
    if (!m_profile) { *error = QStringLiteral("The delivery was not prepared."); return false; }
    // A delivery at another frame rate: each timeline frame stands for the
    // delivery frames whose times round into it (FFmpeg's fps filter), counted
    // from the range's start so paused segments join up.
    AVRational rate{m_profile->frame_rate_num(), m_profile->frame_rate_den()};
    // An image sequence has a file for each timeline frame, whatever rate is asked.
    const QString converted = mezzanine || !ExportSpec::sequenceFormat(ExportSpec::normalized(spec)).isEmpty()
                                  ? QString() : ExportSpec::convertedRate(spec, m_profile->fps());
    if (!converted.isEmpty() && av_parse_video_rate(&rate, converted.toLatin1().constData()) < 0) {
        *error = QStringLiteral("The frame rate %1 cannot be delivered.").arg(converted);
        return false;
    }
    *settings = ExportSpec::encoderSettings(spec, av_q2d(rate), mezzanine);
    // The delivery says where on the timeline's clock it starts: the
    // timeline's starting timecode, and for part of it the first frame's.
    if (!mezzanine)
        ExportSpec::stampStart(*settings, qint64(projectStartFrame()) + first, m_profile->frame_rate_num(), m_profile->frame_rate_den(), rate.num, rate.den);
    *rateNum = rate.num; *rateDen = rate.den;
    return true;
}

bool Timeline::writeDelivery(const QString &path, const QVariantMap &spec, bool mezzanine, int first, int last,
                                const std::function<bool(int)> &progress, QString *error, bool *renderFailed,
                                std::shared_ptr<RenderRequest> request) {
    ExportSpec::EncoderSettings settings;
    int rateNum = 0, rateDen = 1;
    if (!deliverySettings(spec, mezzanine, first, &settings, &rateNum, &rateDen, error)) return false;
    const int origin = qMax(0, spec.value(QStringLiteral("rangeIn"), 0).toInt());
    return writeEncoded(path, settings, rateNum, rateDen, origin, first, last, progress, error, renderFailed, std::move(request));
}

bool Timeline::writeDelivery(const QString &path, const ExportSpec::EncoderSettings &settings, int first, int last,
                                const std::function<bool(int)> &progress, QString *error, bool *renderFailed,
                                std::shared_ptr<RenderRequest> request) {
    if (!m_profile) { *error = QStringLiteral("The delivery was not prepared."); return false; }
    return writeEncoded(path, settings, m_profile->frame_rate_num(), m_profile->frame_rate_den(), first, first, last, progress, error,
                        renderFailed, std::move(request));
}

bool Timeline::writeEncoded(const QString &path, const ExportSpec::EncoderSettings &settings, int rateNum, int rateDen,
                               int origin, int first, int last, const std::function<bool(int)> &progress, QString *error,
                               bool *renderFailed, std::shared_ptr<RenderRequest> request) {
    if (!request) request = std::make_shared<RenderRequest>();
    RenderScope scope(request);
    bool failed = false;
    if (!renderFailed) renderFailed = &failed;
    *renderFailed = false;
    if (!m_profile) { *error = QStringLiteral("The delivery was not prepared."); return false; }
    const double fps = m_profile->fps();
    const AVRational rate{rateNum, rateDen};
    const bool converted = qint64(rateNum) * m_profile->frame_rate_den() != qint64(rateDen) * m_profile->frame_rate_num();
    const auto deliveryFrame = [&](qint64 frame) {
        return av_rescale_rnd(frame - origin, qint64(rate.num) * m_profile->frame_rate_den(),
                              qint64(rate.den) * m_profile->frame_rate_num(), AV_ROUND_NEAR_INF);
    };
    if (settings.video && (!m_linear || m_deliverySpace.isEmpty())) { *error = QStringLiteral("The delivery's pictures were not prepared."); return false; }
    sound::Mixer mixer;
    if (settings.audio) {
        QVariantList flags = trackFlagsVariant();
        for (int i = 0; i < flags.size(); ++i) { auto f = flags[i].toMap(); f["index"] = i; flags[i] = f; }
        mixer.configure({soundEntries(), flags, m_master, m_mediaSources, m_profile->frame_rate_num(), m_profile->frame_rate_den()});
        if (!mixer.unsupported().isEmpty()) { *error = mixer.unsupported(); *renderFailed = true; return false; }
    }
    // Levelled sound: the mixer runs the limiter's look-ahead ahead of the
    // pictures, so what comes out of it lines up with each frame exactly.
    const bool levelling = settings.audio && std::isfinite(settings.loudnessGain);
    sound::TruePeakLimiter limiter;
    LoudnessMeter delivered;
    sound::Samples ahead;
    m_deliveredSound.clear();
    if (levelling) {
        limiter.reset(settings.arate, settings.channels, settings.loudnessGain, settings.truePeakCeiling);
        delivered.reset(settings.arate, settings.channels);
        const qint64 a = sound::Mixer::frameStart(first, fps, settings.arate);
        if (!mixer.render(a, limiter.latency(), settings.arate, settings.channels, ahead, error)) { *renderFailed = true; return false; }
        sound::Samples primed(ahead.size());
        limiter.process(ahead.data(), limiter.latency(), primed.data());
    }
    deliver::Sink sink;
    const int colourspace = m_deliverySpace.startsWith(QLatin1String("rec2020/")) ? 2020 : m_deliverySpace.startsWith(QLatin1String("rec601/")) ? 601 : 709;
    // A sequence's files are numbered by timeline frame, from the first one this pass writes.
    ExportSpec::EncoderSettings opened = settings;
    opened.sequenceFirst = first;
    if (!sink.open(path, opened, width(), height(), rate.num, rate.den, colourspace, error)) return false;
    const QSize size(settings.width > 0 ? settings.width : width(), settings.height > 0 ? settings.height : height());
    // The engine makes the encoder's own planes when it draws the timeline
    // at its size: the output transform, matrix, range and chroma on the GPU.
    const auto planes = sink.planes();
    // Framed to another size, the engine makes the frame at that size.
    const bool direct = settings.video && m_deliverPlanes && planes && (size == QSize(width(), height()) || size == m_exportFrameSize) && !m_exportAlpha;
    engine::OutputColour colour;
    if (planes) {
        colour.format = planes->format;
        colour.fullRange = planes->fullRange;
        colour.space = planes->colourspace == 601 ? engine::ColourSpace::Bt601
                       : planes->colourspace == 2020 ? engine::ColourSpace::Bt2100Pq : engine::ColourSpace::Bt709;
    }
    // What went wrong making a frame: the request's own report first.
    const auto renderProblem = [&] {
        if (request->cancelled()) *error = QStringLiteral("Cancelled");
        else if (!request->error().isEmpty()) *error = request->error();
        else if (error->isEmpty()) *error = QStringLiteral("A frame could not be rendered.");
        *renderFailed = true;
        return false;
    };
    // The encoder takes one frame while the next is made: each frame's
    // pictures and sound go to it together, in order, on a thread of its own.
    struct Made {
        engine::YuvFrame planes;
        QImage picture;
        WorkingFrame linear;
        qint64 copies = 0;
        sound::Samples sound;
        int samples = 0;
    };
    Made made[2];
    int next = 0;
    QString written;  // what stopped the encoder
    std::future<bool> writing;
    const auto settle = qScopeGuard([&] { if (writing.valid()) writing.wait(); });
    const auto writerDone = [&] {
        if (!writing.valid() || writing.get()) return true;
        *error = written;
        return false;
    };
    for (int frame = first; frame <= last; ++frame) {
        Made &m = made[next];
        const qint64 copies = converted ? deliveryFrame(frame + 1) - deliveryFrame(frame) : 1;
        m.copies = settings.video ? std::max<qint64>(0, copies) : 0;
        m.samples = 0;
        if (direct && copies > 0) {
            StageTimer::Scope timed("deliver");
            if (!m_deliverPlanes(frame, colour, m.planes, error) || request->cancelled() || !request->error().isEmpty())
                return renderProblem();
        } else if (settings.video && copies > 0) {
            StageTimer::Scope timed("deliver");
            if (sink.wantsLinear()) m.linear = m_linear->deliverLinear(frame, size, error);
            else m.picture = m_linear->deliver(frame, size, m_deliverySpace, error);
            if ((sink.wantsLinear() ? m.linear.isNull() : m.picture.isNull()) || request->cancelled() || !request->error().isEmpty())
                return renderProblem();
        }
        if (settings.audio) {
            // The frame's own samples on the timeline's clock (exact at any range start).
            const qint64 a = sound::Mixer::frameStart(frame, fps, settings.arate), b = sound::Mixer::frameStart(frame + 1, fps, settings.arate);
            StageTimer::Scope timed("sound");
            if (levelling) {
                if (!mixer.render(a + limiter.latency(), int(b - a), settings.arate, settings.channels, ahead, error)
                    || request->cancelled() || !request->error().isEmpty())
                    return renderProblem();
                m.sound.resize(ahead.size());
                limiter.process(ahead.data(), int(b - a), m.sound.data());
                delivered.push(m.sound.data(), int(b - a));
            } else if (!mixer.render(a, int(b - a), settings.arate, settings.channels, m.sound, error) || request->cancelled() || !request->error().isEmpty())
                return renderProblem();
            m.samples = int(b - a);
        }
        {
            StageTimer::Scope timed("sink.wait");
            if (!writerDone()) return false;
        }
        writing = std::async(std::launch::async, [&sink, &m, &written, direct] {
            StageTimer::Scope timed("sink.picture");
            for (qint64 i = 0; i < m.copies; ++i)
                if (!(direct ? sink.writePlanes(m.planes, &written)
                             : sink.wantsLinear() ? sink.writeLinear(m.linear, &written) : sink.writePicture(m.picture, &written)))
                    return false;
            return m.samples <= 0 || sink.writeSound(m.sound.data(), m.samples, &written);
        });
        next ^= 1;
        if (progress && !progress(frame)) break;
    }
    if (!writerDone()) return false;
    if (levelling) {
        m_deliveredSound = delivered.snapshot();
        m_deliveredSound.insert(QStringLiteral("gain"), settings.loudnessGain);
        m_deliveredSound.insert(QStringLiteral("limited"), limiter.deepestReduction());
    }
    return sink.finish(error);
}


// The mixer given the timeline as it is now (cheap when nothing changed).
void Timeline::configureMixer() {
    if (!m_mixer) m_mixer = std::make_unique<sound::Mixer>();
    QVariantList flags = trackFlagsVariant();
    for (int i = 0; i < flags.size(); ++i) { auto f = flags[i].toMap(); f["index"] = i; flags[i] = f; }
    m_mixer->configure({soundEntries(), flags, m_master, m_mediaSources, m_profile->frame_rate_num(), m_profile->frame_rate_den()});
}

void Timeline::rememberSound(bool on) {
    if (!m_mixer && on) m_mixer = std::make_unique<sound::Mixer>();
    if (m_mixer) m_mixer->remember(on);
}
std::shared_ptr<const sound::MixMemory> Timeline::takeSoundMemory() { return m_mixer ? m_mixer->takeMemory() : nullptr; }
void Timeline::recallSound(const sound::MixMemory &memory) { if (m_mixer) m_mixer->recall(memory); }

quint64 Timeline::soundGeneration() {
    if (!m_profile || frameCount() <= 0) return 0;
    configureMixer();
    return m_mixer->generation();
}

bool Timeline::mixedSamples(int position, int frequency, int channels, std::vector<float> *out) {
    if (frequency < 8000 || !m_profile || frameCount() <= 0) return false;
    configureMixer();
    if (!m_mixer->unsupported().isEmpty()) {
        // Sound it cannot make is a failure, said as such.
        RenderScope::fail(m_mixer->unsupported());
        return true;
    }
    if (m_trackPeaks.size() != size_t(trackCount()) * 6) m_trackPeaks.assign(size_t(trackCount()) * 6, 0.0f);
    const double fps = m_profile->fps();
    const int frame = qBound(0, position, frameCount() - 1);
    const int count = sound::Mixer::frameSamples(frame, fps, frequency);
    // The meters, as the taps and the routing filter make them.
    const double meterPosition = frame + double(m_meterWindow.samples(frequency, count).first) * fps / frequency;
    sound::Taps taps;
    taps.stage = [this](int track, int stage, const sound::Samples &data, int ch, int rate) {
        const int n = ch > 0 ? int(data.size()) / ch : 0;
        const auto range = TapAccess::range(this, rate, n);
        const auto captured = TapAccess::capture(this);
        std::vector<float> trace(captured ? size_t(n) * 2 : 0, 0.f);
        float peak[2] = {0.f, 0.f};
        for (int i = captured ? 0 : range.first; i < (captured ? n : range.last); ++i)
            for (int c = 0; c < ch; ++c) {
                const int slot = c == 0 ? 0 : 1;
                const float a = std::fabs(data[size_t(i) * ch + c]);
                if (i >= range.first && i < range.last && a > peak[slot]) peak[slot] = a;
                if (captured) trace[size_t(2 * i + slot)] = qMax(trace[size_t(2 * i + slot)], a);
            }
        if (ch == 1) {
            peak[1] = peak[0];
            if (captured) for (int i = 0; i < n; ++i) trace[size_t(2 * i + 1)] = trace[size_t(2 * i)];
        }
        if (captured) captured->addTrack(track, stage, rate, std::move(trace));
        TapAccess::write(this, track, stage, peak[0], peak[1]);
    };
    taps.route = [this, meterPosition](int, const QString &bus, const sound::Samples &post, int ch, int rate, const QVariantMap &mix,
                                       const std::vector<double> &reductions) {
        // The gain reduction within the meter window, as the compressor reports it.
        double reduction = 0;
        const auto range = m_meterWindow.samples(rate, ch > 0 ? int(post.size()) / ch : 0);
        for (int i = range.first; i < range.last && i < int(reductions.size()); ++i) reduction = qMax(reduction, reductions[size_t(i)]);
        auto meter = AudioEngine::routeMeter(post, ch, rate, m_meterWindow, mix, reduction, meterPosition);
        if (!bus.isEmpty()) meter["id"] = bus;
        m_audioRouting.addMeter(meter);
        if (m_meterWindow.capture)
            m_meterWindow.capture->addRouting(post, ch, rate, meter, mix,
                                              reductions.empty() ? nullptr : std::make_shared<const std::vector<double>>(reductions), rate);
    };
    QString error;
    if (!m_mixer->render(sound::Mixer::frameStart(frame, fps, frequency), count, frequency, channels, *out, &error, &taps)) {
        if (!RenderScope::cancelled()) RenderScope::fail(error);  // a cancelled read is no failure
        return true;
    }
    return true;
}

bool Timeline::mixedAudio(int position, int frequency, int channels, QByteArray *out) {
    std::vector<float> mixed;
    if (!mixedSamples(position, frequency, channels, &mixed)) return false;
    out->resize(int(mixed.size() * sizeof(int16_t)));
    auto *pcm = reinterpret_cast<int16_t *>(out->data());
    for (size_t i = 0; i < mixed.size(); ++i) pcm[i] = int16_t(std::lrint(std::clamp(mixed[i], -1.f, 1.f) * 32767.f));
    return true;
}

std::vector<float> Timeline::soundAt(int position, int frequency, int channels, QString *error) {
    RenderScope render;
    AudioEngine::ReadScope soundRead;
    std::vector<float> mixed;
    const bool made = mixedSamples(position, frequency, channels, &mixed);
    const QString problem = RenderScope::current()->error();
    if (error) *error = made ? problem : QStringLiteral("There is no sound to mix.");
    if (!made || !problem.isEmpty() || RenderScope::cancelled()) return {};
    return mixed;
}

QByteArray Timeline::audioAt(int position, int frequency, int channels) {
    StageTimer::Scope timed("sound.frame");
    RenderScope render;
    AudioEngine::ReadScope soundRead;
    clearAudioMeters();
    if (RenderScope::cancelled() || !RenderScope::current()->error().isEmpty())
        return {};
    if (QByteArray mixed; mixedAudio(position, frequency, channels, &mixed)) {
        if (RenderScope::cancelled() || !RenderScope::current()->error().isEmpty()) {
            clearAudioMeters();
            return {};
        }
        return mixed;
    }
    return {};
}
