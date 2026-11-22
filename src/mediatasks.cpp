#include "mediatasks.h"
#include "mediaplayer.h"
#include "filepicker.h"
#include "mediacsv.h"
#include "relinkmatcher.h"
#include "nativevideo.h"
#include "rawpixels.h"
#include <algorithm>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QWaitCondition>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUuid>
#include <QSet>
#include <cmath>
#include <functional>
#include <mutex>
#include <numeric>
#include <thread>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using Stop = std::shared_ptr<std::atomic_bool>;
QString digest(const QString &path, const Stop &stop, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (stop->load()) return {};
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) { *error = file.errorString(); return {}; }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
QString safeName(const QString &source, QString pattern, int number, const QString &extension = {}) {
    const QFileInfo info(source);
    if (pattern.trimmed().isEmpty()) pattern = QStringLiteral("{name}");
    pattern.replace(QStringLiteral("{name}"), info.completeBaseName());
    pattern.replace(QStringLiteral("{index}"), QString::number(number).rightJustified(4, QLatin1Char('0')));
    pattern.replace(QStringLiteral("{date}"), QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    if (pattern.contains('/') || pattern.contains('\\') || pattern.contains('{') || pattern.contains('}')
        || pattern == QStringLiteral(".") || pattern == QStringLiteral("..")) return {};
    return pattern + QLatin1Char('.') + (extension.isEmpty() ? info.suffix() : extension);
}
bool copyVerified(const QString &source, const QString &destination, const Stop &stop,
                  QString *hash, QString *error, const std::function<void(qint64,qint64)> &progress) {
    const QFileInfo original(source);
    const QString folder = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(folder)) { *error = QObject::tr("Cannot create destination folder."); return false; }
    if (QFileInfo::exists(destination)) {
        const QString wanted = digest(source, stop, error);
        if (wanted.isEmpty()) return false;
        if (wanted == digest(destination, stop, error)) { *hash = wanted; return true; }
        *error = QObject::tr("Destination already exists with different content; it was kept."); return false;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) { *error = input.errorString(); return false; }
    QTemporaryFile stage(folder + QStringLiteral("/.omaedit-copy-XXXXXX"));
    if (!stage.open()) { *error = stage.errorString(); return false; }
    QCryptographicHash copied(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    while (!input.atEnd()) {
        if (stop->load()) return false;
        const QByteArray chunk = input.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFile::NoError) { *error = input.errorString(); return false; }
        if (stage.write(chunk) != chunk.size()) { *error = stage.errorString(); return false; }
        copied.addData(chunk); bytes += chunk.size(); progress(bytes, original.size());
    }
    if (!stage.flush()) { *error = stage.errorString(); return false; }
    const QString expected = QString::fromLatin1(copied.result().toHex());
    if (digest(stage.fileName(), stop, error) != expected) {
        if (!stop->load()) *error = QObject::tr("Checksum verification failed.");
        return false;
    }
    const QFileInfo after(source);
    if (after.size() != original.size() || after.lastModified() != original.lastModified()) {
        *error = QObject::tr("Source changed during the transfer; retry when it is stable."); return false;
    }
    stage.setPermissions(original.permissions());
    stage.close();
    if (stop->load()) return false;
    if (!QFile::rename(stage.fileName(), destination)) { *error = QObject::tr("Cannot publish the verified copy; destination may already exist."); return false; }
    stage.setAutoRemove(false); *hash = expected; return true;
}
QVariantMap probeMedia(const QString &path, QImage *thumbnail) {
    MediaPlayer probe;
    if (!probe.open(path) || probe.frameCount() <= 0) return {};
    QVariantMap meta = probe.metadata();
    const QFileInfo info(path);
    meta.insert(QStringLiteral("path"), path); meta.insert(QStringLiteral("name"), info.fileName());
    meta.insert(QStringLiteral("duration"), probe.frameCount()); meta.insert(QStringLiteral("fps"), probe.fps());
    meta.insert(QStringLiteral("width"), probe.width()); meta.insert(QStringLiteral("height"), probe.height());
    const bool image = QStringList{"jpg", "jpeg", "png", "tif", "tiff", "bmp", "webp", "exr", "psd"}.contains(info.suffix().toLower());
    meta.insert(QStringLiteral("hasVideo"), image || !meta.value(QStringLiteral("videoCodec")).toString().isEmpty());
    meta.insert(QStringLiteral("hasAudio"), !meta.value(QStringLiteral("audioCodec")).toString().isEmpty());
    if (!meta.value(QStringLiteral("hasVideo")).toBool() && !meta.value(QStringLiteral("hasAudio")).toBool()) return {};
    if (MediaPlayer::isStillPath(path)) meta.insert(QStringLiteral("still"), true);
    meta.insert(QStringLiteral("size"), info.size()); meta.insert(QStringLiteral("modified"), info.lastModified().toString(Qt::ISODate));
    if (!meta.contains(QStringLiteral("created"))) meta.insert(QStringLiteral("created"),
        (info.birthTime().isValid() ? info.birthTime() : info.lastModified()).toString(Qt::ISODate));
    meta.insert(QStringLiteral("imported"), QDateTime::currentDateTime().toString(Qt::ISODate));
    if (thumbnail && meta.value(QStringLiteral("hasVideo")).toBool())
        *thumbnail = probe.frameAt(qMin(probe.frameCount() - 1, probe.frameCount()/10)).scaledToWidth(480, Qt::SmoothTransformation);
    return meta;
}
// What one clip's encode (a proxy, a reframed 360° clip) came to. `failure`
// says why not; neither done nor failed means the task was stopped.
struct Encoded {
    QVariantMap row;
    bool done = false;
    QString failure;
    QString destination;     // published file, to announce
    QVariantMap metadata;    // reframe: the new clip's facts
    QImage thumbnail;
};
// Destinations being written by the clips of one batch. Two clips that would
// make the same file take turns, so the second finds the first's file and is
// answered as it would be had they run one after the other.
struct Claims { QMutex mutex; QWaitCondition freed; QSet<QString> busy; };
class Claim {
public:
    Claim(Claims &claims, const QString &path) : m_claims(claims), m_path(path) {
        QMutexLocker lock(&m_claims.mutex);
        while (m_claims.busy.contains(m_path)) m_claims.freed.wait(&m_claims.mutex);
        m_claims.busy.insert(m_path);
    }
    ~Claim() { QMutexLocker lock(&m_claims.mutex); m_claims.busy.remove(m_path); m_claims.freed.wakeAll(); }
private:
    Q_DISABLE_COPY(Claim)
    Claims &m_claims; QString m_path;
};
// A tool process works behind everything else on the machine: it may use every
// core, and playback, the window and the editor's own work still come first.
// (Lowest scheduling priority instead of a cap on its threads.)
void runsBehind(QProcess &process) {
    process.setChildProcessModifier([] { ::setpriority(PRIO_PROCESS, 0, 19); });
}
// While the editor plays, a tool keeps to a small part of the machine (about
// what it had when it was held to two threads) and has all of it the rest of
// the time. Priority alone does not do this: with every core busy the cores,
// their caches and the memory are shared, and playback was measured at half
// its speed beside a batch that had the lowest priority.
// Owned by the batch thread that waits on the tool; `yielding` is the task
// list's flag, set from the GUI thread.
class ToolShare {
public:
    explicit ToolShare(const std::atomic_bool *yielding) : m_yielding(yielding) {}
    // From the loop that waits on the tool: follows a change of the flag.
    void follow(const QProcess &process) {
        const bool narrow = m_yielding && m_yielding->load();
        const qint64 pid = process.processId();
        if (narrow == m_narrow || pid <= 0) return;
        m_narrow = narrow;
        cpu_set_t all;
        CPU_ZERO(&all);
        if (::sched_getaffinity(0, sizeof all, &all) != 0) return; // this thread's: the most a tool may have
        cpu_set_t share = all;
        if (narrow) {
            // The last of the machine's processors (the small cores, where
            // there are two kinds): two, or a sixteenth of a large machine.
            int keep = qMax(2, CPU_COUNT(&all) / 16);
            CPU_ZERO(&share);
            for (int cpu = CPU_SETSIZE - 1; cpu >= 0 && keep > 0; --cpu)
                if (CPU_ISSET(cpu, &all)) { CPU_SET(cpu, &share); --keep; }
        }
        // Every thread of the tool; one started meanwhile by a thread not yet
        // moved is caught by the second pass.
        const QDir threads(QStringLiteral("/proc/%1/task").arg(pid));
        for (int pass = 0; pass < 2; ++pass)
            for (const QString &thread : threads.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                ::sched_setaffinity(pid_t(thread.toLongLong()), sizeof share, &share);
    }
private:
    const std::atomic_bool *m_yielding;
    bool m_narrow = false;
};
// The calling thread likewise (a batch's own threads: probing, RAW developing).
void threadRunsBehind() { ::setpriority(PRIO_PROCESS, id_t(::syscall(SYS_gettid)), 19); }
// How many clips a batch encodes at once: one for every eight of the machine's
// threads (an encode at proxy size gains little beyond about eight), so a small
// machine works through them one by one and a large one is not left idle.
// OMAEDIT_ENCODE_JOBS sets it, for measuring.
int encodesAtOnce(int clips) {
    bool set = false; const int asked = qEnvironmentVariableIntValue("OMAEDIT_ENCODE_JOBS", &set);
    return qBound(1, set && asked > 0 ? asked : QThread::idealThreadCount() / 8, qMax(1, clips));
}
// The threads FFmpeg is told to use, 0 for its own choice (the default: the
// tools are held back by their priority, not by a count). OMAEDIT_ENCODE_THREADS
// sets it, for measuring.
int encodeThreads() { return qMax(0, qEnvironmentVariableIntValue("OMAEDIT_ENCODE_THREADS")); }
struct EncodeJob {
    QString path; int i = 0; int count = 1; QVariantMap options; Stop stop;
    QString threads;                              // FFmpeg's -threads, "0" = its own choice
    std::function<void(double)> part;             // this clip's progress, 0..1
    std::function<void(const QString &)> say;     // a message for the task's row
    Claims *claims = nullptr;
    const std::atomic_bool *yielding = nullptr;   // the editor is playing: see ToolShare
};
Encoded encodeReframe(const EncodeJob &job) {
    const QString &path = job.path; const int i = job.i; const QVariantMap &options = job.options;
    const Stop &stop = job.stop; const QString &threads = job.threads; const auto &part = job.part; const auto &say = job.say;
    Claims &claims = *job.claims;
    Encoded out; QVariantMap &row = out.row; row = {{"source", path}, {"state", "completed"}};
    auto failed = [&](const QString &message) { out.failure = message; };
    const auto original = probeMedia(path,nullptr);
    if (!original.value("hasVideo").toBool()) { failed(MediaTasks::tr("Choose a readable 360° video.")); return out; }
    if (original.value("videoCodec").toString() == "prores_raw") { failed(MediaTasks::tr("Develop RAW footage to ProRes before reframing it.")); return out; }
    const QString projection = options.value("projection","e").toString();
    const QString acceleration = options.value("acceleration","auto").toString();
    const int width=options.value("width",1920).toInt(), height=options.value("height",1080).toInt();
    const double yaw=options.value("yaw",0.).toDouble(), pitch=options.value("pitch",0.).toDouble(), roll=options.value("roll",0.).toDouble();
    const double fov=options.value("fov",90.).toDouble();
    if (!QStringList{"e","dfisheye"}.contains(projection) || !QStringList{"auto","software"}.contains(acceleration)
        || width<16 || height<16 || width>7680 || height>4320 || width%2 || height%2
        || !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll) || !std::isfinite(fov)
        || std::abs(yaw)>180 || std::abs(pitch)>90 || std::abs(roll)>180 || fov<10 || fov>170) {
        failed(MediaTasks::tr("Invalid projection, view angles, field of view or output size.")); return out;
    }
    const QString folder=options.value("destination").toString();
    const QString name=safeName(path,options.value("pattern","{name}_reframed").toString(),i+1,"mov");
    if (folder.isEmpty() || name.isEmpty() || !QDir().mkpath(folder)) { failed(MediaTasks::tr("Choose a writable destination folder and filename pattern.")); return out; }
    const QString destination=QDir(folder).absoluteFilePath(name);
    const Claim claim(claims, destination);
    if (QFileInfo::exists(destination)) { failed(MediaTasks::tr("Destination exists; choose another name.")); return out; }
    QTemporaryFile stage(QDir(folder).filePath(".omaedit-reframe-XXXXXX.mov"));
    if (!stage.open()) { failed(stage.errorString()); return out; } stage.close();
    const double vertical=2*std::atan(std::tan(fov*M_PI/360)*height/width)*180/M_PI;
    const QString transform=QString("input=%1:output=flat:w=%2:h=%3:yaw=%4:pitch=%5:roll=%6:h_fov=%7:v_fov=%8")
        .arg(projection).arg(width).arg(height).arg(yaw,0,'g',12).arg(pitch,0,'g',12).arg(roll,0,'g',12).arg(fov,0,'g',12).arg(vertical,0,'g',12);
    bool encoded=false; QByteArray log;
    const bool tryGpu=acceleration=="auto";
    for (int attempt=0;attempt<(tryGpu?2:1) && !stop->load();++attempt) {
        const bool gpu=tryGpu && attempt==0;
        QStringList args{"-hide_banner","-nostdin","-y","-v","error"};
        if (gpu) args << "-init_hw_device" << "vulkan=omaedit" << "-filter_hw_device" << "omaedit";
        args << "-threads" << threads << "-i" << path << "-map" << "0:v:0" << "-map" << "0:a?" << "-vf"
             << (gpu ? "format=gbrpf32le,hwupload,v360_vulkan="+transform+QString(",scale_vulkan=w=%1:h=%2,hwdownload,format=gbrpf32le,format=yuv422p10le,setsar=1").arg(width).arg(height)
                     : "format=gbrp16le,v360="+transform+",format=yuv422p10le,setsar=1")
             << "-fps_mode" << "passthrough" << "-c:v" << "prores_ks" << "-profile:v" << "3" << "-threads" << threads
             << "-c:a" << "pcm_s24le" << "-progress" << "pipe:1" << stage.fileName();
        QProcess process; runsBehind(process); process.start("ffmpeg",args);
        if (!process.waitForStarted(5000)) { log=process.errorString().toUtf8(); break; }
        log.clear();
        ToolShare share(job.yielding); share.follow(process);
        while (process.state()!=QProcess::NotRunning && !process.waitForFinished(100)) {
            share.follow(process);
            log+=process.readAllStandardError(); log=log.right(12000);
            const auto update=process.readAllStandardOutput().split('\n');
            for (const auto &line:update) if (line.startsWith("out_time_us=")) {
                const double duration=original.value("duration").toDouble()/original.value("fps").toDouble();
                part(qBound(0.,line.mid(12).toDouble()/1000000/duration,1.));
            }
            if (stop->load()) { process.kill(); process.waitForFinished(5000); break; }
        }
        log+=process.readAllStandardError();
        encoded=process.exitStatus()==QProcess::NormalExit && process.exitCode()==0;
        if (encoded) { row["processor"]=gpu?"vulkan":"software"; break; }
        if (gpu && !stop->load()) say(MediaTasks::tr("GPU reframing failed; retrying on the CPU."));
    }
    if (stop->load()) return out;
    if (!encoded) { failed(QString::fromUtf8(log.right(1500))); return out; }
    QImage thumbnail; auto metadata=probeMedia(stage.fileName(),&thumbnail);
    if (metadata.value("duration")!=original.value("duration") || std::abs(metadata.value("fps").toDouble()-original.value("fps").toDouble())>.001) {
        failed(MediaTasks::tr("Reframed clip timing differs from its source.")); return out;
    }
    if (!QFile::rename(stage.fileName(),destination)) { failed(MediaTasks::tr("Could not publish the reframed clip.")); return out; }
    stage.setAutoRemove(false); metadata["path"]=destination; metadata["name"]=name;
    out.destination=destination; out.metadata=metadata; out.thumbnail=std::move(thumbnail);
    row["destination"]=destination; row["message"]=MediaTasks::tr("Reframed to ProRes 422 HQ and imported.");
    out.done = true; return out;
}
Encoded encodeProxy(const EncodeJob &job) {
    const QString &path = job.path; const int i = job.i; const QVariantMap &options = job.options;
    const Stop &stop = job.stop; const QString &threads = job.threads; const auto &part = job.part; const auto &say = job.say;
    Claims &claims = *job.claims;
    Encoded out; QVariantMap &row = out.row; row = {{"source", path}, {"state", "completed"}};
    auto failed = [&](const QString &message) { out.failure = message; };
    QImage unused; const QVariantMap original = probeMedia(path, nullptr);
    if (!original.value("hasVideo").toBool() || original.value("duration").toInt() <= 1) { failed(MediaTasks::tr("Proxies require a readable video clip.")); return out; }
    const QString preset = options.value("preset", "h264-720").toString();
    if (!QStringList{"h264-540", "h264-720", "prores-1080"}.contains(preset)) { failed(MediaTasks::tr("Unknown proxy preset.")); return out; }
    const QString folder = options.value("destination").toString();
    const QString name = safeName(path, options.value("pattern", "{name}_proxy").toString(), i+1, preset.startsWith("prores") ? "mov" : "mp4");
    if (folder.isEmpty() || name.isEmpty() || !QDir().mkpath(folder)) { failed(MediaTasks::tr("Choose a writable proxy folder and a valid naming pattern.")); return out; }
    const QString destination = QDir(folder).absoluteFilePath(name); row.insert("destination", destination);
    const Claim claim(claims, destination);
    if (QFileInfo::exists(destination)) {
        // Made before (another project with the same clip, or an earlier
        // run): a proxy that fits its original is used as it is.
        const QVariantMap kept = probeMedia(destination, nullptr);
        if (kept.value("hasVideo").toBool() && kept.value("duration").toInt() == original.value("duration").toInt()
            && std::abs(kept.value("fps").toDouble() - original.value("fps").toDouble()) <= 0.001) {
            row["encoder"] = QString();
            row["message"] = MediaTasks::tr("Used the proxy already in the folder.");
            out.destination = destination;
            out.done = true; return out;
        }
        failed(MediaTasks::tr("Another file with this proxy's name is in the folder and does not match the clip. Choose a different name or folder."));
        return out;
    }
    QTemporaryFile stage(QDir(folder).absoluteFilePath(".omaedit-proxy-XXXXXX." + QFileInfo(name).suffix()));
    if (!stage.open()) { failed(stage.errorString()); return out; } stage.close();
    const int height = preset == "h264-540" ? 540 : preset == "h264-720" ? 720 : 1080;
    const bool raw = original.value("videoCodec").toString()=="prores_raw";
    const auto rawSettings=options.value("sourceSettings").toMap().value(path).toMap();
    const int rawHeight=qMin(height,original.value("height").toInt())/2*2;
    const int rawWidth=qMax(2,qRound(original.value("width").toDouble()*rawHeight/qMax(1,original.value("height").toInt()))/2*2);
    const QString acceleration = options.value("acceleration", "auto").toString();
    if (!QStringList{"auto", "software"}.contains(acceleration)) { failed(MediaTasks::tr("Unknown proxy processing mode.")); return out; }
    const bool tryGpu = preset.startsWith("prores") && acceleration == "auto";
    bool encoded = false;
    QByteArray log;
    for (int attempt = 0; attempt < (tryGpu ? 2 : 1) && !stop->load(); ++attempt) {
        const bool gpu = tryGpu && attempt == 0;
        QStringList args{"-hide_banner", "-nostdin", "-y"};
        if (gpu) args << "-init_hw_device" << "vulkan=omaedit" << "-filter_hw_device" << "omaedit";
        if (raw) args << "-f" << "rawvideo" << "-pixel_format" << "rgba64le" << "-video_size" << QString("%1x%2").arg(rawWidth).arg(rawHeight)
                      << "-framerate" << QString::number(original.value("fps").toDouble(),'g',12) << "-i" << "pipe:0";
        args << "-threads" << threads << "-i" << path << "-map" << "0:v:0" << "-map" << (raw ? "1:a?" : "0:a?") << "-vf"
             << (QString("scale=-2:'min(%1,ih)'").arg(height) + (gpu ? ",format=yuv422p10le,hwupload" : ""))
             << "-fps_mode" << "passthrough";
        if (preset.startsWith("prores")) {
            args << "-c:v" << (gpu ? "prores_ks_vulkan" : "prores_ks") << "-profile:v" << "0";
            if (!gpu) args << "-pix_fmt" << "yuv422p10le";
            args << "-c:a" << "pcm_s16le";
        } else args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-c:a" << "aac" << "-b:a" << "128k" << "-movflags" << "+faststart";
        if (raw) args << "-color_primaries" << "bt709" << "-color_trc" << "bt709" << "-colorspace" << "bt709";
        args << "-threads" << threads << "-progress" << "pipe:1" << stage.fileName();
        QProcess process; runsBehind(process); process.start("ffmpeg", args);
        if (!process.waitForStarted(5000)) { log = process.errorString().toUtf8(); break; }
        log.clear();
        ToolShare share(job.yielding); share.follow(process);
        if (raw) {
            // One RAW clip is developed at a time: its decoder is not known to be
            // safe beside another of its kind.
            static QBasicMutex oneRaw; QMutexLocker developing(&oneRaw);
            NativeVideo decoder; QString problem;
            bool decoded=decoder.open(path,&problem);
            const int frames=original.value("duration").toInt();
            for (int frame=0;decoded && frame<frames && !stop->load();++frame) {
                const auto linear=decoder.frame(frame/original.value("fps").toDouble(),rawSettings,&problem);
                auto pixels=RawPixels::display(linear.scaled(rawWidth,rawHeight,Qt::IgnoreAspectRatio,Qt::SmoothTransformation),true);
                if (pixels.isNull() || process.state()!=QProcess::Running) { decoded=false; break; }
                decoded=process.write(reinterpret_cast<const char *>(pixels.constBits()),pixels.sizeInBytes())==pixels.sizeInBytes();
                QElapsedTimer blocked; blocked.start();
                while (decoded && process.bytesToWrite()>0 && !stop->load()) {
                    process.waitForBytesWritten(100); share.follow(process);
                    log+=process.readAllStandardError(); log=log.right(12000); process.readAllStandardOutput();
                    if (process.state()!=QProcess::Running || blocked.elapsed()>30000) { decoded=false; break; }
                }
                part(double(frame+1)/qMax(1,frames));
            }
            if (!decoded || stop->load()) {
                process.kill(); process.waitForFinished(5000);
                if (!problem.isEmpty()) log+=problem.toUtf8();
            }
            process.closeWriteChannel();
        }
        while (process.state()!=QProcess::NotRunning && !process.waitForFinished(100)) {
            share.follow(process);
            log += process.readAllStandardError(); if (log.size() > 12000) log = log.right(12000);
            const auto lines = process.readAllStandardOutput().split('\n');
            for (const auto &line : lines) if (line.startsWith("out_time_us=")) {
                const double duration = original.value("duration").toDouble() / original.value("fps").toDouble();
                const double done = qBound(0.0, line.mid(12).toDouble() / 1000000.0 / duration, 1.0);
                part(done);
            }
            if (stop->load()) { process.kill(); process.waitForFinished(5000); break; }
        }
        log += process.readAllStandardError();
        encoded = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (encoded) {
            row["encoder"] = gpu ? "prores_ks_vulkan" : preset.startsWith("prores") ? "prores_ks" : "libx264";
            row["message"] = gpu ? MediaTasks::tr("Encoded with Vulkan ProRes.") : tryGpu ? MediaTasks::tr("GPU unavailable; encoded on the CPU.") : MediaTasks::tr("Encoded on the CPU.");
            break;
        }
        if (gpu && !stop->load()) say(MediaTasks::tr("GPU encoding failed; retrying on the CPU."));
    }
    if (stop->load()) return out;
    if (!encoded) { failed(QString::fromUtf8(log.right(1500))); return out; }
    const QVariantMap proxy = probeMedia(stage.fileName(), nullptr);
    if (proxy.value("duration").toInt() != original.value("duration").toInt()
        || std::abs(proxy.value("fps").toDouble() - original.value("fps").toDouble()) > 0.001) { failed(MediaTasks::tr("Proxy frame count or frame rate differs from its original.")); return out; }
    if (!QFile::rename(stage.fileName(), destination)) { failed(MediaTasks::tr("Could not publish the proxy file.")); return out; }
    stage.setAutoRemove(false);
    out.destination = destination;
    out.done = true; return out;
}
QString csvCell(QString text) { text.replace('"', "\"\""); return QLatin1Char('"') + text + QLatin1Char('"'); }
}
MediaTasks::MediaTasks(QObject *parent) : QObject(parent), m_chooser(new FileChooser(this)) {
    connect(m_chooser, &FileChooser::selectedMany, this, [this](const QList<QUrl> &urls) {
        QStringList paths;
        for (const QUrl &url : urls) if (url.isLocalFile()) paths.append(url.toLocalFile());
        emit pathsChosen(m_purpose, paths);
    });
    connect(m_chooser, &FileChooser::failed, this, &MediaTasks::error);
}
void MediaTasks::browse(const QString &purpose, const QString &kind) {
    m_purpose = purpose;
    if (kind == QStringLiteral("files")) m_chooser->openFiles(tr("Select Media"));
    else if (kind == QStringLiteral("file")) m_chooser->openFile(tr("Select Media"));
    else if (kind == QStringLiteral("png")) m_chooser->saveFile(tr("Save Source Snapshot"), QStringLiteral("source-frame.png"), tr("PNG image"), {QStringLiteral("*.png")});
    else if (kind == QStringLiteral("exr")) m_chooser->saveFile(tr("Save Linear Source Snapshot"), QStringLiteral("source-frame.exr"), tr("Linear OpenEXR or sRGB PNG"), {QStringLiteral("*.exr"), QStringLiteral("*.png")});
    else if (kind == QStringLiteral("save")) m_chooser->saveFile(tr("Save Report"), QStringLiteral("media-report.csv"), tr("CSV report"), {QStringLiteral("*.csv")});
    else m_chooser->openDirectory(tr("Choose Folder"));
}
QVariantList MediaTasks::describePaths(const QStringList &paths) {
    const QString home = QDir::homePath();
    QVariantList out;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        QString folder = QDir::toNativeSeparators(info.absolutePath());
        if (folder == home || folder.startsWith(home + QLatin1Char('/')))
            folder = QStringLiteral("~") + folder.mid(home.size());
        out.append(QVariantMap{{QStringLiteral("path"), path},
                               {QStringLiteral("name"), info.fileName().isEmpty() ? path : info.fileName()},
                               {QStringLiteral("folder"), folder},
                               {QStringLiteral("isDir"), info.isDir()},
                               {QStringLiteral("exists"), info.exists()},
                               {QStringLiteral("bytes"), info.isFile() ? info.size() : qint64(0)}});
    }
    return out;
}
MediaTasks::~MediaTasks() {
    cancelAll();
    if (m_thread) { m_thread->wait(); delete m_thread; m_thread = nullptr; }
}
QStringList MediaTasks::formats() {
    return {"mp4", "mov", "mkv", "avi", "mts", "m2ts", "webm", "mxf", "m4v", "mpg", "mpeg", "wmv",
        "mp3", "wav", "flac", "aac", "m4a", "ogg", "aif", "aiff", "opus", "jpg", "jpeg", "png", "tif", "tiff", "bmp", "webp", "exr",
        "psd"}; // PSD: the flattened composite FFmpeg reads (layers are not accessible)
}
QVariantMap MediaTasks::job(const QString &id) const {
    for (const QVariant &v : m_jobs) if (v.toMap().value(QStringLiteral("id")).toString() == id) return v.toMap();
    return {};
}
void MediaTasks::update(const QString &id, const QVariantMap &fields) {
    for (int i = 0; i < m_jobs.size(); ++i) {
        QVariantMap item = m_jobs.at(i).toMap();
        if (item.value(QStringLiteral("id")).toString() != id) continue;
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) item.insert(it.key(), it.value());
        m_jobs[i] = item; emit changed(); return;
    }
}
QString MediaTasks::submit(const QString &kind, const QStringList &paths, const QVariantMap &options) {
    if (!QStringList{"import", "clone", "proxy", "reframe360", "verify", "duplicates", "relink", "relinkApply", "attach", "watch", "csvImport", "csvExport", "providerManifest"}.contains(kind) || paths.isEmpty()) return {};
    if (kind == QStringLiteral("watch")) {
        for (int i = m_jobs.size()-1; i >= 0; --i) {
            const QVariantMap previous = m_jobs.at(i).toMap();
            if (previous.value(QStringLiteral("kind")).toString() != kind || previous.value(QStringLiteral("options")).toMap().value(QStringLiteral("watchId")) != options.value(QStringLiteral("watchId"))) continue;
            if (previous.value(QStringLiteral("state")).toString() == QStringLiteral("running") || previous.value(QStringLiteral("state")).toString() == QStringLiteral("queued")) return previous.value(QStringLiteral("id")).toString();
            // Keep one current monitoring record; successful imports retain their metadata in the project.
            m_jobs.removeAt(i);
        }
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_jobs.append(QVariantMap{{"id", id}, {"kind", kind}, {"paths", paths}, {"options", options},
        {"state", "queued"}, {"percent", 0}, {"label", paths.isEmpty() ? QString() : QFileInfo(paths.first()).fileName()}, {"report", QVariantList{}}});
    m_queue.enqueue(id); emit changed(); startNext(); return id;
}
void MediaTasks::cancel(const QString &id) {
    if (id == m_active && m_stop) { m_stop->store(true); update(id, {{"message", tr("Cancelling…")}}); }
    else if (m_queue.removeAll(id)) update(id, {{"state", "cancelled"}});
}
void MediaTasks::cancelAll() {
    if (m_stop) m_stop->store(true);
    while (!m_queue.isEmpty()) update(m_queue.dequeue(), {{"state", "cancelled"}});
}
QString MediaTasks::retry(const QString &id) {
    const QVariantMap previous = job(id);
    if (previous.isEmpty() || previous.value("state") == "running" || previous.value("state") == "queued" || previous.value("state") == "applying") return {};
    return submit(previous.value("kind").toString(), previous.value("paths").toStringList(), previous.value("options").toMap());
}
void MediaTasks::clearFinished() {
    for (int i = m_jobs.size()-1; i >= 0; --i) {
        const QString state = m_jobs.at(i).toMap().value("state").toString();
        if (state != "running" && state != "queued" && state != "applying") m_jobs.removeAt(i);
    }
    emit changed();
}
bool MediaTasks::saveReport(const QString &id, const QString &path) {
    const QVariantMap item = job(id); if (item.isEmpty()) return false;
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { emit error(file.errorString()); return false; }
    QByteArray output("Source,Destination,State,Message,SHA256\r\n");
    for (const QVariant &v : item.value("report").toList()) {
        const QVariantMap row = v.toMap(); QStringList cells;
        for (const char *key : {"source", "destination", "state", "message", "sha256"}) cells.append(csvCell(row.value(QLatin1String(key)).toString()));
        output += cells.join(',').toUtf8() + "\r\n";
    }
    return file.write(output) == output.size() && file.commit();
}
void MediaTasks::startNext() {
    if (m_thread || m_queue.isEmpty()) return;
    m_active = m_queue.dequeue(); const QString id = m_active; const QVariantMap request = job(id);
    m_stop = std::make_shared<std::atomic_bool>(false); const Stop stop = m_stop;
    update(id, {{"state", "running"}, {"message", tr("Scanning files…")}});
    m_thread = QThread::create([this, id, request, stop] { run(id, request, stop); });
    connect(m_thread, &QThread::finished, this, [this] {
        QThread *thread = m_thread; m_thread = nullptr; m_active.clear(); m_stop.reset();
        thread->deleteLater(); emit changed(); startNext();
    });
    m_thread->start();
}
void MediaTasks::run(const QString &id, const QVariantMap &request, const Stop &stop) {
    const bool watching = request.value("kind").toString() == QStringLiteral("watch");
    const QString kind = watching ? QStringLiteral("import") : request.value("kind").toString();
    const QVariantMap options = request.value("options").toMap();
    QVariantList report;
    int failures = 0, completed = 0;
    QElapsedTimer progressTimer; progressTimer.start();
    auto progress = [this, id, &progressTimer](const QVariantMap &fields) {
        if (!fields.contains(QStringLiteral("label")) && progressTimer.elapsed() < 100) return;
        progressTimer.restart();
        QMetaObject::invokeMethod(this, [this, id, fields] { update(id, fields); }, Qt::QueuedConnection);
    };
    struct Source { QString path; QString relative; QString card; };
    QList<Source> sources;
    QSet<QString> seen;
    QSet<QString> matchedSources;
    int searchedFiles=0, matchCount=0;
    QVariantList missing;
    QSet<QString> requestedSources;
    for (const auto &v : options.value("missing").toList()) {
        const auto path=v.toMap().value("path").toString();
        if (!path.isEmpty() && !requestedSources.contains(path)) { missing.append(v); requestedSources.insert(path); }
    }
    const QVariantMap matchRules=RelinkMatcher::normalized(options);
    const QStringList inputs = request.value("paths").toStringList();
    if (inputs.isEmpty() && kind != QStringLiteral("watch")) { // a job with no files can only fail
        const QVariantMap result{{"state", "failed"}, {"message", tr("No files were given.")}, {"percent", 100}};
        QMetaObject::invokeMethod(this, [this, id, result] { update(id, result); emit finished(id, result); }, Qt::QueuedConnection); return;
    }
    if (kind == QStringLiteral("providerManifest")) {
        QFile file(inputs.first()); QString error; QVariantMap descriptor;
        if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
        else if (file.size() > 1024*1024) error = tr("Provider manifest exceeds 1 MB.");
        else { const QJsonDocument json = QJsonDocument::fromJson(file.readAll()); if (!json.isObject()) error = tr("Provider manifest must be a JSON object."); else descriptor = json.toVariant().toMap(); }
        const QVariantMap result{{"state", error.isEmpty() ? "completed" : "failed"}, {"descriptor", descriptor}, {"message", error}, {"percent", 100}};
        QMetaObject::invokeMethod(this, [this, id, result] { update(id, result); emit finished(id, result); }, Qt::QueuedConnection); return;
    }
    if (kind == QStringLiteral("csvImport") || kind == QStringLiteral("csvExport")) {
        QString error; QVariantList records; QStringList headers;
        const QString &path = inputs.first();
        if (kind == QStringLiteral("csvImport")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
            else if (file.size() > 64 * 1024 * 1024) error = tr("CSV exceeds 64 MB.");
            else records = MediaCsv::parse(file.readAll(), &headers, &error);
        } else {
            QSaveFile file(path);
            const QByteArray bytes = MediaCsv::encode(options.value(QStringLiteral("records")).toList(), options.value(QStringLiteral("headers")).toStringList());
            // NOLINTNEXTLINE(bugprone-branch-clone): two different failures, one message source
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) error = file.errorString();
            else if (stop->load()) file.cancelWriting();
            else if (!file.commit()) error = file.errorString();
        }
        const QVariantMap result{{"state", stop->load() ? "cancelled" : error.isEmpty() ? "completed" : "failed"},
            {"percent", 100}, {"records", records}, {"headers", headers}, {"message", error.isEmpty() ? tr("Metadata CSV ready.") : error},
            {"report", QVariantList{QVariantMap{{"source", path}, {"state", error.isEmpty() ? "completed" : "failed"}, {"message", error}}}}};
        QMetaObject::invokeMethod(this, [this, id, result] { update(id, result); emit finished(id, result); }, Qt::QueuedConnection);
        return;
    }
    for (const QString &input : inputs) {
        if (stop->load()) break;
        const QFileInfo info(input);
        if (!info.exists()) { report.append(QVariantMap{{"source", input}, {"state", "failed"}, {"message", tr("Source is missing.")}}); ++failures; continue; }
        QStringList files;
        QString card;
        if (info.isDir()) {
            for (const QString &folder : {QStringLiteral("DCIM"), QStringLiteral("PRIVATE"), QStringLiteral("AVCHD"), QStringLiteral("CONTENTS")})
                if (QDir(input).exists(folder)) { card = info.fileName(); break; }
            QDirIterator walk(input, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (walk.hasNext() && !stop->load()) files.append(walk.next());
            std::sort(files.begin(), files.end());
        } else files.append(info.absoluteFilePath());
        for (const QString &file : files) {
            const QString canonical = QFileInfo(file).canonicalFilePath();
            if (seen.contains(canonical)) continue;
            seen.insert(canonical);
            const QString relative = info.isDir() ? info.fileName() + QLatin1Char('/') + QDir(input).relativeFilePath(file) : QFileInfo(file).fileName();
            sources.append({canonical, relative, card});
        }
    }
    if (watching) {
        const QString key = options.value(QStringLiteral("watchId")).toString() + QLatin1Char(':') + options.value(QStringLiteral("generation")).toString();
        const QVariantMap previous = m_watchFiles.value(key); QVariantMap current;
        QList<Source> stable;
        for (const Source &source : std::as_const(sources)) {
            const QFileInfo file(source.path);
            if (!formats().contains(file.suffix().toLower())) continue;
            const QString signature = QString::number(file.size()) + QLatin1Char(':') + QString::number(file.lastModified().toMSecsSinceEpoch());
            current.insert(source.path, signature);
            if (previous.value(source.path).toString() == signature) stable.append(source);
        }
        m_watchFiles.insert(key, current); sources = stable;
    }
    const QVariantList known = options.value("known").toList();
    QSet<QString> knownPaths;
    QSet<qint64> knownSizes;
    QSet<QString> knownHashes;
    const QString detection = options.value("duplicates", "path").toString();
    for (const QVariant &v : known) {
        const QVariantMap entry = v.toMap(); const QString path = entry.value("path").toString();
        knownPaths.insert(QFileInfo(path).canonicalFilePath()); knownSizes.insert(QFileInfo(path).size());
        if (detection == "hash" && QFileInfo::exists(path)) {
            QString error; const QString hash = digest(path, stop, &error); if (!hash.isEmpty()) knownHashes.insert(hash);
        }
    }
    QHash<qint64, QString> sizeFirst;
    QHash<QString, QString> hashFirst;
    // Proxies and reframes: several clips at once, each by a tool process that
    // works behind the rest of the machine (runsBehind). The batch's threads
    // share only what `shared` locks (the parts done, and the progress
    // throttle); each writes its own slot of `results`, read after the join,
    // so the report lists the clips in their order whichever finished first.
    const bool encodes = kind == "proxy" || kind == "reframe360";
    if (encodes && !sources.isEmpty()) {
        const int count = sources.size();
        const QString threads = QString::number(encodeThreads());
        struct { QMutex mutex; QList<double> parts; int started = 0; } shared;
        shared.parts.fill(0.0, count);
        QList<Encoded> results(count);
        std::atomic_int next{0};
        Claims claims;
        auto percent = [&] { return int(std::accumulate(shared.parts.cbegin(), shared.parts.cend(), 0.0) * 100 / count); };
        auto work = [&] {
            threadRunsBehind();
            for (int i; !stop->load() && (i = next.fetch_add(1)) < count;) {
                const QString path = sources.at(i).path;
                EncodeJob job{path, i, count, options, stop, threads, {}, {}, &claims, &m_yielding};
                job.part = [&, i](double done) { QMutexLocker lock(&shared.mutex); shared.parts[i] = done; progress({{"percent", percent()}}); };
                job.say = [&](const QString &message) { QMutexLocker lock(&shared.mutex); progress({{"message", message}, {"label", QFileInfo(path).fileName()}}); };
                {
                    QMutexLocker lock(&shared.mutex);
                    progress({{"percent", percent()}, {"label", QFileInfo(path).fileName()}, {"message", tr("%1 of %2").arg(++shared.started).arg(count)}});
                }
                Encoded &out = results[i];
                out = kind == "proxy" ? encodeProxy(job) : encodeReframe(job);
                if (!out.done && out.failure.isEmpty()) continue; // stopped
                { QMutexLocker lock(&shared.mutex); shared.parts[i] = 1; }
                if (!out.done || out.destination.isEmpty()) continue;
                // Announced as each is ready, not at the end of the batch.
                if (kind == "proxy") QMetaObject::invokeMethod(this, [this, id, path, destination = out.destination] { emit proxyReady(id, path, destination); }, Qt::QueuedConnection);
                else QMetaObject::invokeMethod(this, [this, id, destination = out.destination, metadata = out.metadata, thumbnail = out.thumbnail] { emit mediaReady(id, destination, metadata, thumbnail); }, Qt::QueuedConnection);
            }
        };
        std::vector<std::thread> helpers;
        for (int j = 0, n = encodesAtOnce(count); j < n; ++j) helpers.emplace_back(work);
        for (auto &helper : helpers) helper.join();
        for (Encoded &out : results) {
            if (!out.failure.isEmpty()) { out.row.insert("state", "failed"); out.row.insert("message", out.failure); ++failures; report.append(out.row); }
            else if (out.done) { report.append(out.row); ++completed; }
        }
    }
    for (int i = 0; !encodes && i < sources.size() && !stop->load(); ++i) {
        const Source &source = sources.at(i); const QFileInfo info(source.path);
        QString path = source.path, hash, error;
        QVariantMap row{{"source", path}, {"state", "completed"}};
        progress({{"percent", i * 100 / qMax(1, sources.size())}, {"label", info.fileName()}, {"message", tr("%1 of %2").arg(i+1).arg(sources.size())}});
        auto failed = [&](const QString &message) { row.insert("state", "failed"); row.insert("message", message); ++failures; report.append(row); };
        if (kind == "import" && !formats().contains(info.suffix().toLower())) {
            row.insert("state", "unsupported"); row.insert("message", tr("Unsupported extension. Supported: %1").arg(formats().join(", "))); report.append(row); ++failures; continue;
        }
        if (kind == "verify" || kind == "duplicates") {
            if (kind == "duplicates" && detection == "size") {
                if (sizeFirst.contains(info.size())) { row.insert("state", "duplicate"); row.insert("destination", sizeFirst.value(info.size())); row.insert("message", tr("Same file size; content is not verified.")); }
                else sizeFirst.insert(info.size(), path);
            } else {
                hash = digest(path, stop, &error);
                if (hash.isEmpty()) { if (!stop->load()) failed(error); continue; }
                row.insert("sha256", hash);
                if (kind == "duplicates") {
                    if (hashFirst.contains(hash)) { row.insert("state", "duplicate"); row.insert("destination", hashFirst.value(hash)); }
                    else hashFirst.insert(hash, path);
                } else {
                    const QVariantMap expected = options.value("checksums").toMap();
                    if (expected.contains(path) && expected.value(path).toString() != hash) { failed(tr("Checksum differs from the previous verification.")); continue; }
                    QMetaObject::invokeMethod(this, [this, id, path, hash] { emit checksumReady(id, path, hash); }, Qt::QueuedConnection);
                }
            }
            report.append(row); ++completed; continue;
        }
        if (kind == "attach" || kind == "relinkApply") {
            const QVariantMap candidates = options.value("originals").toMap();
            const QVariantMap mapping = options.value("mapping").toMap();
            QStringList originals;
            if (kind == "attach") originals.append(options.value("source").toString());
            else for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) if (QFileInfo(it.value().toString()).canonicalFilePath() == path) originals.append(it.key());
            QVariantMap candidate = probeMedia(path, nullptr);
            if (candidate.isEmpty()) { failed(tr("Replacement media could not be decoded.")); continue; }
            const bool recheck=kind=="relinkApply" && options.value("recheckMatch").toBool();
            const auto recheckRules=RelinkMatcher::normalized(options.value("matchOptions").toMap());
            if (recheck && recheckRules.value("matchChecksum").toBool()) {
                hash=digest(path,stop,&error);
                if (hash.isEmpty()) { if (!stop->load()) failed(error); continue; }
                candidate["checksum"]=hash;
            }
            bool valid = true;
            for (const QString &originalPath : originals) {
                const QVariantMap original = candidates.value(originalPath).toMap();
                if (recheck && (!RelinkMatcher::fileMatches(original,path,info.size(),recheckRules) || !RelinkMatcher::metadataMatches(original,candidate,recheckRules))) {
                    failed(tr("Replacement no longer satisfies the search rules. Search again before applying.")); valid=false;break;
                }
                if ((original.value("fps").toDouble() > 0 && std::abs(original.value("fps").toDouble()-candidate.value("fps").toDouble()) > 0.001)
                    || (original.value("duration").toInt() > 0 && original.value("duration").toInt() != candidate.value("duration").toInt())) {
                    failed(tr("Replacement must have the same frame rate and frame count as its source.")); valid = false; break;
                }
            }
            if (!valid || stop->load()) continue;
            row.insert("metadata", candidate); row.insert("destination", path); row.insert("state", "validated");
            if (kind == "attach") {
                if (!candidate.value("hasVideo").toBool()) { failed(tr("A proxy must include video.")); continue; }
                const QString original = originals.value(0);
                QMetaObject::invokeMethod(this, [this, id, original, path] { emit proxyReady(id, original, path); }, Qt::QueuedConnection);
            }
            report.append(row); ++completed; continue;
        }
        if (kind == "relink") {
            ++searchedFiles;
            QVariantList possible;
            for (const auto &v : missing) {
                const auto entry=v.toMap();
                if (RelinkMatcher::fileMatches(entry,path,info.size(),matchRules)) possible.append(entry);
            }
            if (possible.isEmpty()) continue;
            // Probe/hash at most once for this candidate, after inexpensive rules.
            auto candidate=probeMedia(path,nullptr);
            if (candidate.isEmpty()) continue;
            if (matchRules.value("matchChecksum").toBool()) {
                hash=digest(path,stop,&error);
                if (hash.isEmpty()) { if (!stop->load()) failed(error); continue; }
                candidate["checksum"]=hash;
            }
            for (const auto &v : possible) {
                if (stop->load()) break;
                const auto entry=v.toMap();
                if (!RelinkMatcher::metadataMatches(entry,candidate,matchRules)) continue;
                const auto original=entry.value("path").toString();
                const auto evidence=RelinkMatcher::evidence(entry,matchRules);
                report.append(QVariantMap{{"source",original},{"destination",path},{"state","match"},
                    {"metadata",candidate},{"sha256",hash},{"message",tr("Matched: %1").arg(evidence.join(", "))}});
                matchedSources.insert(original); ++matchCount;
            }
            continue;
        }
        if (kind == "import" && (knownPaths.contains(path) || (detection == "size" && knownSizes.contains(info.size())))) {
            row.insert("state", "duplicate"); row.insert("message", detection == "size" ? tr("Same size as existing media; skipped by the selected rule.") : tr("Already in the library.")); report.append(row); continue;
        }
        QImage thumbnail;
        QVariantMap meta;
        if (kind == "import") {
            const QString lut = options.value("metadata").toMap().value("inputLut").toString();
            if (!lut.isEmpty() && (!QFileInfo(lut).isReadable() || QFileInfo(lut).suffix().compare("cube", Qt::CaseInsensitive))) { failed(tr("Choose a readable .cube input LUT.")); continue; }
            meta = probeMedia(path, &thumbnail);
            if (meta.isEmpty()) { failed(tr("This format could not be decoded.")); continue; }
            if (stop->load()) break;
            if (detection == "hash") {
                hash = digest(path, stop, &error);
                if (hash.isEmpty()) { if (!stop->load()) failed(error); continue; }
                if (knownHashes.contains(hash)) { row.insert("state", "duplicate"); row.insert("sha256", hash); report.append(row); continue; }
            }
        }
        const QString mode = kind == "clone" ? QStringLiteral("copy") : options.value("mode", "leave").toString();
        if (mode != "leave" && mode != "copy" && mode != "move") { failed(tr("Unknown import mode.")); continue; }
        if (mode != "leave") {
            const QString name = kind == "clone" ? info.fileName() : safeName(path, options.value("pattern", "{name}").toString(), i+1);
            if (name.isEmpty()) { failed(tr("Naming pattern must contain a filename. Use {name}, {index} and {date}.")); continue; }
            QStringList destinations{options.value("destination").toString()};
            if (kind == "clone" && !options.value("destination2").toString().isEmpty()) destinations.append(options.value("destination2").toString());
            bool ok = true;
            for (const QString &folder : destinations) {
                if (folder.isEmpty()) { error = tr("Choose a destination folder."); ok = false; break; }
                const QString relative = options.value("hierarchy", true).toBool() ? QFileInfo(source.relative).path() + QLatin1Char('/') + name : name;
                const QString target = QDir::cleanPath(QDir(folder).absoluteFilePath(relative));
                // Compare the real files, not the spelling: a destination reached through a
                // symlink or a portal path can be the source itself, and a move would then delete it.
                const QString canonicalTarget = QFileInfo(target).canonicalFilePath();
                if (target == source.path || (!canonicalTarget.isEmpty() && canonicalTarget == QFileInfo(source.path).canonicalFilePath())) {
                    error = tr("Source and destination are the same file."); ok = false; break;
                }
                row.insert("destination", target);
                if (!copyVerified(source.path, target, stop, &hash, &error, [&](qint64 bytes, qint64 total) {
                    progress({{"percent", int((i + (total > 0 ? double(bytes)/total : 0.0))*100/qMax(1, sources.size()))}});
                })) { ok = false; break; }
                if (path == source.path) path = target;
                if (kind == "clone") { row.insert("sha256", hash); report.append(row); }
            }
            if (!ok) { if (!stop->load()) failed(error); continue; }
            if (mode == "move" && !QFile::remove(source.path)) { failed(tr("Copy verified, but the original could not be removed.")); continue; }
        }
        if (!hash.isEmpty()) row.insert("sha256", hash);
        if (kind == "import") {
            meta.insert("path", path); meta.insert("name", QFileInfo(path).fileName());
            const QVariantMap logging = options.value("metadata").toMap();
            for (auto it = logging.constBegin(); it != logging.constEnd(); ++it) meta.insert(it.key(), it.value());
            if (!source.card.isEmpty()) meta.insert("cameraCard", source.card);
            if (!hash.isEmpty()) meta.insert("checksum", hash);
            QMetaObject::invokeMethod(this, [this, id, path, meta, thumbnail] { emit mediaReady(id, path, meta, thumbnail); }, Qt::QueuedConnection);
            knownPaths.insert(source.path); knownPaths.insert(path); knownSizes.insert(info.size()); if (!hash.isEmpty()) knownHashes.insert(hash);
            report.append(row);
        }
        ++completed;
    }
    if (kind == "relink") {
        QSet<QString> unresolved;
        for (const auto &v : missing) {
            const auto entry=v.toMap(); const auto path=entry.value("path").toString();
            if (matchedSources.contains(path) || unresolved.contains(path)) continue;
            unresolved.insert(path);
            const auto reason=RelinkMatcher::missingFacts(entry,matchRules);
            report.append(QVariantMap{{"source",path},{"state","unmatched"},{"message",!reason.isEmpty() ? reason : stop->load() ? tr("Search cancelled before a match was found.") : tr("No candidate satisfied all selected rules.")}});
        }
        const QString state=stop->load() ? "cancelled" : failures ? (matchCount ? "partial" : "failed") : "completed";
        const QString summary=tr("%1 files searched · %2 candidates for %3 sources · %4 unresolved")
            .arg(searchedFiles).arg(matchCount).arg(matchedSources.size()).arg(unresolved.size());
        const QVariantMap result{{"state",state},{"report",report},{"completed",matchedSources.size()},{"failed",failures},
            {"matches",matchCount},{"matchedSources",matchedSources.size()},{"unmatched",unresolved.size()},{"searchedFiles",searchedFiles},
            {"percent",stop->load() ? 0 : 100},{"message",summary},{"options",options}};
        QMetaObject::invokeMethod(this,[this,id,result]{update(id,result);emit finished(id,result);},Qt::QueuedConnection);
        return;
    }
    const QString state = stop->load() ? QStringLiteral("cancelled") : failures ? (completed ? QStringLiteral("partial") : QStringLiteral("failed")) : QStringLiteral("completed");
    const QVariantMap result{{"state", state}, {"report", report}, {"completed", completed}, {"failed", failures},
        {"percent", stop->load() ? 0 : 100}, {"message", failures == 1 ? tr("%1 completed · 1 error").arg(completed) : tr("%1 completed · %2 errors").arg(completed).arg(failures)}, {"options", options}};
    QMetaObject::invokeMethod(this, [this, id, result] { update(id, result); emit finished(id, result); }, Qt::QueuedConnection);
}
