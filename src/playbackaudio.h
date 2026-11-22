#pragma once
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

// The playback queue owns sample order; the output only accepts interleaved PCM
// and reports its queued bytes. This boundary also permits a deterministic
// output clock when testing transport without opening a physical audio device.
class PlaybackAudioOutput {
public:
    virtual ~PlaybackAudioOutput() = default;
    virtual bool open(const QString &device, int frequency, int channels,
                      int bufferBytes, double volume) = 0;
    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual void reset() = 0;
    // Freeze consumption and return processed PCM bytes since the last reset.
    // reset() starts a fresh stream after the caller has parked its cursor.
    virtual qint64 suspend() = 0;
    virtual qint64 writableBytes() const = 0;
    virtual qint64 bufferedBytes() const = 0;
    virtual qint64 write(const char *data, qint64 size) = 0;
    qint64 write(const QByteArray &pcm) { return write(pcm.constData(), pcm.size()); }
    virtual void setVolume(double volume) = 0;
    // True when every call may come from one thread that is not the one the
    // output was made on (the feeder's, see FedAudioOutput). An output that
    // says no is fed by the playback pump itself, as all were before.
    virtual bool feederSafe() const { return false; }
    // True when the device has played everything it was given and is waiting
    // for more.
    virtual bool starved() const { return bufferedBytes() == 0; }
    static std::unique_ptr<PlaybackAudioOutput> create();
};

class QObject;
class QThread;
class QTimer;

// A device fed from a thread of its own, so that the thread that makes the
// sound may be held (an edit rebuilding the timeline, a slow picture) without
// the device going hungry.
//
// Who owns what:
//  - The playback thread (PlayerWorker's) calls everything declared here. It
//    appends sound to the ring with write(), which never waits for the device.
//  - The feeder thread owns the device outright: it is opened, written, reset,
//    suspended and closed there and nowhere else (a QAudioSink belongs to the
//    thread that made it). Every 4 ms it moves sound from the ring into the
//    device, up to the lead asked for.
//  - Hand-overs: the ring and the counters beside it are guarded by m_ring
//    (held only to copy bytes, never across a device call); open, close,
//    reset, suspend, hold, release, truncate and played are calls queued for
//    the feeder thread under m_calls and waited for there (onFeeder), so each
//    is whole with respect to the feeding.
//
// Stream positions are bytes since the last reset().
class FedAudioOutput final : public PlaybackAudioOutput {
public:
    explicit FedAudioOutput(std::unique_ptr<PlaybackAudioOutput> device);
    ~FedAudioOutput() override;
    bool open(const QString &device, int frequency, int channels, int bufferBytes, double volume) override;
    bool isOpen() const override { return m_open.load(); }
    void close() override;
    void reset() override;      // drops the ring and what the device holds; no longer held
    qint64 suspend() override;  // stops the feed and the device; the bytes the device played
    qint64 writableBytes() const override;
    // The ring and what the device held when last fed (at most 4 ms ago).
    qint64 bufferedBytes() const override;
    qint64 write(const char *data, qint64 size) override;
    using PlaybackAudioOutput::write;
    void setVolume(double volume) override;

    // Sound is taken into the ring but none reaches the device until release():
    // sound made ahead of its first picture waits for it.
    void hold();
    void release();
    // How full the device is kept, in bytes (0: as full as it takes). What it
    // holds can no longer be taken back, so this is how soon a change is heard.
    void setLead(qint64 bytes);
    // The bytes the device has played, asked of the device now.
    qint64 played();
    // Drops the ring's sound from the first of `starts` (stream positions,
    // ascending) that the device has not been given any of; that position, or
    // -1 when nothing was dropped.
    qint64 truncate(const QList<qint64> &starts);
    // How many times the device was found starved with the ring empty too,
    // after it had been given sound: it ran dry (or the sound came to its end).
    quint64 dryTimes() const { return m_dry.load(); }

private:
    void start();
    void feed();      // feeder thread
    void runCalls();  // feeder thread
    void onFeeder(std::function<void()> call);
    std::unique_ptr<PlaybackAudioOutput> m_device;  // feeder thread, once started
    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;  // lives on the feeder thread, with the three timers
    QTimer *m_timer = nullptr;     // every 4 ms: feed()
    QTimer *m_feedNow = nullptr;   // started from the playback thread: feed() at once
    QTimer *m_wake = nullptr;      // started from the playback thread: runCalls()
    std::mutex m_calls;            // guards the three below
    std::condition_variable m_called;
    std::deque<std::function<void()>> m_pending;  // calls for the feeder thread, in order
    quint64 m_posted = 0, m_ran = 0;
    mutable QMutex m_ring;         // guards the five below
    QByteArray m_bytes;            // sound not yet given to the device
    qint64 m_written = 0;          // given to the device
    qint64 m_deviceHeld = 0;       // what the device held when last fed
    qint64 m_lead = 0;
    bool m_dryNow = false;
    // Feeder thread only.
    bool m_held = false, m_suspended = false;
    int m_unit = 4;
    std::atomic_bool m_open{false};
    std::atomic<quint64> m_dry{0};
};
