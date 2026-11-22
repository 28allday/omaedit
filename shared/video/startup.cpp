#include "startup.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
namespace SuiteVideo {
namespace {
constexpr int frames = 16;
QVariantList samples(const AVFrame *frame) {
    QVariantList result;
    const auto *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc || desc->nb_components < 3 || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        return result;
    for (int component = 0; component < 3; ++component) {
        const int w = component ? AV_CEIL_RSHIFT(frame->width, desc->log2_chroma_w) : frame->width;
        const int h = component ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 8; ++x) {
                uint32_t value = 0;
                av_read_image_line2(&value, const_cast<const uint8_t **>(frame->data), frame->linesize, desc,
                                    (2 * x + 1) * w / 16, (2 * y + 1) * h / 12, component, 1, 0,
                                    sizeof(value));
                result.append(int(value * 65535ULL / ((1ULL << desc->comp[component].depth) - 1)));
            }
    }
    return result;
}
bool agrees(const QVariantMap &cpu, const QVariantMap &candidate, int tolerance = 514) {
    const auto a = cpu.value("samples").toList(), b = candidate.value("samples").toList();
    if (a.isEmpty() || a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (std::abs(a[i].toInt() - b[i].toInt()) > tolerance)
            return false;
    return true;
}
bool faster(const QVariantMap &cpu, const QVariantMap &candidate) {
    const auto a = cpu.value("batchMs").toList(), b = candidate.value("batchMs").toList();
    if (a.size() != 3 || b.size() != 3)
        return false;
    double fastestCpu = 1e9, slowestGpu = 0;
    for (int i = 0; i < 3; ++i) {
        if (a[i].toDouble() <= 0 || b[i].toDouble() <= 0)
            return false;
        fastestCpu = std::min(fastestCpu, a[i].toDouble());
        slowestGpu = std::max(slowestGpu, b[i].toDouble());
    }
    return slowestGpu < fastestCpu * .85;
}
void removeSamples(QVariantMap &report) {
    report.remove("samples");
    if (report.contains("timeline")) {
        auto timeline = report.value("timeline").toMap();
        timeline.remove("samples");
        report["timeline"] = timeline;
    }
}
QString cacheKey(const QString &preference, const QString &device, const QString &drawingDevice) {
    // The Vulkan devices that offer decoding belong in it too: a driver or
    // its settings can add or hide the video queues. And the device that
    // draws: it is the first tried.
    QByteArray identity = preference.toUtf8() + '\n' + device.toUtf8() + '\n' + vulkanDecodeDevices().join(',').toUtf8()
                        + '\n' + drawingDevice.toUtf8();
    QStringList paths{QCoreApplication::applicationFilePath(),
                      "/usr/lib/libcuda.so.1",
                      "/usr/lib/libavcodec.so",
                      "/usr/lib/libavformat.so",
                      "/usr/lib/libavutil.so"};
    for (const auto &name : QDir("/usr/lib/dri").entryList(QDir::Files, QDir::Name))
        paths.append("/usr/lib/dri/" + name);
    for (const auto &name :
         QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name)) {
        paths.append("/dev/dri/" + name);
        paths.append("/sys/class/drm/" + name + "/device/driver/module");
    }
    for (const auto &path : paths) {
        const QFileInfo info(path);
        identity += '\n' + info.canonicalFilePath().toUtf8() + ':' + QByteArray::number(info.size()) + ':' +
                    QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + ':' +
                    QByteArray::number(info.isReadable());
    }
    for (const auto *path :
         {"/proc/sys/kernel/osrelease", "/sys/module/nvidia/version", "/sys/module/amdgpu/srcversion"}) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            identity += '\n' + file.readAll();
    }
    for (const auto *name : {"LIBVA_DRIVER_NAME", "LIBVA_DRIVERS_PATH", "CUDA_VISIBLE_DEVICES",
                             "NVIDIA_VISIBLE_DEVICES", "DRI_PRIME", "LD_LIBRARY_PATH", "LD_PRELOAD"})
        identity += '\n' + QByteArray(name) + '=' + qgetenv(name);
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}
QVariantMap probe(const DecodeOptions &options, int timeout) {
    QVariantMap result{{"backend", options.backend}, {"device", options.device}, {"valid", false}};
    if (const auto restriction = hardwareRestriction(options); !restriction.isEmpty()) {
        result["error"] = restriction;
        return result;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        result["error"] = "Cannot create decoder-check temporary storage.";
        return result;
    }
    QProcess child;
    auto env = QProcessEnvironment::systemEnvironment();
    // The parent owns cleanup even if a driver call in the child is killed.
    env.insert("TMPDIR", scratch.path());
    env.insert("MANGOHUD", "0");
    env.insert("DISABLE_MANGOHUD", "1");
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(),
                {"--omaedit-decode-probe", options.backend, options.device});
    if (!child.waitForStarted(std::min(1000, timeout)) || !child.waitForFinished(timeout)) {
        child.kill();
        child.waitForFinished(500);
        result["error"] = "Decoder check timed out or could not start.";
        return result;
    }
    const auto bytes = child.readAllStandardOutput();
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0 || bytes.size() > 262144) {
        result["error"] = "Decoder check failed.";
        return result;
    }
    const auto output = QJsonDocument::fromJson(bytes).toVariant().toMap();
    if (!output.value("valid").toBool()) {
        result["error"] = "Decoder check returned no valid frames.";
        return result;
    }
    for (auto it = output.begin(); it != output.end(); ++it)
        result[it.key()] = it.value();
    return result;
}
} // namespace
QString normalizedDecodePreference(const QString &value) {
    if (value == "software" || value == "vulkan" || value == "vaapi" || value == "nvdec")
        return value;
    return "auto";
}
QVariantMap benchmarkDecode(const QString &path, const DecodeOptions &options) {
    QVariantMap report{{"valid", false},
                       {"backend", options.backend},
                       {"device", options.device},
                       {"fixture", "H.264 1280x720, 16 frames at 25 fps"}};
    QVariantList timings, reference;
    QString error;
    for (int pass = 0; pass < 3; ++pass) {
        QElapsedTimer timer;
        timer.start();
        Decoder decoder;
        if (!decoder.open(path, options, &error)) {
            report["error"] = error;
            return report;
        }
        for (int index = 0; index < frames; ++index) {
            const auto *frame = decoder.frame(index / 25., &error);
            if (!frame) {
                report["error"] = error;
                return report;
            }
            if (pass == 0)
                reference.append(samples(frame));
        }
        timings.append(timer.nsecsElapsed() / 1e6);
        report["decoder"] = decoder.information();
        if (options.backend != "software" &&
            (decoder.information().value("hardwareFrames").toInt() == 0 ||
             decoder.information().value("active").toString() != options.backend ||
             decoder.information().value("fallbacks").toInt() > 0)) {
            report["error"] = "The requested device did not decode every sample without fallback.";
            return report;
        }
    }
    auto sorted = timings;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.toDouble() < b.toDouble(); });
    report["samples"] = reference;
    report["batchMs"] = timings;
    report["medianMs"] = sorted[1];
    report["valid"] = !reference.isEmpty();
    return report;
}
int decodeProbeMain(int argc, char **argv, DecodeProbeExtension extension) {
    if (argc < 3 || QByteArray(argv[1]) != "--omaedit-decode-probe")
        return -1;
    const QString backend = QString::fromUtf8(argv[2]);
    if (backend != "software" && backend != "vulkan" && backend != "vaapi" && backend != "nvdec")
        return 2;
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    const auto path = directory.filePath("qualification.mp4");
    if (!directory.isValid() || !QFile::copy(":/suite-video/qualification.mp4", path))
        return 2;
    DecodeOptions options;
    options.backend = backend;
    options.device = argc > 3 ? QString::fromUtf8(argv[3]) : QString();
    options.ioTimeoutMs = 2000;
    auto report = benchmarkDecode(path, options);
    if (extension && report.value("valid").toBool())
        report["timeline"] = extension(path, options);
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly))
        return 2;
    output.write(QJsonDocument::fromVariant(report).toJson(QJsonDocument::Compact));
    output.write("\n");
    output.flush();
    return report.value("valid").toBool() ? 0 : 2;
}
QVariantMap decodeStartup() { return qApp->property("omaedit.decodeStartup").toMap(); }
QStringList vulkanCandidates(QStringList decodeDevices, const QString &drawingDevice) {
    if (!drawingDevice.isEmpty() && decodeDevices.removeAll(drawingDevice) > 0) decodeDevices.prepend(drawingDevice);
    return decodeDevices;
}
QVariantMap configureDecoding(const QString &drawingDevice) {
    const auto saved =
        normalizedDecodePreference(QSettings().value("hardware/videoDecoder", "auto").toString());
    const auto preference = normalizedDecodePreference(qEnvironmentVariable("OMAEDIT_VIDEO_DECODER", saved));
    const auto device = qEnvironmentVariable(
        "OMAEDIT_VIDEO_DECODER_DEVICE", QSettings().value("hardware/videoDecoderDevice").toString());
    QVariantMap report{{"schemaVersion", 1},
                       {"preference", preference},
                       {"savedPreference", saved},
                       {"requestedDevice", device},
                       {"drawingDevice", drawingDevice},
                       {"selected", "software"}};
    report["inheritedVaapiDriver"] = qEnvironmentVariable("LIBVA_DRIVER_NAME");
    report["vaapiDriverSelection"] = "automatic per render node; NVIDIA uses NVDEC";
    // libva's global override can route an AMD render node through NVIDIA's
    // wrapper. Let libva identify the driver for each explicitly chosen node.
    // This changes only OmaEdit's environment, before its media threads start.
    qunsetenv("LIBVA_DRIVER_NAME");
    const auto finish = [&](const DecodeOptions &options, const QString &reason) {
        setDefaultDecodeOptions(options);
        report["selected"] = options.backend;
        report["device"] = options.device;
        report["reason"] = reason;
        qApp->setProperty("omaedit.decodeStartup", report);
        return report;
    };
    if (preference == "software")
        return finish({}, "Software decoding preference.");
    if (device.toUtf8().size() >= 128 || device.contains('&') || device.contains('?') ||
        device.contains('=') || device.contains('\\'))
        return finish({}, "Invalid decoder device setting; software retained.");
    const auto key = cacheKey(preference, device, drawingDevice);
    const auto cached = QSettings().value("hardware/decodeQualification").toMap();
    const auto age = QDateTime::currentSecsSinceEpoch() - cached.value("checkedAt").toLongLong();
    if (cached.value("key").toString() == key && age >= 0 && age < 86400) {
        const auto previous = cached.value("report").toMap();
        const auto backend = previous.value("selected").toString();
        if (backend == "software" || backend == "vulkan" || backend == "vaapi" || backend == "nvdec") {
            const auto launchContext = report;
            report = previous;
            for (auto it = launchContext.begin(); it != launchContext.end(); ++it)
                report[it.key()] = it.value();
            report["cached"] = true;
            report["cacheAgeSeconds"] = age;
            DecodeOptions options;
            options.backend = backend;
            options.device = previous.value("device").toString();
            return finish(options, previous.value("reason").toString());
        }
    }
    QElapsedTimer deadline;
    deadline.start();
    // Each check is a child process (about 2.3 s to start on a Ryzen 9
    // 5950X): a 2 s reference budget failed there before any decoder was
    // tried. The whole qualification stays inside the 7 s deadline below.
    auto cpu = probe({}, 4000);
    if (!cpu.value("valid").toBool() ||
        (cpu.contains("timeline") && !cpu.value("timeline").toMap().value("valid").toBool()))
        return finish({}, "Software reference check failed; existing software playback retained.");
    QList<DecodeOptions> candidates;
    // Vulkan Video first, on any GPU that offers it; the other interfaces
    // only follow for what it cannot decode (or where no GPU offers it).
    if (preference == "auto" || preference == "vulkan") {
        QStringList devices = vulkanCandidates(vulkanDecodeDevices(), drawingDevice);
        if (preference == "vulkan" && !device.isEmpty())
            devices = {device};
        for (const auto &index : devices.mid(0, 2)) {
            DecodeOptions options;
            options.backend = "vulkan";
            options.device = index;
            candidates.append(options);
        }
    }
    if (preference == "auto" || preference == "vaapi") {
        QStringList nodes;
        if (preference == "vaapi" && !device.isEmpty())
            nodes.append(device);
        else
            for (const auto &name :
                 QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name))
                nodes.append("/dev/dri/" + name);
        for (const auto &node : nodes.mid(0, 3)) {
            DecodeOptions options;
            options.backend = "vaapi";
            options.device = node;
            candidates.append(options);
        }
    }
    if (preference == "auto" || preference == "nvdec") {
        DecodeOptions options;
        options.backend = "nvdec";
        options.device = preference == "nvdec" && !device.isEmpty() ? device : QString("0");
        candidates.append(options);
    }
    QVariantList checked;
    DecodeOptions selected;
    for (const auto &candidate : candidates) {
        // A qualified Vulkan decoder is the choice; the others are fallbacks.
        if (selected.backend == "vulkan" && candidate.backend != "vulkan")
            break;
        const int remaining = 7000 - int(deadline.elapsed());
        if (remaining <= 0)
            break;
        auto result = probe(candidate, std::min(4000, remaining));
        const bool timelineValid =
            !cpu.contains("timeline") ||
            (result.value("timeline").toMap().value("valid").toBool() &&
             agrees(cpu.value("timeline").toMap(), result.value("timeline").toMap(), 771));
        const bool valid = result.value("valid").toBool() && agrees(cpu, result) && timelineValid;
        result["agreesWithSoftware"] = valid;
        if (valid) {
            const bool benefit = faster(cpu, result) &&
                                 (!cpu.contains("timeline") ||
                                  faster(cpu.value("timeline").toMap(), result.value("timeline").toMap()));
            result["consistentlyFaster"] = benefit;
            // Hardware that decodes correctly is used, in discovery order
            // (Vulkan first), whether or not it beats the CPU on this small
            // sample: it frees the CPU, and its pictures stay on the GPU.
            if (selected.backend == "software")
                selected = candidate;
        }
        removeSamples(result);
        checked.append(result);
    }
    removeSamples(cpu);
    report["softwareReference"] = cpu;
    report["candidates"] = checked;
    report["elapsedMs"] = deadline.elapsed();
    const auto completed = finish(
        selected,
        selected.backend == "software"
            ? "Software retained: no hardware decoder passed the numerical checks."
            : "Hardware passed numerical checks. Individual formats can fall back to software.");
    QSettings().setValue(
        "hardware/decodeQualification",
        QVariantMap{{"key", key}, {"checkedAt", QDateTime::currentSecsSinceEpoch()}, {"report", completed}});
    return completed;
}
} // namespace SuiteVideo
