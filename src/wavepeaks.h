#pragma once

// A sound's waveform as editors draw one: for each block of samples its
// lowest and highest sample and its RMS, a channel at a time, at several
// sizes of block (256 samples, then four times as many, and so on), so that
// any zoom reads about one to a few blocks for each pixel. Made once for a
// source in the background (PlayerWorker's peak thread), kept on disk between
// runs, and read by every picture of that sound (WaveformItem).
//
// Threads: a Data is written by its one maker and read by any painter; its
// lock covers both. Everything else here is plain values.

#include <QByteArray>
#include <QObject>
#include <QReadWriteLock>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <memory>
#include <vector>

namespace WavePeaks {

constexpr int kBlock = 256;   // samples in a block of the finest level
constexpr int kFactor = 4;    // each level's blocks are this many of the one below
constexpr int kLevels = 8;    // the coarsest block: 256 × 4^7 samples (87 s at 48 kHz)
constexpr float kScale = 32767.f;

// The lowest and highest sample of a stretch and its RMS (-1..1, 0..1).
struct Span {
    float lo = 0, hi = 0, rms = 0;
    bool any = false;  // false: nothing is known of the stretch yet
};

class Data {
public:
    Data(int rate, int channels, qint64 samples);
    int rate() const { return m_rate; }
    int channels() const { return m_channels; }
    // How long the source is, and how much of it has peaks so far.
    qint64 samples() const { return m_samples; }
    qint64 made() const;
    bool complete() const;
    // Samples [first, last) of one channel, or of all of them as one
    // (channel < 0: the lowest and highest of any, and their mean power).
    // `level` < 0 picks the coarsest that still gives a block or more to the
    // stretch; a level's blocks not made yet are read from the level below.
    Span span(int channel, double first, double last, int level = -1) const;
    // A level's blocks: how many, and one of them.
    int levelCount() const { return kLevels; }
    static int blockSize(int level);
    qint64 blocks(int level) const;
    Span block(int level, int channel, qint64 index) const;
    // What it holds in memory, and as a file.
    qint64 bytes() const;

    // Its maker's: `frames` more samples (interleaved, this many channels)
    // after those it has, and the end of the source. A Data read back from a
    // file is complete.
    void append(const float *interleaved, qint64 frames);
    void finish();

    QByteArray packed() const;
    static std::shared_ptr<Data> unpacked(const QByteArray &bytes);

private:
    struct Level {
        std::vector<qint16> lo, hi;
        std::vector<quint16> rms;
    };
    struct Open {  // a block being filled, a channel
        float lo = 0, hi = 0;
        double squares = 0;
        qint64 count = 0;
    };
    struct Sum {  // stretches read one after another, as one (in the units the blocks are kept in)
        qint16 lo = 32767, hi = -32767;
        double squares = 0, count = 0;
    };
    void close(int level);
    void walk(int level, int channel, qint64 from, qint64 to, qint64 firstSample, qint64 lastSample, Sum &sum) const;
    Span read(int level, int channel, qint64 from, qint64 to, qint64 firstSample, qint64 lastSample) const;
    int m_rate, m_channels;
    qint64 m_samples;
    qint64 m_made = 0;
    bool m_complete = false;
    Level m_levels[kLevels];
    std::vector<Open> m_open;  // kLevels × channels
    mutable QReadWriteLock m_lock;
};

// What a clip's waveform is read from, and where in it each of the clip's
// frames lies. Worked out from the clip's entry alone, the same on the peak
// thread (which makes the peaks) and in the picture (which reads them).
//
// A clip that plays a file as it is (forwards or backwards, at any constant
// speed) reads that file's peaks, shared by every clip of the file. Any other
// (a speed ramp or a speed map, a compound, a nested timeline, a tone, a clip
// with an EQ, a channel map or normalising) has peaks of its own sound as the
// mixer plays it, its level and fades left out: those are drawn on top,
// whichever kind it is (Gain).
struct Source {
    enum Kind { None, File, Clip } kind = None;
    QString id;         // names the sound: the store's key
    QString path;       // File: what is read
    int stream = -1;
    bool reverse = false;
    QVariantMap clip;   // Clip: the clip alone from frame 0, level and fades off
    // The source's sample (at 48 kHz, on the reader's own clock: the reversed
    // one for a reversed clip) at timeline frame f: at + (f × 48000 / fps − first) × step.
    double first = 0, at = 0, step = 1;
    double fps = 25;
    static Source of(const QVariantMap &entry, double fps);
    // A file on its own (a bin clip, the source viewer): frame 0 is its start.
    static Source ofFile(const QString &path, double fps);
    // Timeline frame → the position in a Data of `rate` and `samples` (the
    // forward file's, even for a reversed clip: the stretch [a, b) of a
    // reversed clip comes back as [b', a') with b' < a').
    double position(double frame, int rate, qint64 samples) const;
};

// A clip's level over its length as the mixer applies it: its volume or its
// volume keyframes, its fades and the crossfades at its two ends.
class Gain {
public:
    explicit Gain(const QVariantMap &entry = {});
    // At a timeline frame (as Source: absolute).
    double at(double frame) const;
    bool unity() const { return m_unity; }
private:
    bool m_unity = true;
    double m_start = 0, m_frames = 0, m_gain = 1;
    std::vector<std::pair<double, double>> m_keys;
    int m_fadeIn = 0, m_fadeOut = 0, m_mixIn = 0, m_mixOut = 0;
    QString m_shape;
    bool m_equalPowerOut = false;
};

// How a sound's own samples are read again, for a picture zoomed in past the
// finest peaks: what its maker read them with.
struct Recipe {
    Source::Kind kind = Source::None;
    QString path;        // a file's, as it was opened (a proxy when the timeline plays one)
    int stream = -1;
    QVariantMap clip, sources;  // a clip's own sound: the clip, and the project's media settings
    int fpsNum = 25, fpsDen = 1;
};

// Every sound's peaks in memory, by Source::id. Any thread may call; changed()
// is always sent from the store's own thread (the window's).
class Store : public QObject {
    Q_OBJECT
public:
    static Store *instance();
    std::shared_ptr<Data> find(const QString &id) const;
    // What says exactly which sound the peaks under `id` are of (the file as
    // it is on disk, or all a clip's sound is made from).
    QString mark(const QString &id) const;
    void put(const QString &id, const QString &mark, const std::shared_ptr<Data> &data, const Recipe &recipe = {});
    Recipe recipe(const QString &id) const;
    // More of a sound's peaks are made.
    void touch(const QString &id);
    // Peak values kept for sounds no picture holds (Preferences ▸ Storage);
    // those in use are always kept.
    void setSpare(qint64 values);
    void clear();
signals:
    void changed(const QString &id);
private:
    struct Entry { QString mark; std::shared_ptr<Data> data; quint64 used = 0; Recipe recipe; };
    void trim();
    mutable QReadWriteLock m_lock;
    mutable QHash<QString, Entry> m_entries;
    mutable quint64 m_clock = 0;
    qint64 m_spare = 8000;
};

}  // namespace WavePeaks
