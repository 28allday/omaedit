#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <QThread>
#include <QVariant>
#include <atomic>
#include <functional>
#include <memory>
namespace SuiteProcessing {
// App-local, one worker, latest queued request per client. All public methods
// and completions belong to this object's owning (normally GUI) thread.
class LatestQueue : public QObject {
    Q_OBJECT
public:
    using Work=std::function<QVariant(const std::atomic<bool> &cancelled)>;
    using Completion=std::function<void(const QVariant &value,const QString &error)>;
    explicit LatestQueue(QObject *parent=nullptr);
    ~LatestQueue() override;
    // Cost describes retained input bytes. At most one oversized request may
    // wait alone, so valid large CPU frames remain usable without an open queue.
    bool submit(quint64 client,qsizetype cost,Work work,Completion completion);
    void cancel(quint64 client);
    void cancelAll();
    QVariantMap information() const;
signals:
    void changed();
private:
    struct Job {quint64 client=0;qsizetype cost=0;Work work;Completion completion;std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);};
    void dispatch();
    QThread m_thread;
    QObject *m_worker=nullptr;
    QHash<quint64,Job> m_pending;
    QList<quint64> m_order;
    std::shared_ptr<std::atomic<bool>> m_runningCancel;
    quint64 m_runningClient=0,m_submitted=0,m_completed=0,m_coalesced=0,m_cancelled=0,m_rejected=0;
    qsizetype m_pendingBytes=0;
    bool m_stopping=false;
    static constexpr qsizetype maximumPendingBytes=256*1024*1024;
    static constexpr int maximumPendingClients=32;
};
}
