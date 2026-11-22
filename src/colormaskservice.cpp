#include "colormaskservice.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

ColorMaskService::ColorMaskService(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(120000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_failure = QStringLiteral("The mask provider timed out after two minutes.");
        m_process.kill();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output += m_process.readAllStandardOutput();
        while (m_output.contains('\n')) {
            const int end = m_output.indexOf('\n');
            const auto status = QJsonDocument::fromJson(m_output.left(end)).toVariant().toMap();
            m_output.remove(0, end + 1);
            if (status.contains("progress"))
                emit progress(qBound(0, status.value("progress").toInt(), 99),
                              status.value("message").toString().left(240));
        }
        if (m_output.size() > 65536)
            m_output = m_output.right(65536);
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_failure = QStringLiteral("Could not start the mask provider: %1").arg(m_process.errorString());
            complete(-1, QProcess::CrashExit);
        }
    });
    connect(&m_process, &QProcess::finished, this, &ColorMaskService::complete);
}
ColorMaskService::~ColorMaskService() {
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}
void ColorMaskService::start(const QImage &image, const QVariantMap &settings, const QString &kind,
                             const QString &prompt) {
    if (busy()) {
        emit progress(0, QStringLiteral("A mask request is already running."));
        return;
    }
    const QString command = settings.value("command").toString();
    if (command.isEmpty() || !QFileInfo(command).isExecutable()) {
        emit finished({}, QStringLiteral("Configure an executable mask provider first."));
        return;
    }
    if (image.isNull() || (kind != "face" && kind != "object")) {
        emit finished({}, QStringLiteral("Choose a face or object mask and an online video frame."));
        return;
    }
    m_directory = std::make_unique<QTemporaryDir>();
    m_imageSize = image.size();
    m_output.clear();
    m_cancelled = false;
    m_failure.clear();
    const QString input = m_directory->filePath("input.png"), output = m_directory->filePath("matte.png"),
                  request = m_directory->filePath("request.json");
    const QVariantMap data{{"protocol", "omaedit.mask.v1"},
                           {"input", input},
                           {"output", output},
                           {"kind", kind},
                           {"prompt", prompt.left(4096)},
                           {"width", image.width()},
                           {"height", image.height()},
                           {"frame", settings.value("frame")}};
    QFile file(request);
    if (!m_directory->isValid() || !image.save(input) || !file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument::fromVariant(data).toJson()) < 0) {
        m_failure = QStringLiteral("Could not prepare the mask request.");
        complete(-1, QProcess::CrashExit);
        return;
    }
    file.close();
    QStringList arguments =
        QProcess::splitCommand(settings.value("arguments", "--request {request} --output {output}").toString());
    for (QString &arg : arguments) {
        arg.replace("{request}", request);
        arg.replace("{input}", input);
        arg.replace("{output}", output);
    }
    m_process.setWorkingDirectory(m_directory->path());
    emit progress(0, QStringLiteral("Generating %1 mask…").arg(kind));
    m_timeout.start();
    m_process.start(command, arguments);
}
void ColorMaskService::cancel() {
    if (!busy())
        return;
    m_cancelled = true;
    if (m_process.state() == QProcess::NotRunning)
        complete(-1, QProcess::CrashExit);
    else
        m_process.kill();
}
void ColorMaskService::complete(int code, QProcess::ExitStatus status) {
    if (!m_directory)
        return;
    m_timeout.stop();
    QString result, error = m_failure;
    if (m_cancelled)
        error = QStringLiteral("Mask request cancelled.");
    else if (error.isEmpty() && (status != QProcess::NormalExit || code != 0))
        error = QStringLiteral("The mask provider failed (exit %1).").arg(code);
    if (error.isEmpty()) {
        const QString output = m_directory->filePath("matte.png");
        QImageReader reader(output);
        const QSize size = reader.size();
        if (reader.format().toLower() != "png" || !QFileInfo::exists(output) ||
            QFileInfo(output).size() > 128 * 1024 * 1024 || size != m_imageSize ||
            qint64(size.width()) * size.height() > 32 * 1024 * 1024)
            error = QStringLiteral("The provider must return a PNG matte with the input frame's dimensions.");
        else {
            const QImage matte = reader.read();
            if (matte.isNull())
                error = QStringLiteral("Could not read the provider's matte.");
            else {
                QFile file(output);
                if (!file.open(QIODevice::ReadOnly)) {
                    m_directory.reset();
                    emit finished({}, QStringLiteral("Could not read the returned matte file."));
                    return;
                }
                const QByteArray bytes = file.readAll();
                const QString directory =
                    QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/ColorMasks";
                result = directory + "/" +
                         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) +
                         ".png";
                QSaveFile saved(result);
                if (!QDir().mkpath(directory) || !saved.open(QIODevice::WriteOnly) ||
                    saved.write(bytes) != bytes.size() || !saved.commit()) {
                    result.clear();
                    error = QStringLiteral("Could not store the generated matte.");
                }
            }
        }
    }
    m_directory.reset();
    emit finished(result, error);
}
