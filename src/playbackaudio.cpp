#include "playbackaudio.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <limits>

namespace {
class DeviceOutput final : public PlaybackAudioOutput {
public:
    bool open(const QString &id, int frequency, int channels, int bytes, double volume) override {
        close();
        auto device=QMediaDevices::defaultAudioOutput();
        for (const auto &candidate : QMediaDevices::audioOutputs())
            if (!id.isEmpty() && QString::fromUtf8(candidate.id())==id) { device=candidate; break; }
        QAudioFormat format;
        format.setSampleRate(frequency); format.setChannelCount(channels); format.setSampleFormat(QAudioFormat::Int16);
        m_sink=std::make_unique<QAudioSink>(device,format);
        m_sink->setVolume(volume); m_sink->setBufferSize(bytes);
        m_device=m_sink->start();
        return m_device!=nullptr;
    }
    bool isOpen() const override { return m_sink && m_device; }
    void close() override {
        if (m_sink) m_sink->stop();
        m_device=nullptr; m_sink.reset();
    }
    void reset() override {
        if (!isOpen()) return;
        m_sink->reset(); m_device=m_sink->start();
    }
    qint64 suspend() override {
        if (!isOpen()) return 0;
        m_sink->suspend();
        // bytesFree() is not valid in SuspendedState. The processed-data
        // clock excludes underrun silence; round its microseconds back to
        // complete sample frames on the output device's clock.
        const auto format=m_sink->format();
        const qint64 frames=qint64((__int128(m_sink->processedUSecs())*format.sampleRate()+500000)/1000000);
        return frames*format.bytesPerFrame();
    }
    qint64 writableBytes() const override { return isOpen()?m_sink->bytesFree():0; }
    qint64 bufferedBytes() const override { return isOpen()?qMax<qint64>(0,m_sink->bufferSize()-m_sink->bytesFree()):0; }
    qint64 write(const char *data, qint64 size) override { return isOpen()?m_device->write(data,size):-1; }
    void setVolume(double volume) override { if(m_sink)m_sink->setVolume(volume); }
    // The sink is made in open(), so it belongs to whichever thread opens it.
    bool feederSafe() const override { return true; }
    // The sink says so itself (idle: nothing left to play); its buffer figure
    // alone may read empty while the sound server still holds a period.
    bool starved() const override { return isOpen() && m_sink->state()==QAudio::IdleState && bufferedBytes()==0; }
private:
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_device=nullptr;
};
}
std::unique_ptr<PlaybackAudioOutput> PlaybackAudioOutput::create() { return std::make_unique<DeviceOutput>(); }

FedAudioOutput::FedAudioOutput(std::unique_ptr<PlaybackAudioOutput> device) : m_device(std::move(device)) {}

FedAudioOutput::~FedAudioOutput() {
    if (!m_thread) return;
    onFeeder([this] { m_timer->stop(); m_feedNow->stop(); m_wake->stop(); m_device->close(); });
    m_thread->quit();
    m_thread->wait();
    delete m_context;  // and the timers with it
    delete m_thread;
}

// The feeder thread starts with the first open: a worker that never plays
// sound (most tests, a headless render) has none.
void FedAudioOutput::start() {
    if (m_thread) return;
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("sound feeder"));
    m_context = new QObject;
    m_timer = new QTimer(m_context);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(4);
    m_feedNow = new QTimer(m_context);
    m_wake = new QTimer(m_context);
    for (QTimer *once : {m_feedNow, m_wake}) { once->setSingleShot(true); once->setInterval(0); }
    QObject::connect(m_timer, &QTimer::timeout, m_context, [this] { feed(); });
    QObject::connect(m_feedNow, &QTimer::timeout, m_context, [this] { feed(); });
    QObject::connect(m_wake, &QTimer::timeout, m_context, [this] { runCalls(); });
    m_context->moveToThread(m_thread);  // the timers go with it
    m_thread->start(QThread::HighPriority);
    QMetaObject::invokeMethod(m_timer, "start", Qt::QueuedConnection);
}

// One call on the feeder thread, waited for. The call and whatever it captures
// are handed over under m_calls, and so is the word that it has run.
void FedAudioOutput::onFeeder(std::function<void()> call) {
    quint64 ticket = 0;
    {
        const std::lock_guard<std::mutex> lock(m_calls);
        m_pending.push_back(std::move(call));
        ticket = ++m_posted;
    }
    QMetaObject::invokeMethod(m_wake, "start", Qt::QueuedConnection);
    std::unique_lock<std::mutex> lock(m_calls);
    m_called.wait(lock, [&] { return m_ran >= ticket; });
}

void FedAudioOutput::runCalls() {
    for (;;) {
        std::function<void()> call;
        {
            const std::lock_guard<std::mutex> lock(m_calls);
            if (m_pending.empty()) return;
            call = std::move(m_pending.front());
            m_pending.pop_front();
        }
        call();
        {
            const std::lock_guard<std::mutex> lock(m_calls);
            ++m_ran;
        }
        m_called.notify_all();
    }
}

bool FedAudioOutput::open(const QString &device, int frequency, int channels, int bufferBytes, double volume) {
    start();
    bool opened = false;
    onFeeder([&] {
        opened = m_device->open(device, frequency, channels, bufferBytes, volume);
        m_unit = qMax(1, channels) * 2;
        m_held = m_suspended = false;
        QMutexLocker lock(&m_ring);
        m_bytes.clear(); m_written = m_deviceHeld = m_lead = 0; m_dryNow = false;
    });
    m_open = opened;
    return opened;
}

void FedAudioOutput::close() {
    m_open = false;
    if (!m_thread) return;
    onFeeder([this] {
        m_device->close();
        QMutexLocker lock(&m_ring);
        m_bytes.clear(); m_written = m_deviceHeld = 0; m_dryNow = false;
    });
}

void FedAudioOutput::reset() {
    if (!m_thread) return;
    onFeeder([this] {
        { QMutexLocker lock(&m_ring); m_bytes.clear(); m_written = m_deviceHeld = m_lead = 0; m_dryNow = false; }
        m_held = m_suspended = false;
        if (m_device->isOpen()) m_device->reset();
    });
}

qint64 FedAudioOutput::suspend() {
    if (!m_thread) return 0;
    qint64 processed = 0;
    onFeeder([&] { m_suspended = true; processed = m_device->suspend(); });
    return processed;
}

qint64 FedAudioOutput::writableBytes() const { return m_open ? std::numeric_limits<qint32>::max() : 0; }

qint64 FedAudioOutput::bufferedBytes() const {
    QMutexLocker lock(&m_ring);
    return m_bytes.size() + m_deviceHeld;
}

qint64 FedAudioOutput::write(const char *data, qint64 size) {
    if (!m_open) return -1;  // closed, or the device refused sound (feed): said as a failed write
    { QMutexLocker lock(&m_ring); m_bytes.append(data, size); }
    // On its way at once, not at the next tick (an audition starts now).
    QMetaObject::invokeMethod(m_feedNow, "start", Qt::QueuedConnection);
    return size;
}

void FedAudioOutput::setVolume(double volume) {
    if (!m_thread) return;
    onFeeder([&] { m_device->setVolume(volume); });
}

void FedAudioOutput::hold() { if (m_thread) onFeeder([this] { m_held = true; }); }
void FedAudioOutput::release() { if (m_thread) onFeeder([this] { m_held = false; feed(); }); }
void FedAudioOutput::setLead(qint64 bytes) { QMutexLocker lock(&m_ring); m_lead = bytes; }

qint64 FedAudioOutput::played() {
    if (!m_thread) return 0;
    qint64 out = 0;
    onFeeder([&] {
        const qint64 held = m_device->isOpen() ? m_device->bufferedBytes() : 0;
        QMutexLocker lock(&m_ring);
        out = qMax<qint64>(0, m_written - held);
    });
    return out;
}

qint64 FedAudioOutput::truncate(const QList<qint64> &starts) {
    if (!m_thread) return -1;
    qint64 cut = -1;
    onFeeder([&] {
        QMutexLocker lock(&m_ring);
        for (const qint64 start : starts) {
            if (start < m_written) continue;
            if (start < m_written + m_bytes.size()) { cut = start; m_bytes.truncate(start - m_written); }
            break;
        }
    });
    return cut;
}

// Feeder thread: the ring into the device, as far as the lead allows.
void FedAudioOutput::feed() {
    if (!m_device->isOpen() || m_suspended) return;
    while (!m_held) {
        const qint64 held = m_device->bufferedBytes();
        qint64 room = m_device->writableBytes();
        QByteArray chunk;
        {
            QMutexLocker lock(&m_ring);
            if (m_lead > 0) room = qMin(room, m_lead - held);
            room = qMin<qint64>(room, m_bytes.size()) / m_unit * m_unit;
            if (room > 0) chunk = m_bytes.left(room);
        }
        if (chunk.isEmpty()) break;
        // Only this thread takes from the ring's front, so what was copied is
        // still there when the device has had it.
        const qint64 written = m_device->write(chunk.constData(), chunk.size());
        if (written < 0) {
            m_device->close();
            QMutexLocker lock(&m_ring);
            m_bytes.clear(); m_deviceHeld = 0;
            m_open = false;
            return;
        }
        if (written == 0) break;
        QMutexLocker lock(&m_ring);
        m_bytes.remove(0, written);
        m_written += written;
        if (written < chunk.size()) break;
    }
    const qint64 held = m_device->bufferedBytes();
    const bool starved = m_device->starved();
    QMutexLocker lock(&m_ring);
    m_deviceHeld = held;
    const bool dry = !m_held && m_written > 0 && m_bytes.isEmpty() && starved;
    if (dry && !m_dryNow) ++m_dry;
    m_dryNow = dry;
}
