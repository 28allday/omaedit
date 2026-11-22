#pragma once
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QVariantMap>
#include <atomic>
#include <memory>

// Maps the beats of recordings in the background (src/beatmap.h does the
// finding): one file at a time on a thread of its own, its spectra on several,
// so neither the interface nor playback waits for it. Each job is a row in
// Background Tasks and can be cancelled there.
//
// A result is {bpm, beats: [seconds from the start of the file's sound],
// duration, size, modified, strength}. It is kept on disk under the file as it
// is (path, size and time of change), so asking again for a file that has not
// changed answers at once.
class BeatMapper : public QObject {
    Q_OBJECT
public:
    explicit BeatMapper(QObject *parent = nullptr);
    ~BeatMapper() override;
    // False when the path is no file, or is being mapped already.
    bool map(const QString &path);
    void cancel(const QString &path);
    bool mapping(const QString &path) const { return m_jobs.contains(path); }
    bool busy() const { return !m_jobs.isEmpty(); }
    // The editor is playing: a job's spectra keep to one thread meanwhile.
    void setYielding(bool playing) { m_yielding->store(playing); }
    // The work of one job, as its thread does it. `beats` empty with no error:
    // the sound has no steady beat. Tests and the command line call it directly.
    static QVariantMap analyse(const QString &path, const std::function<bool()> &cancelled = {},
                               const std::function<void(int)> &progress = {}, QString *error = nullptr,
                               int threads = 0, bool *fromCache = nullptr);
    // What the result is kept under: the file as it is on disk.
    static QString cacheKey(const QString &path);
signals:
    void taskChanged(const QVariantMap &task);
    // `result` is empty when the job failed or was cancelled (`error` says which).
    void finished(const QString &path, const QVariantMap &result, const QString &error, bool cancelled);

private:
    struct Job {
        std::shared_ptr<std::atomic_bool> stop;
        QVariantMap row;
    };
    QHash<QString, Job> m_jobs;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic_bool> m_yielding = std::make_shared<std::atomic_bool>(false);
};
