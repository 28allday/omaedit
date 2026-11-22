#include "viewerprocessor.h"
#include "viewergpu.h"
#include "engine.h"
#include "engines.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDateTime>
struct ViewerProcessor::Cache {
    struct Entry {QByteArray key;ViewerProcessing::Result result;};
    QList<Entry> entries;
    qsizetype bytes=0;
    std::unique_ptr<ViewerGpu> gpu;
    quint64 preferenceRevision=0;
    static constexpr qsizetype budget=64*1024*1024;
    ViewerProcessing::Result process(const ViewerProcessing::Request &request,const std::atomic<bool> &cancelled,const QString &preference,quint64 revision) {
        if(preferenceRevision!=revision){entries.clear();bytes=0;gpu.reset();preferenceRevision=revision;}
        auto key=ViewerProcessing::key(request)+'|'+preference.toUtf8();
        if(!request.file.isEmpty()){const QFileInfo file(request.file);key+='|'+QByteArray::number(file.size())+'|'+QByteArray::number(file.lastModified().toMSecsSinceEpoch());}
        for(qsizetype i=0;i<entries.size();++i)if(entries[i].key==key){auto entry=entries.takeAt(i);entries.prepend(entry);auto result=entry.result;result.cacheHit=true;result.milliseconds=0;return result;}
        ViewerProcessing::ColourTransform transform;
        if(preference=="auto")transform=[this](const QImage &image,const QVariantMap &options,const std::atomic<bool> &stop,QVariantMap *diagnostics,QString *error){
            if(!gpu) {
                ViewerGpu::Options settings;settings.compute=computeOptions();settings.compiler=qEnvironmentVariable("OMAEDIT_VIEWER_SHADER_COMPILER");
                gpu=std::make_unique<ViewerGpu>(settings);
            }
            return gpu->convert(image,options,stop,diagnostics,error);
        };
        auto result=ViewerProcessing::render(request,cancelled,transform);
        if(preference=="cpu" && result.conversion.value("backend")!="engine")result.conversion={{"backend","cpu"},{"reason","CPU preference."}};
        if(gpu)result.conversion["context"]=gpu->information();
        const auto size=result.frame.sizeInBytes();
        if(!cancelled && !result.frame.isNull() && result.error.isEmpty() && size<=budget) {
            while(!entries.isEmpty() && (bytes+size>budget || entries.size()>=3))bytes-=entries.takeLast().result.frame.sizeInBytes();
            entries.prepend({key,result});bytes+=size;
        }
        return result;
    }
};
ViewerProcessor::ViewerProcessor(QObject *parent):QObject(parent),m_cache(std::make_shared<Cache>()) {
    connect(&m_queue,&SuiteProcessing::LatestQueue::changed,this,&ViewerProcessor::changed);
}
ViewerProcessor::~ViewerProcessor()=default;
SuiteProcessing::ComputeContext::Options ViewerProcessor::computeOptions() {
    SuiteProcessing::ComputeContext::Options options;
    options.loader=qEnvironmentVariable("OMAEDIT_VIEWER_VULKAN_LOADER");
    options.loaderLock=&engine::loaderMutex();
    bool ok=false;const int device=qEnvironmentVariableIntValue("OMAEDIT_VIEWER_GPU_DEVICE",&ok);
    if(ok && device>=0 && device<128)options.deviceIndex=device;
    else if(options.loader.isEmpty()) {
        // The engines' GPU, by its place in the loader's list (the same list here).
        int index=-1;render::sharedDevice(&index);
        if(index>=0)options.deviceIndex=index;
    }
    return options;
}
quint64 ViewerProcessor::registerClient(){return ++m_nextClient;}
void ViewerProcessor::cancel(quint64 client){m_queue.cancel(client);}
void ViewerProcessor::setPreference(const QString &preference) {
    const auto next=preference=="cpu"?QString("cpu"):QString("auto");
    if(m_preference==next)return;
    m_preference=next;++m_preferenceRevision;m_queue.cancelAll();m_lastConversion.clear();emit preferenceChanged();emit changed();
}
bool ViewerProcessor::submit(quint64 client,QObject *receiver,const ViewerProcessing::Request &request,std::function<void(const ViewerProcessing::Result &)> completion) {
    QPointer<QObject> guard(receiver);auto cache=m_cache;const auto preference=m_preference;const auto revision=m_preferenceRevision;
    return m_queue.submit(client,request.frame.sizeInBytes(),[cache,request,preference,revision](const std::atomic<bool> &cancelled){return QVariant::fromValue(cache->process(request,cancelled,preference,revision));},
        [this,guard,revision,completion=std::move(completion)](const QVariant &value,const QString &error){
            if(!guard || revision!=m_preferenceRevision)return;
            auto result=value.value<ViewerProcessing::Result>();if(!error.isEmpty())result.error=error;
            ++m_delivered;if(result.cacheHit)++m_cacheHits;m_lastMilliseconds=result.milliseconds;m_lastConversion=result.conversion;
            // Application callback is last: it can destroy its receiver/processor.
            emit changed();if(guard)completion(result);
        });
}
QVariantMap ViewerProcessor::information() const {
    auto info=m_queue.information();info["backend"]=m_lastConversion.value("backend","cpu");info["preference"]=m_preference;info["conversion"]=m_lastConversion;info["cacheBudgetBytes"]=Cache::budget;info["cacheEntryLimit"]=3;info["delivered"]=m_delivered;info["cacheHits"]=m_cacheHits;info["lastProcessingMs"]=m_lastMilliseconds;return info;
}
