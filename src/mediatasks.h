#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QImage>
#include <QThread>
#include <QStringList>
class FileChooser;
#include <QQueue>
#include <atomic>
#include <memory>

// Background ingest/transfer/verification/encoding jobs, one job at a time; a
// proxy or reframe job encodes several of its clips at once. All disk walks,
// hashes, media probes and child processes run on the job thread.
class MediaTasks : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QStringList formats READ formats CONSTANT)
public:
    explicit MediaTasks(QObject *parent = nullptr);
    ~MediaTasks() override;
    QVariantList jobs() const { return m_jobs; }
    bool running() const { return m_thread != nullptr; }
    static QStringList formats();
    Q_INVOKABLE QString submit(const QString &kind, const QStringList &paths, const QVariantMap &options = {});
    Q_INVOKABLE void cancel(const QString &id);
    Q_INVOKABLE QString retry(const QString &id);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE bool saveReport(const QString &id, const QString &path);
    void cancelAll();
    // The editor is playing (or stopped): while it plays, the tool processes
    // of a running job keep to a small part of the machine.
    void setYielding(bool playing) { m_yielding.store(playing); }
    void annotate(const QString &id, const QVariantMap &fields) { update(id, fields); }
    Q_INVOKABLE QVariantMap record(const QString &id) const { return job(id); }
    Q_INVOKABLE void browse(const QString &purpose, const QString &kind = QStringLiteral("folder"));
    // What a chosen source list shows: per path {path, name, folder (home
    // shortened to ~), isDir, exists, bytes (files only; a folder is walked
    // when the task runs, never here)}.
    Q_INVOKABLE static QVariantList describePaths(const QStringList &paths);
signals:
    void changed();
    void pathsChosen(const QString &purpose, const QStringList &paths);
    void mediaReady(const QString &jobId, const QString &path, const QVariantMap &metadata, const QImage &thumbnail);
    void proxyReady(const QString &jobId, const QString &source, const QString &proxy);
    void checksumReady(const QString &jobId, const QString &source, const QString &hash);
    void finished(const QString &id, const QVariantMap &result);
    void error(const QString &message);
private:
    QVariantMap job(const QString &id) const;
    void update(const QString &id, const QVariantMap &fields);
    void startNext();
    void run(const QString &id, const QVariantMap &request, const std::shared_ptr<std::atomic_bool> &stop);
    FileChooser *m_chooser = nullptr;
    QString m_purpose;
    QVariantList m_jobs;
    QHash<QString, QVariantMap> m_watchFiles;
    QQueue<QString> m_queue;
    QThread *m_thread = nullptr;
    QString m_active;
    std::shared_ptr<std::atomic_bool> m_stop;
    std::atomic_bool m_yielding{false};  // set on the GUI thread, read by a job's threads
};
