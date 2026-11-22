#include "transcriber.h"
#include "engines.h"
#include "storagepreferences.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QHash>
#include <QSet>
#include <QThread>
#ifdef Q_OS_LINUX
#include <sched.h>
#endif

// The speech model that comes with the package: whisper.cpp's multilingual
// base model, in the program's own share folder (/usr/share/omaedit/whisper
// for /usr/bin/omaedit), or else in one of the system's data folders.
static const QLatin1String kPackagedModel("omaedit/whisper/ggml-base.bin");

static QStringList packagedModelPlaces() {
    QStringList places{QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../share/") + kPackagedModel)};
    const QString own = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        if (dir != own)
            places << QDir::cleanPath(dir + QLatin1Char('/') + kPackagedModel);
    return places;
}

QString Transcriber::packagedModel() {
    const QStringList places = packagedModelPlaces();
    for (const QString &place : places)
        if (QFileInfo(place).isFile())
            return place;
    return QString();
}

Transcriber::Transcriber(QObject *parent) : QObject(parent) {
    QSettings settings(QStringLiteral("omaedit"), QStringLiteral("omaedit"));
    m_command = settings.value(QStringLiteral("transcriptCommand"), QStringLiteral("whisper-cli")).toString();
    // Empty: the model that comes with the package. (An earlier version
    // stored its default, a model the user downloaded, when Preferences
    // were reset; while that file is there it is still the user's choice.)
    m_model = settings.value(QStringLiteral("transcriptModel")).toString();
    // The packaged model is multilingual: by default it finds the language.
    m_language = settings.value(QStringLiteral("transcriptLanguage"), QStringLiteral("auto")).toString();
}

// The user's own model while it is there; otherwise the packaged one. A
// user's model on a drive that is not mounted comes back when it is.
QString Transcriber::model() const {
    if (!m_model.isEmpty() && QFileInfo(m_model).isFile())
        return m_model;
    const QString packaged = packagedModel();
    if (!packaged.isEmpty())
        return packaged;
    return m_model.isEmpty() ? packagedModelPlaces().first() : m_model;
}

Transcriber::~Transcriber() { abandon(); }

// QProcess destruction can emit finished while waiting for its child; none
// of that may reach an owner that is partly destroyed.
void Transcriber::abandon() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        QFile::remove(m_wav); QFile::remove(m_base + QStringLiteral(".json")); // its work files, of no use now
    }
}

static void storeTranscriptSetting(const char *key, const QString &value) {
    QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit")).setValue(QLatin1String(key), value);
}

void Transcriber::setCommand(const QString &command) {
    const QString clean = command.trimmed().isEmpty() ? QStringLiteral("whisper-cli") : command.trimmed();
    if (clean == m_command)
        return;
    m_command = clean;
    storeTranscriptSetting("transcriptCommand", clean);
    emit settingsChanged();
}

// The path shown is the model in use: given back unchanged it changes
// nothing; the packaged model's path, or none, is the packaged model.
void Transcriber::setModel(const QString &path) {
    QString clean = path.trimmed();
    if (clean == model())
        return;
    if (clean == packagedModel())
        clean.clear();
    if (clean == m_model)
        return;
    m_model = clean;
    storeTranscriptSetting("transcriptModel", clean);
    if (!clean.isEmpty() && !QFileInfo(clean).isFile() && !packagedModel().isEmpty())
        emit message(tr("No speech model at %1: transcription uses the one that comes with OmaEdit").arg(clean));
    emit settingsChanged();
}

void Transcriber::setLanguage(const QString &language) {
    const QString clean = language.trimmed().isEmpty() ? QStringLiteral("auto") : language.trimmed();
    if (clean == m_language)
        return;
    m_language = clean;
    storeTranscriptSetting("transcriptLanguage", clean);
    emit settingsChanged();
}

static QString resolveCommand(const QString &command) {
    if (command.contains(QLatin1Char('/')))
        return QFileInfo(command).isExecutable() ? command : QString();
    return QStandardPaths::findExecutable(command);
}

// A packaged whisper.cpp links a shared ggml that loads its compute
// backends (libggml-cpu-*, libggml-vulkan, ...) at run time from its own
// folder, the library folder or beside the command; with none there it
// aborts on the first clip. A build with the backends compiled in has no
// shared ggml beside it and is taken as it is.
static bool whisperBackendMissing(const QString &program) {
    if (QFileInfo::exists(qEnvironmentVariable("GGML_BACKEND_PATH")))
        return false;
    const QString exe = QFileInfo(program).canonicalFilePath();
    if (exe.isEmpty())
        return false;
    const QDir bin = QFileInfo(exe).absoluteDir();
    QString libDir;
    for (const QString &dir : {bin.absolutePath(), bin.absoluteFilePath(QStringLiteral("../lib")),
                               bin.absoluteFilePath(QStringLiteral("../lib64"))})
        if (!QDir(dir).entryList({QStringLiteral("libggml.so*")}, QDir::Files).isEmpty()) {
            libDir = QDir::cleanPath(dir);
            break;
        }
    if (libDir.isEmpty())
        return false;
    for (const QString &dir : {libDir + QStringLiteral("/ggml"), libDir, bin.absolutePath()}) {
        const QStringList found = QDir(dir).entryList({QStringLiteral("libggml-*.so*")}, QDir::Files);
        for (const QString &name : found)
            if (!name.startsWith(QLatin1String("libggml-base")))
                return false;
    }
    return true;
}

// The package brings ffmpeg, whisper.cpp, its compute backends and a model,
// so each of these is a real fault, said plainly.
QString Transcriber::status() const {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
        return tr("Transcription cannot run: ffmpeg, which takes the sound out of the clip, was not found.");
    if (resolveCommand(m_command).isEmpty())
        return tr("Transcription cannot run: the command “%1” was not found. Preferences ▸ Media ▸ "
                  "Transcription names the command.")
            .arg(m_command);
    if (whisperBackendMissing(resolveCommand(m_command)))
        return tr("Transcription cannot run: “%1” finds no compute backend (the ggml libraries it loads are missing).")
            .arg(m_command);
    const QString chosen = model();
    if (!QFileInfo(chosen).isFile())
        return m_model.isEmpty()
                   ? tr("Transcription cannot run: the speech model that comes with OmaEdit is missing (%1).").arg(chosen)
                   : tr("Transcription cannot run: there is no speech model at %1. Preferences ▸ Media ▸ "
                        "Transcription names the model.").arg(chosen);
    return QString();
}

// whisper.cpp runs best on one thread a core. A thread on each core's second
// hardware thread as well makes its workers wait on one another: a minute of
// speech took 2.6 s on 16 threads of a 16-core, 32-thread processor, and from
// 4 to 24 s on 32 while other work ran. So every core it may use, once.
int Transcriber::threads() {
#ifdef Q_OS_LINUX
    cpu_set_t usable;
    CPU_ZERO(&usable);
    if (sched_getaffinity(0, sizeof usable, &usable) == 0) {
        // Each processor's core: the hardware threads sharing it (the
        // kernel's topology), else its package and core number in
        // /proc/cpuinfo (a sandbox may show no /sys), else itself.
        QHash<int, QString> listed;
        QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
        if (cpuinfo.open(QIODevice::ReadOnly)) {
            int processor = -1;
            QString package;
            for (const QByteArray &line : cpuinfo.readAll().split('\n')) {
                const QString text = QString::fromLatin1(line);
                const QString value = text.section(QLatin1Char(':'), 1).trimmed();
                if (text.startsWith(QLatin1String("processor"))) processor = value.toInt();
                else if (text.startsWith(QLatin1String("physical id"))) package = value;
                else if (text.startsWith(QLatin1String("core id")) && processor >= 0) listed.insert(processor, package + QLatin1Char('/') + value);
            }
        }
        QSet<QString> cores;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &usable))
                continue;
            QFile siblings(QStringLiteral("/sys/devices/system/cpu/cpu%1/topology/thread_siblings_list").arg(cpu));
            if (siblings.open(QIODevice::ReadOnly))
                cores.insert(QStringLiteral("sys ") + QString::fromLatin1(siblings.readAll()).trimmed());
            else
                cores.insert(listed.value(cpu, QStringLiteral("cpu %1").arg(cpu)));
        }
        if (!cores.isEmpty())
            return int(cores.size());
    }
#endif
    return qMax(1, QThread::idealThreadCount());
}

void Transcriber::setJob(const QVariantMap &job) {
    m_job = job;
    emit jobChanged();
}

void Transcriber::start(int index, const QString &path, const QString &libraryId, const QString &name) {
    const QDir dir(StoragePreferences::cacheRoot()
                   + QStringLiteral("/omaedit-transcripts"));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    const QFileInfo info(path);
    const QString key = QStringLiteral("%1|%2|%3").arg(path).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    m_base = dir.absoluteFilePath(QStringLiteral("clip-%1").arg(hash));
    m_wav = m_base + QStringLiteral(".wav");
    m_stderr.clear();
    QFile::remove(m_base + QStringLiteral(".json"));
    // The index can name another clip by the time whisper is done (a removal,
    // another project): the result goes to the clip with this ID only.
    setJob({{QStringLiteral("index"), index}, {QStringLiteral("path"), path},
            {QStringLiteral("libraryId"), libraryId},
            {QStringLiteral("stage"), QStringLiteral("audio")}, {QStringLiteral("percent"), 0}});
    emit message(tr("AI · extracting audio from %1…").arg(name));
    // Stage 1: ffmpeg → 16 kHz mono WAV (what whisper.cpp reads).
    m_process = std::make_unique<QProcess>();
    m_process->setProgram(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
    m_process->setArguments({QStringLiteral("-y"), QStringLiteral("-nostdin"), QStringLiteral("-loglevel"),
                                    QStringLiteral("error"), QStringLiteral("-i"), path, QStringLiteral("-vn"),
                                    QStringLiteral("-ac"), QStringLiteral("1"), QStringLiteral("-ar"),
                                    QStringLiteral("16000"), QStringLiteral("-f"), QStringLiteral("wav"),
                                    m_wav});
    connect(m_process.get(), &QProcess::readyReadStandardError, this, [this] {
        m_stderr += QString::fromUtf8(m_process->readAllStandardError());
    });
    connect(m_process.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(m_wav)) {
            finish(false, QStringLiteral("Audio extraction failed: %1")
                                           .arg(m_stderr.trimmed().section(QLatin1Char('\n'), -1)));
            return;
        }
        textStage();
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, QStringLiteral("Cannot start audio extraction: %1").arg(m_process->errorString()));
    });
    m_process->start();
}

void Transcriber::textStage(bool onCpu) {
    // Stage 2: whisper.cpp — JSON out, progress on stderr, no timestamps
    // in the console, on every core.
    const int index = m_job.value(QStringLiteral("index")).toInt();
    QVariantMap job = m_job;
    job.insert(QStringLiteral("stage"), QStringLiteral("text"));
    job.insert(QStringLiteral("percent"), 0);
    setJob(job);
    const QString chosen = model();
    emit message(tr("AI · transcribing with whisper (%1)…").arg(QFileInfo(chosen).fileName()));
    m_stderr.clear();
    // This runs from the extraction process's finished signal. Retire its
    // sender after the event returns, rather than deleting it in the slot.
    if (auto *completed = m_process.release()) {
        completed->disconnect(this);
        completed->deleteLater();
    }
    m_process = std::make_unique<QProcess>();
    m_process->setProgram(resolveCommand(m_command));
    QStringList arguments{QStringLiteral("-m"), chosen, QStringLiteral("-f"), m_wav,
                          QStringLiteral("-oj"), QStringLiteral("-of"), m_base,
                          QStringLiteral("-l"), m_language, QStringLiteral("-pp"),
                          QStringLiteral("-np"), QStringLiteral("-t"), QString::number(threads())};
    // The GPU the editor's engine draws on, when it has one: whisper.cpp's
    // Vulkan backend is told that device and no other. Without one (the CPU
    // chosen, or only a software Vulkan device, on which whisper.cpp is
    // many times slower than on the processor) and on the second try after
    // a run on the GPU failed, it runs on the processor.
    const int gpu = onCpu ? -1 : render::engineGpu();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (gpu < 0)
        arguments << QStringLiteral("-ng");
    else
        environment.insert(QStringLiteral("GGML_VK_VISIBLE_DEVICES"), QString::number(gpu));
    m_process->setProcessEnvironment(environment);
    m_process->setArguments(arguments);
    connect(m_process.get(), &QProcess::readyReadStandardError, this, [this, index] {
        const QString chunk = QString::fromUtf8(m_process->readAllStandardError());
        m_stderr += chunk;
        static const QRegularExpression progress(QStringLiteral("progress\\s*=\\s*(\\d+)%"));
        QRegularExpressionMatchIterator it = progress.globalMatch(chunk);
        int last = -1;
        while (it.hasNext())
            last = it.next().captured(1).toInt();
        if (last >= 0) {
            QVariantMap job = m_job;
            job.insert(QStringLiteral("percent"), qBound(0, last, 99));
            job.insert(QStringLiteral("index"), index);
            setJob(job);
        }
    });
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, [this] {
        m_process->readAllStandardOutput(); // -np keeps it quiet; drain anyway
    });
    connect(m_process.get(), &QProcess::finished, this, [this, gpu](int code, QProcess::ExitStatus status) {
        QFile json(m_base + QStringLiteral(".json"));
        if (status != QProcess::NormalExit || code != 0 || !json.open(QIODevice::ReadOnly)) {
            if (gpu >= 0) { // the processor always works: the GPU's driver may not
                emit message(tr("AI · whisper stopped on the GPU; transcribing on the processor instead…"));
                textStage(true);
                return;
            }
            QString why = m_stderr.trimmed().section(QLatin1Char('\n'), -1).left(200);
            if (m_stderr.contains(QStringLiteral("GGML_ASSERT(device)"))
                || m_stderr.contains(QStringLiteral("ggml_backend_load_best")))
                why = tr("whisper found no compute backend to run on");
            finish(false, tr("Transcription failed: %1").arg(why));
            return;
        }
        QString language;
        const QVariantList segments = parseWhisperJson(json.readAll(), &language);
        const int index = locate ? locate(m_job) : -1;
        if (index < 0) {
            finish(false, QStringLiteral("The clip left the library before its transcript was ready."));
            return;
        }
        m_job.insert(QStringLiteral("index"), index);
        if (store)
            store(index, segments, language);
        finish(true, segments.isEmpty() ? QStringLiteral("No speech found") : QString());
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, QStringLiteral("Cannot start transcription: %1").arg(m_process->errorString()));
    });
    m_process->start();
}

void Transcriber::finish(bool ok, const QString &error) {
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        (void)m_process.release();
    }
    // The sound taken out for whisper and whisper's answer are work files:
    // read by now or of no use, and large (about 115 MB an hour of sound).
    if (!m_base.isEmpty()) { QFile::remove(m_wav); QFile::remove(m_base + QStringLiteral(".json")); }
    const int index = m_job.value(QStringLiteral("index")).toInt();
    if (ok)
        setJob({});
    else
        setJob({{QStringLiteral("index"), -1}, {QStringLiteral("error"), error},
                {QStringLiteral("forIndex"), index}});
    emit finished(ok, index, error);
}

void Transcriber::cancel() {
    if (!m_process)
        return;
    auto *process = m_process.get();
    process->disconnect(this);
    const auto complete = [this] { finish(false, QStringLiteral("Transcription cancelled")); };
    if (process->state() == QProcess::NotRunning) {
        complete();
        return;
    }
    // Keep ownership until the process exits so a retry cannot reuse its
    // output paths while the old process is still writing. Never wait on GUI.
    connect(process, &QProcess::finished, this, [complete](int, QProcess::ExitStatus) { complete(); });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete();
    });
    auto job = m_job;
    job.insert(QStringLiteral("stage"), QStringLiteral("cancelling"));
    setJob(job);
    process->kill();
}

// whisper.cpp -oj: {"result": {"language": "en"}, "transcription": [{"offsets":
// {"from": ms, "to": ms}, "text": " …"}, …]}. Segments come out in ms with
// their text trimmed; empty or bracketed noise-only lines are dropped.
QVariantList Transcriber::parseWhisperJson(const QByteArray &json, QString *language) {
    QVariantList out;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (language)
        *language = root.value(QStringLiteral("result")).toObject().value(QStringLiteral("language")).toString();
    for (const auto &v : root.value(QStringLiteral("transcription")).toArray()) {
        const QJsonObject seg = v.toObject();
        const QJsonObject offsets = seg.value(QStringLiteral("offsets")).toObject();
        const QString text = seg.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || (text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']'))))
            continue;
        const int from = offsets.value(QStringLiteral("from")).toInt();
        const int to = qMax(from, offsets.value(QStringLiteral("to")).toInt());
        out.append(QVariantMap{{QStringLiteral("start"), from}, {QStringLiteral("end"), to},
                               {QStringLiteral("text"), text}});
    }
    return out;
}
