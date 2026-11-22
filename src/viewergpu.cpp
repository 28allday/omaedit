#include "viewergpu.h"
#include "gpuprocessing.h"
#include "colourpipeline.h"
#include "selection.h"
#include <QElapsedTimer>
#include <cmath>
#include <memory>
namespace {
bool matches(const QImage &gpu,const QImage &cpu,const QImage &input) {
    if(gpu.size()!=cpu.size() || gpu.format()!=QImage::Format_RGBA32FPx4 || cpu.format()!=gpu.format())return false;
    for(int y=0;y<gpu.height();++y) {
        const auto *a=reinterpret_cast<const float*>(gpu.constScanLine(y)),*b=reinterpret_cast<const float*>(cpu.constScanLine(y)),*c=reinterpret_cast<const float*>(input.constScanLine(y));
        for(int x=0;x<gpu.width()*4;++x) {
            if(!std::isfinite(a[x]) || !std::isfinite(b[x]))return false;
            if(x%4==3){if(a[x]!=c[x] || b[x]!=c[x])return false;}
            else if(qAbs(double(a[x])-b[x])>3e-5*qMax(1.,qAbs(double(b[x]))))return false;
        }
    }
    return true;
}
}
ViewerGpu::ViewerGpu():ViewerGpu(Options{}){}
ViewerGpu::ViewerGpu(const Options &options):m_options(options),m_compiler(options.compiler),m_context(options.compute){}
void ViewerGpu::resetDecisions(){m_decisions.clear();m_stages.clear();}
QVariantMap ViewerGpu::information() const {
    auto info=m_context.information();info["decisionCount"]=m_decisions.size();info["stageCount"]=m_stages.size();info["calibrations"]=m_calibrations;info["runtimeDemotions"]=m_demotions;return info;
}
ViewerGpu::GpuImage ViewerGpu::apply(const QImage &image,const SuiteProcessing::ComputeStage &stage) {
    const auto input=QByteArray::fromRawData(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes());
    auto computed=m_context.process(input,image.width(),image.height(),{stage});
    GpuImage result;result.backend=computed.backend;result.reason=computed.fallbackReason;result.error=computed.error;
    if(!computed.error.isEmpty() || computed.pixels.isEmpty())return result;
    auto storage=std::make_unique<QByteArray>(std::move(computed.pixels));
    result.image=QImage(reinterpret_cast<const uchar*>(storage->constData()),image.width(),image.height(),image.width()*16,QImage::Format_RGBA32FPx4,[](void *data){delete static_cast<QByteArray*>(data);},storage.get());
    if(!result.image.isNull())(void)storage.release();
    else result.error="Cannot allocate the Vulkan colour image wrapper.";
    for(const auto &key:image.textKeys())result.image.setText(key,image.text(key));
    result.image.setDotsPerMeterX(image.dotsPerMeterX());result.image.setDotsPerMeterY(image.dotsPerMeterY());
    result.image.setDevicePixelRatio(image.devicePixelRatio());
    result.image.setOffset(image.offset());
    return result;
}
QImage ViewerGpu::convert(const QImage &image,const QVariantMap &options,const std::atomic<bool> &cancelled,QVariantMap *diagnostics,QString *error) {
    if(error)error->clear();
    auto view=options;view["from"]="rec709";view["fromGamma"]="linear";
    const auto cpu=[&]{return ColourPipeline::display(image,view,error);};
    return convertOperation(image,"oma_interchange",ColourPipeline::spaceName(options.value("gamut","srgb").toString(),options.value("gamma","srgb").toString()),
                            options.value("view","Standard").toString(),cpu,cancelled,diagnostics,error);
}
QImage ViewerGpu::convertSignal(const QImage &image,const QString &from,const QString &to,const std::atomic<bool> &cancelled,QVariantMap *diagnostics,QString *error) {
    if(error)error->clear();
    return convertOperation(image,from,to,{},[&]{return ColourPipeline::convert(image,from,to,error);},cancelled,diagnostics,error);
}
QImage ViewerGpu::convertOperation(const QImage &image,const QString &from,const QString &to,const QString &display,
                                 const std::function<QImage()> &cpu,const std::atomic<bool> &cancelled,QVariantMap *diagnostics,QString *) {
    if(diagnostics)diagnostics->clear();
    const auto report=[&](QVariantMap info){if(diagnostics)*diagnostics=std::move(info);};
    if(cancelled)return {};
    if(image.format()!=QImage::Format_RGBA32FPx4 || image.bytesPerLine()!=image.width()*16
        || qsizetype(image.width())*image.height()<m_options.minimumPixels || image.sizeInBytes()>m_options.compute.maximumImageBytes) {
        report({{"backend","cpu"},{"reason","Image size/format stays on the CPU."}});return cpu();
    }
    const auto stageKey=from+'\n'+to+'\n'+display;
    const auto key=stageKey+'\n'+QString::number(image.width())+'x'+QString::number(image.height());
    const auto retire=[&](const QString &reason) {
        Decision failed;failed.report={{"backend","cpu"},{"reason",reason}};
        if(m_decisions.size()>=64)m_decisions.clear();
        m_decisions.insert(key,failed);++m_demotions;
        report(failed.report);return cpu();
    };
    try {
        if(!m_decisions.contains(key)) {
            Decision decision;QElapsedTimer setup;setup.start();
            if(!m_stages.contains(stageKey)) {
                if(m_stages.size()>=32)m_stages.clear();
                m_stages.insert(stageKey,GpuProcessing::colour(from,to,display,m_compiler));
            }
            const auto stage=m_stages.value(stageKey);
            if(cancelled)return {};
            if(stage.spirv.isEmpty())decision.report={{"backend","cpu"},{"reason",stage.unavailableReason}};
            else {
                auto reference=cpu();if(reference.isNull())return {};
                if(cancelled)return {};
                auto warmed=apply(image,stage);
                if(warmed.backend!="vulkan" || warmed.image.isNull())decision.report={{"backend","cpu"},{"reason",warmed.reason.isEmpty()?warmed.error:warmed.reason}};
                else {
                    QVector<double> cpuTimes,gpuTimes;bool agreement=matches(warmed.image,reference,image);
                    for(int sample=0;sample<3 && agreement;++sample) {
                        if(cancelled)return {};
                        QElapsedTimer timer;
                        // Alternate ordering to reduce a consistent CPU-first bias.
                        const auto sampleCpu=[&]{timer.start();reference=cpu();cpuTimes.append(timer.nsecsElapsed()/1000000.);};
                        const auto sampleGpu=[&]{timer.start();warmed=apply(image,stage);gpuTimes.append(timer.nsecsElapsed()/1000000.);};
                        if(sample%2){sampleGpu();if(cancelled)return {};sampleCpu();}
                        else {sampleCpu();if(cancelled)return {};sampleGpu();}
                        agreement=warmed.backend=="vulkan" && warmed.error.isEmpty() && matches(warmed.image,reference,image);
                    }
                    if(cancelled)return {};
                    decision.report=SuitePerformance::selectBackend(cpuTimes,gpuTimes,agreement);
                    if(!agreement && !warmed.reason.isEmpty())decision.report["reason"]=warmed.reason;
                    decision.report["setupAndComparisonMs"]=setup.nsecsElapsed()/1000000.;
                    decision.report["scope"]="Float colour conversion including validation, output ownership, upload, fence completion and download; common frame scaling/validation/display packing excluded on both sides.";
                    decision.report["device"]=m_context.information().value("device");
                    ++m_calibrations;
                    if(m_decisions.size()>=64)m_decisions.clear();
                    m_decisions.insert(key,decision);
                    report(decision.report);
                    return decision.report.value("backend")=="vulkan" ? warmed.image : reference;
                }
            }
            decision.report["setupAndComparisonMs"]=setup.nsecsElapsed()/1000000.;
            if(m_decisions.size()>=64)m_decisions.clear();
            m_decisions.insert(key,decision);
        }
        auto &decision=m_decisions[key];
        if(decision.report.value("backend")!="vulkan"){report(decision.report);return cpu();}
        if(!m_stages.contains(stageKey)){if(m_stages.size()>=32)m_stages.clear();m_stages.insert(stageKey,GpuProcessing::colour(from,to,display,m_compiler));}
        QElapsedTimer timer;timer.start();auto output=apply(image,m_stages.value(stageKey));const auto ms=timer.nsecsElapsed()/1000000.;
        auto actual=decision.report;actual["lastConversionMs"]=ms;
        if(output.backend!="vulkan" || output.image.isNull() || !output.error.isEmpty()) {
            decision.report["backend"]="cpu";decision.report["reason"]=output.reason.isEmpty()?output.error:output.reason;++m_demotions;
            actual=decision.report;report(actual);
            return !output.image.isNull() && output.error.isEmpty()?output.image:cpu();
        }
        if(ms>=decision.report.value("cpu").toMap().value("medianMs").toDouble()*.9)++decision.slowRuns;else decision.slowRuns=0;
        if(decision.slowRuns>=3){decision.report["backend"]="cpu";decision.report["reason"]="Vulkan no longer beats the measured CPU cost; CPU retained for following frames.";++m_demotions;actual["nextBackend"]="cpu";}
        report(actual);return output.image;
    } catch(const std::exception &e) {return retire(QString("GPU colour preparation failed: ")+QString::fromUtf8(e.what()));}
    catch(...) {return retire("GPU colour preparation failed; CPU retained.");}
}
