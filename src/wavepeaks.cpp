#include "wavepeaks.h"
#include "audiotimeline.h"
#include "projectfile.h"
#include "sound/dsp.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace WavePeaks {

namespace {
constexpr quint32 kMagic = 0x4f4d5750;  // "OMWP"
constexpr quint32 kVersion = 2;         // 1 was a peak a frame (playerworkerdecor.cpp)

// The samples in a block of each level.
constexpr qint64 kSizes[kLevels] = {256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304};
static_assert(kBlock == 256 && kFactor == 4 && kLevels == 8);

qint16 down(float v) { return qint16(std::floor(std::clamp(v, -1.f, 1.f) * kScale)); }
qint16 up(float v) { return qint16(std::ceil(std::clamp(v, -1.f, 1.f) * kScale)); }

}  // namespace

Data::Data(int rate, int channels, qint64 samples)
    : m_rate(qMax(1, rate)), m_channels(qBound(1, channels, 2)), m_samples(qMax<qint64>(0, samples)),
      m_open(size_t(kLevels) * size_t(qBound(1, channels, 2))) {}

int Data::blockSize(int level) { return int(kSizes[qBound(0, level, kLevels - 1)]); }

qint64 Data::made() const {
    QReadLocker lock(&m_lock);
    return m_made;
}

bool Data::complete() const {
    QReadLocker lock(&m_lock);
    return m_complete;
}

qint64 Data::blocks(int level) const {
    QReadLocker lock(&m_lock);
    return level < 0 || level >= kLevels ? 0 : qint64(m_levels[level].lo.size()) / m_channels;
}

qint64 Data::bytes() const {
    QReadLocker lock(&m_lock);
    qint64 values = 0;
    for (const Level &level : m_levels) values += qint64(level.lo.size());
    return values * 6;
}

Span Data::block(int level, int channel, qint64 index) const {
    QReadLocker lock(&m_lock);
    if (level < 0 || level >= kLevels || index < 0 || index * m_channels >= qint64(m_levels[level].lo.size())) return {};
    const qint64 size = kSizes[level];
    return read(level, channel, index * size, (index + 1) * size, index * size, (index + 1) * size);
}

// A level's block is closed: written down, and counted into the block of the
// level above that it is a part of.
void Data::close(int level) {
    Level &to = m_levels[level];
    for (int c = 0; c < m_channels; ++c) {
        Open &open = m_open[size_t(level) * size_t(m_channels) + size_t(c)];
        to.lo.push_back(down(open.lo));
        to.hi.push_back(up(open.hi));
        to.rms.push_back(quint16(std::lround(std::min(1., std::sqrt(open.squares / double(qMax<qint64>(1, open.count)))) * 65535.)));
        if (level + 1 < kLevels) {
            Open &above = m_open[size_t(level + 1) * size_t(m_channels) + size_t(c)];
            above.lo = above.count ? std::min(above.lo, open.lo) : open.lo;
            above.hi = above.count ? std::max(above.hi, open.hi) : open.hi;
            above.squares += open.squares;
            above.count += open.count;
        }
        open = {};
    }
    if (level + 1 < kLevels && m_open[size_t(level + 1) * size_t(m_channels)].count >= kSizes[level + 1]) close(level + 1);
}

void Data::append(const float *interleaved, qint64 frames) {
    QWriteLocker lock(&m_lock);
    if (m_complete) return;
    for (qint64 i = 0; i < frames; ++i) {
        for (int c = 0; c < m_channels; ++c) {
            float v = interleaved[i * m_channels + c];
            if (!std::isfinite(v)) v = 0;
            Open &open = m_open[size_t(c)];
            open.lo = open.count ? std::min(open.lo, v) : v;
            open.hi = open.count ? std::max(open.hi, v) : v;
            open.squares += double(v) * double(v);
            ++open.count;
        }
        ++m_made;
        if (m_open[0].count >= kBlock) close(0);
    }
}

void Data::finish() {
    QWriteLocker lock(&m_lock);
    if (m_complete) return;
    // The last block of each level is as long as the source's end leaves it.
    for (int level = 0; level < kLevels; ++level)
        if (m_open[size_t(level) * size_t(m_channels)].count > 0) close(level);
    m_samples = m_made;
    m_complete = true;
}

// Samples [firstSample, lastSample) from the blocks of `level` that lie
// within [from, to): a block wholly inside is read as it is, one cut by an
// end is read from the level below (the finest level's as it is: 256 samples
// are the least this tells apart), and so is one not made yet.
void Data::walk(int level, int channel, qint64 from, qint64 to, qint64 firstSample, qint64 lastSample, Sum &sum) const {
    const Level &blocks = m_levels[level];
    const qint64 size = kSizes[level], count = qint64(blocks.lo.size()) / m_channels;
    const int c0 = channel < 0 ? 0 : qMin(channel, m_channels - 1), c1 = channel < 0 ? m_channels : c0 + 1;
    for (qint64 i = from / size; i * size < to; ++i) {
        const qint64 start = i * size, end = start + size;
        if (i < count && (level == 0 || (start >= firstSample && end <= lastSample))) {
            const double n = double(std::min(end, m_made) - start);
            for (int c = c0; c < c1; ++c) {
                const size_t k = size_t(i) * size_t(m_channels) + size_t(c);
                const double rms = blocks.rms[k] / 65535.;
                sum.lo = std::min(sum.lo, blocks.lo[k]);
                sum.hi = std::max(sum.hi, blocks.hi[k]);
                sum.squares += rms * rms * n;
                sum.count += n;
            }
        } else if (level > 0) {
            walk(level - 1, channel, std::max(start, from), std::min(end, to), firstSample, lastSample, sum);
        }
    }
}

Span Data::read(int level, int channel, qint64 from, qint64 to, qint64 firstSample, qint64 lastSample) const {
    Sum sum;
    walk(level, channel, from, to, firstSample, lastSample, sum);
    if (sum.count <= 0) return {};
    return {sum.lo / kScale, sum.hi / kScale, float(std::sqrt(sum.squares / sum.count)), true};
}

Span Data::span(int channel, double first, double last, int level) const {
    QReadLocker lock(&m_lock);
    if (last < first) std::swap(first, last);
    const qint64 a = std::max<qint64>(0, qint64(std::floor(first)));
    const qint64 b = std::min(m_made, std::max(a + 1, qint64(std::ceil(last))));
    if (b <= a) return {};
    if (level < 0) {
        level = 0;
        while (level + 1 < kLevels && kSizes[level + 1] <= b - a) ++level;
    }
    return read(qBound(0, level, kLevels - 1), channel, a, b, a, b);
}

QByteArray Data::packed() const {
    QReadLocker lock(&m_lock);
    QByteArray body;
    {
        QDataStream out(&body, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::LittleEndian);
        out << qint32(m_rate) << qint32(m_channels) << qint64(m_made);
        for (const Level &level : m_levels) {
            out << quint32(level.lo.size());
            for (const qint16 v : level.lo) out << v;
            for (const qint16 v : level.hi) out << v;
            for (const quint16 v : level.rms) out << v;
        }
    }
    // What it is and which version, then the peaks packed (a recording's
    // pack to rather less than they are), then a sum over all of it.
    QByteArray all;
    QDataStream(&all, QIODevice::WriteOnly) << kMagic << kVersion;
    all += qCompress(body, 1);
    QDataStream(&all, QIODevice::Append) << quint16(qChecksum(QByteArrayView(all)));
    return all;
}

std::shared_ptr<Data> Data::unpacked(const QByteArray &bytes) {
    if (bytes.size() < 14) return nullptr;
    const QByteArray whole = bytes.left(bytes.size() - 2);
    quint16 sum = 0;
    QDataStream(bytes.right(2)) >> sum;
    quint32 magic = 0, version = 0;
    QDataStream(whole) >> magic >> version;
    if (sum != qChecksum(QByteArrayView(whole)) || magic != kMagic || version != kVersion) return nullptr;
    const QByteArray body = qUncompress(whole.mid(8));
    QDataStream in(body);
    in.setByteOrder(QDataStream::LittleEndian);
    qint32 rate = 0, channels = 0;
    qint64 samples = 0;
    in >> rate >> channels >> samples;
    if (in.status() != QDataStream::Ok || rate < 1 || channels < 1 || channels > 2 || samples < 0) return nullptr;
    auto data = std::make_shared<Data>(rate, channels, samples);
    qint64 expected = samples;
    for (int l = 0; l < kLevels; ++l) {
        quint32 count = 0;
        in >> count;
        // As many blocks as its samples make, no more and no fewer.
        expected = l == 0 ? (samples + kBlock - 1) / kBlock : (expected + kFactor - 1) / kFactor;
        if (in.status() != QDataStream::Ok || qint64(count) != expected * channels) return nullptr;
        Level &level = data->m_levels[l];
        level.lo.resize(count);
        level.hi.resize(count);
        level.rms.resize(count);
        for (qint16 &v : level.lo) in >> v;
        for (qint16 &v : level.hi) in >> v;
        for (quint16 &v : level.rms) in >> v;
    }
    if (in.status() != QDataStream::Ok) return nullptr;
    data->m_made = samples;
    data->m_complete = true;
    return data;
}

// ── Where a clip's sound is read from ───────────────────────────────────────

namespace {
double frameStart(double frame, double fps) {
    // As the mixer cuts frames (sound::Mixer::frameStart): fps as a float.
    return double(std::llrint(frame * 48000. / double(float(fps))));
}
double firstPositive(const QVariantMap &m, std::initializer_list<const char *> keys) {
    for (const char *key : keys)
        if (const double v = m.value(QLatin1String(key)).toDouble(); v > 0) return v;
    return 0;
}
QString digest(const QVariant &what) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(what).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex());
}
}  // namespace

Source Source::ofFile(const QString &path, double fps) {
    Source s;
    s.fps = fps;
    if (path.isEmpty() || !(fps > 0)) return s;
    s.kind = File;
    s.path = path;
    s.id = QStringLiteral("file|-1|") + path;
    return s;
}

// The conditions and the arithmetic are sound::Mixer::State::layout's, clip
// for clip: the picture of a clip is of the samples it plays.
Source Source::of(const QVariantMap &e, double fps) {
    Source s;
    s.fps = fps;
    const QString path = e.value(QStringLiteral("path")).toString();
    if (!(fps > 0) || path.isEmpty() || e.value(QStringLiteral("gap")).toBool() || e.value(QStringLiteral("videoOnly")).toBool()
        || e.value(QStringLiteral("disabled")).toBool() || e.value(QStringLiteral("offline")).toBool()
        || !e.value(QStringLiteral("title")).toString().isEmpty() || path.startsWith(QLatin1String("color:"))
        || (e.contains(QStringLiteral("generator")) && e.value(QStringLiteral("generator")).toString() != QLatin1String("tone")))
        return s;
    const QStringList bypass = e.value(QStringLiteral("bypass")).toStringList();
    const bool nests = !e.value(QStringLiteral("compoundJson")).toString().isEmpty() || ProjectFile::isProject(path);
    const bool made = nests || path.startsWith(QLatin1String("tone:"));
    const bool precise = e.contains(QStringLiteral("audioTiming"));
    const double speed = e.value(QStringLiteral("speed"), 1.).toDouble() == 0 ? 1. : e.value(QStringLiteral("speed"), 1.).toDouble();
    s.stream = e.value(QStringLiteral("audioStream"), -1).toInt();
    s.reverse = speed < 0;
    bool plain = !made;
    double frames = 0;
    if (precise) {
        AudioTimeline::Time start, in, duration;
        const QVariantMap timing = e.value(QStringLiteral("audioTiming")).toMap();
        if (!AudioTimeline::readTime(timing.value(QStringLiteral("start")), &start) || !AudioTimeline::readTime(timing.value(QStringLiteral("in")), &in)
            || !AudioTimeline::readTime(timing.value(QStringLiteral("duration")), &duration) || std::abs(std::abs(speed) - 1) > 1e-9)
            return s;
        const auto at = [](AudioTimeline::Time time) { return double(std::llround(static_cast<long double>(time.ticks) / time.rate * 48000)); };
        s.first = at(start);
        s.at = at(in);
        const auto end = AudioTimeline::add(start, duration);
        frames = end ? (at(*end) - s.first) * fps / 48000. : 0;
    } else {
        const int mixIn = e.value(QStringLiteral("mixIn")).toInt();
        const double in = e.value(QStringLiteral("in")).toDouble();
        frames = e.value(QStringLiteral("frames")).toDouble() + mixIn;
        s.first = frameStart(e.value(QStringLiteral("start")).toDouble() - mixIn, fps);
        s.at = frameStart(in, fps);
        const QVariantMap meta = e.value(QStringLiteral("sourceMeta")).toMap();
        const double forced = meta.value(QStringLiteral("forceFps")).toDouble(), native = firstPositive(meta, {"sourceFps", "fps"});
        const double effective = std::abs(speed) * (forced > 0 && native > 0 ? forced / native : 1);
        const bool retimed = !e.value(QStringLiteral("retime")).toMap().isEmpty();
        const bool marked = e.contains(QStringLiteral("sourceOrigin")) && !retimed;
        const bool unity = qFuzzyCompare(effective, 1.);
        const bool shaped = (!bypass.contains(QLatin1String("eq")) && !e.value(QStringLiteral("eq")).toString().isEmpty())
                            || (!bypass.contains(QLatin1String("audioMap")) && !e.value(QStringLiteral("audioMap")).toString().isEmpty())
                            || (!bypass.contains(QLatin1String("normalize")) && e.value(QStringLiteral("normalize")).toBool());
        plain = plain && !retimed && !shaped && !(marked && !unity);
        if (plain && !unity) {
            // Another constant speed: read that much faster, from its in point on the faster clock.
            s.step = effective;
            s.at = double(std::llround(s.at * effective));
        }
        if (plain && marked) s.at += double(std::llround(e.value(QStringLiteral("sourceOrigin")).toDouble() * 48000));
    }
    if (plain) {
        s.kind = File;
        s.path = path;
        s.id = QStringLiteral("file|%1|").arg(s.stream) + path;
        return s;
    }
    // Its own sound, as the mixer makes it: the clip alone from frame 0,
    // with all that shapes the sound and none of what sets its level.
    s.kind = Clip;
    for (const char *key : {"path", "generator", "compoundJson", "normalize", "audioStream", "speed", "in", "sourceMeta", "sourceOrigin", "retime", "eq", "audioMap"})
        if (e.contains(QLatin1String(key))) s.clip.insert(QLatin1String(key), e.value(QLatin1String(key)));
    QStringList off = bypass;
    off.removeAll(QStringLiteral("level"));
    if (!off.isEmpty()) s.clip.insert(QStringLiteral("bypass"), off);
    if (precise) {
        QVariantMap timing = e.value(QStringLiteral("audioTiming")).toMap();
        timing.insert(QStringLiteral("start"), AudioTimeline::writeTime({0, 1}));
        s.clip.insert(QStringLiteral("audioTiming"), timing);
    }
    s.clip.insert(QStringLiteral("start"), 0);
    s.clip.insert(QStringLiteral("frames"), int(std::ceil(frames)));
    s.clip.insert(QStringLiteral("track"), 0);
    s.clip.insert(QStringLiteral("lane"), 0);
    s.at = 0;
    s.step = 1;
    s.reverse = false;  // (the mixer has played it backwards already)
    s.stream = -1;
    s.id = QStringLiteral("clip|%1|").arg(double(float(fps)), 0, 'g', 9) + digest(s.clip);
    return s;
}

double Source::position(double frame, int rate, qint64 samples) const {
    const double on48 = at + (frame * 48000. / double(float(fps)) - first) * step;
    const double native = on48 * double(rate) / 48000.;
    return reverse ? double(samples) - native : native;
}

// ── A clip's level ───────────────────────────────────────────────────────────

Gain::Gain(const QVariantMap &e) {
    if (e.isEmpty()) return;
    const bool level = !e.value(QStringLiteral("bypass")).toStringList().contains(QLatin1String("level"));
    if (e.contains(QStringLiteral("audioTiming"))) {
        // A precise cut: its level alone.
        m_gain = level ? sound::gain(std::clamp(e.value(QStringLiteral("volume")).toDouble(), -100., 36.)) : 1;
        m_unity = m_gain == 1;
        m_frames = -1;
        return;
    }
    m_mixIn = e.value(QStringLiteral("mixIn")).toInt();
    m_mixOut = e.value(QStringLiteral("mixOut")).toInt();
    m_start = e.value(QStringLiteral("start")).toDouble() - m_mixIn;
    m_frames = e.value(QStringLiteral("frames")).toDouble() + m_mixIn;
    m_gain = level ? sound::gain(std::clamp(e.value(QStringLiteral("volume")).toDouble(), -60., 12.)) : 1;
    if (level)
        for (const QVariant &v : e.value(QStringLiteral("volumeKf")).toList()) {
            const QVariantMap m = v.toMap();
            const QVariantList pair = v.toList();
            if (!m.isEmpty()) m_keys.push_back({m.value(QStringLiteral("frame")).toDouble(), std::clamp(m.value(QStringLiteral("db")).toDouble(), -60., 12.)});
            else if (pair.size() == 2) m_keys.push_back({pair[0].toDouble(), std::clamp(pair[1].toDouble(), -60., 12.)});
        }
    std::sort(m_keys.begin(), m_keys.end());
    m_fadeIn = e.value(QStringLiteral("fadeIn")).toInt();
    m_fadeOut = e.value(QStringLiteral("fadeOut")).toInt();
    m_shape = e.value(QStringLiteral("audioCurve")).toString();
    m_equalPowerOut = m_shape == QLatin1String("equalPower");
    m_unity = m_gain == 1 && m_keys.empty() && m_fadeIn <= 0 && m_fadeOut <= 0 && m_mixIn <= 0 && m_mixOut <= 0;
}

// (sound::Mixer::render's own arithmetic, a sample at a time there.)
double Gain::at(double timelineFrame) const {
    if (m_unity) return 1;
    if (m_frames < 0) return m_gain;
    const double frame = timelineFrame - m_start;
    double g = m_gain;
    if (!m_keys.empty()) {
        double db = m_keys.back().second;
        if (frame <= m_keys.front().first) db = m_keys.front().second;
        else if (frame < m_keys.back().first)
            for (size_t i = 1; i < m_keys.size(); ++i)
                if (frame <= m_keys[i].first) {
                    const auto &a = m_keys[i - 1], &b = m_keys[i];
                    const double t = b.first > a.first ? (frame - a.first) / (b.first - a.first) : 1;
                    db = a.second + (b.second - a.second) * t;
                    break;
                }
        g = sound::gain(db);
    }
    const int span = m_shape.isEmpty() ? 0 : 1;
    if (m_fadeIn > 0 && frame < m_fadeIn) g *= sound::fadeGain(m_shape, std::clamp(frame / std::max(1, m_fadeIn - span), 0., 1.));
    if (m_fadeOut > 0 && frame > m_frames - m_fadeOut)
        g *= sound::fadeGain(m_shape, std::clamp(1 - (frame - (m_frames - m_fadeOut)) / std::max(1, m_fadeOut - span), 0., 1.));
    // The crossfade into it rises along a line (the curve of the clip before
    // it is that clip's to say, and the picture of this one does not know it).
    if (m_mixIn > 0 && frame < m_mixIn) g *= std::clamp(frame / m_mixIn, 0., 1.);
    if (m_mixOut > 0 && frame > m_frames - m_mixOut) {
        const double m = std::clamp((frame - (m_frames - m_mixOut)) / m_mixOut, 0., 1.);
        g *= m_equalPowerOut ? std::cos(m * M_PI / 2) : 1 - m;
    }
    return g;
}

// ── The store ────────────────────────────────────────────────────────────────

Store *Store::instance() {
    static Store *store = [] {
        auto *made = new Store;
        // Its signals leave from the application's thread, wherever it was first asked for.
        if (QCoreApplication::instance()) {
            made->moveToThread(QCoreApplication::instance()->thread());
            made->setParent(nullptr);
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, made, [made] { made->clear(); });
        }
        return made;
    }();
    return store;
}

std::shared_ptr<Data> Store::find(const QString &id) const {
    QWriteLocker lock(&m_lock);
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) return nullptr;
    it->used = ++m_clock;
    return it->data;
}

QString Store::mark(const QString &id) const {
    QReadLocker lock(&m_lock);
    return m_entries.value(id).mark;
}

Recipe Store::recipe(const QString &id) const {
    QReadLocker lock(&m_lock);
    return m_entries.value(id).recipe;
}

void Store::put(const QString &id, const QString &mark, const std::shared_ptr<Data> &data, const Recipe &recipe) {
    {
        QWriteLocker lock(&m_lock);
        m_entries.insert(id, {mark, data, ++m_clock, recipe});
        trim();
    }
    touch(id);
}

void Store::touch(const QString &id) {
    QMetaObject::invokeMethod(this, [this, id] { emit changed(id); }, thread() == QThread::currentThread() ? Qt::DirectConnection : Qt::QueuedConnection);
}

void Store::setSpare(qint64 values) {
    QWriteLocker lock(&m_lock);
    m_spare = qMax<qint64>(0, values);
    trim();
}

void Store::clear() {
    QWriteLocker lock(&m_lock);
    m_entries.clear();
}

// The sounds nothing but the store holds, least lately read first, go until
// what is left of them is within the spare allowance.
void Store::trim() {
    qint64 spare = 0;
    QList<QPair<quint64, QString>> idle;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (it->data.use_count() == 1) {
            spare += it->data->bytes() / 6;
            idle.append({it->used, it.key()});
        }
    std::sort(idle.begin(), idle.end());
    for (int i = 0; i < idle.size() && spare > m_spare; ++i) {
        // (The one just put is its maker's still, and is never here.)
        spare -= m_entries.value(idle.at(i).second).data->bytes() / 6;
        m_entries.remove(idle.at(i).second);
    }
}

}  // namespace WavePeaks
