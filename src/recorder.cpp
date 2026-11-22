#include "recorder.h"
#include "audioengine.h"
#include <QAudioSink>

#include <QAudioDevice>
#include <QAudioSource>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QMediaDevices>
#include <QtEndian>

#include <cmath>

Recorder::Recorder(QObject *parent) : QObject(parent) {
    m_format.setSampleRate(48000);
    m_format.setChannelCount(2);
    m_format.setSampleFormat(QAudioFormat::Int16);
    // Device hot-plug: keep the input list live.
    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &Recorder::inputsChanged);
}

Recorder::~Recorder() {
    if (m_recording)
        stop();
    closeSource();
}

QVariantList Recorder::inputs() const {
    QVariantList out;
    const QAudioDevice def = QMediaDevices::defaultAudioInput();
    for (const QAudioDevice &d : QMediaDevices::audioInputs()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromUtf8(d.id()));
        m.insert(QStringLiteral("name"), d.description());
        m.insert(QStringLiteral("default"), d.id() == def.id());
        out.append(m);
    }
    return out;
}

bool Recorder::available() const {
    return !QMediaDevices::audioInputs().isEmpty();
}

void Recorder::refreshInputs() { emit inputsChanged(); }

void Recorder::setInputDevice(const QString &id) {
    if (id == m_inputId)
        return;
    m_inputId = id;
    emit inputDeviceChanged();
    if (m_source && !m_recording) { // re-open the monitor on the new device
        closeSource();
        if (m_monitoring && openSource())
            openMonitor();
    }
}

void Recorder::setMonitoring(bool on) {
    if (on == m_monitoring)
        return;
    m_monitoring = on;
    emit monitoringChanged();
    if (on) {
        if (openSource()) openMonitor();
    }
    else if (!m_recording)
        closeSource();
    if (!on) closeMonitor();
    if (!on && !m_recording) {
        m_level = -100.0;
        emit levelChanged();
    }
}

double Recorder::seconds() const {
    return m_recording ? double(m_bytes) / double(m_format.bytesForDuration(1000000)) : 0.0;
}

void Recorder::setError(const QString &e) {
    if (e == m_error)
        return;
    m_error = e;
    emit errorChanged();
}

bool Recorder::openSource() {
    if (m_source)
        return m_device != nullptr;
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!m_inputId.isEmpty())
        for (const QAudioDevice &d : QMediaDevices::audioInputs())
            if (QString::fromUtf8(d.id()) == m_inputId)
                device = d;
    if (device.isNull()) {
        setError(QStringLiteral("No audio input device."));
        return false;
    }
    QAudioFormat format = m_format;
    if (!device.isFormatSupported(format))
        format = device.preferredFormat();
    if (format.sampleFormat() != QAudioFormat::Int16) {
        format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) {
            setError(QStringLiteral("%1 cannot record 16-bit PCM.").arg(device.description()));
            return false;
        }
    }
    m_format = format;
    m_source = new QAudioSource(device, m_format, this);
    m_source->setBufferSize(m_format.bytesForDuration(200000));
    m_device = m_source->start();
    if (!m_device) {
        setError(QStringLiteral("Could not open %1.").arg(device.description()));
        delete m_source;
        m_source = nullptr;
        return false;
    }
    connect(m_device, &QIODevice::readyRead, this, &Recorder::onReadyRead);
    setError(QString());
    return true;
}

void Recorder::closeSource() {
    closeMonitor();
    if (!m_source)
        return;
    m_source->stop();
    delete m_source;
    m_source = nullptr;
    m_device = nullptr;
}

QList<float> Recorder::peaksOf(const QByteArray &s16, int channels) {
    QList<float> out;
    const auto *samples = reinterpret_cast<const qint16 *>(s16.constData());
    const int frames = channels > 0 ? s16.size() / (channels * int(sizeof(qint16))) : 0;
    for (int c = 0; c < channels; ++c) {
        double peak = 0.0;
        for (int i = 0; i < frames; ++i)
            peak = qMax(peak, qAbs(samples[i * channels + c]) / 32768.0);
        out.append(peak > 0.0 ? float(qMax(-100.0, 20.0 * std::log10(peak))) : -100.0f);
    }
    return out;
}

void Recorder::onReadyRead() {
    if (!m_device)
        return;
    QByteArray chunk = m_device->readAll();
    if (chunk.isEmpty())
        return;
    if (m_inputChannel != QStringLiteral("both"))
        chunk = AudioEngine::mapChannels(chunk, m_format.channelCount(), m_inputChannel);
    const QList<float> peaks = peaksOf(chunk, m_format.channelCount());
    double level = -100.0;
    for (float p : peaks)
        level = qMax(level, double(p));
    m_level = level;
    if (m_recording && !writeSamples(chunk))
        stop();
    if (m_monitoring && m_monitorDevice) {
        auto output=AudioEngine::mapChannels(chunk,m_format.channelCount(),m_monitorMono ? QStringLiteral("mono") : m_monitorMapping);
        const qint64 bytes=qMin<qint64>(output.size(),m_monitorSink->bytesFree());
        if(bytes>0)m_monitorDevice->write(output.constData(),bytes);
    }
    emit levelChanged();
}

QString Recorder::start(const QString &dir, const QString &prefix) {
    if (m_recording)
        return m_path;
    if (!openSource())
        return QString();
    QDir().mkpath(dir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString base = (prefix.isEmpty() ? QStringLiteral("Recording") : prefix) + QLatin1Char('_') + stamp;
    // The prefix is a word shown to the editor, in their language: whatever
    // it holds, it names one file in `dir`.
    base.replace(QLatin1Char('/'), QLatin1Char('_'));
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".wav");
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir + QLatin1Char('/') + base + QStringLiteral("_%1.wav").arg(n);
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) {
        setError(QStringLiteral("Cannot write %1.").arg(path));
        return QString();
    }
    m_bytes = 0;
    m_writeError.clear();
    m_path = path;
    if (!writeHeader(0) || !m_file.flush()) {
        const auto reason = m_file.errorString();
        m_file.close();
        if (!m_monitoring) closeSource();
        setError(tr("Cannot start recording to %1: %2").arg(path, reason));
        return {};
    }
    setError({});
    m_recording = true;
    m_clock.start();
    emit recordingChanged();
    return path;
}

bool Recorder::writeSamples(const QByteArray &samples) {
    if (samples.isEmpty()) return true;
    // A WAV header counts bytes in 32 bits: the take ends, kept and valid,
    // where the next block would no longer fit (4 GiB; ~46 min of 8 x 96 kHz).
    const qint64 block = qMax(1, m_format.channelCount() * 2);
    const qint64 room = (qint64(0xFFFFFFFFu) - 36 - m_bytes) / block * block;
    const bool full = samples.size() > room;
    const qint64 wanted = full ? room : samples.size();
    const qint64 written = wanted > 0 ? m_file.write(samples.constData(), wanted) : 0;
    m_bytes += qMax<qint64>(0, written);
    if (written == wanted) return !full;
    m_writeError = tr("Could not write all the audio (%1).").arg(m_file.errorString());
    return false;
}

QString Recorder::stop() {
    if (!m_recording)
        return {};
    m_recording = false;
    // Use the same channel mapping and checked write for the final buffer.
    if (m_device && m_writeError.isEmpty())
        writeSamples(AudioEngine::mapChannels(m_device->readAll(), m_format.channelCount(), m_inputChannel));
    if (!m_file.flush() && m_writeError.isEmpty())
        m_writeError = m_file.errorString();
    // A partial write can end halfway through a sample. Keep only complete
    // frames and repair the header where possible, without calling it success.
    m_bytes = qMax<qint64>(0, qMin(m_bytes, m_file.size() - 44));
    m_bytes -= m_bytes % qMax(1, m_format.bytesPerFrame());
    if ((!m_file.resize(44 + m_bytes) || !m_file.seek(0) || !writeHeader(m_bytes) || !m_file.flush()) &&
        m_writeError.isEmpty())
        m_writeError = m_file.errorString();
    m_file.close();
    if (m_file.error() != QFileDevice::NoError && m_writeError.isEmpty())
        m_writeError = m_file.errorString();
    const QString path = m_path;
    const double secs = double(m_bytes) / double(m_format.bytesForDuration(1000000));
    if (!m_monitoring)
        closeSource();
    m_level = -100.0;
    emit recordingChanged();
    emit levelChanged();
    if (!m_writeError.isEmpty()) {
        const auto message = tr("Recording stopped: %1 Any partial recording is kept at %2.").arg(m_writeError, path);
        setError(message);
        emit recordingFailed(path, message);
        return {};
    }
    emit finished(path, secs);
    return path;
}

// Canonical 44-byte PCM WAV header.
bool Recorder::writeHeader(qint64 dataBytes) {
    const quint16 channels = quint16(m_format.channelCount());
    const quint32 rate = quint32(m_format.sampleRate());
    const quint16 bits = 16;
    const quint16 blockAlign = quint16(channels * bits / 8);
    const quint32 byteRate = rate * blockAlign;
    QByteArray h;
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); h.append(b, 4); };
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); h.append(b, 2); };
    h.append("RIFF");
    u32(quint32(36 + dataBytes));
    h.append("WAVE");
    h.append("fmt ");
    u32(16);
    u16(1); // PCM
    u16(channels);
    u32(rate);
    u32(byteRate);
    u16(blockAlign);
    u16(bits);
    h.append("data");
    u32(quint32(dataBytes));
    return m_file.write(h) == h.size();
}

void Recorder::closeMonitor() {
    if(m_monitorSink){m_monitorSink->stop();delete m_monitorSink;}
    m_monitorSink=nullptr;m_monitorDevice=nullptr;
}
void Recorder::openMonitor() {
    closeMonitor();
    auto device=QMediaDevices::defaultAudioOutput();
    for(const auto &d:QMediaDevices::audioOutputs())if(QString::fromUtf8(d.id())==m_monitorOutput)device=d;
    if(device.isNull() || !device.isFormatSupported(m_format)) {setError("The monitoring output cannot play the input format.");return;}
    m_monitorSink=new QAudioSink(device,m_format,this);
    m_monitorSink->setBufferSize(m_format.bytesForDuration(60000));
    m_monitorSink->setVolume(m_monitorMuted?0.:(m_monitorDim?.1:1.));
    m_monitorDevice=m_monitorSink->start();
    if(!m_monitorDevice)setError("Could not start input monitoring.");
}
void Recorder::setMonitorOptions(bool muted,bool dim,bool mono,const QString &output) {
    bool change=output!=m_monitorOutput;
    m_monitorMuted=muted;m_monitorDim=dim;m_monitorMono=mono;m_monitorOutput=output;
    if(change && m_monitoring)openMonitor();
    if(m_monitorSink)m_monitorSink->setVolume(muted?0.:(dim?.1:1.));
}
