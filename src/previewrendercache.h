#pragma once
#include <atomic>
#include <vector>

#include <QObject>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QVariantMap>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

class QThread;

// Playback-only pictures of a frozen timeline. The render thread owns its own
// timeline; neither it nor cached pictures ever enter the project.
class PreviewRenderCache : public QObject {
    Q_OBJECT
public:
    explicit PreviewRenderCache(QObject *parent = nullptr);
    ~PreviewRenderCache() override;
    void begin(const QVariantMap &snapshot, int playhead, const QString &root,
               qint64 byteLimit, const QString &processing = QStringLiteral("auto"));
    // Synchronously makes every old picture unavailable. The render's finished
    // frames are held until the next `begin`, which takes over those that are
    // still its timeline's pictures (an edit comes here first and begins the
    // new render a moment later).
    void invalidate();
    // The same, and nothing is held: for a render that is not to be followed
    // by another, or whose frames cannot be trusted.
    void clear();
    void setBusy(bool busy);
    void act(const QString &action);
    QImage image(int frame);
    // The same frame as a viewer draws it, for playback: at `draw`'s size
    // (the picture's own when that is not smaller), through `display` (gamut,
    // gamma and view, as Timeline::setShownAs takes them), 8 bits with colour
    // multiplied by alpha, tagged as TimelineRenderer::shown's. Null when the
    // frame is not kept, is not a working picture or the engine cannot:
    // image() then.
    QImage shown(int frame, const QSize &draw, const QVariantMap &display);
    // What is known of a kept frame, for choosing between it and drawing the
    // frame again: the render it belongs to, the first frame of its range,
    // and how long the background render took to make it (below 0 when that
    // was not timed). False when the frame is not kept.
    bool about(int frame, QString *render, int *range, double *madeMs) const;
    QVariantMap status() const;
    // The frames a background render helps: not those a plain clip plays
    // directly, nor those hidden under a clip that fills the frame. `tracks`
    // and `profile` as a snapshot holds them; without a profile nothing is
    // known to fill the frame.
    static QVariantList affectedRanges(const QVariantList &entries, int frames, const QVariantList &tracks = {},
                                       const QVariantMap &profile = {});

    // Whether two snapshots (entries, tracks, project, profile, mediaSources,
    // proxyMode, frames) show the same picture at every frame: everything a
    // picture depends on is the same, whatever labels, markers, names,
    // levels or track locks differ. Files the clips name are not looked at.
    static bool samePictures(const QVariantMap &a, const QVariantMap &b);

    // What exactly a frame's picture shows, for keeping a picture of it
    // between runs (a filmstrip's tiles): the two together change whenever
    // the picture would. `settingsKey` is everything but the clips (profile,
    // project, tracks, nested timelines, media settings, proxy choice), every
    // file the snapshot names as it is now on disk, and `drawnBy` (whatever
    // the caller's pictures depend on besides); it asks the file system, so
    // is not for the thread that serves the viewer. `frameKeys` gives, for
    // any frame of these clips, what each clip drawn on it shows there (the
    // marks a render's finished frames are carried over an edit by); the
    // clips are read once, when it is made.
    static QByteArray settingsKey(const QVariantMap &snapshot, const QString &drawnBy);
    static std::function<QByteArray(int)> frameKeys(const QVariantList &entries);

    // Lossless, bounded storage, including float colour and pixel-aspect tags.
    static bool writeFrame(const QString &path, const QImage &image, QString *error);
    static QImage readFrame(const QString &path);
    // A frame kept as hardware video (cachecodec): the working picture again.
    QImage readCoded(const QString &path);
private:
    // Reads a hardware-video frame into the reading engine's source, as
    // working light; its size and tags.
    bool loadCoded(const QString &path, int *width, int *height, QVariantMap *tags);
public:
signals:
    void changed(const QVariantMap &status);
    void dependenciesChanged();
private:
    struct Job;
    void run();
    void poll();
    void retire(std::shared_ptr<Job> job);
    void watch();
    static bool dependenciesMoved(Job &job);
    // The render whose files the watcher looks at (under m_mutex).
    std::shared_ptr<Job> m_watched;
    bool m_watchNow = false;
    std::unique_ptr<QThread> m_watcher;
    std::shared_ptr<Job> m_current, m_pending;
    // The render `invalidate` ended, held for the next one to look through.
    std::shared_ptr<Job> m_last;
    // Renders that are over, for the worker thread to let go of (it removes
    // their folders).
    std::vector<std::shared_ptr<Job>> m_retired;
    // A hardware-video frame that could not be read back because its reader
    // would not start (not because its file is bad), how often in a row, and
    // whether to keep frames any machine reads from now on.
    bool m_readerUnavailable = false;
    int m_readerFailures = 0;
    std::atomic_bool m_codecDistrusted{false};
    std::unique_ptr<QThread> m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stopping = false, m_busy = false, m_paused = false;
    QTimer m_poll;
    QVariantMap m_lastStatus;
    // Reading hardware-video frames back: decoders by their parameters, and
    // an engine that turns the planes into working light.
    struct Coded;
    std::unique_ptr<Coded> m_coded;
    std::mutex m_codedMutex;
};
