#include "hardwareprobe.h"
#include "vulkaninventory.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTemporaryDir>
#include <memory>

namespace SuiteHardware {
HardwareProbe::HardwareProbe(QObject *parent) : QObject(parent) {
    m_state = {{"schemaVersion", schemaVersion}, {"encoderBusy", false}, {"gpuBusy", false}};
}
HardwareProbe::~HardwareProbe() {
    // Do not deliver callbacks into partially destroyed state.
    for (auto *p : findChildren<QProcess *>()) {
        p->disconnect(this);
        if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(1000); }
    }
}
void HardwareProbe::publish() {
    m_state["encoderBusy"] = m_encoderBusy;
    m_state["gpuBusy"] = m_gpuBusy;
    m_state["encoders"] = m_encoders;
    m_state["encoderDetails"] = m_details;
    emit changed();
}
void HardwareProbe::run(const QString &program, const QStringList &args, int timeoutMs,
                        const std::function<void(Result)> &callback) {
    auto *p = new QProcess(this);
    auto *timer = new QTimer(p);
    timer->setSingleShot(true);
    struct State { QByteArray out, err; bool done = false, timedOut = false; };
    auto state = std::make_shared<State>();
    const auto read = [p, state] {
        state->out += p->readAllStandardOutput(); state->out.truncate(256 * 1024);
        state->err += p->readAllStandardError(); state->err = state->err.right(8192);
    };
    const auto finish = [p, timer, state, read, callback](bool ok, const QString &error) {
        if (state->done) return;
        state->done = true; timer->stop(); read();
        Result result{ok && !state->timedOut, state->out,
                      state->timedOut ? QStringLiteral("Probe timed out") : error};
        if (!result.ok && result.error.isEmpty()) result.error = QString::fromUtf8(state->err).trimmed();
        if (!result.ok && result.error.isEmpty()) result.error = QStringLiteral("Probe failed");
        p->deleteLater(); callback(result);
    };
    connect(p, &QProcess::readyReadStandardOutput, this, read);
    connect(p, &QProcess::readyReadStandardError, this, read);
    connect(timer, &QTimer::timeout, this, [p, state] { state->timedOut = true; p->kill(); });
    connect(p, &QProcess::errorOccurred, this, [p, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(false, p->errorString());
    });
    connect(p, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0,
               status == QProcess::CrashExit ? QStringLiteral("Probe process crashed") : QString());
    });
    timer->start(qMax(1, timeoutMs));
    p->start(program, args, QIODevice::ReadOnly);
}
QStringList HardwareProbe::renderNodes() {
    QStringList out;
#ifdef Q_OS_LINUX
    const QDir dri("/dev/dri");
    for (const auto &name : dri.entryList({"renderD*"}, QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
        out.append(dri.absoluteFilePath(name));
#endif
    return out;
}
QSet<QString> HardwareProbe::codecNames(const QByteArray &listing) {
    QSet<QString> names;
    static const QRegularExpression row(QStringLiteral("^\\s*[VAS][.A-Z]{5}\\s+([a-zA-Z0-9_-]+)\\s"));
    for (const auto &line : QString::fromUtf8(listing).split('\n')) {
        const auto m = row.match(line);
        if (m.hasMatch()) names.insert(m.captured(1));
    }
    return names;
}
void HardwareProbe::probeGpu(const QString &executable, int timeoutMs) {
    if (m_gpuBusy) return;
    m_gpuBusy = true; publish();
    run(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable,
        {"--suite-gpu-probe"}, timeoutMs, [this](const Result &r) {
        const auto value = QJsonDocument::fromJson(r.output).toVariant().toMap();
        if (r.ok && value.value("schemaVersion").toInt() == schemaVersion
            && value.value("devices").metaType().id() == QMetaType::QVariantList
            && value.value("available").metaType().id() == QMetaType::Bool
            && value.value("cpuFallback").toBool()
            && value.value("processingBackend").toString() == "cpu")
            m_state["gpu"] = value;
        else m_state["gpu"] = QVariantMap{{"available", false}, {"devices", QVariantList{}},
                {"preferredBackend", "vulkan"}, {"processingBackend", "cpu"}, {"cpuFallback", true},
                {"reason", r.ok ? "Invalid GPU probe response" : r.error}};
        m_state["gpuCheckedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_gpuBusy = false; publish();
    });
}
void HardwareProbe::probeEncoders() { probeEncoders(Options{}); }
void HardwareProbe::probeEncoders(const Options &options) {
    if (m_encoderBusy) return;
    m_options = options;
    if (m_options.ffmpeg.isEmpty()) m_options.ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (m_options.discoverRenderNodes) m_options.renderNodes = renderNodes();
    m_options.renderNodes.removeAll({}); m_options.renderNodes.removeDuplicates();
    m_encoderBusy = true; m_jobs.clear(); m_details.clear();
    m_encoders = {{"nvenc", "probing"}, {"vaapi", "probing"}, {"vulkan", "probing"}};
    m_state.remove("decoderDetails"); m_state.remove("ffmpegVersion"); m_state.remove("encoderError");
    // Keep the legacy encoder map empty until software listing is available.
    // Consumers can distinguish an unfinished listing from a confirmed absence.
    m_state["encoderBusy"] = true; emit changed();
    run(m_options.ffmpeg.isEmpty() ? "/nonexistent/ffmpeg" : m_options.ffmpeg,
        {"-hide_banner", "-encoders"}, m_options.timeoutMs, [this](const Result &r) {
        const auto have = codecNames(r.output);
        m_encoders["ffmpeg"] = r.ok && !have.isEmpty();
        if (!r.ok || have.isEmpty()) {
            m_state["encoderError"] = r.ok ? "FFmpeg returned no encoder listing" : r.error;
            finishEncoders(); return;
        }
        for (const auto &name : {"libx264", "libx265", "libsvtav1", "libvpx-vp9", "prores_ks", "dnxhd", "mpeg2video", "ffv1",
                                 "aac", "libopus", "libmp3lame", "flac", "pcm_s16le", "pcm_s24le", "libvorbis"})
            m_encoders[name] = have.contains(name);
        m_encoders["renderNodes"] = m_options.renderNodes;
        for (const auto &name : {"h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_vaapi", "hevc_vaapi", "av1_vaapi", "prores_ks_vulkan"}) {
            const QString encoder(name);
            m_encoders[encoder] = false;
            if (have.contains(encoder)) {
                if (encoder.endsWith("_nvenc") || encoder.endsWith("_vulkan")) m_jobs.append({encoder, {}});
                else for (const auto &device : m_options.renderNodes) m_jobs.append({encoder, device});
            }
            if (!have.contains(encoder) || (encoder.endsWith("_vaapi") && m_options.renderNodes.isEmpty()))
                m_details.append(QVariantMap{{"encoder", encoder}, {"device", ""}, {"available", false},
                      {"reason", have.contains(encoder) ? "No VA-API render nodes found" : "Not compiled into FFmpeg"}});
        }
        publish();
        // Decode support is reported as compiled, not usable or enabled.
        run(m_options.ffmpeg, {"-hide_banner", "-decoders"}, m_options.timeoutMs, [this](const Result &r) {
            const auto names = codecNames(r.output);
            QStringList nvdec;
            for (const auto &name : names) if (name.endsWith("_cuvid")) nvdec.append(name);
            nvdec.sort();
            m_state["decoderDetails"] = QVariantMap{{"listed", r.ok}, {"nvdecCompiled", nvdec},
                {"playbackBackend", "cpu"}, {"hardwareDecodeTested", false}, {"error", r.error}};
            run(m_options.ffmpeg, {"-hide_banner", "-hwaccels"}, m_options.timeoutMs, [this](const Result &r) {
                QStringList methods;
                static const QSet<QString> known{"vdpau", "cuda", "vaapi", "qsv", "drm", "opencl", "vulkan", "d3d11va", "dxva2", "videotoolbox", "mediacodec", "amf"};
                for (const auto &line : QString::fromUtf8(r.output).split('\n'))
                    if (known.contains(line.trimmed())) methods.append(line.trimmed());
                auto dec = m_state.value("decoderDetails").toMap();
                dec["compiledMethods"] = methods; dec["methodsListed"] = r.ok;
                if (!r.ok) dec["error"] = r.error;
                m_state["decoderDetails"] = dec;
                run(m_options.ffmpeg, {"-version"}, m_options.timeoutMs, [this](const Result &r) {
                    m_state["ffmpegVersion"] = r.ok ? QString::fromUtf8(r.output).section('\n', 0, 0) : QString();
                    nextEncoder();
                });
            });
        });
    });
}
void HardwareProbe::nextEncoder() {
    if (m_jobs.isEmpty()) { probeDecoders(); return; }
    const auto job = m_jobs.takeFirst();
    QStringList args{"-hide_banner", "-nostdin", "-loglevel", "error"};
    if (job.encoder.endsWith("_vulkan")) args << "-init_hw_device" << "vulkan=omaedit" << "-filter_hw_device" << "omaedit";
    if (!job.device.isEmpty()) args << "-vaapi_device" << job.device;
    args << "-f" << "lavfi" << "-i" << "color=black:s=256x256:d=0.2:r=25";
    if (!job.device.isEmpty()) args << "-vf" << "format=nv12,hwupload";
    if (job.encoder.endsWith("_vulkan")) args << "-vf" << "format=yuv422p10le,hwupload" << "-profile:v" << "0";
    args << "-c:v" << job.encoder << "-frames:v" << "2" << "-f" << "null" << "-";
    run(m_options.ffmpeg, args, m_options.timeoutMs, [this, job](const Result &r) {
        m_details.append(QVariantMap{{"encoder", job.encoder}, {"device", job.device}, {"available", r.ok}, {"reason", r.error}});
        if (r.ok) {
            m_encoders[job.encoder] = true;
            if (!job.device.isEmpty()) {
                auto preferred = m_encoders.value("recommendedDevices").toMap();
                if (!preferred.contains(job.encoder)) preferred[job.encoder] = job.device;
                m_encoders["recommendedDevices"] = preferred;
            }
        }
        publish(); nextEncoder();
    });
}
void HardwareProbe::probeDecoders() {
    auto dec = m_state.value("decoderDetails").toMap();
    dec["runtimeTests"] = QVariantList{};
    m_decodeJobs.clear();
    const auto methods = dec.value("compiledMethods").toStringList();
    if (methods.contains("vaapi"))
        for (const auto &node : m_options.renderNodes) m_decodeJobs.append({"vaapi", node});
    if (methods.contains("cuda")) m_decodeJobs.append(Job{"cuda", {}});
    m_state["decoderDetails"] = dec;
    if (m_decodeJobs.isEmpty()) { finishEncoders(); return; }
    if (!m_encoders.value("libx264").toBool()) {
        dec["fixtureError"] = "H.264 decoder test needs the FFmpeg libx264 software encoder";
        m_state["decoderDetails"] = dec; finishEncoders(); return;
    }
    m_decodeDir = std::make_unique<QTemporaryDir>();
    if (!m_decodeDir->isValid()) {
        dec["fixtureError"] = "Could not create temporary decoder-test media";
        m_state["decoderDetails"] = dec; finishEncoders(); return;
    }
    const auto path = m_decodeDir->filePath("decode.mp4");
    run(m_options.ffmpeg, {"-hide_banner", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i",
        "testsrc2=s=256x256:r=25", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-threads", "1", "-frames:v", "2", "-y", path},
        m_options.timeoutMs, [this, path](const Result &r) {
        if (!r.ok || QFileInfo(path).size() <= 0) {
            auto dec = m_state.value("decoderDetails").toMap();
            dec["fixtureError"] = r.ok ? "Decoder-test media was not created" : r.error;
            m_state["decoderDetails"] = dec; finishEncoders(); return;
        }
        nextDecoder();
    });
}
void HardwareProbe::nextDecoder() {
    if (m_decodeJobs.isEmpty()) { finishEncoders(); return; }
    const auto job = m_decodeJobs.takeFirst();
    QStringList args{"-hide_banner", "-nostdin", "-loglevel", "error", "-hwaccel", job.encoder};
    if (!job.device.isEmpty()) args << "-hwaccel_device" << job.device;
    // Require hardware frames and explicitly download them: a silent software
    // decode cannot pass this filter chain. Vulkan is not used as a codec proxy.
    args << "-hwaccel_output_format" << job.encoder << "-i" << m_decodeDir->filePath("decode.mp4")
         << "-vf" << "hwdownload,format=nv12" << "-frames:v" << "2" << "-f" << "null" << "-";
    run(m_options.ffmpeg, args, m_options.timeoutMs, [this, job](const Result &r) {
        auto dec = m_state.value("decoderDetails").toMap();
        auto tests = dec.value("runtimeTests").toList();
        tests.append(QVariantMap{{"backend", job.encoder == "cuda" ? "nvdec" : job.encoder},
            {"codec", "h264"}, {"device", job.device}, {"available", r.ok}, {"reason", r.error}});
        dec["runtimeTests"] = tests; dec["hardwareDecodeTested"] = true;
        m_state["decoderDetails"] = dec; publish(); nextDecoder();
    });
}
void HardwareProbe::finishEncoders() {
    m_decodeDir.reset(); m_decodeJobs.clear();
    m_encoders["vulkan"] = m_encoders.value("prores_ks_vulkan").toBool() ? "yes" : "no";
    for (const auto &backend : {"nvenc", "vaapi"}) {
        bool any = false;
        for (const auto &codec : {"h264", "hevc", "av1"})
            any |= m_encoders.value(QString(codec) + '_' + backend).toBool();
        m_encoders[backend] = any ? "yes" : "no";
    }
    m_state["encoderCheckedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_encoderBusy = false; publish();
}
}
