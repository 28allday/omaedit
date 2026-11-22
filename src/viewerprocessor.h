#pragma once
#include "viewerprocessing.h"
#include "latestqueue.h"
#include "compute.h"
#include <QPointer>
#include <functional>
class ViewerProcessor : public QObject {
    Q_OBJECT
public:
    explicit ViewerProcessor(QObject *parent=nullptr);
    ~ViewerProcessor() override;
    quint64 registerClient();
    void cancel(quint64 client);
    void setPreference(const QString &preference);
    QString preference() const { return m_preference; }
    bool submit(quint64 client,QObject *receiver,const ViewerProcessing::Request &request,
                std::function<void(const ViewerProcessing::Result &)> completion);
    QVariantMap information() const;
    // How the viewer's own GPU context is made: on the GPU the engines draw
    // on (the one chosen in the settings, or the best), so a GPU the editor
    // left alone stays asleep, unless OMAEDIT_VIEWER_GPU_DEVICE names one;
    // and taking turns with the engines in the Vulkan loader.
    static SuiteProcessing::ComputeContext::Options computeOptions();
signals:
    void changed();
    void preferenceChanged();
private:
    struct Cache;
    std::shared_ptr<Cache> m_cache;
    SuiteProcessing::LatestQueue m_queue;
    quint64 m_nextClient=0,m_delivered=0,m_cacheHits=0;
    double m_lastMilliseconds=0;
    QString m_preference="auto";
    QVariantMap m_lastConversion;
    quint64 m_preferenceRevision=0;
};
