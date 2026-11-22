#pragma once

#include <QObject>
#include <QSet>
#include <QHash>
#include <QQueue>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

class QThreadPool;
class QTimer;
class SoundScanThread;

// The Audio page's sound library: every audio file under the user's sound
// folders (default ~/Music and the XDG sounds dirs), probed once (ffprobe:
// duration, sample rate, channels; ffmpeg: a 48-bucket peak thumbnail) and
// cached as JSON in AppDataLocation. Files are sorted into the R128-era
// categories the page shows — Dialogue, Ambience, Music, Effects, Foley,
// Room Tones — by folder and file-name conventions (DIA_/AMB_/MUS_/SFX_/
// FOL_/RT_ prefixes and plain words), and every guess can be overridden
// per file. Favourites, tags and smart collections (saved searches)
// persist alongside.
//
// Item shape: {path, name, folder, category, duration (s), sampleRate,
// channels, size, favorite, tags[], peaks[48], probed, missing}.
class SoundLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int pending READ pending NOTIFY scanningChanged)
    Q_PROPERTY(QString scanStatus READ scanStatus NOTIFY scanningChanged)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)

public:
    explicit SoundLibrary(QObject *parent = nullptr);
    ~SoundLibrary() override;

    QVariantList items() const { return m_items; }
    QStringList folders() const { return m_folders; }
    QVariantList collections() const { return m_collections; }
    // Off: no folder is adopted or searched; sounds filed by path still work.
    void setFolderScanning(bool on) { m_folderScanning = on; }
    bool scanning() const { return m_loading || m_discovering || m_pending > 0; }
    int pending() const { return m_pending; }
    QString scanStatus() const;
    static QStringList categories();
    static QStringList audioExtensions();
    // The category a path would be filed under with no override.
    static QString guessCategory(const QString &path);

    Q_INVOKABLE void addFolder(const QString &path);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void cancelScan();
    // Files the project already knows (its library's audio) join the
    // catalogue under their own folder without being copied anywhere.
    Q_INVOKABLE void addPaths(const QStringList &paths);
    // Files a sound under a category; returns what is wrong, empty on success.
    Q_INVOKABLE QString setCategory(const QString &path, const QString &category);
    Q_INVOKABLE void setFavorite(const QString &path, bool on);
    Q_INVOKABLE void setTags(const QString &path, const QStringList &tags);
    Q_INVOKABLE QVariantMap item(const QString &path) const;
    // Smart collections: {name, query, category} — a saved search.
    Q_INVOKABLE void addCollection(const QString &name, const QString &query,
                                   const QString &category);
    Q_INVOKABLE void removeCollection(const QString &name);
    // Test/probe hooks: the cache file location and the tool names.
    Q_INVOKABLE QString cachePath() const;
    void setToolNames(const QString &ffprobe, const QString &ffmpeg);

signals:
    void itemsChanged();
    void foldersChanged();
    void collectionsChanged();
    void scanningChanged();
    void probed(const QString &path);

private:
    struct Probe {
        double duration = 0;
        int sampleRate = 0;
        int channels = 0;
        QList<float> peaks;
        bool ok = false;
        bool unchanged = false;
        qint64 modifiedMs = 0;
        qint64 size = 0;
    };
    void load();
    // Applies the loaded catalogue under the edits made while it was read.
    // Closing: merge only (no default folder, no signals, no scan).
    void finishLoad(bool closing);
    void save();
    void scanFolders();
    void startProbes();
    void applyProbe(const QString &path, const Probe &probe);
    void publishChanges();
    static Probe runProbeWith(const QString &path, const QString &ffprobe, const QString &ffmpeg,
                              const std::atomic<bool> &alive);
    int indexOf(const QString &path) const;
    void touch();

    QVariantList m_items;
    QStringList m_folders;
    QVariantList m_collections;
    QVariantMap m_overrides;     // path → category
    QSet<QString> m_favorites;
    QVariantMap m_tags;          // path → [tags]
    QVariantMap m_cache;         // path → {duration, sampleRate, channels, peaks, mtime}
    QHash<QString, int> m_indices;
    QSet<QString> m_metadataEdits;
    QQueue<QString> m_probeQueue;
    int m_pending = 0;
    int m_inflight = 0;
    int m_discovered = 0;
    bool m_discovering = false;
    bool m_folderScanning = true;
    quint64 m_generation = 0;
    QString m_scanMessage;
    bool m_itemsDirty = false;
    QTimer *m_publishTimer = nullptr;
    SoundScanThread *m_scanner = nullptr;
    std::shared_ptr<std::atomic<bool>> m_scanAlive;
    QString m_ffprobe = QStringLiteral("ffprobe");
    QString m_ffmpeg = QStringLiteral("ffmpeg");
    QThreadPool *m_pool = nullptr;
    QThreadPool *m_savePool = nullptr;   // one thread: the load, then saves, in order
    bool m_saveQueued = false;
    // The catalogue file is read on the save worker. Until it lands, the
    // library is empty and "scanning"; edits made meanwhile are remembered by
    // key and win over the loaded values when the two are merged.
    struct LoadResult;
    std::shared_ptr<LoadResult> m_loadResult;
    bool m_loading = false, m_scanCancelledWhileLoading = false;
    QSet<QString> m_removedFolders, m_editedOverrides, m_editedFavorites, m_editedTags, m_touchedCollections;
    // The destructor cancels and joins pooled probes before destroying their receiver.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};
