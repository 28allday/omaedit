#pragma once

#include <QAudioFormat>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QVariantList>

class QAudioSource;
class QAudioSink;
class QIODevice;

// Audio-page recording: the default (or chosen) input to a 48 kHz 16-bit
// WAV, with a live input level whether or not a take is rolling (input
// monitoring = the meter, not a passthrough). Lives on the GUI thread —
// QAudioSource pull mode is cheap — and reports peaks per buffer.
class Recorder : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool monitoring READ monitoring WRITE setMonitoring NOTIFY monitoringChanged)
    Q_PROPERTY(double level READ level NOTIFY levelChanged)          // dBFS peak, -100 silent
    Q_PROPERTY(double seconds READ seconds NOTIFY levelChanged)
    Q_PROPERTY(QString path READ path NOTIFY recordingChanged)
    Q_PROPERTY(QString inputDevice READ inputDevice WRITE setInputDevice NOTIFY inputDeviceChanged)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY inputsChanged)  // [{id, name, default}]
    Q_PROPERTY(bool available READ available NOTIFY inputsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool recording() const { return m_recording; }
    bool monitoring() const { return m_monitoring; }
    void setMonitoring(bool on);
    void setMonitorOptions(bool muted, bool dim, bool mono, const QString &output);
    void setMonitorMapping(const QString &mode) { m_monitorMapping = mode; }
    // Preferences ▸ Audio ▸ Input channels: "both", "left", "right" — what a
    // stereo input contributes to the recorded (stereo) file.
    void setInputChannel(const QString &mode) { m_inputChannel = mode; }
    double level() const { return m_level; }
    double seconds() const;
    QString path() const { return m_path; }
    QString inputDevice() const { return m_inputId; }
    void setInputDevice(const QString &id);
    QVariantList inputs() const;
    bool available() const;
    QString error() const { return m_error; }
    // Where takes land: <dir>/<prefix>_YYYYMMDD_HHMMSS.wav.
    Q_INVOKABLE QString start(const QString &dir, const QString &prefix);
    Q_INVOKABLE QString stop(); // the finished file ("" when nothing rolled)
    Q_INVOKABLE void refreshInputs();
    static QList<float> peaksOf(const QByteArray &s16, int channels);

signals:
    void recordingChanged();
    void monitoringChanged();
    void levelChanged();
    void inputDeviceChanged();
    void inputsChanged();
    void errorChanged();
    void finished(const QString &path, double seconds);
    void recordingFailed(const QString &path, const QString &message);

private:
    bool openSource();
    void closeSource();
    void onReadyRead();
    friend class OmaEditTest;
    bool writeHeader(qint64 dataBytes);
    bool writeSamples(const QByteArray &samples);
    QString m_writeError;
    void setError(const QString &e);

    QAudioSource *m_source = nullptr;
    QAudioSink *m_monitorSink = nullptr;
    QIODevice *m_monitorDevice = nullptr;
    bool m_monitorMuted=false, m_monitorDim=false, m_monitorMono=false;
    QString m_monitorMapping = QStringLiteral("stereo"), m_inputChannel = QStringLiteral("both");
    QString m_monitorOutput;
    void openMonitor();
    void closeMonitor();
    QIODevice *m_device = nullptr;
    QAudioFormat m_format;
    QFile m_file;
    QString m_path;
    QString m_inputId;
    bool m_recording = false;
    bool m_monitoring = false;
    double m_level = -100.0;
    qint64 m_bytes = 0;
    QElapsedTimer m_clock;
    QString m_error;
};
