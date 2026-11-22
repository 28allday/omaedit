#include "mixer.h"
#include "projectfile.h"
#include "renderrequest.h"

#include "audiosamplesource.h"
#include "audiotimeline.h"
#include "inserts.h"
#include "loudness.h"
#include "retimemap.h"
#include "projectsafety.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>

#include <rubberband/RubberBandStretcher.h>

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace sound {

namespace {

// While it lives, numbers too small to matter are nothing. A filter's memory
// decaying after the sound stops (a narrow hum notch rings for seconds) passes
// through values the processor works out many times slower than ordinary
// ones, for no audible difference. The thread's own setting comes back after.
class TinyNumbersAreZero {
public:
#if defined(__SSE2__)
    TinyNumbersAreZero() : m_before(_mm_getcsr()) { _mm_setcsr(m_before | 0x8040); }  // flush to zero, denormals are zero
    ~TinyNumbersAreZero() { _mm_setcsr((_mm_getcsr() & ~0x8040u) | (m_before & 0x8040u)); }
#elif defined(__aarch64__)
    TinyNumbersAreZero() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(m_before));
        const unsigned long long flushed = m_before | (1ull << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(flushed));
    }
    ~TinyNumbersAreZero() { __asm__ __volatile__("msr fpcr, %0" : : "r"(m_before)); }
#else
    TinyNumbersAreZero() = default;
#endif
    TinyNumbersAreZero(const TinyNumbersAreZero &) = delete;
    TinyNumbersAreZero &operator=(const TinyNumbersAreZero &) = delete;

private:
#if defined(__SSE2__)
    unsigned int m_before;
#elif defined(__aarch64__)
    unsigned long long m_before = 0;
#endif
};

double number(const QVariantMap &m, const QString &key, double fallback = 0) {
    const double v = m.value(key, fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}

// A clip's level keyframes ({frame, db} maps, clip frames from its first).
struct Keys {
    std::vector<std::pair<double, double>> points;
    explicit Keys(const QVariantList &list) {
        for (const auto &v : list) {
            const auto m = v.toMap();
            if (m.isEmpty()) {
                // Stored as [frame, db] pairs too.
                const auto pair = v.toList();
                if (pair.size() == 2) points.push_back({pair[0].toDouble(), std::clamp(pair[1].toDouble(), -60., 12.)});
                continue;
            }
            points.push_back({m.value("frame").toDouble(), std::clamp(m.value("db").toDouble(), -60., 12.)});
        }
        std::sort(points.begin(), points.end());
    }
    bool empty() const { return points.empty(); }
    // dB at a clip frame: linear between keys, held before the first and after the last.
    double at(double frame) const {
        if (frame <= points.front().first) return points.front().second;
        if (frame >= points.back().first) return points.back().second;
        for (size_t i = 1; i < points.size(); ++i)
            if (frame <= points[i].first) {
                const auto &a = points[i - 1], &b = points[i];
                const double t = b.first > a.first ? (frame - a.first) / (b.first - a.first) : 1;
                return a.second + (b.second - a.second) * t;
            }
        return points.back().second;
    }
};

// What a clip contributes to its track, worked out once per configuration.
struct Clip {
    QString path;
    int track = 0;
    qint64 first = 0, end = 0;   // timeline samples it covers, [first, end)
    qint64 source = 0;           // its source sample at `first`
    double startFrame = 0;       // timeline frame of `first`, for clip-relative frames
    int frames = 0;
    double db = 0, pan = 0;
    std::shared_ptr<Keys> keys;
    int fadeIn = 0, fadeOut = 0;
    QString shape;
    int stream = -1;
    // Crossfades: the frames it rises over at its head (it starts that many
    // frames before its start) and falls over at its tail; the curve is the
    // outgoing clip's (linear or equal power).
    int mixIn = 0, mixOut = 0;
    bool equalPowerIn = false, equalPowerOut = false;
    QString eq;  // its EQ preset, if any
    QString audioMap;  // "mono", "swap", "left", "right" or nothing
    bool normalize = false;  // brought to -23 LUFS by a gain measured on its source
    bool reverse = false;    // played backwards at its own speed (exact samples)
    // A speed ramp: each sample reads the source at its own source time
    // (varispeed); `in` is its first position on the ramp's own clock.
    std::shared_ptr<RetimeMap> ramp;
    qint64 in = 0;
    double sourceScale = 1;  // an interpreted rate's factor
    // A constant speed other than 1× (with an interpreted rate's factor):
    // stretched in time with its pitch kept.
    double stretch = 1;
    // A speed map of constant segments (no ramp): stretched segment by segment,
    // its pitch kept; holds are silent.
    std::shared_ptr<RetimeMap> segments;
    // A nested sequence (an OmaEdit timeline document) or a compound clip:
    // its own mix, read on the sample clock.
    std::shared_ptr<Mixer> nested;
    qint64 nestSamples = 0;  // its length at 48 kHz (for reading it backwards)
    QByteArray nestContent;  // a compound's own clips, as a digest: two compounds are not one source
};

// Where a clip's samples come from: its file (through a reader, forwards or
// backwards) or a nested sequence's own mix, read on either clock.
struct Source {
    AudioSampleSource::Reader *reader = nullptr;
    Mixer *nest = nullptr;
    qint64 nestSamples = 0;
    bool reverse = false;
    explicit operator bool() const { return reader || nest; }
    qint64 sampleCount(int rate) const { return reader ? reader->sampleCount(rate) : nestSamples * rate / 48000; }
    QVector<float> read(qint64 first, int n, int rate, int ch, QString *error) const {
        if (reader) return reader->read(first, n, rate, ch, error);
        if (n <= 0) return {};
        // Backwards: the same span of the forward mix, last sample first.
        const qint64 from = reverse ? sampleCount(rate) - first - n : first;
        QVector<float> pcm(n * ch, 0.f);
        const qint64 lo = std::max<qint64>(0, from), hi = from + n;
        if (lo < hi) {
            Samples mixed;
            QString why;
            if (!nest->render(lo, int(hi - lo), rate, ch, mixed, error ? error : &why)) return {};
            std::copy(mixed.begin(), mixed.end(), pcm.begin() + (lo - from) * ch);
        }
        if (reverse)
            for (int a = 0, b = n - 1; a < b; ++a, --b)
                for (int q = 0; q < ch; ++q) std::swap(pcm[a * ch + q], pcm[b * ch + q]);
        return pcm;
    }
};

// A clip stretched in time, its pitch kept (Rubber Band, real-time mode), as
// a stream: it carries on while reads follow on, and starts again (primed
// with the audio just before) after a jump.
struct Stretch {
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    qint64 next = -1;        // the timeline sample it produces next
    double input = 0;        // its next source sample to feed
    double expected = 0;     // the source sample that output position stands for
    double speed = 1;        // source samples per output sample
    bool reverse = false;    // reading the source backwards
    int channels = 0;
};

// A chain of inserts and whether it has run on the sample just before.
struct Chain {
    std::vector<std::unique_ptr<Insert>> inserts;
    QVariant made;     // what the inserts were made from (an fx list, a preset's name)
    qint64 next = -1;  // the sample it expects next; anything else resets it
    std::vector<double> reductions;  // the largest gain reduction at each sample of the last block (dB)
    bool saved = false;  // the render in hand holds it as it was before
    void run(Samples &data, qint64 first, int n, int channels, int rate) {
        reductions.clear();
        if (inserts.empty()) return;
        if (first != next)
            for (auto &i : inserts) i->reset(channels, rate);
        for (auto &i : inserts) {
            i->process(data.data(), n, channels, rate);
            if (!i->reductions.empty()) {
                if (reductions.empty()) reductions.assign(static_cast<size_t>(n), 0.);
                for (int k = 0; k < n && k < int(i->reductions.size()); ++k) reductions[size_t(k)] = std::max(reductions[size_t(k)], i->reductions[size_t(k)]);
            }
        }
        next = first + n;
    }
    // The chain as it is, its inserts' memory included.
    Chain copy() const {
        Chain c;
        for (const auto &i : inserts) c.inserts.push_back(i->clone());
        c.made = made;
        c.next = next;
        c.reductions = reductions;
        return c;
    }
};

}  // namespace

// One render's: each chain it ran, by what it is (0 a clip's, by its place; 1 a
// track's, by index; 2 a bus's, by id; 3 the master's), as it stood before.
struct MixMemory {
    struct Kept { int kind = 0; QString key; Chain chain; };
    std::vector<Kept> chains;
};

struct Mixer::State {
    TimelineSound timeline;
    bool remembering = false;
    std::shared_ptr<MixMemory> memory;  // the last render's, until taken
    // A clip is the one it was when it plays the same source at the same
    // place on the same track.
    static QString place(const Clip &c) {
        return c.path + QLatin1Char('#') + QString::number(c.stream) + QLatin1Char('#') + QString::number(c.track) + QLatin1Char('#')
               + QString::number(c.first);
    }
    quint64 generation = 0;  // see Mixer::generation()
    double fps = 25;
    std::vector<Clip> clips;
    std::vector<Chain> clipChains;           // clip EQ presets, by clip
    std::vector<Stretch> stretches;          // stretched clips' streams, by clip
    bool stretchSamples(const Clip &c, Stretch &st, qint64 from, int n, int rate, int ch, bool reverse, double start, double speed,
                        QVector<float> &out, QString *error);
    std::unordered_map<int, Chain> trackChains;  // by track index
    std::unordered_map<QString, Chain> busChains;  // by bus id
    Chain masterChain;
    // A render that fails or is cancelled part-way leaves every insert as it
    // was: each chain is copied as it stands before the render first runs it.
    std::vector<std::pair<Chain *, Chain>> before;
    void run(Chain &chain, Samples &data, qint64 first, int n, int channels, int rate) {
        if (!chain.inserts.empty() && !chain.saved) {
            before.emplace_back(&chain, chain.copy());
            chain.saved = true;
        }
        chain.run(data, first, n, channels, rate);
    }
    // Each strip's fader, read from its mix map once and kept while the map
    // stays the same.
    using KeptFader = std::pair<QVariantMap, Fader>;
    std::unordered_map<int, KeptFader> trackFaders;
    std::unordered_map<QString, KeptFader> busFaders;
    KeptFader masterFader;
    template <typename Map, typename Key>
    static const Fader &faderOf(Map &faders, const Key &key, const QVariantMap &mix) {
        auto it = faders.find(key);
        if (it == faders.end()) it = faders.emplace(key, KeptFader{mix, Fader(mix)}).first;
        else if (it->second.first != mix) it->second = KeptFader{mix, Fader(mix)};
        return it->second.second;
    }
    QString unsupported;
    int layoutRate = 0;  // the clock the clips are laid out on
    void layout(int rate);
    // The project files this mixer is nested in (a sequence that holds itself
    // has no sound to make), and nested projects as last read from disk.
    QStringList lineage;
    struct NestDocument { QDateTime modified; qint64 size = -1; QVariantMap document; };
    QHash<QString, NestDocument> nestDocuments;
    // Normalise gains, measured once per source range (reading it whole).
    QHash<QString, double> normalizations;
    double normalization(const Clip &c, int rate, QString *error);
    bool rampSamples(const Clip &c, qint64 from, int n, int rate, int ch, QVector<float> &out, QString *error);
    // One stretch of samples, within a timeline frame: everything mixed.
    bool segment(qint64 first, int count, int rate, int ch, Samples &out, QString *error, const Taps *taps);
    // Open readers by path and stream: decoders and their caches survive edits.
    // Each remembers the render that last used it; when there are many, the
    // ones longest unused are closed, never one the render in hand has used.
    struct KeptReader { std::unique_ptr<AudioSampleSource::Reader> reader; quint64 used = 0; };
    std::unordered_map<QString, KeptReader> readers;
    quint64 renderCount = 0;
    QSet<QString> silentSources;  // files without a sound stream: they sound as silence

    Source source(const Clip &c, QString *error, bool reverse = false) {
        Source from;
        from.reverse = reverse;
        if (c.nested) {
            from.nest = c.nested.get();
            from.nestSamples = c.nestSamples;
        } else {
            from.reader = reader(c.path, c.stream, error, reverse);
        }
        return from;
    }
    AudioSampleSource::Reader *reader(const QString &path, int stream, QString *error, bool reverse = false) {
        const QString key = path + QLatin1Char('#') + QString::number(stream) + (reverse ? QStringLiteral("#r") : QString());
        auto it = readers.find(key);
        if (it != readers.end()) {
            it->second.used = renderCount;
            return it->second.reader.get();
        }
        // Known to be picture-only: not opened again.
        if (stream < 0 && silentSources.contains(path)) return nullptr;
        AudioSampleSource::Options options;
        options.streamIndex = stream;
        options.reverse = reverse;
        auto opened = AudioSampleSource::Reader::open(path, options, error);
        if (!opened) {
            // A picture-only file plays as silence, as it always has.
            if (stream < 0 && error && error->contains(QLatin1String("no decodable sound stream"))) {
                error->clear();
                silentSources.insert(path);
            }
            return nullptr;
        }
        while (readers.size() > 32) {
            auto oldest = readers.end();
            for (auto r = readers.begin(); r != readers.end(); ++r)
                if (r->second.used < renderCount && (oldest == readers.end() || r->second.used < oldest->second.used)) oldest = r;
            if (oldest == readers.end()) break;  // every one is in use now
            readers.erase(oldest);
        }
        return readers.emplace(key, KeptReader{std::move(opened), renderCount}).first->second.reader.get();
    }

    // Why a clip cannot be mixed here yet, or nothing.
    static QString notYet(const QVariantMap &e) {
        // A freeze holds the picture only, and source timing (explicit picture
        // sampling) only the picture's choice: their sound plays on.
        if (!e.value("retime").toMap().isEmpty()) {
            RetimeMap map;
            if (!map.set(e.value("retime").toMap())) return QStringLiteral("an invalid speed map");
        }
        return {};
    }
};

bool Mixer::sounds(const QVariantMap &e) {
    const QString path = e.value("path").toString();
    if (e.value("gap").toBool() || e.value("videoOnly").toBool() || e.value("disabled").toBool()) return false;
    if (!e.value("title").toString().isEmpty() || path.startsWith(QLatin1String("color:"))) return false;
    if (e.contains("generator") && e.value("generator").toString() != QLatin1String("tone")) return false;
    if (path.isEmpty() || e.value("offline").toBool()) return false;
    return true;
}

// The gain that brings a clip's source range to -23 LUFS integrated (EBU
// R128), within ±15 dB; unity when it is silent. NaN when it cannot be read.
double Mixer::State::normalization(const Clip &c, int rate, QString *error) {
    // What the clip plays of its source: for a clip at a constant speed, that
    // much more or less of it; for a reversed one, on the reversed clock.
    const bool plain = !c.ramp && !c.segments;
    const qint64 length = plain && c.stretch != 1 ? qint64(std::llround(double(c.end - c.first) * c.stretch)) : c.end - c.first;
    const bool reverse = plain && c.reverse;
    const QString key = c.path + QLatin1Char('#') + QString::number(c.stream) + QLatin1Char('#') + QString::number(c.source)
                        + QLatin1Char('+') + QString::number(length) + QLatin1Char('@') + QString::number(rate)
                        + (reverse ? QStringLiteral("#r") : QString()) + QLatin1Char('#') + QString::fromLatin1(c.nestContent);
    if (const auto it = normalizations.constFind(key); it != normalizations.constEnd()) return *it;
    const Source r = source(c, error, reverse);
    if (!r) return std::nan("");
    LoudnessMeter meter;
    meter.reset(rate, 2);
    const int block = rate;  // a second at a time
    for (qint64 at = 0; at < length; at += block) {
        const int n = int(std::min<qint64>(block, length - at));
        const QVector<float> pcm = r.read(c.source + at, n, rate, 2, error);
        if (pcm.size() != n * 2) return std::nan("");
        meter.push(pcm.constData(), n);
    }
    const double loudness = meter.integrated();
    const double g = loudness <= -70 ? 1. : gain(std::clamp(-23. - loudness, -15., 15.));
    if (normalizations.size() > 256) normalizations.clear();
    normalizations.insert(key, g);
    return g;
}

// A ramped clip's samples [from, from + n) (one timeline frame at most): each
// read at its own source time, 4-point interpolated; holds, and frames whose
// speed is below 10% or above 1000%, are silent. Position j of a frame of m
// samples sits at j/m of the frame on the ramp's clock.
bool Mixer::State::rampSamples(const Clip &c, qint64 from, int n, int rate, int ch, QVector<float> &out, QString *error) {
    out = QVector<float>(n * ch, 0.f);
    qint64 frame = qint64(std::floor(double(from) * float(fps) / rate));
    while (Mixer::frameStart(frame + 1, fps, rate) <= from) ++frame;
    while (frame > 0 && Mixer::frameStart(frame, fps, rate) > from) --frame;
    const qint64 frameFirst = Mixer::frameStart(frame, fps, rate);
    const int m = Mixer::frameSamples(frame, fps, rate);
    const int position = int(c.in + (frame - qint64(std::llround(c.startFrame))));
    const double end = c.ramp->sourceLength() * c.sourceScale / fps * rate;
    const auto point = c.ramp->sample(position);
    const double frameSpeed = std::abs(point.nextSource - point.source) * c.sourceScale;
    if (!point.valid || frameSpeed < .1 || frameSpeed > 10) return true;
    std::vector<double> at(static_cast<size_t>(n));
    std::vector<char> silent(static_cast<size_t>(n));
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (int k = 0; k < n; ++k) {
        const int j = int(from - frameFirst) + k;
        double speed = 0;
        const double source = c.ramp->sourceAt(position + double(j) / m, &speed);
        at[size_t(k)] = std::clamp(source * c.sourceScale / fps * rate, 0., end);
        silent[size_t(k)] = speed == 0;
        if (!silent[size_t(k)]) { lo = std::min(lo, at[size_t(k)]); hi = std::max(hi, at[size_t(k)]); }
    }
    if (lo > hi) return true;
    const Source r = this->source(c, error);
    if (!r) return false;
    const qint64 first = std::max<qint64>(0, qint64(std::floor(lo)) - 1), last = qint64(std::ceil(hi)) + 2;
    const int span = int(std::min<qint64>(last - first + 1, 16 * 1024 * 1024 / ch));
    const QVector<float> source = r.read(first, span, rate, ch, error);
    if (source.isEmpty()) return false;
    const auto value = [&](qint64 index, int k) { index -= first; return index >= 0 && index < span ? source[int(index) * ch + k] : 0.f; };
    for (int k = 0; k < n; ++k) {
        if (silent[size_t(k)]) continue;
        const double x = at[size_t(k)];
        const qint64 i = qint64(std::floor(x));
        const float u = float(x - double(i));
        for (int q = 0; q < ch; ++q) {
            const float p0 = value(i - 1, q), p1 = value(i, q), p2 = value(i + 1, q), p3 = value(i + 2, q);
            out[k * ch + q] = u == 0.f ? p1 : p1 + .5f * u * (p2 - p0 + u * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + u * (3.f * (p1 - p2) + p3 - p0)));
        }
    }
    return true;
}

bool Mixer::State::stretchSamples(const Clip &c, Stretch &st, qint64 from, int n, int rate, int ch, bool reverse, double start,
                                  double speed, QVector<float> &out, QString *error) {
    const Source r = source(c, error, reverse);
    if (!r) return false;
    const auto read = [&](double at, int count, std::vector<std::vector<float>> &planes) {
        const qint64 first = qint64(std::llround(at));
        const QVector<float> pcm = first + count <= 0 ? QVector<float>(count * ch, 0.f)
                                 : r.read(std::max<qint64>(0, first), count - int(std::max<qint64>(0, -first)), rate, ch, error);
        const int lead = int(std::max<qint64>(0, -first));  // before the source: silence
        for (int q = 0; q < ch; ++q) {
            planes[size_t(q)].assign(size_t(count), 0.f);
            for (int k = lead; k < count && (k - lead) * ch + q < pcm.size(); ++k) planes[size_t(q)][size_t(k)] = pcm[(k - lead) * ch + q];
        }
    };
    // Carry on while the reads follow on (the speed may change between
    // segments); start again after a jump in time, place or direction.
    const bool follows = st.stretcher && st.next == from && st.channels == ch && st.reverse == reverse && std::abs(st.expected - start) < 2;
    if (follows && std::abs(st.speed - speed) > 1e-12) {
        st.stretcher->setTimeRatio(1.0 / speed);
        st.speed = speed;
    }
    if (!follows) {
        // Afresh at `from`: primed with the audio just before, its start
        // delay discarded, so the stream lines up with the clip's clock.
        using RB = RubberBand::RubberBandStretcher;
        st.stretcher = std::make_unique<RB>(size_t(rate), size_t(ch), RB::OptionProcessRealTime | RB::OptionEngineFiner, 1.0 / speed, 1.0);
        st.channels = ch;
        st.reverse = reverse;
        st.speed = speed;
        st.input = start;
        const int pad = int(st.stretcher->getPreferredStartPad());
        std::vector<std::vector<float>> planes(static_cast<size_t>(ch));
        if (pad > 0) {
            read(st.input - pad, pad, planes);
            std::vector<const float *> in(static_cast<size_t>(ch));
            for (int q = 0; q < ch; ++q) in[size_t(q)] = planes[size_t(q)].data();
            st.stretcher->process(in.data(), size_t(pad), false);
        }
        int skip = int(st.stretcher->getStartDelay());
        std::vector<std::vector<float>> waste(static_cast<size_t>(ch), std::vector<float>(4096));
        while (skip > 0) {
            while (st.stretcher->available() <= 0) {
                const int need = std::max(256, int(st.stretcher->getSamplesRequired()));
                read(st.input, need, planes);
                std::vector<const float *> in(static_cast<size_t>(ch));
                for (int q = 0; q < ch; ++q) in[size_t(q)] = planes[size_t(q)].data();
                st.stretcher->process(in.data(), size_t(need), false);
                st.input += need;
            }
            std::vector<float *> outs(static_cast<size_t>(ch));
            for (int q = 0; q < ch; ++q) outs[size_t(q)] = waste[size_t(q)].data();
            skip -= int(st.stretcher->retrieve(outs.data(), size_t(std::min(skip, std::min(4096, st.stretcher->available())))));
        }
    }
    out = QVector<float>(n * ch, 0.f);
    std::vector<std::vector<float>> planes(static_cast<size_t>(ch)), got(static_cast<size_t>(ch), std::vector<float>(size_t(n)));
    int have = 0;
    while (have < n) {
        while (st.stretcher->available() <= 0) {
            const int need = std::max(256, int(st.stretcher->getSamplesRequired()));
            read(st.input, need, planes);
            if (!error->isEmpty()) return false;
            std::vector<const float *> in(static_cast<size_t>(ch));
            for (int q = 0; q < ch; ++q) in[size_t(q)] = planes[size_t(q)].data();
            st.stretcher->process(in.data(), size_t(need), false);
            st.input += need;
        }
        std::vector<float *> outs(static_cast<size_t>(ch));
        for (int q = 0; q < ch; ++q) outs[size_t(q)] = got[size_t(q)].data() + have;
        have += int(st.stretcher->retrieve(outs.data(), size_t(std::min(n - have, st.stretcher->available()))));
    }
    for (int k = 0; k < n; ++k)
        for (int q = 0; q < ch; ++q) out[k * ch + q] = got[size_t(q)][size_t(k)];
    st.next = from + n;
    st.expected = start + double(n) * speed;
    return true;
}

// The first of `keys` set to a positive number in `meta` (media settings hold
// unset keys as nulls), or 0.
static double firstPositive(const QVariantMap &meta, std::initializer_list<const char *> keys) {
    for (const char *key : keys)
        if (const double v = meta.value(QLatin1String(key)).toDouble(); v > 0) return v;
    return 0;
}

// A sequence's length on its own clock, at 48 kHz: where its last clip ends.
static qint64 sequenceSamples(const TimelineSound &timeline) {
    qint64 frames = 0;
    for (const auto &v : timeline.entries) {
        const auto e = v.toMap();
        frames = std::max(frames, e.value("start").toLongLong() + e.value("frames").toLongLong());
    }
    const double fps = timeline.fpsDen > 0 ? double(timeline.fpsNum) / timeline.fpsDen : 25;
    return Mixer::frameStart(frames, fps, 48000);
}

Mixer::Mixer() : d(std::make_unique<State>()) {}
Mixer::~Mixer() = default;

qint64 Mixer::frameStart(qint64 frame, double fps, int rate) {
    // As the editor's frames have always been cut: fps as a float.
    return qint64(std::llrint(double(frame) * double(rate) / double(float(fps))));
}

int Mixer::frameSamples(qint64 frame, double fps, int rate) {
    return int(frameStart(frame + 1, fps, rate) - frameStart(frame, fps, rate));
}

void Mixer::configure(const TimelineSound &timeline) {
    auto &s = *d;
    if (s.timeline.entries == timeline.entries && s.timeline.tracks == timeline.tracks && s.timeline.master == timeline.master
        && s.timeline.mediaSources == timeline.mediaSources && s.timeline.fpsNum == timeline.fpsNum
        && s.timeline.fpsDen == timeline.fpsDen)
        return;
    s.timeline = timeline;
    ++s.generation;
    s.layout(48000);
}

quint64 Mixer::generation() const { return d->generation; }

// The clips laid out on `rate`'s clock: every position in samples at that
// rate. What a change leaves alone carries on as it was: a clip still at its
// place keeps its stretcher, its EQ's memory and its nested mix, and a strip
// whose inserts are the same list keeps them running, so a fader moved while
// playing costs no limiter its look-ahead and no compressor its envelope.
// Only another clock starts everything afresh.
void Mixer::State::layout(int rate) {
    auto &s = *this;
    const TimelineSound &timeline = s.timeline;
    const bool sameClock = rate == s.layoutRate;
    s.layoutRate = rate;
    s.fps = timeline.fpsDen > 0 ? double(timeline.fpsNum) / timeline.fpsDen : 25;
    std::vector<Clip> oldClips = std::move(s.clips);
    std::vector<Chain> oldClipChains = std::move(s.clipChains);
    std::vector<Stretch> oldStretches = std::move(s.stretches);
    s.clips.clear();
    s.clipChains.clear();
    s.stretches.clear();
    s.unsupported.clear();
    QHash<QString, int> placed;
    if (sameClock && oldClipChains.size() == oldClips.size() && oldStretches.size() == oldClips.size())
        for (int i = 0; i < int(oldClips.size()); ++i)
            if (const QString at = place(oldClips[size_t(i)]); !placed.contains(at)) placed.insert(at, i);
    std::vector<int> was;  // each clip's index in the layout before, or -1
    // The clip joins the layout: with the nested mix it had, or a new one.
    const auto add = [&](Clip &c, const TimelineSound *inner, const QString &name) {
        int old = -1;
        if (const auto it = placed.find(place(c)); it != placed.end()) {
            old = it.value();
            placed.erase(it);
        }
        if (inner) {
            if (old >= 0) c.nested = std::move(oldClips[size_t(old)].nested);
            if (!c.nested) c.nested = std::make_shared<Mixer>();
            c.nested->d->lineage = s.lineage;
            if (ProjectFile::isProject(c.path)) c.nested->d->lineage.append(QFileInfo(c.path).canonicalFilePath());
            c.nested->configure(*inner);
            c.nestSamples = sequenceSamples(*inner);
            if (!c.nested->unsupported().isEmpty()) {
                s.unsupported = name + QStringLiteral(" › ") + c.nested->unsupported();
                return false;
            }
        }
        s.clips.push_back(c);
        was.push_back(old);
        return true;
    };
    for (const auto &v : timeline.entries) {
        const auto e = v.toMap();
        if (!Mixer::sounds(e)) continue;
        const QString name = e.value("name", e.value("path")).toString();
        if (const QString why = State::notYet(e); !why.isEmpty()) {
            s.unsupported = name + QStringLiteral(": ") + why;
            return;
        }
        Clip c;
        c.path = e.value("path").toString();
        // A nested sequence or a compound: mixed on its own, first.
        TimelineSound inner;
        bool nests = false;
        if (e.contains("compoundJson") && !e.value("compoundJson").toString().isEmpty()) {
            const QByteArray compound = e.value("compoundJson").toString().toUtf8();
            inner.entries = QJsonDocument::fromJson(compound).toVariant().toList();
            inner.mediaSources = timeline.mediaSources;
            inner.fpsNum = timeline.fpsNum;
            inner.fpsDen = timeline.fpsDen;
            nests = true;
            if (e.value("normalize").toBool()) c.nestContent = QCryptographicHash::hash(compound, QCryptographicHash::Sha1).toHex();
        } else if (ProjectFile::isProject(c.path)) {
            const QFileInfo file(c.path);
            if (s.lineage.contains(file.canonicalFilePath())) {
                s.unsupported = name + QStringLiteral(": a sequence nested inside itself");
                return;
            }
            // Read again only when the file has changed.
            if (!s.nestDocuments.contains(c.path) && s.nestDocuments.size() >= 64) s.nestDocuments.clear();
            NestDocument &kept = s.nestDocuments[c.path];
            if (kept.size != file.size() || kept.modified != file.lastModified() || kept.document.isEmpty())
                kept = {file.lastModified(), file.size(), ProjectSafety::readDocument(c.path)};
            const QVariantMap doc = kept.document;
            if (doc.isEmpty()) {
                s.unsupported = name + QStringLiteral(": not an OmaEdit project");
                return;
            }
            inner.entries = doc.value("entries").toList();
            inner.tracks = doc.value("tracks").toList();
            inner.master = doc.value("master").toMap();
            inner.mediaSources = doc.value("mediaSources").toMap();
            // The project's own media settings reach the nest's clips of the same media.
            for (auto it = timeline.mediaSources.cbegin(); it != timeline.mediaSources.cend(); ++it) inner.mediaSources[it.key()] = it.value();
            for (auto &v : inner.entries) {
                auto inside = v.toMap();
                const auto settings = timeline.mediaSources.value(inside.value("path").toString()).toMap();
                if (!settings.isEmpty()) { inside["sourceMeta"] = settings; v = inside; }
            }
            const QVariantMap profile = doc.value("profile").toMap();
            inner.fpsNum = profile.value("fpsNum", timeline.fpsNum).toInt();
            inner.fpsDen = profile.value("fpsDen", timeline.fpsDen).toInt();
            nests = true;
        }
        c.track = e.value("track", e.value("lane", 0)).toInt();
        // What its bypass switches off: level (and its keys and pan), EQ,
        // channel map, normalise.
        const QStringList bypass = e.value("bypass").toStringList();
        const bool level = !bypass.contains(QLatin1String("level"));
        if (e.contains("audioTiming")) {
            // A precise sound cut: placed in exact time, each boundary rounded
            // once to the sample; only its level and pan apply.
            AudioTimeline::Time start, in, duration;
            const auto timing = e.value("audioTiming").toMap();
            if (!AudioTimeline::readTime(timing.value("start"), &start) || !AudioTimeline::readTime(timing.value("in"), &in)
                || !AudioTimeline::readTime(timing.value("duration"), &duration)) {
                s.unsupported = name + QStringLiteral(": its precise timing is invalid");
                return;
            }
            const auto at = [&](AudioTimeline::Time time) { return qint64(std::llround(static_cast<long double>(time.ticks) / time.rate * rate)); };
            c.first = at(start);
            c.end = c.first + (at(*AudioTimeline::add(start, duration)) - at(start));
            c.source = at(in);
            c.startFrame = double(c.first) * s.fps / rate;
            c.frames = int(std::ceil(double(c.end - c.first) * s.fps / rate));
            c.db = level ? std::clamp(e.value("volume").toDouble(), -100., 36.) : 0;
            c.pan = level ? std::clamp(e.value("pan").toDouble(), -1., 1.) : 0;
            c.stream = e.value("audioStream", -1).toInt();
            // Reversed, its in point is on the reversed source's clock.
            const double speed = e.value("speed", 1.).toDouble();
            if (std::abs(std::abs(speed) - 1) > 1e-9) {
                s.unsupported = name + QStringLiteral(": a precise cut at another speed");
                return;
            }
            c.reverse = speed < 0;
            if (!add(c, nests ? &inner : nullptr, name)) return;
            continue;
        }
        c.mixIn = e.value("mixIn").toInt();
        c.mixOut = e.value("mixOut").toInt();
        c.equalPowerOut = e.value("audioCurve").toString() == QLatin1String("equalPower");
        // A clip coming in over a crossfade starts that many frames early; its
        // source starts at its in point all the same.
        const qint64 start = e.value("start").toLongLong() - c.mixIn, in = e.value("in").toLongLong();
        c.frames = e.value("frames").toInt() + c.mixIn;
        c.first = frameStart(start, s.fps, rate);
        c.end = frameStart(start + c.frames, s.fps, rate);
        c.source = frameStart(in, s.fps, rate);
        c.startFrame = double(start);
        c.db = level ? std::clamp(e.value("volume").toDouble(), -60., 12.) : 0;
        c.pan = level ? std::clamp(e.value("pan").toDouble(), -1., 1.) : 0;
        const auto keys = e.value("volumeKf").toList();
        if (level && !keys.isEmpty()) {
            // Keys with no usable point leave the clip at its plain level.
            const auto read = std::make_shared<Keys>(keys);
            if (!read->empty()) c.keys = read;
        }
        c.fadeIn = e.value("fadeIn").toInt();
        c.fadeOut = e.value("fadeOut").toInt();
        c.shape = e.value("audioCurve").toString();
        c.stream = e.value("audioStream", -1).toInt();
        c.reverse = e.value("speed", 1.).toDouble() < 0;
        c.in = in;
        {
            const QVariantMap meta = e.value("sourceMeta").toMap();
            const double forced = meta.value("forceFps").toDouble(), native = firstPositive(meta, {"sourceFps", "fps"});
            const double factor = forced > 0 && native > 0 ? forced / native : 1;
            // A clip at speed 0 is built at unity (the timeline's own rule).
            const double clipSpeed = e.value("speed", 1.).toDouble() == 0 ? 1. : e.value("speed", 1.).toDouble();
            const double effective = std::abs(clipSpeed) * factor;
            const bool marked = e.contains("sourceOrigin") && e.value("retime").toMap().isEmpty();
            if (marked && !nests && !qFuzzyCompare(effective, 1.)) {
                // A marked clip off unity speed promises exact sync at its cuts:
                // every sample is read at its own source time along the clip's
                // conform (varispeed, as a ramp is), never through a stretcher
                // that restarts at a cut. The origin and the direction are in
                // the conform.
                const int nativeFrames = int(firstPositive(meta, {"sourceDuration", "duration"}));
                auto map = std::make_shared<RetimeMap>();
                if (nativeFrames <= 0 || native <= 0
                    || !map->set(RetimeMap::conform(nativeFrames, native, s.fps, clipSpeed, factor, e.value("sourceOrigin").toDouble()))) {
                    s.unsupported = name + QStringLiteral(": a marked clip without its source's timing");
                    return;
                }
                c.ramp = map;
                c.sourceScale = factor;
            } else if (e.value("retime").toMap().isEmpty() && !qFuzzyCompare(effective, 1.)) {
                c.stretch = effective;
                // Its source position at its first sample: the in point on the
                // stretched clip's own clock, read at that speed.
                c.source = qint64(std::llround(double(frameStart(in, s.fps, rate)) * effective));
            }
            // A marked source origin at unity: that many whole samples on, on the
            // clip's own clock (the reversed one too, as the timeline has it).
            if (marked && !c.ramp)
                c.source += qint64(std::llround(e.value("sourceOrigin").toDouble() * rate));
        }
        if (const auto retime = e.value("retime").toMap(); !retime.isEmpty()) {
            auto map = std::make_shared<RetimeMap>();
            map->set(retime);
            (map->hasRamp() ? c.ramp : c.segments) = map;
            const QVariantMap meta = e.value("sourceMeta").toMap();
            const double forced = meta.value("forceFps").toDouble(), native = firstPositive(meta, {"sourceFps", "fps"});
            c.sourceScale = forced > 0 && native > 0 ? forced / native : 1;
        }
        c.eq = bypass.contains(QLatin1String("eq")) ? QString() : e.value("eq").toString();
        c.audioMap = bypass.contains(QLatin1String("audioMap")) ? QString() : e.value("audioMap").toString();
        c.normalize = !bypass.contains(QLatin1String("normalize")) && e.value("normalize").toBool();
        if (!add(c, nests ? &inner : nullptr, name)) return;
    }
    // Each clip's EQ and stretcher: the ones it had, where it is the clip it was.
    s.clipChains.resize(s.clips.size());
    s.stretches.resize(s.clips.size());
    for (size_t i = 0; i < s.clips.size(); ++i) {
        const QVariant preset = s.clips[i].eq;
        if (was[i] >= 0) s.stretches[i] = std::move(oldStretches[size_t(was[i])]);
        if (was[i] >= 0 && oldClipChains[size_t(was[i])].made == preset) {
            s.clipChains[i] = std::move(oldClipChains[size_t(was[i])]);
            continue;
        }
        s.clipChains[i].inserts = presetInserts(s.clips[i].eq);
        s.clipChains[i].made = preset;
    }
    // Each strip's inserts: kept running where its fx list is the one they were made from.
    const auto strip = [sameClock](Chain &chain, Chain *old, const QVariantList &fx) {
        const QVariant list = fx;
        if (sameClock && old && old->made == list) {
            if (&chain != old) chain = std::move(*old);
            return;
        }
        chain = {};
        chain.inserts = insertsFor(fx);
        chain.made = list;
    };
    std::unordered_map<int, Chain> oldTracks = std::move(s.trackChains);
    s.trackChains.clear();
    QSet<int> trackIndexes;
    for (int t = 0; t < timeline.tracks.size(); ++t) {
        const auto track = timeline.tracks[t].toMap();
        const int index = track.value("index", t).toInt();
        trackIndexes.insert(index);
        const auto old = oldTracks.find(index);
        strip(s.trackChains[index], old == oldTracks.end() ? nullptr : &old->second, track.value("mix").toMap().value("fx").toList());
    }
    std::unordered_map<QString, Chain> oldBuses = std::move(s.busChains);
    s.busChains.clear();
    QSet<QString> busIds;
    for (const auto &v : busList(timeline.master)) {
        const auto bus = v.toMap();
        const QString id = bus.value("id").toString();
        busIds.insert(id);
        const auto old = oldBuses.find(id);
        strip(s.busChains[id], old == oldBuses.end() ? nullptr : &old->second, bus.value("mix").toMap().value("fx").toList());
    }
    strip(s.masterChain, &s.masterChain, timeline.master.value("fx").toList());
    // Faders of strips that are gone.
    std::erase_if(s.trackFaders, [&](const auto &kept) { return !trackIndexes.contains(kept.first); });
    std::erase_if(s.busFaders, [&](const auto &kept) { return !busIds.contains(kept.first); });
    // A crossfade's curve is the outgoing clip's: the clip before, on its track.
    for (Clip &c : s.clips) {
        if (c.mixIn <= 0) continue;
        const Clip *before = nullptr;
        for (const Clip &o : s.clips)
            if (o.track == c.track && o.mixOut > 0 && o.end > c.first && o.first < c.first && (!before || o.first > before->first)) before = &o;
        if (before) c.equalPowerIn = before->equalPowerOut;
    }

}

QString Mixer::unsupported() const { return d->unsupported; }

void Mixer::remember(bool on) {
    d->remembering = on;
    if (!on) d->memory.reset();
}

std::shared_ptr<const MixMemory> Mixer::takeMemory() { return std::exchange(d->memory, nullptr); }

void Mixer::recall(const MixMemory &memory) {
    auto &s = *d;
    for (const MixMemory::Kept &kept : memory.chains) {
        Chain *chain = nullptr;
        if (kept.kind == 3) chain = &s.masterChain;
        else if (kept.kind == 1) {
            if (const auto it = s.trackChains.find(kept.key.toInt()); it != s.trackChains.end()) chain = &it->second;
        } else if (kept.kind == 2) {
            if (const auto it = s.busChains.find(kept.key); it != s.busChains.end()) chain = &it->second;
        } else {
            for (size_t i = 0; !chain && i < s.clips.size() && i < s.clipChains.size(); ++i)
                if (State::place(s.clips[i]) == kept.key) chain = &s.clipChains[i];
        }
        // Only the chain it was: one made from another list since has no use
        // for this memory, and starts as a new chain does.
        if (!chain || chain->made != kept.chain.made || chain->inserts.size() != kept.chain.inserts.size()) continue;
        *chain = kept.chain.copy();
    }
    for (auto &st : s.stretches) st.next = -1;  // a stretcher starts again where it is next read
}

bool Mixer::render(qint64 first, int count, int rate, int channels, Samples &out, QString *error, const Taps *taps) {
    auto &s = *d;
    if (!s.unsupported.isEmpty()) { *error = s.unsupported; return false; }
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 8) { *error = QStringLiteral("The sound format is outside what the mixer makes."); return false; }
    if (rate != s.layoutRate) {
        s.layout(rate);
        if (!s.unsupported.isEmpty()) { *error = s.unsupported; return false; }
    }
    out.assign(size_t(count) * channels, 0.f);
    ++s.renderCount;
    const TinyNumbersAreZero quiet;
    // A render that fails or is cancelled part-way leaves every insert as it
    // was, so the same samples asked again come out as they would have: each
    // chain the render runs is kept as it stood (State::run), and put back.
    const auto interrupted = [] { return RenderScope::cancelled() || (RenderScope::current() && !RenderScope::current()->error().isEmpty()); };
    const auto settle = [&s](bool keep) {
        std::shared_ptr<MixMemory> memory = keep && s.remembering ? std::make_shared<MixMemory>() : nullptr;
        for (auto &[chain, was] : s.before) {
            if (!keep) { *chain = std::move(was); continue; }
            chain->saved = false;
            if (!memory) continue;
            // Which chain it is, said in a way that outlives the next layout.
            MixMemory::Kept kept;
            kept.kind = -1;
            if (chain == &s.masterChain) kept.kind = 3;
            else if (!s.clipChains.empty() && chain >= s.clipChains.data() && chain < s.clipChains.data() + s.clipChains.size()) {
                kept.kind = 0;
                kept.key = State::place(s.clips[size_t(chain - s.clipChains.data())]);
            }
            for (auto it = s.trackChains.begin(); kept.kind < 0 && it != s.trackChains.end(); ++it)
                if (&it->second == chain) { kept.kind = 1; kept.key = QString::number(it->first); }
            for (auto it = s.busChains.begin(); kept.kind < 0 && it != s.busChains.end(); ++it)
                if (&it->second == chain) { kept.kind = 2; kept.key = it->first; }
            if (kept.kind < 0) continue;
            kept.chain = std::move(was);
            memory->chains.push_back(std::move(kept));
        }
        s.before.clear();
        s.memory = std::move(memory);
    };
    const auto rollBack = [&] {
        settle(false);
        for (auto &st : s.stretches) st.next = -1;  // a stretcher starts again where it is next read
    };
    // A timeline frame at a time: the block every insert has always seen.
    qint64 at = first;
    const qint64 last = first + count;
    while (at < last) {
        qint64 frame = qint64(std::floor(double(at) * float(s.fps) / rate));
        while (frameStart(frame + 1, s.fps, rate) <= at) ++frame;
        while (frame > 0 && frameStart(frame, s.fps, rate) > at) --frame;
        const qint64 end = std::min(last, frameStart(frame + 1, s.fps, rate));
        Samples part;
        if (!s.segment(at, int(end - at), rate, channels, part, error, taps) || interrupted()) {
            rollBack();
            if (error && error->isEmpty()) *error = RenderScope::cancelled() ? QStringLiteral("Cancelled") : RenderScope::current()->error();
            return false;
        }
        std::copy(part.begin(), part.end(), out.begin() + (at - first) * channels);
        at = end;
    }
    settle(true);
    return true;
}

bool Mixer::State::segment(qint64 first, int count, int rate, int ch, Samples &out, QString *error, const Taps *taps) {
    auto &s = *this;
    out.assign(size_t(count) * ch, 0.f);
    const qint64 last = first + count;

    // Each track's clips, summed.
    QHash<int, Samples> tracks;
    for (size_t index = 0; index < s.clips.size(); ++index) {
        const Clip &c = s.clips[index];
        const qint64 from = std::max(first, c.first), to = std::min(last, c.end);
        if (from >= to) continue;
        const int n = int(to - from);
        if (!c.nested && !c.path.startsWith(QLatin1String("tone:"))) {
            // A picture-only file plays as silence; any other failure is one.
            if (!s.reader(c.path, c.stream, error, false)) {
                if (s.silentSources.contains(c.path)) continue;
                return false;
            }
        }
        QVector<float> pcm;
        if (c.path.startsWith(QLatin1String("tone:"))) {
            // The tone generator: a 1 kHz sine at full scale (whatever the path
            // names, as it has always sounded), in phase from the clip's source.
            pcm.resize(n * ch);
            const qint64 at = c.source + (from - c.first);
            for (int i = 0; i < n; ++i) {
                const float v = float(std::sin(2 * M_PI * 1000. * double(at + i) / rate));
                for (int k = 0; k < ch; ++k) pcm[i * ch + k] = v;
            }
        } else if (c.nested && !c.ramp && !c.segments && c.stretch == 1 && !c.reverse) {
            Samples inner;
            if (!c.nested->render(c.source + (from - c.first), n, rate, ch, inner, error)) return false;
            pcm = QVector<float>(inner.begin(), inner.end());
        } else if (c.ramp) {
            if (!s.rampSamples(c, from, n, rate, ch, pcm, error)) return false;
        } else if (c.segments) {
            // This frame's segment: where it reads and how fast.
            qint64 frame = qint64(std::floor(double(from) * float(s.fps) / rate));
            while (frameStart(frame + 1, s.fps, rate) <= from) ++frame;
            while (frame > 0 && frameStart(frame, s.fps, rate) > from) --frame;
            const qint64 frameFirst = frameStart(frame, s.fps, rate);
            const int m = frameSamples(frame, s.fps, rate);
            const auto point = c.segments->sample(int(c.in + (frame - qint64(std::llround(c.startFrame)))));
            const double perSample = (point.nextSource - point.source) * c.sourceScale / s.fps * rate / m;
            if (!point.valid || perSample == 0) {
                pcm = QVector<float>(n * ch, 0.f);  // a hold: silence
                s.stretches[index].next = -1;
            } else {
                double at = point.source * c.sourceScale / s.fps * rate + double(from - frameFirst) * perSample;
                const bool backwards = perSample < 0;
                if (backwards) {
                    const Source r = s.source(c, error, true);
                    if (!r) return false;
                    at = double(r.sampleCount(rate)) - at;
                }
                if (!s.stretchSamples(c, s.stretches[index], from, n, rate, ch, backwards, at, std::abs(perSample), pcm, error)) return false;
            }
        } else if (c.stretch != 1) {
            if (!s.stretchSamples(c, s.stretches[index], from, n, rate, ch, c.reverse,
                                  double(c.source) + double(from - c.first) * c.stretch, c.stretch, pcm, error))
                return false;
        } else {
            const Source r = s.source(c, error, c.reverse);
            if (!r) return false;
            pcm = r.read(c.source + (from - c.first), n, rate, ch, error);
        }
        if (pcm.size() != n * ch) {
            if (error->isEmpty()) *error = QStringLiteral("The sound of %1 could not be read.").arg(QFileInfo(c.path).fileName());
            return false;
        }
        const double g0 = gain(c.db);
        for (int i = 0; i < n; ++i) {
            // Clip-relative frame of this sample, for keyframes and fades.
            const double frame = double(from - c.first + i) * s.fps / rate;
            double g = c.keys ? gain(c.keys->at(frame)) : g0;
            // A shaped fade reaches its end at its last frame's start (as the
            // fade curves always have); the default dB fade spans every frame
            // of the fade, ending as its last frame ends.
            const int span = c.shape.isEmpty() ? 0 : 1;
            if (c.fadeIn > 0 && frame < c.fadeIn) {
                const double x = std::clamp(frame / std::max(1, c.fadeIn - span), 0., 1.);
                g *= fadeGain(c.shape, x);
            }
            if (c.fadeOut > 0 && frame > c.frames - c.fadeOut) {
                const double x = std::clamp(1 - (frame - (c.frames - c.fadeOut)) / std::max(1, c.fadeOut - span), 0., 1.);
                g *= fadeGain(c.shape, x);
            }
            // Crossfades: rising over the head's overlap, falling over the tail's.
            if (c.mixIn > 0 && frame < c.mixIn) {
                const double m = std::clamp(frame / c.mixIn, 0., 1.);
                g *= c.equalPowerIn ? std::sin(m * M_PI / 2) : m;
            }
            if (c.mixOut > 0 && frame > c.frames - c.mixOut) {
                const double m = std::clamp((frame - (c.frames - c.mixOut)) / c.mixOut, 0., 1.);
                g *= c.equalPowerOut ? std::cos(m * M_PI / 2) : 1 - m;
            }
            const double left = ch < 2 ? 1. : std::min(1., 1. - c.pan), right = std::min(1., 1. + c.pan);
            pcm[i * ch] = float(pcm[i * ch] * g * left);
            for (int k = 1; k < ch; ++k) pcm[i * ch + k] = float(pcm[i * ch + k] * g * (k == 1 ? right : 1.));
        }
        if (!s.clipChains[index].inserts.empty()) {
            Samples own(pcm.begin(), pcm.end());
            s.run(s.clipChains[index], own, from, n, ch, rate);
            std::copy(own.begin(), own.end(), pcm.begin());
        }
        // Its channel map: the two sides folded, swapped, or one on both.
        if (!c.audioMap.isEmpty() && ch >= 2)
            for (int i = 0; i < n; ++i) {
                float &l = pcm[i * ch], &r = pcm[i * ch + 1];
                if (c.audioMap == QLatin1String("mono")) l = r = (l + r) / 2;
                else if (c.audioMap == QLatin1String("swap")) std::swap(l, r);
                else if (c.audioMap == QLatin1String("left")) r = l;
                else if (c.audioMap == QLatin1String("right")) l = r;
            }
        if (c.normalize) {
            const double g = s.normalization(c, rate, error);
            if (!std::isfinite(g)) return false;
            for (auto &v : pcm) v = float(v * g);
        }
        Samples &sum = tracks[c.track];
        if (sum.empty()) sum.assign(size_t(count) * ch, 0.f);
        const size_t at = size_t(from - first) * ch;
        for (int i = 0; i < n * ch; ++i) sum[at + i] += pcm[i];
    }

    // Routing: each track's fader, then outputs and sends to buses and the
    // master, as the mix maps say; buses in dependency order.
    const double pos = double(first) * s.fps / rate;  // the block's first sample, in timeline frames
    const QVariantList buses = busList(s.timeline.master);
    QHash<QString, int> ids;
    for (int b = 0; b < buses.size(); ++b) ids[buses[b].toMap().value("id").toString()] = b;
    QSet<QString> solo, ancestors, downstream;
    for (const auto &v : buses)
        if (v.toMap().value("mix").toMap().value("solo").toBool()) solo.insert(v.toMap().value("id").toString());
    ancestors = downstream = solo;
    const auto destinations = [](const QVariantMap &m) {
        QStringList list{m.value("output", "master").toString()};
        for (const auto &v : m.value("sends").toList())
            if (!v.toMap().value("bypass").toBool()) list.append(v.toMap().value("bus").toString());
        return list;
    };
    for (int pass = 0; pass < buses.size(); ++pass)
        for (const auto &v : buses) {
            const auto b = v.toMap();
            const QString id = b.value("id").toString();
            for (const auto &dest : destinations(b.value("mix").toMap())) {
                if (ancestors.contains(dest)) ancestors.insert(id);
                if (downstream.contains(id) && dest != "master") downstream.insert(dest);
            }
        }
    QHash<QString, Samples> pending;
    QString sourceBus;
    const auto add = [&](const QString &dest, const Samples &data, double amount) {
        if (!solo.isEmpty() && !ancestors.contains(dest) && !downstream.contains(sourceBus)) return;
        Samples *into = &out;
        if (!dest.isEmpty() && dest != "master") {
            if (!ids.contains(dest)) return;
            into = &pending[dest];
            if (into->empty()) into->assign(out.size(), 0.f);
        }
        for (size_t i = 0; i < std::min(into->size(), data.size()); ++i) (*into)[i] += float(data[i] * amount);
    };
    const auto route = [&](const QVariantMap &mix, const Samples &pre, const Samples &post) {
        add(mix.value("output", "master").toString(), post, 1);
        for (const auto &v : mix.value("sends").toList()) {
            const auto send = v.toMap();
            const QString id = send.value("bus").toString();
            if (!id.isEmpty() && !send.value("bypass").toBool())
                add(id, send.value("pre").toBool() ? pre : post, gain(number(send, "level", -100)));
        }
    };
    if (s.timeline.tracks.isEmpty()) {
        // A compound's clips: summed as they are, no tracks, no master.
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it)
            for (size_t i = 0; i < out.size(); ++i) out[i] += it.value()[i];
        return true;
    }
    bool anySolo = false;
    for (const auto &v : s.timeline.tracks) anySolo = anySolo || v.toMap().value("solo").toBool();
    for (int t = 0; t < s.timeline.tracks.size(); ++t) {
        const auto track = s.timeline.tracks[t].toMap();
        const int index = track.value("index", t).toInt();
        const auto mix = track.value("mix").toMap();
        Samples pre = tracks.value(index);
        if (pre.empty()) pre.assign(out.size(), 0.f);
        if (taps && taps->stage) taps->stage(index, 0, pre, ch, rate);
        s.run(s.trackChains[index], pre, first, count, ch, rate);
        if (taps && taps->stage) taps->stage(index, 1, pre, ch, rate);
        Samples post = pre;
        faderOf(s.trackFaders, index, mix).run(post, ch, rate, s.fps, pos);
        if (taps && taps->stage) taps->stage(index, 2, post, ch, rate);
        // The delivery's channel map: the track folded to mono on one channel.
        const QString mapping = s.timeline.master.value("channelMap").toMap().value(QString::number(index), "stereo").toString();
        if (mapping != QLatin1String("stereo")) {
            const int target = mapping == "left" ? 0 : mapping == "right" ? 1 : mapping == "center" ? 2 : mapping == "lfe" ? 3
                               : mapping == "surroundLeft" ? 4 : mapping == "surroundRight" ? 5 : -1;
            const auto mapped = [&](Samples &d) {
                for (int i = 0; i < count; ++i) {
                    const float mono = ch >= 2 ? (d[size_t(i) * ch] + d[size_t(i) * ch + 1]) * .5f : d[size_t(i) * ch];
                    for (int c = 0; c < ch; ++c) d[size_t(i) * ch + size_t(c)] = c == target ? mono : 0;
                }
            };
            mapped(pre);
            mapped(post);
        }
        if (taps && taps->route) taps->route(index, QString(), post, ch, rate, mix, s.trackChains[index].reductions);
        if (track.value("muted").toBool() || (track.value("disabled").toBool() && track.value("audio").toBool())
            || (anySolo && !track.value("solo").toBool())) {
            std::fill(pre.begin(), pre.end(), 0.f);
            std::fill(post.begin(), post.end(), 0.f);
        }
        route(mix, pre, post);
    }
    QSet<QString> done;
    for (int pass = 0; pass < buses.size(); ++pass)
        for (int b = 0; b < buses.size(); ++b) {
            const auto bus = buses[b].toMap();
            const QString id = bus.value("id").toString();
            if (done.contains(id)) continue;
            bool waiting = false;
            for (const auto &v : buses) {
                const auto other = v.toMap();
                if (done.contains(other.value("id").toString())) continue;
                const auto m = other.value("mix").toMap();
                if (m.value("output") == id) waiting = true;
                for (const auto &sv : m.value("sends").toList())
                    if (sv.toMap().value("bus") == id) waiting = true;
            }
            if (waiting) continue;
            Samples pre = pending.value(id);
            if (pre.empty()) pre.assign(out.size(), 0.f);
            s.run(s.busChains[id], pre, first, count, ch, rate);
            const auto mix = bus.value("mix").toMap();
            if (mix.value("muted").toBool()) std::fill(pre.begin(), pre.end(), 0.f);
            Samples post = pre;
            faderOf(s.busFaders, id, mix).run(post, ch, rate, s.fps, pos);
            if (taps && taps->route) taps->route(-1, id, post, ch, rate, mix, s.busChains[id].reductions);
            sourceBus = id;
            route(mix, pre, post);
            done.insert(id);
        }
    // The master's inserts, then its fader.
    s.run(s.masterChain, out, first, count, ch, rate);
    if (s.masterFader.first != s.timeline.master) s.masterFader = {s.timeline.master, Fader(s.timeline.master)};
    s.masterFader.second.run(out, ch, rate, s.fps, pos);
    if (taps && taps->route) taps->route(-2, QStringLiteral("master"), out, ch, rate, {}, s.masterChain.reductions);
    return true;
}

}  // namespace sound
