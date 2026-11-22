#pragma once
#include "compute.h"
#include "shadercompiler.h"
#include <QImage>
#include <QHash>
#include <atomic>
class ViewerGpu {
public:
    struct Options {SuiteProcessing::ComputeContext::Options compute;QString compiler;qsizetype minimumPixels=262144;};
    ViewerGpu();
    explicit ViewerGpu(const Options &options);
    // Worker-only. Input is already validated, tightly packed working RGBA32F.
    QImage convert(const QImage &image,const QVariantMap &options,const std::atomic<bool> &cancelled,
                   QVariantMap *diagnostics,QString *error);
    QImage convertSignal(const QImage &image,const QString &from,const QString &to,
                         const std::atomic<bool> &cancelled,QVariantMap *diagnostics,QString *error);
    void resetDecisions();
    QVariantMap information() const;
private:
    QImage convertOperation(const QImage &image,const QString &from,const QString &to,const QString &view,
                            const std::function<QImage()> &cpu,const std::atomic<bool> &cancelled,
                            QVariantMap *diagnostics,QString *error);
    struct Decision {QVariantMap report;int slowRuns=0;};
    struct GpuImage {QImage image;QString backend,reason,error;};
    GpuImage apply(const QImage &image,const SuiteProcessing::ComputeStage &stage);
    Options m_options;
    SuiteProcessing::ShaderCompiler m_compiler;
    SuiteProcessing::ComputeContext m_context;
    QHash<QString,SuiteProcessing::ComputeStage> m_stages;
    QHash<QString,Decision> m_decisions;
    quint64 m_calibrations=0,m_demotions=0;
};
