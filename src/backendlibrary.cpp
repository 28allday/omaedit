#include "backend_p.h"

// ── Media page: storage, import, logging ─────────────────────────────────────


void Backend::importFolderDialog() {
    m_chooserMode = ChooserMode::ImportFolder;
    m_chooser->openDirectory(QStringLiteral("Import Folder"));
}

void Backend::importUrls(const QList<QUrl> &urls) {
    if (urls.size() == 1 && urls.first().isLocalFile() && ProjectFile::isProject(urls.first().toLocalFile())) {
        loadProject(urls.first()); return;
    }
    QStringList paths;
    for (const QUrl &url : urls) if (url.isLocalFile()) paths.append(url.toLocalFile());
    startMediaTask(QStringLiteral("import"), paths, {});
}

bool Backend::clipIsUsed(int libraryIndex) const {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return false;
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    const QString path = entry.value(QStringLiteral("path")).toString();
    const QString id = entry.value(QStringLiteral("multicam")).toBool()
                           ? entry.value(QStringLiteral("id")).toString() : QString();
    for (const QVariantList *list : {&m_timeline, &m_connected})
        for (const QVariant &v : *list) {
            const QVariantMap e = v.toMap();
            if (e.value(ClipKey::path).toString() == path)
                return true;
            if (!id.isEmpty()
                && QJsonDocument::fromJson(e.value(ClipKey::mcam).toString().toUtf8())
                           .object().value(QStringLiteral("id")).toString() == id)
                return true;
        }
    return false;
}

bool Backend::libraryOffline(int libraryIndex) const {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return false;
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    if (entry.value(QStringLiteral("multicam")).toBool()) {
        for (const QVariant &a : entry.value(QStringLiteral("angles")).toList()) {
            const QString path = a.toMap().value(QStringLiteral("path")).toString();
            if (!QFileInfo::exists(path) && !path.contains(QLatin1Char(':'))) {
                const_cast<Backend *>(this)->setStatus(
                    tr("%1: angle %2 is offline — its file is missing.")
                        .arg(entry.value(QStringLiteral("name")).toString(),
                             a.toMap().value(QStringLiteral("name")).toString()));
                return true;
            }
        }
        // The recording its sound comes from, when that is no angle.
        const QVariantMap sound = entry.value(QStringLiteral("sound")).toMap();
        const QString path = sound.value(QStringLiteral("path")).toString();
        if (!path.isEmpty() && !QFileInfo::exists(path) && !path.contains(QLatin1Char(':'))) {
            const_cast<Backend *>(this)->setStatus(
                tr("%1: its sound source %2 is offline — its file is missing.")
                    .arg(entry.value(QStringLiteral("name")).toString(), sound.value(QStringLiteral("name")).toString()));
            return true;
        }
        return false;
    }
    if (!entry.value(QStringLiteral("offline")).toBool()) return false;
    if (m_mediaPlayback != QStringLiteral("original") && QFileInfo::exists(entry.value(QStringLiteral("proxyPath")).toString())) return false;
    const_cast<Backend *>(this)->setStatus(
        tr("%1 is offline — its file is missing.")
            .arg(entry.value(QStringLiteral("name")).toString()));
    return true;
}

bool Backend::setLibraryField(int libraryIndex, const QString &key, const QVariant &value) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return false;
    QVariantMap entry = m_library.at(libraryIndex).toMap();
    if (entry.value(key) == value)
        return true;
    if (value.isValid())
        entry.insert(key, value);
    else
        entry.remove(key);
    m_library[libraryIndex] = entry;
    // What the editor writes about a clip is nothing the player reads.
    static const QSet<QString> logging{QStringLiteral("rating"), QStringLiteral("favorite"), QStringLiteral("rejected"),
        QStringLiteral("tags"), QStringLiteral("keywords"), QStringLiteral("notes"), QStringLiteral("clipMarkers"),
        QStringLiteral("customMetadata"), QStringLiteral("transcript"), QStringLiteral("transcriptLanguage"), QStringLiteral("checksum"),
        QStringLiteral("beatMap")};
    if (logging.contains(key)) announceLibraryKeepingSources();
    else emit libraryChanged();
    setDirty(true);
    return true;
}

// The fields worth a place in the project file: nothing the probe can
// regenerate, only what the editor typed or clicked.
QVariantMap Backend::loggingOf(const QVariantMap &entry) {
    return ProjectSafety::libraryLogging(entry);
}

void Backend::setClipRating(int libraryIndex, int rating) {
    setLibraryField(libraryIndex, QStringLiteral("rating"), qBound(0, rating, 5));
}

void Backend::setClipFavorite(int libraryIndex, bool on) {
    if (on)
        setLibraryField(libraryIndex, QStringLiteral("rejected"), QVariant());
    setLibraryField(libraryIndex, QStringLiteral("favorite"), on ? QVariant(true) : QVariant());
}

void Backend::setClipRejected(int libraryIndex, bool on) {
    if (on)
        setLibraryField(libraryIndex, QStringLiteral("favorite"), QVariant());
    setLibraryField(libraryIndex, QStringLiteral("rejected"), on ? QVariant(true) : QVariant());
}

void Backend::setClipTags(int libraryIndex, const QStringList &tags) {
    QStringList clean;
    for (const QString &tag : tags) {
        const QString t = tag.trimmed();
        if (!t.isEmpty() && !clean.contains(t, Qt::CaseInsensitive))
            clean.append(t);
    }
    setLibraryField(libraryIndex, QStringLiteral("tags"), clean.isEmpty() ? QVariant() : QVariant(clean));
}

void Backend::setClipKeywords(int libraryIndex, const QStringList &keywords) {
    QStringList clean;
    for (const QString &word : keywords) {
        const QString w = word.trimmed();
        if (!w.isEmpty() && !clean.contains(w, Qt::CaseInsensitive))
            clean.append(w);
    }
    setLibraryField(libraryIndex, QStringLiteral("keywords"), clean.isEmpty() ? QVariant() : QVariant(clean));
}

void Backend::setClipNotes(int libraryIndex, const QString &notes) {
    setLibraryField(libraryIndex, QStringLiteral("notes"), notes.isEmpty() ? QVariant() : QVariant(notes));
}

void Backend::renameLibraryClip(int libraryIndex, const QString &name) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QString clean = name.trimmed();
    if (clean.isEmpty())
        return;
    setLibraryField(libraryIndex, QStringLiteral("name"), clean);
}

void Backend::setClipField(int libraryIndex, const QString &key, const QString &value) {
    static const QStringList allowed = {
        QStringLiteral("reel"), QStringLiteral("scene"), QStringLiteral("shot"),
        QStringLiteral("take"), QStringLiteral("angle"), QStringLiteral("goodTake"),
        QStringLiteral("comments")};
    if (!allowed.contains(key))
        return;
    const QString clean = value.trimmed();
    if (key == QStringLiteral("goodTake"))
        setLibraryField(libraryIndex, key, clean.isEmpty() || clean == QStringLiteral("0") ? QVariant() : QVariant(true));
    else
        setLibraryField(libraryIndex, key, clean.isEmpty() ? QVariant() : QVariant(clean));
}

void Backend::addClipMarker(int libraryIndex, int frame, const QString &type, const QString &text) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size() || frame < 0)
        return;
    QVariantList markers = m_library.at(libraryIndex).toMap().value(QStringLiteral("clipMarkers")).toList();
    QVariantMap marker;
    marker.insert(QStringLiteral("frame"), frame);
    marker.insert(QStringLiteral("type"), type.isEmpty() ? QStringLiteral("standard") : type);
    marker.insert(QStringLiteral("text"), text);
    bool replaced = false;
    for (int i = 0; i < markers.size(); ++i) {
        if (markers.at(i).toMap().value(QStringLiteral("frame")).toInt() == frame) {
            markers[i] = marker; // one marker per frame — re-adding retypes it
            replaced = true;
        }
    }
    if (!replaced)
        markers.append(marker);
    std::sort(markers.begin(), markers.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("frame")).toInt()
               < b.toMap().value(QStringLiteral("frame")).toInt();
    });
    setLibraryField(libraryIndex, QStringLiteral("clipMarkers"), markers);
}

void Backend::removeClipMarker(int libraryIndex, int frame) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    QVariantList markers = m_library.at(libraryIndex).toMap().value(QStringLiteral("clipMarkers")).toList();
    for (int i = markers.size() - 1; i >= 0; --i)
        if (markers.at(i).toMap().value(QStringLiteral("frame")).toInt() == frame)
            markers.removeAt(i);
    setLibraryField(libraryIndex, QStringLiteral("clipMarkers"), markers.isEmpty() ? QVariant() : QVariant(markers));
}

void Backend::createSubclip(int libraryIndex, const QString &name) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size() || libraryOffline(libraryIndex))
        return;
    const QVariantMap source = m_library.at(libraryIndex).toMap();
    QVariantMap sub = source;
    sub.insert(QStringLiteral("libraryId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    // A subclip starts life with the source's range and none of its logging
    // except the tags — a fresh note and rating are the point of it.
    for (const char *key : {"notes", "keywords", "rating", "favorite", "rejected", "clipMarkers"})
        sub.remove(QLatin1String(key));
    sub.insert(QStringLiteral("subclip"), true);
    auto recorded = source;
    recorded["duration"] = source.value("sourceDuration", source.value("duration"));
    const QPair<int, int> range = rangeOf(recorded);
    if (range.first >= 0) {
        sub.insert(QStringLiteral("rangeIn"), range.first);
        sub.insert(QStringLiteral("rangeOut"), range.second);
    }
    const QString clean = name.trimmed();
    sub.insert(QStringLiteral("name"),
               clean.isEmpty() ? QStringLiteral("%1 subclip").arg(source.value(QStringLiteral("name")).toString())
                               : clean);
    m_library.insert(libraryIndex + 1, sub);
    if (m_previewLibraryIndex > libraryIndex) { ++m_previewLibraryIndex; emit sourceChanged(); }
    emit libraryChanged();
    setDirty(true);
    setStatus(tr("Created %1").arg(sub.value(QStringLiteral("name")).toString()));
}

void Backend::removeFromLibrary(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QString name = m_library.at(libraryIndex).toMap().value(QStringLiteral("name")).toString();
    if (m_previewLibraryIndex == libraryIndex) {
        viewTimeline();
        m_previewLibraryIndex = -1;
        emit sourceChanged();
    } else if (m_previewLibraryIndex > libraryIndex) {
        --m_previewLibraryIndex;
        emit sourceChanged();
    }
    m_library.removeAt(libraryIndex);
    emit libraryChanged();
    setDirty(true);
    setStatus(tr("Removed %1 from the project").arg(name));
}

void Backend::revealClip(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QString path = m_library.at(libraryIndex).toMap().value(QStringLiteral("path")).toString();
    const QFileInfo info(path);
    if (!info.exists()) {
        setStatus(tr("%1 was not found. It may have been moved or deleted.").arg(info.fileName()));
        if (m_backgroundTasks) m_backgroundTasks->post("error", tr("Could not open project"), m_status);
        return;
    }
    setStatus(tr("Revealed %1").arg(info.fileName()));
    if (QStandardPaths::isTestModeEnabled())
        return; // never launch a file manager from the test suite
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

// A marked range resolves the usual way: only an in point runs to the end, only
// an out point starts from the head, neither means the whole clip.
QPair<int, int> Backend::rangeOf(const QVariantMap &entry) {
    const int in = entry.value(QStringLiteral("rangeIn"), -1).toInt();
    const int out = entry.value(QStringLiteral("rangeOut"), -1).toInt();
    if (in < 0 && out < 0)
        return {-1, -1};
    const int duration = entry.value(QStringLiteral("duration")).toInt();
    return {in < 0 ? 0 : in, out < 0 ? qMax(0, duration - 1) : out};
}


void Backend::moveMediaToBin(const QVariantList &indexes, const QString &binId) {
    const QVariantMap bin = m_mediaCatalog->bin(binId);
    if (!binId.isEmpty() && (bin.isEmpty() || bin.value(QStringLiteral("smart")).toBool())) return;
    bool changed = false;
    for (const QVariant &index : indexes) {
        const int i = index.toInt();
        if (i < 0 || i >= m_library.size()) continue;
        QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("binId")).toString() == binId) continue;
        entry.insert(QStringLiteral("binId"), binId);
        m_library[i] = entry; changed = true;
    }
    if (changed) { emit libraryChanged(); setDirty(true); }
}

void Backend::duplicateMedia(int index, const QString &name) {
    if (index < 0 || index >= m_library.size()) return;
    QVariantMap entry = m_library.at(index).toMap();
    entry.insert(QStringLiteral("libraryId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    entry.insert(QStringLiteral("duplicateRef"), true);
    entry.insert(QStringLiteral("name"), name.trimmed().isEmpty()
        ? entry.value(QStringLiteral("name")).toString() + QStringLiteral(" copy") : name.trimmed());
    m_library.insert(index + 1, entry);
    if (m_previewLibraryIndex > index) { ++m_previewLibraryIndex; emit sourceChanged(); }
    emit libraryChanged(); setDirty(true);
}

void Backend::setMediaFields(const QVariantList &indexes, const QVariantMap &fields) {
    const QVariantMap clean = plainVariant(fields).toMap();
    for (const QVariant &index : indexes) {
        const int i = index.toInt();
        if (i < 0 || i >= m_library.size()) continue;
        for (auto it = clean.constBegin(); it != clean.constEnd(); ++it) {
            if (it.key() == QStringLiteral("notes")) setClipNotes(i, it.value().toString());
            else if (it.key() == QStringLiteral("keywords")) setClipKeywords(i, it.value().toStringList());
            else if (it.key() == QStringLiteral("tags")) setClipTags(i, it.value().toStringList());
            else if (it.key() == QStringLiteral("rating")) setClipRating(i, it.value().toInt());
            else setClipField(i, it.key(), it.value().toString());
        }
    }
}

bool Backend::editMediaMarker(int index, int oldFrame, int frame, const QString &type,
                              const QString &text, const QString &notes) {
    if (index < 0 || index >= m_library.size() || frame < 0) return false;
    const QVariantMap entry = m_library.at(index).toMap();
    if (frame >= entry.value(QStringLiteral("duration")).toInt()) return false;
    QVariantList markers = entry.value(QStringLiteral("clipMarkers")).toList();
    int target = -1;
    for (int i = 0; i < markers.size(); ++i) {
        const int at = markers.at(i).toMap().value(QStringLiteral("frame")).toInt();
        if (at == oldFrame) target = i;
        else if (at == frame) { setStatus(tr("There is already a marker at that frame.")); return false; }
    }
    if (target < 0) return false;
    markers[target] = QVariantMap{{"frame", frame}, {"type", type}, {"text", text}, {"notes", notes}};
    std::sort(markers.begin(), markers.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("frame")).toInt() < b.toMap().value(QStringLiteral("frame")).toInt();
    });
    setLibraryField(index, QStringLiteral("clipMarkers"), markers);
    return true;
}
