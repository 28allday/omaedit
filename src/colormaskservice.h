#pragma once
#include <QImage>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <memory>

// Provider-neutral local process protocol. Invoked only by Generate.
class ColorMaskService : public QObject {
    Q_OBJECT
  public:
    explicit ColorMaskService(QObject *parent = nullptr);
    ~ColorMaskService() override;
    bool busy() const { return bool(m_directory); }
    void start(const QImage &image, const QVariantMap &settings, const QString &kind, const QString &prompt);
    void cancel();
  signals:
    void progress(int percent, const QString &message);
    void finished(const QString &path, const QString &error);

  private:
    QProcess m_process;
    QTimer m_timeout;
    std::unique_ptr<QTemporaryDir> m_directory;
    QSize m_imageSize;
    QByteArray m_output;
    bool m_cancelled = false;
    QString m_failure;
    void complete(int exitCode, QProcess::ExitStatus status);
};
