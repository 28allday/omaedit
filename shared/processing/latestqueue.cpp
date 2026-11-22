#include "latestqueue.h"
#include <exception>
namespace SuiteProcessing {
LatestQueue::LatestQueue(QObject *parent):QObject(parent),m_worker(new QObject) {
    m_thread.setObjectName("Suite display processing");m_worker->moveToThread(&m_thread);
    connect(&m_thread,&QThread::finished,m_worker,&QObject::deleteLater);m_thread.start();
}
LatestQueue::~LatestQueue() {
    m_stopping=true;if(m_runningCancel)*m_runningCancel=true;
    m_pending.clear();m_order.clear();m_thread.quit();m_thread.wait();
}
bool LatestQueue::submit(quint64 client,qsizetype cost,Work work,Completion completion) {
    Q_ASSERT(QThread::currentThread()==thread());
    if(m_stopping || !client || cost<0 || !work || !completion)return false;
    const bool replaces=m_pending.contains(client);
    const qsizetype previous=replaces ? m_pending.value(client).cost : 0;
    const auto remaining=m_pendingBytes-previous;
    if((!replaces && m_pending.size()>=maximumPendingClients)
        || (remaining>0 && (cost>maximumPendingBytes || remaining>maximumPendingBytes-cost))) {
        ++m_rejected;return false;
    }
    Job job;job.client=client;job.cost=cost;job.work=std::move(work);job.completion=std::move(completion);
    if(replaces)++m_coalesced;else m_order.append(client);
    m_pending.insert(client,std::move(job));m_pendingBytes=remaining+cost;++m_submitted;
    dispatch();emit changed();return true;
}
void LatestQueue::cancel(quint64 client) {
    Q_ASSERT(QThread::currentThread()==thread());
    auto it=m_pending.find(client);
    if(it!=m_pending.end()){m_pendingBytes-=it->cost;m_pending.erase(it);m_order.removeAll(client);++m_cancelled;}
    if(m_runningClient==client && m_runningCancel && !m_runningCancel->exchange(true))++m_cancelled;
    emit changed();
}
void LatestQueue::cancelAll() {
    Q_ASSERT(QThread::currentThread()==thread());
    m_cancelled+=m_pending.size();m_pending.clear();m_order.clear();m_pendingBytes=0;
    if(m_runningCancel && !m_runningCancel->exchange(true))++m_cancelled;
    emit changed();
}
void LatestQueue::dispatch() {
    if(m_stopping || m_runningCancel || m_order.isEmpty())return;
    auto job=m_pending.take(m_order.takeFirst());m_pendingBytes-=job.cost;
    m_runningClient=job.client;m_runningCancel=job.cancelled;
    QMetaObject::invokeMethod(m_worker,[this,job=std::move(job)]() mutable {
        QVariant value;QString error;
        try {if(!*job.cancelled)value=job.work(*job.cancelled);}
        catch(const std::exception &e){error=QString::fromUtf8(e.what());}
        catch(...){error="Unknown background processing exception.";}
        QMetaObject::invokeMethod(this,[this,job=std::move(job),value=std::move(value),error]() mutable {
            m_runningCancel.reset();m_runningClient=0;
            // Start the next fair request before calling application code, which
            // may enqueue more work or delete its processor/owner.
            if(!*job.cancelled)++m_completed;
            dispatch();emit changed();
            if(!*job.cancelled)job.completion(value,error);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
QVariantMap LatestQueue::information() const {
    return {{"schemaVersion",1},{"workers",1},{"busy",bool(m_runningCancel)},{"pendingClients",m_pending.size()},{"pendingBytes",m_pendingBytes},{"maximumPendingBytes",maximumPendingBytes},{"maximumPendingClients",maximumPendingClients},{"submitted",m_submitted},{"completed",m_completed},{"coalesced",m_coalesced},{"cancelled",m_cancelled},{"rejected",m_rejected}};
}
}
