#include "backend.h"
#include "psddocument.h"
#include <QDir>
#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include "playerworker.h"
#include "multicamtiming.h"
#include "mediarefs.h"
#include <QDesktopServices>
#include <QSettings>
#include <QSaveFile>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <QUuid>
#include <QRegularExpression>
#include "thumbnailprovider.h"
#include <QFileInfo>
#include <QJSValue>
#include <QEventLoop>

namespace {
QVariant mediaPlain(const QVariant &value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) return mediaPlain(value.value<QJSValue>().toVariant());
    if (value.metaType().id() == QMetaType::QVariantMap) {
        QVariantMap out; const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) out.insert(it.key(), mediaPlain(it.value()));
        return out;
    }
    if (value.metaType().id() == QMetaType::QVariantList) {
        QVariantList out; for (const QVariant &v : value.toList()) out.append(mediaPlain(v)); return out;
    }
    return value;
}
QVariantMap playbackSources(const QVariantList &library) {
    QVariantMap sources;
    for (const QVariant &v : library) {
        const QVariantMap entry=v.toMap();
        const QString path=entry.value("path").toString();
        if(entry.value("multicam").toBool())continue;
        QVariantMap source;
        for(const char *key:{"proxyPath","preferProxy","inputLut","inputSpace","width","height","fps","fpsNum","fpsDen","duration","pixelAspect","forceFps","sourceFps","sourceDuration","startTimecodeFrames"})
            source.insert(QLatin1String(key),entry.value(QLatin1String(key)));
        source["preferProxy"]=entry.value("preferProxy",true);
        if(entry.value("videoCodec").toString()=="prores_raw" || entry.contains("rawExposure")) {
            source["rawExposure"]=entry.value("rawExposure",0.);
            source["rawRed"]=entry.value("rawRed",1.);
            source["rawBlue"]=entry.value("rawBlue",1.);
        }
        source["originalAvailable"]=QFileInfo::exists(path);
        source["proxyAvailable"]=QFileInfo::exists(entry.value("proxyPath").toString());
        if(!sources.contains(path))sources.insert(path,source);
    }
    // A multicam owns enough source metadata to survive removal of an
    // individual browser row. Real library rows remain authoritative.
    for(const auto &value:library) {
        const auto camera=value.toMap();
        if(camera.value("multicamVersion").toInt()!=2)continue;
        // (Its sound source too, when it has one of its own.)
        auto owned=camera.value("angles").toList();
        if(camera.contains("sound"))owned.append(camera.value("sound"));
        owned+=camera.value("recordings").toList(); // lined up with it, in use or not
        for(const auto &v:owned) {
            const auto angle=v.toMap();const auto path=angle.value("path").toString();
            if(path.isEmpty() || sources.contains(path))continue;
            auto source=angle.value("sourceMeta").toMap();
            source["originalAvailable"]=QFileInfo::exists(path);
            source["proxyAvailable"]=QFileInfo::exists(source.value("proxyPath").toString());
            sources[path]=source;
        }
    }
    return sources;
}
}
bool Backend::mediaTaskCurrent(const QString &id) const {
    return m_mediaTasks && m_mediaTasks->record(id).value(QStringLiteral("options")).toMap()
        .value(QStringLiteral("generation"), -1).toInt() == m_mediaGeneration;
}
void Backend::setupMedia() {
    m_mediaTasks = new MediaTasks(this);
    // Proxies and reframes give playback the machine while it runs.
    connect(this, &Backend::playingChanged, m_mediaTasks, [this] { m_mediaTasks->setYielding(playing()); });
    m_mediaPlayback = QSettings().value(QStringLiteral("media/playback"), QStringLiteral("preferred")).toString();
    if (!QStringList{"original", "proxy", "preferred"}.contains(m_mediaPlayback)) m_mediaPlayback = QStringLiteral("preferred");
    connect(this, &Backend::libraryChanged, this, &Backend::syncMediaSources);
    connect(m_worker, &PlayerWorker::clipAttributesRestored, this, &Backend::restoreClipAttributes);
    syncMediaSources();
    auto *refresh = new QTimer(this);
    refresh->setInterval(5000);
    connect(refresh, &QTimer::timeout, this, &Backend::refreshMediaStatus);
    refresh->start();
    connect(refresh, &QTimer::timeout, this, [this] {
        if (m_restoringLibrary) return;
        for (const QVariant &v : m_mediaCatalog->watches()) {
            const QVariantMap watch = v.toMap();
            if (watch.value(QStringLiteral("enabled")).toBool()) startMediaTask(QStringLiteral("watch"), {watch.value(QStringLiteral("path")).toString()},
                {{"watchId", watch.value(QStringLiteral("id"))}, {"binId", watch.value(QStringLiteral("binId"))}, {"mode", "leave"}});
        }
    });
    connect(m_mediaCatalog, &MediaCatalog::changed, this, [this] {
        QSet<QString> enabled;
        for (const QVariant &v : m_mediaCatalog->watches()) if (v.toMap().value(QStringLiteral("enabled")).toBool()) enabled.insert(v.toMap().value(QStringLiteral("id")).toString());
        for (const QVariant &v : m_mediaTasks->jobs()) {
            const QVariantMap job = v.toMap();
            if (job.value(QStringLiteral("kind")).toString() == QStringLiteral("watch") && !enabled.contains(job.value(QStringLiteral("options")).toMap().value(QStringLiteral("watchId")).toString())) m_mediaTasks->cancel(job.value(QStringLiteral("id")).toString());
        }
    });
    connect(m_worker, &PlayerWorker::mediaRelinked, this, [this](bool ok, const QVariantMap &mapping, const QString &token, const QString &message) {
        const QVariantMap result = m_relinkResults.take(token);
        if (!mediaTaskCurrent(token)) return;
        if (ok) {
            m_library = MediaRefs::rewrite(m_library, mapping).toList();
            QVariantMap metadata;
            for (const QVariant &v : result.value(QStringLiteral("report")).toList()) {
                const QVariantMap row = v.toMap(); metadata.insert(row.value(QStringLiteral("destination")).toString(), row.value(QStringLiteral("metadata")));
            }
            for (int i = 0; i < m_library.size(); ++i) {
                QVariantMap entry = m_library.at(i).toMap();
                const QString path = entry.value(QStringLiteral("path")).toString();
                if (!metadata.contains(path)) continue;
                const QVariantMap facts = metadata.value(path).toMap();
                for (const auto *key : {"checksum","timecode","timecodeFrames"}) if (!facts.contains(key)) entry.remove(key);
                for (auto it = facts.constBegin(); it != facts.constEnd(); ++it)
                    if (it.key() != QStringLiteral("name") && it.key() != QStringLiteral("imported")) entry.insert(it.key(), it.value());
                entry.remove(QStringLiteral("offline")); m_library[i] = entry;
                QMetaObject::invokeMethod(m_worker, [this, path, entry] { m_worker->queueMediaDecor(path, entry.value(QStringLiteral("duration")).toInt(), entry.value(QStringLiteral("fps")).toDouble(), entry.value(QStringLiteral("hasAudio")).toBool(), entry.value(QStringLiteral("hasVideo"), true).toBool() || !entry.value(QStringLiteral("hasAudio")).toBool()); }, Qt::QueuedConnection);
            }
            emit libraryChanged(); setDirty(true);
        }
        const auto summary=ok ? (mapping.size()==1 ? tr("1 source relinked.") : tr("%1 sources relinked.").arg(mapping.size())) : message;
        m_mediaTasks->annotate(token, {{"state", ok ? "completed" : "failed"}, {"message", summary}, {"relinked", ok ? mapping.size() : 0}});
        setStatus(summary); if (!ok) emit failed(summary);
    });
    connect(m_worker, &PlayerWorker::mediaPreviewReady, this, [this](const QString &purpose, const QString &token, const QImage &image, int frame) {
        m_mediaPreviewInFlight.remove(purpose);
        if (token.section(QLatin1Char(':'), 1).toInt() == m_mediaGeneration && !image.isNull() && m_thumbs) {
            const QString key = QStringLiteral("media-preview:") + purpose;
            m_thumbs->insert(key, image);
            m_mediaPreviews.insert(purpose, QVariantMap{{"libraryId", token.section(QLatin1Char(':'), 0, 0)}, {"frame", frame}, {"thumb", QStringLiteral("image://thumbs/") + key + QLatin1Char('?') + QString::number(++m_thumbKey)}});
            emit mediaPreviewsChanged();
        }
        const QVariantMap pending = m_pendingMediaPreview.take(purpose);
        if (!pending.isEmpty()) sendMediaPreview(purpose, pending);
    });
    connect(m_mediaTasks, &MediaTasks::error, this, [this](const QString &message) { setStatus(message); emit failed(message); });
    connect(m_mediaTasks, &MediaTasks::mediaReady, this, [this](const QString &id, const QString &path, const QVariantMap &meta, const QImage &thumbnail) {
        if (!mediaTaskCurrent(id)) return;
        // A second queued import may have been submitted before the first landed.
        for (const QVariant &v : m_library)
            if (v.toMap().value(QStringLiteral("path")).toString() == path && !v.toMap().value(QStringLiteral("offline")).toBool()) return;
        QVariantMap entry = meta;
        const QString binId = m_mediaTasks->record(id).value(QStringLiteral("options")).toMap().value(QStringLiteral("binId")).toString();
        if (!m_mediaCatalog->bin(binId).isEmpty()) entry.insert(QStringLiteral("binId"), binId);
        onImported(true, path, entry, thumbnail); setDirty(true);
        QMetaObject::invokeMethod(m_worker, [this, path, meta] {
            m_worker->queueMediaDecor(path, meta.value(QStringLiteral("duration")).toInt(), meta.value(QStringLiteral("fps")).toDouble(), meta.value(QStringLiteral("hasAudio")).toBool(), meta.value(QStringLiteral("hasVideo"), true).toBool() || !meta.value(QStringLiteral("hasAudio")).toBool());
        }, Qt::QueuedConnection);
    });
    connect(m_mediaTasks, &MediaTasks::proxyReady, this, [this](const QString &id, const QString &source, const QString &proxy) {
        if (!mediaTaskCurrent(id)) return;
        const SourceSyncHold once(this);
        for (int i = 0; i < m_library.size(); ++i) {
            if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() != source) continue;
            setLibraryField(i, QStringLiteral("proxyPath"), proxy);
            setLibraryField(i, QStringLiteral("proxy"), true);
        }
    });
    connect(m_mediaTasks, &MediaTasks::checksumReady, this, [this](const QString &id, const QString &path, const QString &hash) {
        if (!mediaTaskCurrent(id)) return;
        for (int i = 0; i < m_library.size(); ++i)
            if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == path) setLibraryField(i, QStringLiteral("checksum"), hash);
    });
    connect(m_mediaTasks, &MediaTasks::finished, this, [this](const QString &id, const QVariantMap &result) {
        if (!mediaTaskCurrent(id)) return;
        if (!m_timelineImportTask.isEmpty() && id == m_timelineImportTask) {
            QTimer::singleShot(0, this, [this, id] {
                if (id == m_timelineImportTask && mediaTaskCurrent(id) && !m_projectLoadPending)
                    placeImportedTimeline();
            });
            return;
        }
        if (m_mediaTasks->record(id).value(QStringLiteral("kind")).toString() == QStringLiteral("providerManifest") && result.value(QStringLiteral("state")).toString() == QStringLiteral("completed")) {
            const QString provider = m_mediaCatalog->registerProvider(result.value(QStringLiteral("descriptor")).toMap());
            m_mediaTasks->annotate(id, {{"state", provider.isEmpty() ? "failed" : "completed"}, {"message", provider.isEmpty() ? m_status : tr("Storage provider added.")}}); return;
        }
        if (m_mediaTasks->record(id).value(QStringLiteral("kind")).toString() == QStringLiteral("csvImport") && result.value(QStringLiteral("state")).toString() == QStringLiteral("completed")) { applyMediaCsv(id, result); return; }
        if (m_mediaTasks->record(id).value(QStringLiteral("kind")).toString() == QStringLiteral("relinkApply")
            && result.value(QStringLiteral("state")).toString() == QStringLiteral("completed")) {
            const QVariantMap mapping = m_mediaTasks->record(id).value(QStringLiteral("options")).toMap().value(QStringLiteral("mapping")).toMap();
            m_relinkResults.insert(id, result);
            m_mediaTasks->annotate(id, {{"state", "applying"}, {"message", tr("Updating project references…")}});
            QMetaObject::invokeMethod(m_worker, [this, mapping, id] { m_worker->relinkMedia(mapping, id); }, Qt::QueuedConnection);
            return;
        }
        if (m_mediaTasks->record(id).value("kind").toString()=="relinkApply" && result.value("state")!="completed") {
            const auto message=tr("No project references changed. %1").arg(result.value("message").toString());
            m_mediaTasks->annotate(id,{{"message",message}});setStatus(message);return;
        }
        if (m_mediaTasks->record(id).value(QStringLiteral("kind")).toString() == QStringLiteral("watch") && result.value(QStringLiteral("completed")).toInt() == 0) return;
        setStatus(tr("Media task %1: %2").arg(result.value(QStringLiteral("state")).toString(), result.value(QStringLiteral("message")).toString()));
    });
}
QString Backend::copyImportDestination(const QString &sourceDrive) const {
    const QString last = QSettings().value(QStringLiteral("media/copyImportDestination")).toString();
    if (!last.isEmpty() && QFileInfo(last).isDir() && (sourceDrive.isEmpty() || m_volumes->driveOf(last) != sourceDrive))
        return last;
    if (!m_projectPath.isEmpty())
        return QFileInfo(m_projectPath).absoluteDir().filePath(QStringLiteral("Media"));
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(videos.isEmpty() ? QDir::homePath() : videos).filePath(QStringLiteral("OmaEdit Media"));
}

QString Backend::startMediaTask(const QString &kind, const QStringList &paths, const QVariantMap &options) {
    QVariantMap settings = mediaPlain(options).toMap();
    const QString mode = settings.value(QStringLiteral("mode")).toString();
    const QString destination = settings.value(QStringLiteral("destination")).toString().trimmed();
    if (kind == QLatin1String("import") && (mode == QLatin1String("copy") || mode == QLatin1String("move")) && !destination.isEmpty()) {
        if (const QString drive = m_volumes->driveOf(destination); !drive.isEmpty())
            for (const QString &path : paths)
                if (m_volumes->driveOf(path) == drive) {
                    setStatus(tr("The destination folder is on the drive being imported from. Choose a folder on another disk."));
                    return QString();
                }
        QSettings().setValue(QStringLiteral("media/copyImportDestination"), QDir::cleanPath(destination));
    }
    settings.insert(QStringLiteral("generation"), m_mediaGeneration);
    settings.insert(QStringLiteral("projectName"), projectName());
    QVariantList known; QVariantMap checksums;
    for (const QVariant &v : m_library) {
        const QVariantMap entry = v.toMap();
        if (entry.value(QStringLiteral("multicam")).toBool()) continue;
        known.append(QVariantMap{{"path", entry.value(QStringLiteral("path"))}, {"size", entry.value(QStringLiteral("size"))}});
        if (!entry.value(QStringLiteral("checksum")).toString().isEmpty()) checksums.insert(entry.value(QStringLiteral("path")).toString(), entry.value(QStringLiteral("checksum")));
    }
    settings.insert(QStringLiteral("known"), known); settings.insert(QStringLiteral("checksums"), checksums);
    if (kind=="proxy") {
        auto sources=m_lastMediaSources;
        for (auto i=m_lastMediaSources.cbegin();i!=m_lastMediaSources.cend();++i) {
            const auto canonical=QFileInfo(i.key()).canonicalFilePath();
            if (!canonical.isEmpty()) sources[canonical]=i.value();
        }
        settings["sourceSettings"]=sources;
    }
    QVariantMap logging;
    const QVariantMap proposed = settings.value(QStringLiteral("metadata")).toMap();
    for (const char *key : {"tags", "keywords", "reel", "scene", "shot", "take", "angle", "notes", "rating", "inputLut", "inputSpace"})
        if (proposed.contains(QLatin1String(key))) logging.insert(QLatin1String(key), proposed.value(QLatin1String(key)));
    settings.insert(QStringLiteral("metadata"), logging);
    return m_mediaTasks->submit(kind, paths, settings);
}

void Backend::syncMediaSources() {
    // Asked for by an announcement that said the sources are as they were.
    if (std::exchange(m_libraryChangeKeepsSources, false)) return;
    if (m_restoringLibrary || !m_mediaTasks) return;
    if (m_sourceSyncHeld > 0) { m_sourceSyncWanted = true; return; }
    ++m_mediaSourceSyncs;
    const QVariantMap sources=playbackSources(m_library);
    bool refreshed=false;
    for(auto &value:m_library) {
        const auto previous=value.toMap();auto camera=previous;
        if(camera.value("multicamVersion").toInt()!=2)continue;
        auto angles=camera.value("angles").toList();
        if(angles.isEmpty())continue;
        for(auto &v:angles) {
            const auto angle=v.toMap();v=MulticamTiming::withMetadata(angle,sources.value(angle.value("path").toString()).toMap());
        }
        QVariantMap definition{{"version",2},{"angles",angles}};
        definition=MulticamTiming::atRate(definition,camera.value("multicamFps").toDouble());
        if(definition.isEmpty())continue;
        angles=definition.value("angles").toList();camera["angles"]=angles;
        // Its sound source follows its recording's attributes as an angle does.
        if(auto sound=camera.value("sound").toMap();!sound.value("path").toString().isEmpty()) {
            const int at=camera.contains("soundAngle")?camera.value("soundAngle").toInt():-1;
            if(at>=0 && at<angles.size())camera["sound"]=angles[at];
            else {
                sound=MulticamTiming::withMetadata(sound,sources.value(sound.value("path").toString()).toMap());
                const auto alone=MulticamTiming::atRate({{"version",2},{"angles",QVariantList{sound}}},camera.value("multicamFps").toDouble());
                if(!alone.isEmpty())camera["sound"]=alone.value("angles").toList().first();
            }
        }
        // (And the recordings lined up with it, in use or not.)
        if(auto recordings=camera.value("recordings").toList();!recordings.isEmpty()) {
            for(auto &value:recordings) {
                const auto recording=MulticamTiming::withMetadata(value.toMap(),sources.value(value.toMap().value("path").toString()).toMap());
                const auto alone=MulticamTiming::atRate({{"version",2},{"angles",QVariantList{recording}}},camera.value("multicamFps").toDouble());
                if(!alone.isEmpty())value=alone.value("angles").toList().first();
            }
            camera["recordings"]=recordings;
        }
        const auto chosen=angles[qBound(0,camera.value("activeAngle").toInt(),int(angles.size())-1)].toMap();
        camera["sourceFps"]=MulticamTiming::nativeRate(chosen);camera["sourceDuration"]=MulticamTiming::nativeFrames(chosen);
        camera["forceFps"]=chosen.value("forceFps");
        int duration=0;for(const auto &v:angles) {const auto angle=v.toMap();duration=qMax(duration,angle.value("offset").toInt()+angle.value("frames").toInt());}
        camera["duration"]=duration;
        if(camera!=previous) {value=camera;refreshed=true;}
    }
    if(sources!=m_lastMediaSources || m_lastMediaSources.isEmpty()) {
        m_lastMediaSources = sources;
        const QString mode = m_mediaPlayback;
        QMetaObject::invokeMethod(m_worker, [this, sources, mode] { m_worker->setMediaSources(sources, mode); }, Qt::QueuedConnection);
    }
    // Listeners may change attributes again. Publish only after this update is
    // queued, and never overwrite the newer source state after they return.
    if(refreshed)emit libraryChanged();
}
void Backend::announceLibraryKeepingSources() {
    m_libraryChangeKeepsSources = true;
    emit libraryChanged();
    m_libraryChangeKeepsSources = false;
}
void Backend::setMediaPlayback(const QString &mode) {
    if (!QStringList{"original", "proxy", "preferred"}.contains(mode) || mode == m_mediaPlayback) return;
    m_mediaPlayback = mode; QSettings().setValue(QStringLiteral("media/playback"), mode);
    m_lastMediaSources.clear(); syncMediaSources(); emit mediaPlaybackChanged();
}
QVariantList Backend::missingMediaEntries() const {
    QVariantList out; QVariantMap facts; QSet<QString> proxyPaths;
    for (const QVariant &v : m_library) {
        const QVariantMap entry = v.toMap(); facts.insert(entry.value(QStringLiteral("path")).toString(), entry);
        proxyPaths.insert(entry.value(QStringLiteral("proxyPath")).toString());
    }
    const QSet<QString> referenced = MediaRefs::paths(QVariantList{QVariant(m_library), QVariant(m_timeline), QVariant(m_connected), QVariant(m_sequences)});
    for (const QString &path : referenced) {
        if (!QFileInfo(path).isAbsolute() || QFileInfo::exists(path) || proxyPaths.contains(path)) continue;
        QVariantMap entry = facts.value(path).toMap();
        if (entry.isEmpty()) entry = m_lastMediaSources.value(path).toMap();
        entry.insert(QStringLiteral("path"), path); entry.insert(QStringLiteral("offline"), true);
        if (!entry.contains(QStringLiteral("name"))) entry.insert(QStringLiteral("name"), QFileInfo(path).fileName());
        out.append(entry);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) { return a.toMap().value(QStringLiteral("path")).toString() < b.toMap().value(QStringLiteral("path")).toString(); });
    return out;
}

QString Backend::relinkMedia(const QVariantMap &mapping, const QVariantMap &matchOptions) {
    const QVariantMap clean = mediaPlain(mapping).toMap();
    QVariantMap originals;
    for (const QVariant &v : m_library) {
        const QVariantMap entry = v.toMap(); originals.insert(entry.value(QStringLiteral("path")).toString(), entry);
    }
    for (const QVariant &v : missingMediaEntries()) { const QVariantMap entry = v.toMap(); originals.insert(entry.value(QStringLiteral("path")).toString(), entry); }
    QStringList paths;
    for (auto it = clean.constBegin(); it != clean.constEnd(); ++it) {
        if (!originals.contains(it.key()) || !QFileInfo(it.value().toString()).isAbsolute()) { setStatus(tr("Select an existing source reference and an absolute replacement path.")); return {}; }
        paths.append(it.value().toString());
    }
    return startMediaTask(QStringLiteral("relinkApply"), paths, {{"mapping", clean}, {"originals", originals}, {"recheckMatch", !matchOptions.isEmpty()}, {"matchOptions", mediaPlain(matchOptions)}});
}
bool Backend::isPhotoshopMedia(int libraryIndex) const {
    return libraryIndex >= 0 && libraryIndex < m_library.size()
        && m_library.at(libraryIndex).toMap().value(QStringLiteral("path")).toString().endsWith(QStringLiteral(".psd"), Qt::CaseInsensitive);
}
QString Backend::splitPhotoshopLayers(int libraryIndex) {
    if (!isPhotoshopMedia(libraryIndex)) return tr("Choose a Photoshop document in Project Media first.");
    if (m_photoshopSplitting) return tr("A Photoshop document is already being split into layers.");
    const QString source = m_library.at(libraryIndex).toMap().value(QStringLiteral("path")).toString();
    m_photoshopSplitting = true;
    setStatus(tr("Reading the layers of %1…").arg(QFileInfo(source).fileName()));
    QPointer<Backend> self(this);
    const int generation = m_mediaGeneration;
    QThreadPool::globalInstance()->start([self, source, generation] {
        QStringList written; QString error, summary;
        // Nothing thrown while reading or writing may leave this thread: it
        // would end the app with the editor's work unsaved.
        try {
        PsdDocument document;
        if (document.read(source, &error)) {
            const QFileInfo info(source);
            // A folder of its own, so nothing already there is written over:
            // not the pictures of an earlier split (they may be on a timeline)
            // and not a folder of the editor's that happens to have the name.
            const auto fresh = [](const QString &wanted) {
                QString folder = wanted;
                for (int n = 2; QFileInfo::exists(folder) && !QDir(folder).isEmpty() && n < 1000; ++n)
                    folder = wanted + QLatin1Char(' ') + QString::number(n);
                return folder;
            };
            // Beside the document, so the pictures travel with it; a folder
            // that cannot be written there goes with the app's own data.
            QString folder = fresh(info.dir().filePath(info.completeBaseName() + QStringLiteral(" layers")));
            if (!QDir().mkpath(folder) || !QFileInfo(folder).isWritable()) {
                folder = fresh(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/photoshop-layers/")
                    + info.completeBaseName() + QLatin1Char('-') + QString::number(qHash(source), 16));
                if (!QDir().mkpath(folder)) error = tr("No folder could be made for the layer pictures.");
            }
            int unread = 0, blended = 0, hidden = 0;
            const auto &layers = document.layers();
            for (int i = 0; i < layers.size() && error.isEmpty(); ++i) {
                const QImage image = document.layerImage(i);
                if (image.isNull()) { ++unread; continue; }
                QString name = layers[i].name;
                name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("-"));
                name = name.simplified().left(80);
                if (name.isEmpty()) name = tr("Layer");
                const QString path = QDir(folder).filePath(QStringLiteral("%1 %2.png").arg(i + 1, 2, 10, QLatin1Char('0')).arg(name));
                if (!image.save(path, "PNG")) { error = tr("The layer pictures could not be written to %1.").arg(folder); break; }
                written.append(path);
                if (layers[i].blend != QLatin1String("norm") && layers[i].blend != QLatin1String("pass")) ++blended;
                if (!layers[i].visible) ++hidden;
            }
            if (error.isEmpty()) {
                const QImage whole = document.merged();
                const QString path = QDir(folder).filePath(info.completeBaseName() + QStringLiteral(" whole picture.png"));
                if (!whole.isNull() && whole.save(path, "PNG")) written.append(path);
                QStringList notes;
                if (hidden) notes << tr("%n hidden in the document", nullptr, hidden);
                if (blended) notes << tr("%n with a blend mode, shown as Normal", nullptr, blended);
                if (document.skipped()) notes << tr("%n without pixels left out (groups, adjustments)", nullptr, document.skipped());
                if (unread) notes << tr("%n could not be read", nullptr, unread);
                summary = tr("%n layer(s) of %1 imported", nullptr, int(layers.size()) - unread).arg(info.fileName())
                    + (notes.isEmpty() ? QString() : QStringLiteral(": ") + notes.join(QStringLiteral("; "))) + QLatin1Char('.');
                if (layers.isEmpty() && written.isEmpty()) error = tr("%1 has no layers and no saved picture to import.").arg(info.fileName());
            }
        }
        } catch (const std::exception &) {
            written.clear();
            error = tr("%1 could not be split into layers.").arg(QFileInfo(source).fileName());
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, written, error, summary, generation] {
            if (!self) return;
            self->m_photoshopSplitting = false;
            // Another project was opened meanwhile: the pictures stay beside
            // the document, and are not imported into a project they were
            // not made for.
            if (generation != self->m_mediaGeneration) return;
            if (!error.isEmpty()) { self->setStatus(error); emit self->failed(error); return; }
            self->startMediaTask(QStringLiteral("import"), written, {});
            self->setStatus(summary);
            self->m_backgroundTasks->post(QStringLiteral("info"), tr("Photoshop layers"), summary);
            emit self->photoshopLayersSplit(written, summary);
        }, Qt::QueuedConnection);
    });
    return {};
}
QString Backend::attachMediaProxy(int index, const QString &path) {
    if (index < 0 || index >= m_library.size()) return {};
    const QVariantMap entry = m_library.at(index).toMap(); const QString source = entry.value(QStringLiteral("path")).toString();
    return startMediaTask(QStringLiteral("attach"), {path}, {{"source", source}, {"originals", QVariantMap{{source, entry}}}});
}
void Backend::detachMediaProxy(const QVariantList &indexes) {
    QSet<QString> paths;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) paths.insert(m_library.at(v.toInt()).toMap().value(QStringLiteral("path")).toString());
    const SourceSyncHold once(this);
    for (int i = 0; i < m_library.size(); ++i) if (paths.contains(m_library.at(i).toMap().value(QStringLiteral("path")).toString())) {
        setLibraryField(i, QStringLiteral("proxyPath"), {}); setLibraryField(i, QStringLiteral("proxy"), {});
    }
}
void Backend::setMediaInput(const QVariantList &indexes, const QString &lut, int space) {
    if (!lut.isEmpty() && (!QFileInfo::exists(lut) || !lut.endsWith(QStringLiteral(".cube"), Qt::CaseInsensitive))) { setStatus(tr("Choose an existing .cube LUT.")); return; }
    if (space != 0 && space != 601 && space != 709 && space != 2020) return;
    QSet<QString> paths;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) paths.insert(m_library.at(v.toInt()).toMap().value(QStringLiteral("path")).toString());
    const SourceSyncHold once(this);
    for (int i = 0; i < m_library.size(); ++i) if (paths.contains(m_library.at(i).toMap().value(QStringLiteral("path")).toString())) {
        setLibraryField(i, QStringLiteral("inputLut"), lut); setLibraryField(i, QStringLiteral("inputSpace"), space);
    }
}
QString Backend::setMediaRaw(const QVariantList &indexes, double exposure, double red, double blue) {
    if (!std::isfinite(exposure) || !std::isfinite(red) || !std::isfinite(blue)
        || exposure < -6 || exposure > 6 || red < .25 || red > 4 || blue < .25 || blue > 4)
        return tr("RAW exposure must be −6 to +6 stops; white-balance multipliers must be 0.25 to 4.");
    QSet<QString> paths;
    for (const auto &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) {
        const auto entry = m_library[v.toInt()].toMap();
        if (entry.value("videoCodec").toString() == "prores_raw") paths.insert(entry.value("path").toString());
    }
    if (paths.isEmpty()) return tr("Select a ProRes RAW source.");
    bool anyChanged=false;
    {
        const SourceSyncHold once(this);
        for (int i=0;i<m_library.size();++i) if (paths.contains(m_library[i].toMap().value("path").toString())) {
            const auto before=m_library[i].toMap();
            const bool changed=before.value("rawExposure",0.).toDouble()!=exposure || before.value("rawRed",1.).toDouble()!=red || before.value("rawBlue",1.).toDouble()!=blue;
            setLibraryField(i,"rawExposure",exposure); setLibraryField(i,"rawRed",red); setLibraryField(i,"rawBlue",blue);
            // A baked proxy cannot follow new sensor-development settings.
            if (changed) { anyChanged=true; setLibraryField(i,"preferProxy",false); }
        }
    }
    if (anyChanged && m_mediaPlayback=="proxy") setMediaPlayback("preferred");
    else syncMediaSources();
    return {};
}
// Clip Attributes: {forceFps: <fps or 0 = as recorded>, startTimecodeFrames:
// <frames or -1 = the file's>}. A frame-rate interpretation changes the clip's
// effective rate and length; the recorded values are kept as sourceFps /
// sourceDuration so "as recorded" is exact. References share it, like Input.
QString Backend::prepareClipAttributes(const QVariantList &indexes, const QVariantMap &attributes, QVariantList *proposed) {
    if(projectBusy() || m_exporting)return tr("Wait for the current project operation before changing clip attributes.");
    QSet<QString> paths;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) {
        const QVariantMap entry = m_library.at(v.toInt()).toMap();
        if (entry.value(QStringLiteral("multicam")).toBool()) return tr("Multicam clips take their attributes from their angles.");
        paths.insert(entry.value(QStringLiteral("path")).toString());
    }
    if (paths.isEmpty()) return tr("Select a clip first.");
    const bool wantsFps = attributes.contains(QStringLiteral("forceFps"));
    const double forced = attributes.value(QStringLiteral("forceFps")).toDouble();
    if (wantsFps && (!std::isfinite(forced) || (forced != 0.0 && (forced < 1.0 || forced > 240.0)))) return tr("Enter a frame rate from 1 to 240, or 0 for the recorded rate.");
    const bool wantsTc = attributes.contains(QStringLiteral("startTimecodeFrames"));
    const qint64 tc = attributes.value(QStringLiteral("startTimecodeFrames")).toLongLong();
    if (wantsTc && tc < -1) return tr("The start timecode cannot be negative.");
    auto next=m_library;
    for (int i = 0; i < next.size(); ++i) {
        QVariantMap entry = next.at(i).toMap();
        if (!paths.contains(entry.value(QStringLiteral("path")).toString())) continue;
        if (wantsFps) {
            const double sourceFps = entry.value(QStringLiteral("sourceFps"), entry.value(QStringLiteral("fps"))).toDouble();
            const int sourceDuration = entry.value(QStringLiteral("sourceDuration"), entry.value(QStringLiteral("duration"))).toInt();
            if (forced > 0.0 && !entry.value(QStringLiteral("hasVideo"), true).toBool()) return tr("%1 has no picture to interpret at another rate.").arg(entry.value(QStringLiteral("name")).toString());
            if (forced > 0.0 && sourceFps > 0.0 && !qFuzzyCompare(forced, sourceFps)) {
                entry["sourceFps"]=sourceFps;entry["sourceDuration"]=sourceDuration;
                entry["forceFps"]=forced;entry["fps"]=forced;
                entry["duration"]=qMax(1,qRound(sourceDuration*sourceFps/forced));
            } else {
                entry.remove("forceFps");
                if (entry.contains(QStringLiteral("sourceFps"))) {
                    entry["fps"]=sourceFps;entry["duration"]=sourceDuration;
                    entry.remove("sourceFps");entry.remove("sourceDuration");
                }
            }
        }
        if(wantsTc){if(tc<0)entry.remove("startTimecodeFrames");else entry["startTimecodeFrames"]=tc;}
        next[i]=entry;
    }
    *proposed=next;
    return {};
}
void Backend::restoreClipAttributes(const QVariantMap &attributes) {
    auto next=m_library;
    for(auto &value:next) {
        auto entry=value.toMap();const auto path=entry.value("path").toString();
        if(entry.value("multicamVersion").toInt()==2) {
            auto angles=entry.value("angles").toList();
            for(auto &v:angles) {
                auto angle=v.toMap();const auto sourcePath=angle.value("path").toString();
                if(!attributes.contains(sourcePath))continue;
                auto metadata=angle.value("sourceMeta").toMap();const auto source=attributes.value(sourcePath).toMap();
                for(const char *key:{"fps","sourceFps","forceFps","duration","sourceDuration","startTimecodeFrames"}) {
                    const auto setting=source.value(key);
                    if(setting.isValid() && !setting.isNull())metadata[key]=setting;
                    else metadata.remove(key);
                }
                v=MulticamTiming::withMetadata(angle,metadata);
            }
            entry["angles"]=angles;
            if(auto sound=entry.value("sound").toMap();attributes.contains(sound.value("path").toString())) {
                auto metadata=sound.value("sourceMeta").toMap();const auto source=attributes.value(sound.value("path").toString()).toMap();
                for(const char *key:{"fps","sourceFps","forceFps","duration","sourceDuration","startTimecodeFrames"}) {
                    const auto setting=source.value(key);
                    if(setting.isValid() && !setting.isNull())metadata[key]=setting;
                    else metadata.remove(key);
                }
                entry["sound"]=MulticamTiming::withMetadata(sound,metadata);
            }
            if(auto recordings=entry.value("recordings").toList();!recordings.isEmpty()) {
                for(auto &recorded:recordings) {
                    const auto recording=recorded.toMap();
                    if(!attributes.contains(recording.value("path").toString()))continue;
                    auto metadata=recording.value("sourceMeta").toMap();const auto source=attributes.value(recording.value("path").toString()).toMap();
                    for(const char *key:{"fps","sourceFps","forceFps","duration","sourceDuration","startTimecodeFrames"}) {
                        const auto setting=source.value(key);
                        if(setting.isValid() && !setting.isNull())metadata[key]=setting;
                        else metadata.remove(key);
                    }
                    recorded=MulticamTiming::withMetadata(recording,metadata);
                }
                entry["recordings"]=recordings;
            }
            value=entry;
        }
        if(!attributes.contains(path))continue;
        const auto source=attributes.value(path).toMap();
        for(const char *key:{"fps","sourceFps","forceFps","duration","sourceDuration","startTimecodeFrames"}) {
            const auto v=source.value(key);
            if(v.isValid() && !v.isNull())entry[key]=v;
            else entry.remove(key);
        }
        value=entry;
    }
    if(next==m_library)return;
    m_library=next;
    // The worker already committed these clocks. Publishing the library must
    // not enqueue a second rebuild or rewrite the operation's history.
    m_lastMediaSources=playbackSources(m_library);
    emit libraryChanged();setDirty(true);
}
QString Backend::setClipAttributes(const QVariantList &indexes,const QVariantMap &attributes) {
    QVariantList next;const auto problem=prepareClipAttributes(indexes,attributes,&next);
    if(!problem.isEmpty())return problem;
    // The answer, for the agent CLI, without blocking this thread on the
    // worker (a long worker job would freeze the interface; a worker call
    // back here would deadlock): the asynchronous path, awaited locally.
    int request=-1;bool done=false;QString error;QEventLoop loop;
    const auto finished=connect(this,&Backend::clipAttributesFinished,&loop,[&](int id,const QString &why){
        if(id!=request)return;
        error=why;done=true;loop.quit();
    });
    request=applyClipAttributes(indexes,attributes,attributes.value("preserveSelectedFootage").toBool());
    if(!done)loop.exec();
    disconnect(finished);
    return error;
}
int Backend::applyClipAttributes(const QVariantList &indexes,const QVariantMap &attributes,bool preserve) {
    const int request=++m_clipAttributesRequest;
    QVariantList next;const auto problem=prepareClipAttributes(indexes,attributes,&next);
    if(!problem.isEmpty() || next==m_library) {
        QTimer::singleShot(0,this,[this,request,problem]{
            if(!problem.isEmpty())setStatus(problem);
            emit clipAttributesFinished(request,problem);
        });
        return request;
    }
    const auto sources=playbackSources(next);
    m_clipAttributesPending=true;emit projectBusyChanged();
    setStatus(tr("Updating clip attributes…"));
    QMetaObject::invokeMethod(m_worker,[this,request,sources,preserve]{
        const auto error=m_worker->applyClipAttributes(sources,preserve);
        QMetaObject::invokeMethod(this,[this,request,sources,error]{
            // Not busy before the library is published: a libraryChanged
            // listener may change attributes again from inside it.
            m_clipAttributesPending=false;emit projectBusyChanged();
            if(error.isEmpty())restoreClipAttributes(sources);
            setStatus(error.isEmpty()?tr("Clip attributes updated"):error);
            emit clipAttributesFinished(request,error);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
    return request;
}
qint64 Backend::clipTimecodeFrames(const QVariantMap &entry) {
    if (entry.contains(QStringLiteral("startTimecodeFrames"))) return entry.value(QStringLiteral("startTimecodeFrames")).toLongLong();
    return entry.value(QStringLiteral("timecodeFrames"), -1).toLongLong();
}
void Backend::preferMediaProxy(const QVariantList &indexes, bool on) {
    QSet<QString> paths;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) paths.insert(m_library.at(v.toInt()).toMap().value(QStringLiteral("path")).toString());
    const SourceSyncHold once(this);
    for (int i = 0; i < m_library.size(); ++i) if (paths.contains(m_library.at(i).toMap().value(QStringLiteral("path")).toString())) setLibraryField(i, QStringLiteral("preferProxy"), on);
}

void Backend::refreshMediaStatus() {
    // stat() on a dropped network mount blocks: look on the pool, apply here.
    if (m_mediaStatusPending)
        return;
    QList<QPair<QString, QString>> wanted; // path, proxy
    for (const QVariant &v : std::as_const(m_library)) {
        const QVariantMap entry = v.toMap();
        if (!entry.value(QStringLiteral("multicam")).toBool())
            wanted.append({entry.value(QStringLiteral("path")).toString(), entry.value(QStringLiteral("proxyPath")).toString()});
    }
    m_mediaStatusPending = true;
    m_interchangePool->start([this, wanted] {
        QHash<QString, QPair<bool, bool>> found; // path|proxy → offline, proxy available
        for (const auto &[path, proxy] : wanted)
            found.insert(path + QLatin1Char('|') + proxy,
                         {QFileInfo(path).isAbsolute() && !QFileInfo::exists(path), !proxy.isEmpty() && QFileInfo::exists(proxy)});
        QMetaObject::invokeMethod(this, [this, found] {
            m_mediaStatusPending = false;
            bool changed = false;
            for (int i = 0; i < m_library.size(); ++i) {
                QVariantMap entry = m_library.at(i).toMap();
                const auto it = found.constFind(entry.value(QStringLiteral("path")).toString() + QLatin1Char('|')
                                                + entry.value(QStringLiteral("proxyPath")).toString());
                if (entry.value(QStringLiteral("multicam")).toBool() || it == found.constEnd()) continue;
                const bool offline = it->first, available = it->second;
                if (entry.value(QStringLiteral("offline")).toBool() == offline && entry.value(QStringLiteral("proxy")).toBool() == available) continue;
                entry.insert(QStringLiteral("offline"), offline); entry.insert(QStringLiteral("proxy"), available); m_library[i] = entry; changed = true;
            }
            if (changed) { m_lastMediaSources.clear(); emit libraryChanged(); }
            emit mediaStatusRefreshed();
        }, Qt::QueuedConnection);
    });
}
void Backend::revealMediaPath(const QString &path) {
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

namespace {
QStringList metadataColumns() {
    return {"libraryId", "path", "name", "rating", "favorite", "rejected", "tags", "keywords", "notes", "reel", "scene", "shot", "take", "angle", "goodTake", "comments"};
}
bool metadataBoolean(const QString &text, QVariant *out) {
    const QString s = text.trimmed().toLower();
    if (s == QStringLiteral("true") || s == QStringLiteral("1")) { *out = true; return true; }
    if (s.isEmpty() || s == QStringLiteral("false") || s == QStringLiteral("0")) { *out = false; return true; }
    return false;
}
bool customValue(const QVariantList &schema, const QString &key, const QVariant &value, QVariant *out) {
    for (const QVariant &v : schema) {
        const QVariantMap field = v.toMap();
        if (field.value(QStringLiteral("key")).toString() != key) continue;
        if (value.toString().isEmpty()) { *out = {}; return true; }
        const QString type = field.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("number")) { bool ok = false; const double number = value.toString().toDouble(&ok); *out = number; return ok && std::isfinite(number); }
        if (type == QStringLiteral("boolean")) return metadataBoolean(value.toString(), out);
        *out = value.toString(); return true;
    }
    return false;
}
}
QString Backend::exportMediaCsv(const QString &path, const QVariantList &indexes) {
    QVariantList selected = indexes;
    if (selected.isEmpty()) for (int i = 0; i < m_library.size(); ++i) selected.append(i);
    QStringList headers = metadataColumns();
    for (const QVariant &v : m_mediaCatalog->schema()) headers.append(QStringLiteral("custom.") + v.toMap().value(QStringLiteral("key")).toString());
    // Whatever the files themselves carry rides along as source.<tag> columns:
    // the union over the selection, read-only on the way back in.
    QStringList sourceColumns;
    for (const QVariant &v : selected) {
        const int index = v.toInt(); if (index < 0 || index >= m_library.size()) continue;
        const QVariantMap tags = m_library.at(index).toMap().value(QStringLiteral("containerTags")).toMap();
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) if (!sourceColumns.contains(QStringLiteral("source.") + it.key())) sourceColumns.append(QStringLiteral("source.") + it.key());
    }
    sourceColumns.sort(); headers += sourceColumns;
    QVariantList records;
    for (const QVariant &v : selected) {
        const int index = v.toInt(); if (index < 0 || index >= m_library.size()) continue;
        QVariantMap record = m_library.at(index).toMap();
        const QVariantMap custom = record.value(QStringLiteral("customMetadata")).toMap();
        for (auto it = custom.constBegin(); it != custom.constEnd(); ++it) record.insert(QStringLiteral("custom.") + it.key(), it.value());
        const QVariantMap tags = record.value(QStringLiteral("containerTags")).toMap();
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) record.insert(QStringLiteral("source.") + it.key(), it.value());
        QVariantMap clean; for (const QString &key : headers) clean.insert(key, record.value(key)); records.append(clean);
    }
    return startMediaTask(QStringLiteral("csvExport"), {path}, {{"headers", headers}, {"records", records}});
}
QString Backend::importMediaCsv(const QString &path) { return startMediaTask(QStringLiteral("csvImport"), {path}, {}); }
void Backend::setCustomMediaFields(const QVariantList &indexes, const QVariantMap &fields) {
    QVariantMap clean;
    const QVariantMap plain = mediaPlain(fields).toMap();
    for (auto it = plain.constBegin(); it != plain.constEnd(); ++it) {
        QVariant normalized;
        if (!customValue(m_mediaCatalog->schema(), it.key(), it.value(), &normalized)) { setStatus(tr("Invalid value for custom field %1.").arg(it.key())); return; }
        clean.insert(it.key(), normalized);
    }
    for (const QVariant &v : indexes) {
        const int index = v.toInt(); if (index < 0 || index >= m_library.size()) continue;
        QVariantMap custom = m_library.at(index).toMap().value(QStringLiteral("customMetadata")).toMap();
        for (auto it = clean.constBegin(); it != clean.constEnd(); ++it) { if (it.value().isValid()) custom.insert(it.key(), it.value()); else custom.remove(it.key()); }
        setLibraryField(index, QStringLiteral("customMetadata"), custom);
    }
}
void Backend::applyMediaCsv(const QString &id, const QVariantMap &result) {
    QHash<QString, int> ids; QHash<QString, QList<int>> paths;
    for (int i = 0; i < m_library.size(); ++i) { const QVariantMap entry = m_library.at(i).toMap(); ids.insert(entry.value(QStringLiteral("libraryId")).toString(), i); paths[entry.value(QStringLiteral("path")).toString()].append(i); }
    QVariantList pending = m_library, report; QSet<int> seen;
    int applied = 0, errors = 0, recordNumber = 0;
    const QStringList allowed = metadataColumns(); const QVariantList schema = m_mediaCatalog->schema();
    for (const QVariant &v : result.value(QStringLiteral("records")).toList()) {
        ++recordNumber; const QVariantMap record = v.toMap();
        const QString identity = record.value(QStringLiteral("libraryId")).toString(), path = record.value(QStringLiteral("path")).toString();
        int index = -1; QString error;
        if (!identity.isEmpty()) index = ids.value(identity, -1);
        else if (paths.value(path).size() == 1) index = paths.value(path).first();
        if (index < 0) error = tr("No unique matching reference; use libraryId for duplicate references.");
        else if (seen.contains(index)) error = tr("This reference appears more than once in the CSV.");
        QVariantMap entry = index >= 0 ? pending.at(index).toMap() : QVariantMap();
        QVariantMap custom = entry.value(QStringLiteral("customMetadata")).toMap();
        for (auto it = record.constBegin(); it != record.constEnd() && error.isEmpty(); ++it) {
            const QString &key = it.key();
            const QString text = it.value().toString();
            if (key == QStringLiteral("libraryId") || key == QStringLiteral("path")) continue;
            if (key.startsWith(QStringLiteral("source."))) continue; // the file's own tags: exported for reference, never written back
            QVariant value = text;
            if (key.startsWith(QStringLiteral("custom."))) {
                const QString name = key.mid(7);
                if (!customValue(schema, name, text, &value)) { error = tr("Invalid value or unknown custom field: %1.").arg(name); break; }
                if (value.isValid()) custom.insert(name, value); else custom.remove(name);
                continue;
            }
            if (!allowed.contains(key)) { error = tr("Unknown metadata column: %1.").arg(key); break; }
            if (key == QStringLiteral("rating")) { bool ok = true; const int rating = text.isEmpty() ? 0 : text.toInt(&ok); if (!ok || rating < 0 || rating > 5) { error = tr("Rating must be from 0 to 5."); break; } value = rating; }
            else if (key == QStringLiteral("favorite") || key == QStringLiteral("rejected") || key == QStringLiteral("goodTake")) { if (!metadataBoolean(text, &value)) { error = tr("%1 must be true or false.").arg(key); break; } }
            else if (key == QStringLiteral("tags") || key == QStringLiteral("keywords")) {
                QStringList values;
                if (text.trimmed().startsWith('[')) {
                    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
                    if (!document.isArray()) { error = tr("Tags and keywords must be a JSON list or separated with semicolons."); break; }
                    for (const QVariant &tag : document.toVariant().toList()) values.append(tag.toString());
                } else values = text.split(';', Qt::SkipEmptyParts);
                QStringList unique; for (const QString &tag : values) if (!tag.trimmed().isEmpty() && !unique.contains(tag.trimmed(), Qt::CaseInsensitive)) unique.append(tag.trimmed());
                value = unique;
            } else if (key == QStringLiteral("name") && text.trimmed().isEmpty()) value = QFileInfo(entry.value(QStringLiteral("path")).toString()).fileName();
            entry.insert(key, value);
        }
        if (entry.value(QStringLiteral("favorite")).toBool() && entry.value(QStringLiteral("rejected")).toBool()) error = tr("A reference cannot be both favourite and rejected.");
        if (error.isEmpty()) { entry.insert(QStringLiteral("customMetadata"), custom); pending[index] = entry; seen.insert(index); ++applied; } else ++errors;
        report.append(QVariantMap{{"source", path.isEmpty() ? identity : path}, {"state", error.isEmpty() ? "applied" : "failed"}, {"message", tr("Record %1: %2").arg(recordNumber).arg(error.isEmpty() ? tr("Metadata applied.") : error)}});
    }
    if (pending != m_library) { m_library = pending; emit libraryChanged(); setDirty(true); }
    const QString message = tr("%1 metadata records applied · %2 errors").arg(applied).arg(errors);
    m_mediaTasks->annotate(id, {{"state", errors ? applied ? "partial" : "failed" : "completed"}, {"report", report}, {"message", message}});
    setStatus(message);
}

void Backend::previewMedia(int index, int frame, bool audio, const QString &purpose) {
    if (index < 0 || index >= m_library.size() || (purpose != QStringLiteral("skim") && purpose != QStringLiteral("compare"))) return;
    const QVariantMap entry = m_library.at(index).toMap();
    if (entry.value(QStringLiteral("multicam")).toBool()) return;
    const QVariantMap request{{"path", entry.value(QStringLiteral("path"))}, {"frame", frame}, {"audio", audio},
        {"token", entry.value(QStringLiteral("libraryId")).toString() + QLatin1Char(':') + QString::number(m_mediaGeneration)}};
    if (m_mediaPreviewInFlight.contains(purpose)) m_pendingMediaPreview.insert(purpose, request);
    else sendMediaPreview(purpose, request);
}
void Backend::sendMediaPreview(const QString &purpose, const QVariantMap &request) {
    m_mediaPreviewInFlight.insert(purpose);
    QMetaObject::invokeMethod(m_worker, [this, purpose, request] {
        m_worker->previewMedia(request.value(QStringLiteral("path")).toString(), request.value(QStringLiteral("frame")).toInt(), request.value(QStringLiteral("audio")).toBool(), purpose, request.value(QStringLiteral("token")).toString());
    }, Qt::QueuedConnection);
}
void Backend::setMediaGang(bool on) {
    if (m_mediaGang == on) return;
    const QVariantMap anchors{{"context",m_audioEditContext},{"frame",m_position},{"position",m_audioPosition},
        {"sourcePath",m_previewShownPath},{"sourceFrame",m_sourcePosition}};
    m_mediaGang = on;
    QMetaObject::invokeMethod(m_worker, [this, on, anchors] { m_worker->setMediaGang(on,anchors); }, Qt::QueuedConnection);
    emit mediaGangChanged();
}
void Backend::stackMedia(const QVariantList &indexes, const QString &name) {
    QSet<int> members;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) members.insert(v.toInt());
    if (members.size() < 2) { setStatus(tr("Select at least two references to make a stack.")); return; }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (int i : members) { QVariantMap entry = m_library.at(i).toMap(); entry.insert(QStringLiteral("stackId"), id); entry.insert(QStringLiteral("stackName"), name.trimmed().isEmpty() ? tr("Stack") : name.trimmed()); m_library[i] = entry; }
    emit libraryChanged(); setDirty(true);
}
void Backend::unstackMedia(const QVariantList &indexes) {
    QSet<QString> stacks;
    for (const QVariant &v : indexes) if (v.toInt() >= 0 && v.toInt() < m_library.size()) stacks.insert(m_library.at(v.toInt()).toMap().value(QStringLiteral("stackId")).toString());
    stacks.remove(QString()); bool changed = false;
    for (int i = 0; i < m_library.size(); ++i) { QVariantMap entry = m_library.at(i).toMap(); if (!stacks.contains(entry.value(QStringLiteral("stackId")).toString())) continue; entry.remove(QStringLiteral("stackId")); entry.remove(QStringLiteral("stackName")); m_library[i] = entry; changed = true; }
    if (changed) { emit libraryChanged(); setDirty(true); }
}
void Backend::autoStackMedia() {
    QHash<QString, QVariantList> groups;
    static const QRegularExpression numbered(QStringLiteral("[ _-]?[0-9]+$"));
    for (int i = 0; i < m_library.size(); ++i) {
        const QVariantMap entry = m_library.at(i).toMap(); if (entry.value(QStringLiteral("multicam")).toBool() || entry.value(QStringLiteral("subclip")).toBool() || entry.value(QStringLiteral("duplicateRef")).toBool()) continue;
        const QFileInfo file(entry.value(QStringLiteral("path")).toString()); const QString stem = file.completeBaseName();
        if (!numbered.match(stem).hasMatch()) continue;
        QString base = stem; base.remove(numbered);
        const QString key = file.absolutePath() + QLatin1Char('/') + base + QLatin1Char('.') + file.suffix().toLower();
        groups[key].append(i);
    }
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) if (it.value().size() > 1) stackMedia(it.value(), QFileInfo(it.key()).completeBaseName());
}

void Backend::setMonitorVolume(double volume) {
    if (!std::isfinite(volume)) return;
    volume = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(volume, m_monitorVolume)) return;
    m_monitorVolume = volume;
    QMetaObject::invokeMethod(m_worker, [this, volume] { m_worker->setMonitorVolume(volume); }, Qt::QueuedConnection);
    emit monitorVolumeChanged();
}
// (A picture drawn for the viewer stands for its working frame.)
bool Backend::sourceLinear() const { return WorkingFrame::isTagged(m_sourceFrame) || !m_sourceFrame.text(QStringLiteral("omaedit.shown")).isEmpty(); }
void Backend::saveSourceFrame(const QString &path) { saveFrameImage(path, true); }
