#pragma once
#include <QObject>
#include <QVariantMap>
#include <QStringList>
#include <QSet>
#include <functional>
#include <memory>
class QTemporaryDir;

namespace SuiteHardware {
// All probes are child processes with deadlines and bounded diagnostic output.
// Each app compiles this module itself; no suite daemon or sibling is needed.
class HardwareProbe : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString ffmpeg;
        QStringList renderNodes;
        bool discoverRenderNodes = true;
        int timeoutMs = 8000;
    };
    explicit HardwareProbe(QObject *parent = nullptr);
    ~HardwareProbe() override;
    QVariantMap snapshot() const { return m_state; }
    bool encodersBusy() const { return m_encoderBusy; }
    bool gpuBusy() const { return m_gpuBusy; }
    void probeEncoders();
    void probeEncoders(const Options &options);
    void probeGpu(const QString &executable = {}, int timeoutMs = 8000);
    static QStringList renderNodes();
    // Compiled codec listings are not evidence of successful hardware decoding.
    static QSet<QString> codecNames(const QByteArray &listing);
signals:
    void changed();
private:
    struct Result { bool ok = false; QByteArray output; QString error; };
    struct Job { QString encoder; QString device; };
    void run(const QString &program, const QStringList &args, int timeoutMs,
             const std::function<void(Result)> &callback);
    void nextEncoder();
    void probeDecoders();
    void nextDecoder();
    void finishEncoders();
    void publish();
    QVariantMap m_state;
    QVariantMap m_encoders;
    QVariantList m_details;
    QList<Job> m_jobs;
    QList<Job> m_decodeJobs;
    std::unique_ptr<QTemporaryDir> m_decodeDir;
    Options m_options;
    bool m_encoderBusy = false;
    bool m_gpuBusy = false;
};
}
