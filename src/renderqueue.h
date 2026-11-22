#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QThreadPool>
#include <QSet>
#include <QHash>
#include <memory>
#include <optional>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class FileChooser;
class QLockFile;

// ── Render presets ───────────────────────────────────────────────────────
// The Deliver page's preset browser: built-in groups (read-only) plus the
// user's custom presets (QSettings, JSON). A preset is {id, name, group,
// builtin, favorite, spec} — `spec` in ExportSpec's keys.
class RenderPresets : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)

public:
    explicit RenderPresets(QObject *parent = nullptr);

    QVariantList presets() const { return m_presets; }
    Q_INVOKABLE QVariantMap preset(const QString &id) const;
    // A new custom preset; returns its id ("" when the name is taken or empty).
    Q_INVOKABLE QString savePreset(const QString &name, const QVariantMap &spec);
    Q_INVOKABLE bool updatePreset(const QString &id, const QVariantMap &spec); // custom only
    Q_INVOKABLE QString duplicatePreset(const QString &id);
    Q_INVOKABLE bool renamePreset(const QString &id, const QString &name);
    Q_INVOKABLE bool deletePreset(const QString &id);
    Q_INVOKABLE void setFavorite(const QString &id, bool on);
    Q_INVOKABLE bool exportPreset(const QString &id, const QUrl &url);
    Q_INVOKABLE int importPresets(const QUrl &url); // presets added
    Q_INVOKABLE void exportPresetDialog(const QString &id);
    Q_INVOKABLE void importPresetsDialog();
    Q_INVOKABLE bool nameTaken(const QString &name) const;

signals:
    void presetsChanged();
    void imported(int count);
    void exported(const QString &path);
    void failed(const QString &message);

private:
    void load();
    void save();
    void rebuild();
    int customIndex(const QString &id) const;
    static QVariantList builtIns();

    QVariantList m_custom;     // [{id, name, spec, favorite}]
    QStringList m_favorites;   // built-in ids
    QVariantList m_presets;    // the merged list QML reads
    FileChooser *m_chooser = nullptr;
    QString m_pendingExport;
};

// ── Render queue ─────────────────────────────────────────────────────────
// Jobs render one at a time through the engine (Backend routes the
// worker's export signals here). Statuses: queued, rendering, paused,
// completed, warning (completed with acknowledged warnings), failed,
// cancelled. Persisted between launches; a job caught rendering at
// shutdown comes back queued.
class RenderQueue : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(bool active READ active NOTIFY jobsChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY jobsChanged)
    Q_PROPERTY(int activeJob READ activeJob NOTIFY jobsChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY jobsChanged)
    Q_PROPERTY(QString afterCompletion READ afterCompletion WRITE setAfterCompletion NOTIFY afterCompletionChanged)

public:
    explicit RenderQueue(QObject *parent = nullptr);
    ~RenderQueue() override;

    QVariantList jobs() const { return m_jobs; }
    bool active() const { return m_activeJob >= 0; }
    bool paused() const { return m_queuePaused; }
    int activeJob() const { return m_activeJob; }
    int pendingCount() const;
    QString afterCompletion() const { return m_afterCompletion; }
    void setAfterCompletion(const QString &action);

    // `spec` already carries the range and caption cues; `warnings` are the
    // acknowledged pre-flight warnings (the job completes as "warning").
    Q_INVOKABLE int enqueue(const QString &name, const QString &path, const QVariantMap &spec,
                            const QString &presetName, const QStringList &warnings = {});
    Q_INVOKABLE void renderAll();
    // Renders only these jobs and leaves every other queued job waiting,
    // until renderAll() releases the whole queue.
    void renderJobs(const QList<int> &ids);
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    Q_INVOKABLE void pauseJob(int id);
    Q_INVOKABLE void resumeJob(int id);
    Q_INVOKABLE void cancelJob(int id);
    Q_INVOKABLE void retryJob(int id);
    Q_INVOKABLE void removeJob(int id);
    Q_INVOKABLE int duplicateJob(int id);
    Q_INVOKABLE void moveJob(int id, int delta);
    Q_INVOKABLE void clearFinished();
    // Every job not rendering now, whatever its state (a new window starts empty).
    void clearAll();
    Q_INVOKABLE QVariantMap job(int id) const;
    Q_INVOKABLE QStringList jobLog(int id) const;

    // Backend feeds these from the worker.
    void addWarning(const QString &warning);
    void attachSnapshot(int id, const QVariantMap &snapshot);
    void onProgress(int percent);
    void onStage(const QString &stage);
    void onPaused(bool paused);
    void onEnded(bool ok, const QString &path, const QString &reason, const QStringList &log);

signals:
    void jobsChanged();
    void snapshotNeeded(int id); // a queued job has no project snapshot yet — the Backend captures one
    void afterCompletionChanged();
    // The engine: Backend forwards to the worker.
    void startRequested(const QString &path, const QVariantMap &spec);
    void pauseRequested();
    void resumeRequested();
    void cancelRequested();
    // Outcomes, for the status bar and desktop notifications.
    void jobFinished(int id, bool ok, const QString &path, const QString &reason);
    void queueDrained(bool allOk);
    void failed(const QString &message);

private:
    int indexOf(int id) const;
    void pump();
    void touch();
    void load();
    void save();
    void tick();

    struct SnapshotSave;
    struct SaveState;
    void finishSnapshot(const std::shared_ptr<SnapshotSave> &result, bool notify = true);
    void cleanSnapshots();
    QThreadPool m_snapshotPool;
    QThreadPool m_savePool; // one thread: queue saves land in order
    std::shared_ptr<SaveState> m_saveState;
    QList<std::shared_ptr<SnapshotSave>> m_snapshotSaves;
    QHash<int, quint64> m_snapshotVersions;
    quint64 m_snapshotVersion = 0;
    QSet<QString> m_snapshotFiles;
    QString m_snapshotNamespace;

    // The default queue and each additional editor have independent leases.
    // A new default owner recovers only queues whose lease can be acquired.
    std::unique_ptr<QLockFile> m_queueLock;
    QString m_settingsGroup;
    QString m_storageError;
    QVariantList m_jobs;
    int m_nextId = 1;
    int m_activeJob = -1;
    bool m_queuePaused = false;
    bool m_anyFailed = false;
    // This run started a job, and one was cancelled: "drained" (which can
    // shut the machine down) means jobs ran to the end and all succeeded.
    bool m_ranThisRun = false;
    bool m_anyCancelled = false;
    bool m_waitingSnapshot = false;
    // Set by renderJobs(): only these jobs may start. Unset, any may.
    std::optional<QSet<int>> m_released;
    QString m_afterCompletion = QStringLiteral("none");
    QElapsedTimer m_elapsed;
    qint64 m_elapsedBefore = 0; // across pauses
    bool m_jobPaused = false;
    QTimer m_ticker;
};
