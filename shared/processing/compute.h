#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <mutex>
namespace SuiteProcessing {
// Point operations on tightly packed, finite straight RGBA32F. CPU callbacks
// must return an owning buffer with the same size and preserve coverage.
struct ComputeStage {
    QString name;
    QByteArray spirv;
    std::function<QByteArray(const QByteArray &,QString *)> cpu;
    QString unavailableReason;
};
struct ComputeResult {
    QByteArray pixels;
    QString backend="cpu";
    QString fallbackReason;
    QString error;
};
class ComputeContext {
public:
    enum class Preference { Cpu, Vulkan };
    struct Options {
        QString loader;
        int deviceIndex=-1;
        int waitMilliseconds=5000;
        bool preferCachedStaging=true;
        // GPU allocation budget; larger valid images retain the CPU path.
        qsizetype maximumImageBytes=256*1024*1024;
        // Held while this context's Vulkan instance and device are made and
        // while they are destroyed, when the program has other Vulkan users:
        // the loader has crashed when entered from two threads at once there.
        // The mutex must outlive the context.
        std::recursive_mutex *loaderLock=nullptr;
    };
    ComputeContext();
    explicit ComputeContext(const Options &options);
    ~ComputeContext();
    ComputeContext(const ComputeContext &)=delete;
    ComputeContext &operator=(const ComputeContext &)=delete;
    // Blocking, single-thread-owned context. Inputs stay alive through fallback.
    ComputeResult process(const QByteArray &input,quint32 width,quint32 height,
                          const QList<ComputeStage> &stages,Preference preference=Preference::Vulkan);
    QVariantMap information() const;
private:
    struct State;
    std::unique_ptr<State> d;
};
// GLSL function/helper text must define vec4 suitePixel(vec4). The wrapper owns
// descriptor bindings, preserves alpha and flags nonfinite output on the GPU.
QString pointShader(const QString &functionText);
}
