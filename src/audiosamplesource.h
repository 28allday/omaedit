#pragma once
#include <functional>
#include <QString>
#include <QVector>
#include <memory>

namespace AudioSampleSource {
struct Options {
    bool reverse = false;
    int streamIndex = -1; // absolute container stream index; -1 chooses default sound
    int videoStreamIndex = -1; // picture stream defining time zero; -1 chooses default picture
    // Output channel -> source channel, or -1 for silence. Empty uses standard
    // layout conversion. A mapped source must be read with this channel count.
    QVector<int> channels;
    // Zero sets no limit. A reader keeps a few seconds of converted samples and,
    // for AAC, two decoded stretches of 16 seconds, however long the recording
    // is; a read that would need more than a finite limit fails instead.
    qint64 cacheByteLimit = 0;
};
// The same decoder used by playback, without video-frame seek quantisation.
// Positions and counts are in samples at the requested output rate. Outside the
// common source interval reads return silence. RenderScope supplies cancellation.
class Reader {
public:
    static std::unique_ptr<Reader> open(const QString &path, const Options &options,
                                      QString *error = nullptr);
    ~Reader();
    int sampleRate() const;
    int channelCount() const;
    qint64 sampleCount(int frequency) const;
    qint64 cacheBytes() const;
    QVector<float> read(qint64 first, int count, int frequency, int channels,
                        QString *error = nullptr);
private:
    struct State;
    explicit Reader(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};
// Runs before each read, on the reading thread, with the source's path and the
// read's first sample at its rate: tests
// fail or cancel a read through it (RenderScope), as a broken decoder would.
// None by default.
void setReadHook(std::function<void(const QString &path, qint64 first, int frequency)> hook);
// How many readers this process has opened so far (tests count them).
quint64 openCount();
}
