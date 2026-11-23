#include "backend_p.h"

// The library row for media that cannot be read right now: the logging is
// kept, the technical facts are zero, and the clip stays listed as offline.
static QVariantMap offlineEntryFor(const QString &path, const QVariantMap &logging, int rangeIn, int rangeOut) {
    QVariantMap entry = logging;
    entry.insert(QStringLiteral("name"), logging.value(QStringLiteral("name"), QFileInfo(path).fileName()));
    entry.insert(QStringLiteral("path"), path);
    entry.insert(QStringLiteral("offline"), true);
    entry.remove(QStringLiteral("thumb"));
    for (const char *key : {"duration", "fps", "width", "height"})
        if (!entry.contains(QLatin1String(key))) entry.insert(QLatin1String(key), 0);
    if (rangeIn >= 0 || rangeOut >= 0) {
        entry.insert(QStringLiteral("rangeIn"), rangeIn);
        entry.insert(QStringLiteral("rangeOut"), rangeOut);
    }
    return entry;
}

QVariantMap Backend::libraryState() const {
    // Qt's shared values freeze this request cheaply. Encoding the potentially
    // large notes, catalogue and inactive timelines belongs to the save worker.
    QVariantList library = m_library;
    // A just-opened project re-probes its clips one at a time. Until each is
    // back, its library line lives only in the pending logging: save it from
    // there, in its line's place, so a save or autosave meanwhile keeps it.
    for (const QString &path : m_restoreOrder) {
        if (!m_restorePending.contains(path))
            continue;
        const int order = m_restoreOrder.indexOf(path);
        int at = 0;
        for (int i = 0; i < library.size(); ++i) {
            const int other = m_restoreOrder.indexOf(library.at(i).toMap().value(QStringLiteral("path")).toString());
            if (other >= 0 && other < order)
                at = i + 1;
        }
        QVariantMap entry = m_pendingMeta.value(path);
        entry.insert(QStringLiteral("path"), path);
        if (!entry.contains(QStringLiteral("name")))
            entry.insert(QStringLiteral("name"), QFileInfo(path).fileName());
        if (const auto range = m_pendingRanges.constFind(path); range != m_pendingRanges.constEnd()) {
            entry.insert(QStringLiteral("rangeIn"), range->first);
            entry.insert(QStringLiteral("rangeOut"), range->second);
        }
        library.insert(at, entry);
        for (const QVariantMap &sub : m_pendingSubclips.values(path)) {
            QVariantMap copy = sub;
            copy.insert(QStringLiteral("path"), path);
            library.insert(++at, copy);
        }
    }
    return {{"library",library},{"markers",m_markers},{"media",m_mediaCatalog->state()}};
}

void Backend::saveProject() {
    if (m_exporting) {
        refuseProjectSave();
        return;
    }
    if (m_projectPath.isEmpty()) {
        saveProjectDialog();
        return;
    }
    pause();
    requestProjectSave(m_projectPath);
}

void Backend::saveProjectAs(const QUrl &url) {
    if (m_exporting) {
        refuseProjectSave();
        return;
    }
    if (!url.isLocalFile())
        return;
    pause();
    QString path = url.toLocalFile();
    path = ProjectFile::withExtension(path);
    requestProjectSave(path);
}

void Backend::saveProjectDialog() {
    if (m_exporting || projectBusy()) {
        refuseProjectSave();
        return;
    }
    pause();
    m_chooserMode = ChooserMode::SaveProject;
    m_chooser->saveFile(QStringLiteral("Save Project"),
                        projectName() + ProjectFile::extension(),
                        QStringLiteral("OmaEdit project"), ProjectFile::savePatterns());
}

void Backend::openProjectDialog() {
    if (m_exporting || projectBusy())
        return;
    pause();
    m_chooserMode = ChooserMode::OpenProject;
    m_chooser->openFile(QStringLiteral("Open Project"),
                        QStringLiteral("OmaEdit project"), ProjectFile::openPatterns());
}

void Backend::loadProject(const QUrl &url) {
    if (projectBusy()) { setStatus("Wait for the current project operation to finish."); return; }
    if (!url.isLocalFile() || m_exporting)
        return;
    const QString path = url.toLocalFile();
    if (!QFileInfo::exists(path)) {
        setStatus(tr("%1 was not found. It may have been moved or deleted.").arg(displayNameFor(url)));
        if (m_backgroundTasks) m_backgroundTasks->post("error", tr("Could not open project"), m_status);
        return;
    }
    setStatus(tr("Opening %1…").arg(displayNameFor(url)));
    // The file's own sequence list comes from the worker as it opens the
    // file; until then the project is one sequence, not the previous
    // project's tabs.
    m_sequences = {QVariantMap{{QStringLiteral("name"), QStringLiteral("Sequence 1")},
                               {QStringLiteral("position"), 0}}};
    m_activeSequence = 0;
    emit sequencesChanged();
    m_sequencesEchoPending = true;
    pause();
    m_projectLoadPending = true; emit projectBusyChanged();
    emit loadProjectRequested(path);
}

void Backend::open(const QUrl &url, bool appendWhenImported) {
    if (!url.isLocalFile()) {
        setStatus(tr("Only local files can be imported."));
        return;
    }

    const QString path = url.toLocalFile();
    // "Open with" and drag-in should treat a project file as a project.
    if (ProjectFile::isProject(path)) {
        loadProject(url);
        return;
    }
    if (!QFileInfo::exists(path)) {
        setStatus(tr("%1 was not found. It may have been moved or deleted.").arg(displayNameFor(url)));
        if (m_backgroundTasks) m_backgroundTasks->post("error", tr("Could not open project"), m_status);
        return;
    }

    bool relink = false;
    for (int i = 0; i < m_library.size(); ++i) {
        const QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("path")).toString() != path)
            continue;
        if (!entry.value(QStringLiteral("offline")).toBool()) {
            setStatus(tr("%1 is already in the library.").arg(displayNameFor(url)));
            return;
        }
        // The file is back: re-import in place, keeping the logging. Its
        // offline subclips come back as copies the way a load restores them.
        QVariantMap logging = entry;
        for (const char *key : {"offline", "duration", "fps", "width", "height", "thumb"})
            logging.remove(QLatin1String(key));
        const bool subclip = entry.contains(QStringLiteral("rangeIn")) || entry.contains(QStringLiteral("rangeOut"));
        if (subclip)
            m_pendingSubclips.insert(path, logging);
        else
            m_pendingMeta.insert(path, logging);
        if (!relink) {
            m_relinkAt.insert(path, i);
            relink = true;
        }
        libraryRowsMoved(i, -1);
        m_library.removeAt(i--);
    }
    if (relink)
        emit libraryChanged();

    if (appendWhenImported)
        m_appendAfterImport = path;
    if (!m_restoringLibrary && !relink) {
        m_importMarks.insert(path, {m_dirty, m_undoText});
        setDirty(true); // a new import belongs to the project file
    }
    setBusy(true);
    setStatus(tr("Importing %1…").arg(displayNameFor(url)));
    emit importRequested(path);
}

// Rows are about to be put in at `at` (count above zero) or the row at `at`
// taken out (count -1): the clip in the source viewer keeps its own row.
void Backend::libraryRowsMoved(int at, int count) {
    if (m_previewLibraryIndex < 0) return;
    if (count > 0 && m_previewLibraryIndex >= at) m_previewLibraryIndex += count;
    else if (count < 0 && m_previewLibraryIndex > at) --m_previewLibraryIndex;
    else if (count < 0 && m_previewLibraryIndex == at) m_previewLibraryIndex = -1;
    else return;
    emit sourceChanged();
}

void Backend::onImported(bool ok, const QString &path, const QVariantMap &meta,
                         const QImage &thumbnail) {
    setBusy(false);

    if (!ok) {
        if (m_appendAfterImport == path)
            m_appendAfterImport.clear();
        // A relink or a project load took the entry out of the library before
        // the probe. The file is unreadable, so the clip goes back as offline
        // media — with its logging and subclips — instead of vanishing from
        // the project on the next save.
        const bool relinking = m_relinkAt.contains(path);
        const bool restoring = m_restorePending.contains(path);
        if (relinking || restoring) {
            const QVariantMap logging = m_pendingMeta.take(path);
            int rangeIn = -1, rangeOut = -1;
            if (const auto range = m_pendingRanges.constFind(path); range != m_pendingRanges.constEnd()) {
                rangeIn = range->first; rangeOut = range->second; m_pendingRanges.erase(range);
            }
            int at = relinking ? qBound(0, m_relinkAt.value(path), int(m_library.size())) : int(m_library.size());
            libraryRowsMoved(at, 1 + int(m_pendingSubclips.count(path)));
            m_library.insert(at, offlineEntryFor(path, logging, rangeIn, rangeOut));
            for (const QVariantMap &sub : m_pendingSubclips.values(path))
                m_library.insert(++at, offlineEntryFor(path, sub, sub.value(QStringLiteral("rangeIn"), -1).toInt(),
                                                       sub.value(QStringLiteral("rangeOut"), -1).toInt()));
            m_pendingSubclips.remove(path);
            m_relinkAt.remove(path);
            if (m_restorePending.remove(path) && m_restorePending.isEmpty())
                m_restoreOrder.clear();
            emit libraryChanged();
            setStatus(tr("Could not read %1. It stays in the library as offline media.").arg(QFileInfo(path).fileName()));
            emit failed(m_status);
            return;
        }
        m_pendingRanges.remove(path);
        m_relinkAt.remove(path);
        // Nothing entered the project: give back the clean state, unless an
        // edit landed while the probe ran.
        const auto mark = m_importMarks.constFind(path);
        if (mark != m_importMarks.constEnd()) {
            if (!mark->dirty && mark->undoText == m_undoText)
                setDirty(false);
            m_importMarks.erase(mark);
        }
        setStatus(tr("Could not read %1. It is not a media file OmaEdit can open.").arg(QFileInfo(path).fileName()));
        emit failed(m_status);
        return;
    }

    m_importMarks.remove(path);
    QVariantMap entry = meta;
    const auto range = m_pendingRanges.constFind(path);
    if (range != m_pendingRanges.constEnd()) {
        entry.insert(QStringLiteral("rangeIn"), range->first);
        entry.insert(QStringLiteral("rangeOut"), range->second);
        m_pendingRanges.erase(range);
    }
    if (m_thumbs && !thumbnail.isNull()) {
        const QString key = QString::number(++m_thumbKey);
        m_thumbs->registerSource(key,path,meta.value("duration").toInt()/10,480);
        m_thumbs->insert(key, thumbnail);
        entry.insert(QStringLiteral("thumb"), QStringLiteral("image://thumbs/") + key);
    }
    const auto logging = m_pendingMeta.constFind(path);
    if (logging != m_pendingMeta.constEnd()) {
        for (auto it = logging->constBegin(); it != logging->constEnd(); ++it) {
            if (QStringList{"duration", "fps", "fpsNum", "fpsDen", "width", "height", "hasAudio", "hasVideo", "size", "pixelAspect"}.contains(it.key()) && meta.contains(it.key())) continue;
            entry.insert(it.key(), it.value());
        }
        m_pendingMeta.erase(logging);
    }
    // Clip Attributes: a frame-rate interpretation is re-derived from the
    // fresh probe — the recorded pair follows the file, the effective pair
    // follows the saved override.
    const double forced = entry.value(QStringLiteral("forceFps")).toDouble();
    if (forced > 0.0 && meta.value(QStringLiteral("fps")).toDouble() > 0.0) {
        const double sourceFps = meta.value(QStringLiteral("fps")).toDouble();
        const int sourceDuration = meta.value(QStringLiteral("duration")).toInt();
        entry.insert(QStringLiteral("sourceFps"), sourceFps);
        entry.insert(QStringLiteral("sourceDuration"), sourceDuration);
        entry.insert(QStringLiteral("fps"), forced);
        entry.insert(QStringLiteral("duration"), qMax(1, qRound(sourceDuration * sourceFps / forced)));
    }
    if (entry.value(QStringLiteral("libraryId")).toString().isEmpty())
        entry.insert(QStringLiteral("libraryId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    // A restored entry takes its line's place: after every entry whose line
    // came earlier, before the offline ones that landed at once.
    int at = m_library.size();
    const bool restored = m_restorePending.remove(path);
    if (restored) {
        const int order = m_restoreOrder.indexOf(path);
        at = 0;
        for (int i = 0; i < m_library.size(); ++i) {
            const int other = m_restoreOrder.indexOf(
                m_library.at(i).toMap().value(QStringLiteral("path")).toString());
            if (other >= 0 && other < order)
                at = i + 1;
        }
        if (m_restorePending.isEmpty())
            m_restoreOrder.clear();
    }
    if (const auto relink = m_relinkAt.constFind(path); relink != m_relinkAt.constEnd()) {
        at = qBound(0, relink.value(), m_library.size());
        m_relinkAt.erase(relink);
    }
    // Subclips of this path, restored as copies of the fresh entry.
    const QList<QVariantMap> subclips = m_pendingSubclips.values(path);
    libraryRowsMoved(at, 1 + int(subclips.size()));
    m_library.insert(at, entry);
    m_pendingSubclips.remove(path);
    for (auto it = subclips.crbegin(); it != subclips.crend(); ++it) {
        QVariantMap sub = entry;
        sub.remove(QStringLiteral("rangeIn"));
        sub.remove(QStringLiteral("rangeOut"));
        for (auto f = it->constBegin(); f != it->constEnd(); ++f)
            if (f.value().isValid() && !(f.key().startsWith(QStringLiteral("range")) && f.value().toInt() < 0))
                sub.insert(f.key(), f.value());
        if (!sub.value(QStringLiteral("duplicateRef")).toBool()) sub.insert(QStringLiteral("subclip"), true);
        m_library.insert(at + 1, sub);
    }
    refreshMulticamThumbs();
    // A project brings its clips back one by one, each with this announcement.
    // The table of sources is built from the whole library, so building it
    // for every clip costs the square of their number: the worker cannot
    // read it before the last probe anyway, so it is built once, for the last.
    if (restored && !m_restorePending.isEmpty()) announceLibraryKeepingSources();
    else emit libraryChanged();
    // A clip the project brought back is part of opening it, which already
    // said so; only a clip the user added is news.
    if (!restored)
        setStatus(tr("Imported %1").arg(entry.value(QStringLiteral("name")).toString()));

    if (m_appendAfterImport == path) {
        m_appendAfterImport.clear();
        emit appendRequested(path, -1, -1);
    }
}
