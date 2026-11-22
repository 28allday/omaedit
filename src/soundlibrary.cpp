#include "soundlibrary.h"
#include <QCoreApplication>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QTimer>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <QSaveFile>
#include <optional>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kPeakBuckets = 48;

} // namespace

// One discovery worker per library. Repeated rescans replace the pending
// request instead of queuing walks. Requests own their data; the worker never
// dereferences the library, including when an unavailable disk delays a read.
class SoundScanThread : public QThread {
    Q_OBJECT
public:
    struct Request {
        quint64 generation;
        QStringList folders;
        QVariantMap cache, overrides, tags;
        QSet<QString> favorites;
        QVariantList previous;
        std::shared_ptr<std::atomic<bool>> alive;
    };
    void submit(Request request) {
        QMutexLocker lock(&mutex);
        if (pending) pending->alive->store(false);
        if (running) running->store(false);
        pending = std::move(request);
        wake.wakeOne();
    }
    void stop() {
        QMutexLocker lock(&mutex);
        stopping = true;
        if (pending) pending->alive->store(false);
        if (running) running->store(false);
        pending.reset();
        wake.wakeOne();
    }
signals:
    void discovered(quint64 generation, int count);
    void ready(quint64 generation, const QVariantList &items, const QStringList &unavailable, bool changed);
protected:
    void run() override {
        for (;;) {
            std::optional<Request> request;
            {
                QMutexLocker lock(&mutex);
                while (!stopping && !pending) wake.wait(&mutex);
                if (stopping) return;
                request = std::move(pending);
                pending.reset();
                running = request->alive;
            }
            scan(*request);
        }
    }
private:
    void scan(const Request &request) {
        QStringList wanted;
        for (const auto &ext : SoundLibrary::audioExtensions()) wanted.append("*." + ext);
        QVariantList items;
        QSet<QString> seen;
        QStringList unavailable;
        QElapsedTimer progress;
        progress.start();
        auto add = [&](const QFileInfo &info, const QString &folder) {
            const QString path = info.absoluteFilePath();
            if (seen.contains(path)) return;
            seen.insert(path);
            const qint64 modified = info.lastModified().toMSecsSinceEpoch();
            const qint64 size = info.size();
            const QVariantMap cached = request.cache.value(path).toMap();
            const bool fresh = cached.contains("mtimeMs") && cached.value("mtimeMs").toLongLong() == modified
                               && cached.value("size").toLongLong() == size;
            QVariantMap row{{"path", path}, {"name", info.fileName()}, {"folder", folder},
                {"size", size}, {"mtimeMs", modified}, {"missing", false},
                {"category", request.overrides.contains(path) ? request.overrides.value(path).toString()
                                                             : SoundLibrary::guessCategory(path)},
                {"favorite", request.favorites.contains(path)}, {"tags", request.tags.value(path).toStringList()},
                {"probed", fresh}, {"duration", fresh ? cached.value("duration").toDouble() : 0.0},
                {"sampleRate", fresh ? cached.value("sampleRate").toInt() : 0},
                {"channels", fresh ? cached.value("channels").toInt() : 0},
                {"peaks", fresh ? cached.value("peaks").toList() : QVariantList()}};
            items.append(row);
            if (progress.elapsed() >= 100) {
                emit discovered(request.generation, int(items.size()));
                progress.restart();
            }
        };
        for (const QString &folder : request.folders) {
            if (!request.alive->load()) return;
            const QFileInfo info(folder);
            if (!info.exists() || !info.isReadable()) {
                unavailable.append(folder);
                continue;
            }
            if (info.isFile()) {
                if (SoundLibrary::audioExtensions().contains(info.suffix().toLower())) add(info, info.absolutePath());
                continue;
            }
            QDirIterator it(folder, wanted, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
            while (request.alive->load() && it.hasNext()) {
                it.next();
                add(it.fileInfo(), folder);
            }
        }
        if (!request.alive->load()) return;
        // Keep known sounds from offline roots visible until the source returns.
        for (const QVariant &value : request.previous) {
            auto row = value.toMap();
            const auto path = row.value("path").toString();
            if (!seen.contains(path) && (unavailable.contains(row.value("folder").toString())
                                        || unavailable.contains(path))) {
                row.insert("missing", true);
                items.append(row);
                seen.insert(path);
            }
        }
        std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) {
            const auto left = a.toMap(), right = b.toMap();
            const int order = left.value("name").toString().localeAwareCompare(right.value("name").toString());
            return order == 0 ? left.value("path").toString() < right.value("path").toString() : order < 0;
        });
        if (request.alive->load()) {
            // An unchanged rescan needs no list reset or QML filtering pass.
            // Compare on this worker and discard duplicate rows here as well.
            const bool changed = items != request.previous;
            emit ready(request.generation, changed ? items : QVariantList(), unavailable, changed);
        }
    }
    QMutex mutex;
    QWaitCondition wake;
    bool stopping = false;
    std::optional<Request> pending;
    std::shared_ptr<std::atomic<bool>> running;
};

SoundLibrary::SoundLibrary(QObject *parent) : QObject(parent) {
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(2);
    m_savePool = new QThreadPool(this);
    m_savePool->setMaxThreadCount(1);
    m_publishTimer = new QTimer(this);
    m_publishTimer->setInterval(100);
    m_publishTimer->setSingleShot(true);
    connect(m_publishTimer, &QTimer::timeout, this, &SoundLibrary::publishChanges);
    m_scanner = new SoundScanThread;
    connect(m_scanner, &QThread::finished, m_scanner, &QObject::deleteLater);
    connect(m_scanner, &SoundScanThread::discovered, this, [this](quint64 generation, int count) {
        if (generation != m_generation || !m_discovering) return;
        m_discovered = count;
        emit scanningChanged();
    }, Qt::QueuedConnection);
    connect(m_scanner, &SoundScanThread::ready, this,
            [this](quint64 generation, const QVariantList &items, const QStringList &unavailable, bool changed) {
        if (generation != m_generation || !m_discovering) return;
        if (changed) {
            m_items = items;
            m_indices.clear();
        }
        for (int i = 0; i < m_items.size(); ++i) {
            const auto row = m_items.at(i).toMap();
            const auto path = row.value("path").toString();
            if (changed) m_indices.insert(path, i);
            if (!row.value("probed").toBool() && !row.value("missing").toBool()) m_probeQueue.enqueue(path);
        }
        // Edits made while discovery was running win over its metadata snapshot.
        for (const auto &path : std::as_const(m_metadataEdits)) {
            const int i = indexOf(path);
            if (i < 0) continue;
            auto row = m_items.at(i).toMap();
            row.insert("favorite", m_favorites.contains(path));
            row.insert("tags", m_tags.value(path).toStringList());
            row.insert("category", m_overrides.contains(path) ? m_overrides.value(path).toString() : guessCategory(path));
            m_items[i] = row;
        }
        m_metadataEdits.clear();
        m_pending = int(m_probeQueue.size());
        m_discovering = false;
        m_scanMessage = unavailable.isEmpty() ? QString() : tr("Could not read: %1").arg(unavailable.join(", "));
        m_itemsDirty = m_itemsDirty || changed;
        publishChanges();
        startProbes();
    }, Qt::QueuedConnection);
    m_scanner->start();
    load();
}

SoundLibrary::~SoundLibrary() {
    m_alive->store(false);
    if (m_scanAlive) m_scanAlive->store(false);
    m_scanner->disconnect(this);
    m_scanner->stop();
    // The discovery worker owns its request and deletes itself when it exits.
    // Closing the editor must not wait on a filesystem read from an offline disk.
    m_pool->clear();
    // Running probes poll cancellation and kill their child process promptly.
    // Wait for those callbacks before destroying the pool/library.
    m_pool->waitForDone();
    if (m_loading) {
        // Edits made before the catalogue landed are merged and kept; no
        // default folder is invented and nothing is signalled while closing.
        m_savePool->waitForDone();
        finishLoad(true);
    }
    if (m_saveQueued)
        save();
    // Closing the editor must not lose the last edits; the file is local.
    m_savePool->waitForDone();
}

QStringList SoundLibrary::categories() {
    return {QStringLiteral("dialogue"), QStringLiteral("ambience"), QStringLiteral("music"),
            QStringLiteral("effects"),  QStringLiteral("foley"),    QStringLiteral("roomtone")};
}

QStringList SoundLibrary::audioExtensions() {
    return {QStringLiteral("wav"),  QStringLiteral("flac"), QStringLiteral("mp3"),
            QStringLiteral("aac"),  QStringLiteral("m4a"),  QStringLiteral("ogg"),
            QStringLiteral("opus"), QStringLiteral("aif"),  QStringLiteral("aiff"),
            QStringLiteral("wma"),  QStringLiteral("caf")};
}

// Conventions first (a DIA_/AMB_/MUS_/SFX_/FOL_/RT_ prefix on the file or
// any folder), then plain words anywhere in the path; effects is the
// default because most libraries are mostly effects.
QString SoundLibrary::guessCategory(const QString &path) {
    const QString lower = path.toLower();
    const QStringList parts = lower.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    struct Rule { const char *prefix; const char *category; };
    static const Rule prefixes[] = {
        {"dia_", "dialogue"}, {"dx_", "dialogue"}, {"vo_", "dialogue"},
        {"amb_", "ambience"}, {"bg_", "ambience"},  {"atmos_", "ambience"},
        {"mus_", "music"},    {"mx_", "music"},
        {"sfx_", "effects"},  {"fx_", "effects"},
        {"fol_", "foley"},    {"fol-", "foley"},
        {"rt_", "roomtone"},  {"rt-", "roomtone"}};
    for (auto it = parts.crbegin(); it != parts.crend(); ++it)
        for (const Rule &r : prefixes)
            if (it->startsWith(QLatin1String(r.prefix)))
                return QLatin1String(r.category);
    struct Word { const char *word; const char *category; };
    static const Word words[] = {
        {"room tone", "roomtone"}, {"roomtone", "roomtone"}, {"room_tone", "roomtone"},
        {"dialog", "dialogue"},    {"dialogue", "dialogue"}, {"voice", "dialogue"},
        {"vocal", "dialogue"},     {"speech", "dialogue"},   {"take", "dialogue"},
        {"adr", "dialogue"},       {"ambien", "ambience"},   {"atmos", "ambience"},
        {"wind", "ambience"},      {"rain", "ambience"},     {"forest", "ambience"},
        {"city", "ambience"},      {"crowd", "ambience"},    {"birds", "ambience"},
        {"music", "music"},        {"song", "music"},        {"track", "music"},
        {"score", "music"},        {"theme", "music"},       {"loop", "music"},
        {"foley", "foley"},        {"footstep", "foley"},    {"cloth", "foley"},
        {"sfx", "effects"},        {"effect", "effects"},    {"impact", "effects"},
        {"whoosh", "effects"},     {"hit", "effects"}};
    for (const Word &w : words)
        if (lower.contains(QLatin1String(w.word)))
            return QLatin1String(w.category);
    return QStringLiteral("effects");
}

QString SoundLibrary::cachePath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/soundlib.json");
}

void SoundLibrary::setToolNames(const QString &ffprobe, const QString &ffmpeg) {
    m_ffprobe = ffprobe;
    m_ffmpeg = ffmpeg;
}

struct SoundLibrary::LoadResult {
    QMutex mutex;
    bool done = false;
    QStringList folders, favorites;
    QVariantList collections;
    QVariantMap overrides, tags, cache;
};

// Ten thousand cached sounds are a few megabytes of JSON: the read and parse
// run on the save worker (so they are ordered before any save) and the
// interface keeps painting. The library reports "scanning" until the
// catalogue has landed and its folders have been walked.
void SoundLibrary::load() {
    m_loading = true;
    m_loadResult = std::make_shared<LoadResult>();
    const auto result = m_loadResult;
    const auto alive = m_alive;
    const QString path = cachePath();
    m_savePool->start(QRunnable::create([this, result, alive, path] {
        LoadResult read;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            read.folders = root.value(QStringLiteral("folders")).toVariant().toStringList();
            read.collections = root.value(QStringLiteral("collections")).toArray().toVariantList();
            read.overrides = root.value(QStringLiteral("overrides")).toObject().toVariantMap();
            read.favorites = root.value(QStringLiteral("favorites")).toVariant().toStringList();
            read.tags = root.value(QStringLiteral("tags")).toObject().toVariantMap();
            read.cache = root.value(QStringLiteral("cache")).toObject().toVariantMap();
        }
        {
            QMutexLocker lock(&result->mutex);
            result->folders = std::move(read.folders); result->favorites = std::move(read.favorites);
            result->collections = std::move(read.collections); result->overrides = std::move(read.overrides);
            result->tags = std::move(read.tags); result->cache = std::move(read.cache);
            result->done = true;
        }
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, result] {
            if (m_loading && result == m_loadResult) finishLoad(false);
        }, Qt::QueuedConnection);
    }));
}

void SoundLibrary::finishLoad(bool closing) {
    if (!m_loading) return;
    // Anything edited before the catalogue landed must reach the file, even
    // an edit (such as removing a folder) that left the in-memory list alone.
    const bool editedWhileLoading = !m_folders.isEmpty() || !m_removedFolders.isEmpty()
        || !m_editedOverrides.isEmpty() || !m_editedFavorites.isEmpty()
        || !m_editedTags.isEmpty() || !m_touchedCollections.isEmpty();
    LoadResult loaded;
    {
        QMutexLocker lock(&m_loadResult->mutex);
        if (!m_loadResult->done) return;
        loaded.folders = m_loadResult->folders; loaded.favorites = m_loadResult->favorites;
        loaded.collections = m_loadResult->collections; loaded.overrides = m_loadResult->overrides;
        loaded.tags = m_loadResult->tags; loaded.cache = m_loadResult->cache;
    }
    m_loading = false;
    m_loadResult.reset();
    // Folders: the loaded list less what was removed, plus what was added.
    QStringList folders;
    for (const auto &folder : std::as_const(loaded.folders))
        if (!m_removedFolders.contains(folder) && !folders.contains(folder)) folders.append(folder);
    for (const auto &folder : std::as_const(m_folders))
        if (!folders.contains(folder)) folders.append(folder);
    const bool foldersChangedNow = folders != m_folders;
    m_folders = folders;
    // Per-file facts: an edit made while loading replaces the loaded value,
    // including an edit that cleared it.
    QVariantMap overrides = loaded.overrides;
    for (const auto &path : std::as_const(m_editedOverrides)) {
        if (m_overrides.contains(path)) overrides.insert(path, m_overrides.value(path));
        else overrides.remove(path);
    }
    m_overrides = overrides;
    QSet<QString> favorites(loaded.favorites.begin(), loaded.favorites.end());
    for (const auto &path : std::as_const(m_editedFavorites)) {
        if (m_favorites.contains(path)) favorites.insert(path);
        else favorites.remove(path);
    }
    m_favorites = favorites;
    QVariantMap tags = loaded.tags;
    for (const auto &path : std::as_const(m_editedTags)) {
        if (m_tags.contains(path)) tags.insert(path, m_tags.value(path));
        else tags.remove(path);
    }
    m_tags = tags;
    QVariantList collections;
    for (const auto &value : std::as_const(loaded.collections))
        if (!m_touchedCollections.contains(value.toMap().value(QStringLiteral("name")).toString())) collections.append(value);
    for (const auto &value : std::as_const(m_collections))
        if (m_touchedCollections.contains(value.toMap().value(QStringLiteral("name")).toString())) collections.append(value);
    const bool collectionsChangedNow = collections != m_collections;
    m_collections = collections;
    // Probes that completed while loading are newer than the cached facts.
    QVariantMap cache = loaded.cache;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) cache.insert(it.key(), it.value());
    m_cache = cache;
    m_removedFolders.clear(); m_editedOverrides.clear(); m_editedFavorites.clear(); m_editedTags.clear(); m_touchedCollections.clear();
    if (closing) {
        // The destructor writes the merged state; only real edits are saved.
        if (editedWhileLoading) m_saveQueued = true;
        return;
    }
    const auto *app = QCoreApplication::instance();
    const bool agent = app && app->property("omaedit.agent").toBool();
    if (m_folders.isEmpty() && !agent && m_folderScanning) {
        const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        if (!music.isEmpty() && QFileInfo::exists(music)) {
            m_folders.append(music);
            emit foldersChanged();
        }
    } else if (foldersChangedNow)
        emit foldersChanged();
    if (collectionsChangedNow) emit collectionsChanged();
    // Scans asked for while loading were held back until the cache and the
    // per-file facts are known; a cancel with no later request skips it.
    const bool scan = !m_scanCancelledWhileLoading;
    m_scanCancelledWhileLoading = false;
    if (scan) scanFolders();
    else emit scanningChanged();
    if (editedWhileLoading) touch();
    if (m_saveQueued) save();
}

// Serialising ten thousand cached sounds is a few megabytes of JSON. The GUI
// thread only snapshots the (implicitly shared) containers; one worker builds
// the document and writes it, in submission order, so a later save never loses
// to an earlier one still committing.
void SoundLibrary::save() {
    if (m_loading) { m_saveQueued = true; return; } // written once the catalogue has landed
    m_saveQueued = false;
    const QString path = cachePath();
    const QStringList folders = m_folders, favorites = m_favorites.values();
    const QVariantList collections = m_collections;
    const QVariantMap overrides = m_overrides, tags = m_tags, cache = m_cache;
    m_savePool->start(QRunnable::create([path, folders, favorites, collections, overrides, tags, cache] {
        QJsonObject root;
        root.insert(QStringLiteral("folders"), QJsonArray::fromStringList(folders));
        root.insert(QStringLiteral("collections"), QJsonArray::fromVariantList(collections));
        root.insert(QStringLiteral("overrides"), QJsonObject::fromVariantMap(overrides));
        root.insert(QStringLiteral("favorites"), QJsonArray::fromStringList(favorites));
        root.insert(QStringLiteral("tags"), QJsonObject::fromVariantMap(tags));
        root.insert(QStringLiteral("cache"), QJsonObject::fromVariantMap(cache));
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }));
}

// Coalesce saves — probes land in bursts.
void SoundLibrary::touch() {
    if (m_saveQueued)
        return;
    m_saveQueued = true;
    QTimer::singleShot(500, this, [this] { if (m_saveQueued) save(); });
}

int SoundLibrary::indexOf(const QString &path) const {
    return m_indices.value(path, -1);
}

QString SoundLibrary::scanStatus() const {
    if (m_discovering) return tr("Searching folders… %1 found").arg(m_discovered);
    if (m_pending) return tr("Reading audio… %1 remaining").arg(m_pending);
    return m_scanMessage;
}

void SoundLibrary::scanFolders() {
    if (!m_folderScanning) {
        emit scanningChanged();
        return;
    }
    if (m_loading) {
        // Hold the scan until the catalogue lands: scanning now would probe
        // files the cache already knows, with empty per-file facts. A request
        // made after a cancel still wants its scan.
        m_scanCancelledWhileLoading = false;
        emit scanningChanged();
        return;
    }
    if (m_scanAlive) m_scanAlive->store(false);
    m_scanAlive = std::make_shared<std::atomic<bool>>(true);
    ++m_generation;
    m_probeQueue.clear();
    m_metadataEdits.clear();
    m_pending = 0;
    m_discovered = 0;
    m_discovering = true;
    m_scanMessage.clear();
    emit scanningChanged();
    m_scanner->submit({m_generation, m_folders, m_cache, m_overrides, m_tags,
                       m_favorites, m_items, m_scanAlive});
}

void SoundLibrary::cancelScan() {
    if (!scanning()) return;
    // A cancel while the catalogue is still being read: no scan exists yet,
    // and the one the landed catalogue would start is not wanted either.
    if (m_loading) m_scanCancelledWhileLoading = true;
    if (m_scanAlive) m_scanAlive->store(false);
    ++m_generation;
    m_probeQueue.clear();
    m_metadataEdits.clear();
    m_pending = 0;
    m_discovering = false;
    m_scanMessage = tr("Scan stopped");
    publishChanges();
}

void SoundLibrary::publishChanges() {
    m_publishTimer->stop();
    if (m_itemsDirty) {
        m_itemsDirty = false;
        emit itemsChanged();
    }
    emit scanningChanged();
}

void SoundLibrary::startProbes() {
    // Only two jobs exist at once, including cancelled jobs winding down from
    // an earlier generation. The rest are paths, not thousands of QRunnables.
    while (m_inflight < 2 && !m_probeQueue.isEmpty()) {
        const QString path = m_probeQueue.dequeue();
        const auto expected = item(path);
        const auto alive = m_alive, scanAlive = m_scanAlive;
        const auto generation = m_generation;
        const auto ffprobe = m_ffprobe, ffmpeg = m_ffmpeg;
        ++m_inflight;
        m_pool->start(QRunnable::create([this, path, expected, alive, scanAlive, generation, ffprobe, ffmpeg] {
            Probe probe = runProbeWith(path, ffprobe, ffmpeg, *scanAlive);
            probe.unchanged = probe.unchanged && probe.modifiedMs == expected.value("mtimeMs").toLongLong()
                              && probe.size == expected.value("size").toLongLong();
            if (!alive->load()) return;
            QMetaObject::invokeMethod(this, [this, path, probe, generation] {
                --m_inflight;
                if (generation == m_generation) {
                    --m_pending;
                    applyProbe(path, probe);
                    if (!m_pending) publishChanges();
                    else if (!m_publishTimer->isActive()) m_publishTimer->start();
                }
                startProbes();
            }, Qt::QueuedConnection);
        }));
    }
}

// Static on purpose: runs on a pool thread with copies of the tool names,
// so it never reads the library; `alive` lets it skip the decode when the
// library is going away.
SoundLibrary::Probe SoundLibrary::runProbeWith(const QString &path, const QString &ffprobe,
                                               const QString &ffmpeg, const std::atomic<bool> &alive) {
    Probe out;
    if (!alive.load()) return out;
    const QFileInfo before(path);
    out.modifiedMs = before.lastModified().toMSecsSinceEpoch();
    out.size = before.size();
    // All filesystem checks stay on the worker. Never cache facts for a file
    // that changed between discovery and decode, or while decoding.
    const auto checked = [&] {
        if (!alive.load()) return out;
        const QFileInfo after(path);
        out.unchanged = before.exists() && after.exists()
                        && out.modifiedMs == after.lastModified().toMSecsSinceEpoch() && out.size == after.size();
        return out;
    };
    const auto waitForProbe = [&alive](QProcess &process, int timeout) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (alive.load() && elapsed.elapsed() < timeout) {
            if (process.waitForFinished(50))
                return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
            if (process.state() == QProcess::NotRunning) return false;
        }
        process.kill();
        process.waitForFinished(1000);
        return false;
    };
    if (!alive.load()) return checked();
    {
        QProcess p;
        p.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                            QStringLiteral("-select_streams"), QStringLiteral("a:0"),
                            QStringLiteral("-show_entries"),
                            QStringLiteral("stream=sample_rate,channels:format=duration"),
                            QStringLiteral("-of"), QStringLiteral("json"), path});
        if (!waitForProbe(p, 15000))
            return checked();
        const QJsonObject root = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
        if (streams.isEmpty())
            return checked();
        const QJsonObject s = streams.first().toObject();
        out.sampleRate = s.value(QStringLiteral("sample_rate")).toString().toInt();
        out.channels = s.value(QStringLiteral("channels")).toInt();
        out.duration = root.value(QStringLiteral("format")).toObject()
                           .value(QStringLiteral("duration")).toString().toDouble();
        out.ok = out.sampleRate > 0;
    }
    if (!out.ok || !alive.load())
        return checked();
    QProcess p;
    p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
                       QStringLiteral("-vn"), QStringLiteral("-ac"), QStringLiteral("1"),
                       QStringLiteral("-ar"), QStringLiteral("8000"), QStringLiteral("-f"),
                       QStringLiteral("s16le"), QStringLiteral("-t"), QStringLiteral("600"),
                       QStringLiteral("pipe:1")});
    if (!waitForProbe(p, 60000))
        return checked();
    const QByteArray pcm = p.readAllStandardOutput();
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int count = pcm.size() / int(sizeof(qint16));
    out.peaks.resize(kPeakBuckets);
    std::fill(out.peaks.begin(), out.peaks.end(), 0.0f);
    if (count > 0) {
        for (int b = 0; b < kPeakBuckets; ++b) {
            const int from = int(qint64(count) * b / kPeakBuckets);
            const int to = int(qint64(count) * (b + 1) / kPeakBuckets);
            int peak = 0;
            for (int i = from; i < to; ++i)
                peak = qMax(peak, qAbs(int(samples[i])));
            out.peaks[b] = float(peak) / 32768.0f;
        }
    }
    return checked();
}

void SoundLibrary::applyProbe(const QString &path, const Probe &probe) {
    if (!probe.unchanged) {
        m_scanMessage = tr("Some sounds changed during the scan. Rescan to update them.");
        return;
    }
    const int i = indexOf(path);
    if (i < 0)
        return;
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("probed"), true);
    if (probe.ok) {
        m.insert(QStringLiteral("duration"), probe.duration);
        m.insert(QStringLiteral("sampleRate"), probe.sampleRate);
        m.insert(QStringLiteral("channels"), probe.channels);
        QVariantList peaks;
        for (float p : probe.peaks)
            peaks.append(double(p));
        m.insert(QStringLiteral("peaks"), peaks);
        QVariantMap cached;
        cached.insert(QStringLiteral("duration"), probe.duration);
        cached.insert(QStringLiteral("sampleRate"), probe.sampleRate);
        cached.insert(QStringLiteral("channels"), probe.channels);
        cached.insert(QStringLiteral("peaks"), peaks);
        cached.insert(QStringLiteral("mtimeMs"), probe.modifiedMs);
        cached.insert(QStringLiteral("size"), probe.size);
        m_cache.insert(path, cached);
    } else {
        m.insert(QStringLiteral("missing"), true);
    }
    m_items[i] = m;
    m_itemsDirty = true;
    emit probed(path);
    touch();
}

void SoundLibrary::addFolder(const QString &path) {
    if (path.trimmed().isEmpty()) return;
    const QString clean = QDir(path).absolutePath();
    if (m_folders.contains(clean))
        return;
    m_removedFolders.remove(clean);
    m_folders.append(clean);
    emit foldersChanged();
    scanFolders();
    touch();
}

void SoundLibrary::removeFolder(const QString &path) {
    const bool present = m_folders.removeAll(path) > 0;
    if (m_loading) m_removedFolders.insert(path); // the catalogue may still list it
    if (!present)
        return;
    emit foldersChanged();
    scanFolders();
    touch();
}

void SoundLibrary::rescan() {
    scanFolders();
}

void SoundLibrary::addPaths(const QStringList &paths) {
    bool changed = false;
    for (const QString &p : paths) {
        const QFileInfo info(p);
        if (p.isEmpty() || m_folders.contains(info.absoluteFilePath()))
            continue;
        if (!audioExtensions().contains(info.suffix().toLower()))
            continue;
        m_removedFolders.remove(info.absoluteFilePath());
        m_folders.append(info.absoluteFilePath());
        changed = true;
    }
    if (!changed)
        return;
    emit foldersChanged();
    scanFolders();
    touch();
}

QString SoundLibrary::setCategory(const QString &path, const QString &name) {
    // Accept a category as it is shown ("Foley", "Room Tone") as well as its id.
    const QString category = name.toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    if (!category.isEmpty() && !categories().contains(category))
        return tr("There is no sound category called “%1”. Use Dialogue, Ambience, Music, Effects, Foley or Room Tone, or leave it empty to go back to the automatic one.").arg(name);
    const int i = indexOf(path);
    if (i < 0)
        return tr("%1 is not in the sound library.").arg(path);
    if (category.isEmpty())
        m_overrides.remove(path);
    else
        m_overrides.insert(path, category);
    if (m_loading) m_editedOverrides.insert(path);
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("category"), category.isEmpty() ? guessCategory(path) : category);
    m_items[i] = m;
    if (m_discovering) m_metadataEdits.insert(path);
    emit itemsChanged();
    touch();
    return {};
}

void SoundLibrary::setFavorite(const QString &path, bool on) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    if (on)
        m_favorites.insert(path);
    else
        m_favorites.remove(path);
    if (m_loading) m_editedFavorites.insert(path);
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("favorite"), on);
    m_items[i] = m;
    if (m_discovering) m_metadataEdits.insert(path);
    emit itemsChanged();
    touch();
}

void SoundLibrary::setTags(const QString &path, const QStringList &tags) {
    const int i = indexOf(path);
    if (i < 0)
        return;
    if (tags.isEmpty())
        m_tags.remove(path);
    else
        m_tags.insert(path, tags);
    if (m_loading) m_editedTags.insert(path);
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("tags"), tags);
    m_items[i] = m;
    if (m_discovering) m_metadataEdits.insert(path);
    emit itemsChanged();
    touch();
}

QVariantMap SoundLibrary::item(const QString &path) const {
    const int i = indexOf(path);
    return i < 0 ? QVariantMap() : m_items.at(i).toMap();
}

void SoundLibrary::addCollection(const QString &name, const QString &query, const QString &category) {
    if (name.trimmed().isEmpty())
        return;
    removeCollection(name);
    if (m_loading) m_touchedCollections.insert(name.trimmed());
    m_collections.append(QVariantMap{{QStringLiteral("name"), name.trimmed()},
                                     {QStringLiteral("query"), query},
                                     {QStringLiteral("category"), category}});
    emit collectionsChanged();
    touch();
}

void SoundLibrary::removeCollection(const QString &name) {
    if (m_loading) m_touchedCollections.insert(name);
    for (int i = m_collections.size() - 1; i >= 0; --i)
        if (m_collections.at(i).toMap().value(QStringLiteral("name")).toString() == name)
            m_collections.removeAt(i);
    emit collectionsChanged();
    touch();
}

#include "soundlibrary.moc"
