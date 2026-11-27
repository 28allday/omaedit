#include "engines.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMutex>
#include <QSettings>

namespace render {
namespace {

struct Shared {
    QMutex mutex;
    bool tried = false, rankedOnce = false;
    QString name;                   // the first GPU of `ranked`, as it calls itself
    AVBufferRef *device = nullptr;  // FFmpeg's, on the first GPU of `ranked`, until an engine on it is lost or the application ends
    int index = -1;
    std::vector<int> ranked;        // every GPU the engines can use, best first
    QString note;
    // Devices given up after an engine on them was lost. Kept, not freed:
    // decoders may still hold pictures on one, and a device that stopped
    // answering is not one to tear down while the program runs.
    std::vector<AVBufferRef *> retired;
    QElapsedTimer sinceLoss;        // running from the last engine lost
    bool gpuGone = false;           // lost again straight away: the CPU draws from here on
    bool choiceKept = false;        // keepEngineChoice: `chosen` is the choice at start-up
    QString chosen;
};
Shared &shared() {
    static Shared s;
    return s;
}

// A GPU engine lost again this soon after the last one was replaced did not
// come back (a GPU that is gone, or without the memory for the work): the
// CPU twin draws for the rest of the session. A loss long after another (a
// driver reset, a laptop woken) gets a fresh GPU engine each time.
constexpr qint64 kLostAgainMs = 60000;

// Which GPUs the engines can use, best first, found once (under the mutex).
// Which GPU draws is the editor's choice (Preferences, Graphics processor):
// nothing, for the best one found; a GPU's id; or "cpu".
void rank(Shared &s) {
    if (s.rankedOnce) return;
    s.rankedOnce = true;
    const QString chosen = s.choiceKept ? s.chosen : QSettings().value(QStringLiteral("hardware/engineGpu")).toString();
    if (qEnvironmentVariable("OMAEDIT_ENGINE") == QLatin1String("cpu")) {
        s.note = QStringLiteral("CPU engine chosen (OMAEDIT_ENGINE=cpu)");
    } else if (chosen == QLatin1String("cpu")) {
        s.note = QStringLiteral("CPU engine chosen in Preferences");
    } else {
        const auto gpus = engine::listGpus();
        s.ranked = engine::rankGpus(gpus, chosen.toStdString());
        if (s.ranked.empty()) s.note = QStringLiteral("No GPU the engine can use; the CPU engine draws");
        for (const auto &g : gpus)
            if (!s.ranked.empty() && g.index == s.ranked.front()) s.name = QString::fromStdString(g.name);
    }
}

// The shared device closes as the application ends, after the editor's
// engines and decoders have gone and before the program does. A Vulkan
// driver can still be working on a device that is open: NVIDIA's compiles
// shaders on threads of its own, after the call that asked for them has
// returned. Left open, the device met the driver's own clean-up at exit,
// which takes its libraries down under those threads: the program crashed on
// its way out, or never ended (at full CPU, then asleep, until it was
// killed). Closed here, the driver finishes that work and stops it first.
// Anything that asks afterwards gets an engine with a device of its own.
void closeDevice() {
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());
    av_buffer_unref(&s.device);
}

// The process's Vulkan device, made on first use.
AVBufferRef *device(int *index) {
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    if (!s.tried) {
        s.tried = true;
        rank(s);
        if (!s.ranked.empty()) {
            const int first = s.ranked.front();
            std::lock_guard<std::recursive_mutex> loader(engine::loaderMutex());
            if (av_hwdevice_ctx_create(&s.device, AV_HWDEVICE_TYPE_VULKAN, QByteArray::number(first).constData(), nullptr, 0) < 0) {
                s.device = nullptr;
                s.note = QStringLiteral("FFmpeg could not open the GPU for Vulkan; engines open their own");
            } else {
                qAddPostRoutine(closeDevice);
            }
            s.index = first;
            if (!s.name.isEmpty()) s.note = s.name;
        }
    }
    *index = s.index;
    return s.device;
}

}  // namespace

std::unique_ptr<engine::Engine> createEngine() {
    int index = -1;
    AVBufferRef *dev = device(&index);
    std::vector<int> ranked;
    {
        Shared &s = shared();
        QMutexLocker lock(&s.mutex);
        if (!s.gpuGone) ranked = s.ranked;
        // Ours for as long as we try it, whatever becomes of the shared one meanwhile.
        dev = s.device ? av_buffer_ref(s.device) : nullptr;
    }
    std::unique_ptr<engine::Engine> made;
    // The best GPU first, then the next should that one not start an engine.
    for (size_t i = 0; i < ranked.size() && !made; ++i) {
        std::string why;
        if (dev && ranked[i] == index) made = engine::createVulkanEngine(ranked[i], &why, dev);
        // FFmpeg's device would not take an engine, or there is none: one of its own.
        if (!made) made = engine::createVulkanEngine(ranked[i], &why);
    }
    av_buffer_unref(&dev);
    return made ? std::move(made) : engine::createCpuEngine();
}

bool engineLost(const engine::Engine *engine) {
    if (!engine || !engine->lost()) return false;
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    // The device it worked on is not given to another engine or decoder.
    if (s.device && engine->sharedDevice() == s.device->data) {
        s.retired.push_back(s.device);
        s.device = nullptr;
    }
    if (s.sinceLoss.isValid() && s.sinceLoss.elapsed() < kLostAgainMs && !s.gpuGone) {
        s.gpuGone = true;
        s.note = QStringLiteral("The GPU engine was lost twice; the CPU engine draws until the program is started again");
        qWarning("The GPU engine was lost again: drawing on the CPU from here on.");
    } else if (!s.gpuGone) {
        qWarning("The GPU engine was lost: making another.");
    }
    s.sinceLoss.start();
    return true;
}

AVBufferRef *sharedDevice(int *index) { return device(index); }

void keepEngineChoice() {
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    if (s.rankedOnce || s.choiceKept) return;
    s.chosen = QSettings().value(QStringLiteral("hardware/engineGpu")).toString();
    s.choiceKept = true;
}

int engineGpu() {
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    rank(s);
    return s.ranked.empty() ? -1 : s.ranked.front();
}

QString engineDevice() {
    int index = -1;
    device(&index);
    QMutexLocker lock(&shared().mutex);
    return shared().note;
}

}  // namespace render
