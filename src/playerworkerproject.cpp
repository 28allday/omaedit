#include "playerworker_p.h"
#include <QDirIterator>

void PlayerWorker::saveProject(const QString &path, const QStringList &library) {
    if (exportActive()) { emit projectSaved(false,path,"A render is using this project."); return; }
    QString error;
    if (QFileInfo::exists(path)) ProjectSafety::makeBackup(path,&error);
    // The backup ring lives under the user's data folder, not beside the
    // project: say so, or a full home partition looks like a broken project disk.
    if (!error.isEmpty()) { emit projectSaved(false,path,tr("Could not keep a backup of the previous version in your data folder (%1). Free some space there and save again.").arg(error)); return; }
    const bool ok = m_timeline.saveDocument(path, currentProjectLibrary(library));
    // Prune whether or not the write succeeded: a failing save must not add a
    // copy of the unchanged project to the ring on every attempt.
    ProjectSafety::pruneBackups(path,qBound(1,QSettings().value("safety/backupLimit",20).toInt(),100));
    emit projectSaved(ok,path,ok?QString():QStringLiteral("The saved project was not replaced; check its folder and free space."));
}

QStringList PlayerWorker::currentProjectLibrary(const QStringList &library) {
    stashActiveSequence();
    auto records = ProjectSafety::libraryRecords(library);
    for (int i=records.size()-1;i>=0;--i) if(records[i].toMap().contains("sequences"))records.removeAt(i);
    // The active timeline's markers are the worker's own, as its clips are:
    // the window's list was taken when Save was asked for, and an edit queued
    // ahead of the save may have added, removed or rippled markers since.
    // (The line keeps its place among the others.)
    int markersAt=records.size();
    for (int i=records.size()-1;i>=0;--i)
        if(records[i].toMap().value("raw").toString().startsWith(QLatin1String("::markers="))){records.removeAt(i);markersAt=i;}
    if (!m_markers.isEmpty()) records.insert(markersAt,QVariantMap{{"raw",ProjectSafety::encodeMarkers(m_markers)}});
    QVariantList list;
    for (int i=0; i<m_sequences.size(); ++i) {
        const auto &s=m_sequences[i];
        QVariantMap item{{"name",s.name},{"id",s.id},{"position",s.position},
                         {"settings",s.settings},{"configured",s.configured},{"lockFormat",s.lockFormat}};
        if (!s.audioPosition.isEmpty()) item["audioPosition"]=s.audioPosition;
        if (!s.binId.isEmpty()) item["binId"]=s.binId;
        if (s.synced) item["synced"]=true;
        if (i!=m_activeSequence) item["snapshot"]=s.snapshot;
        list.append(item);
    }
    records.append(QVariantMap{{"sequences",QVariantMap{{"active",m_activeSequence},{"list",list}}}});
    return ProjectSafety::encodeLibrary(records);
}

void PlayerWorker::saveRecovery(const QString &path,const QString &origin,const QStringList &library,const QString &token) {
    if (QFileInfo::exists(path+".discard")) { emit recoverySaved(path,token,{});return; }
    if(exportActive()){emit recoverySaved(path,token,"A render is using this project.");return;}
    auto doc=m_timeline.snapshotDocument(currentProjectLibrary(library));
    doc["recovery"]=QVariantMap{{"origin",origin},{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QString error;
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))error="Cannot create the recovery folder.";
    else ProjectSafety::writeDocument(path,doc,&error);
    if (QFileInfo::exists(path+".discard")) QFile::remove(path);
    emit recoverySaved(path,token,error);
}

void PlayerWorker::prepareProjectArchive(const QStringList &library,const QString &token) {
    emit projectArchiveReady(m_timeline.snapshotDocument(currentProjectLibrary(library)),token);
}

void PlayerWorker::dropStabJob() {
    if (!m_stabJob)
        return;
    const int track = m_stabJob->track, index = m_stabJob->index;
    m_stabJob.reset();
    emit stabProgress(track, index, -1);
}

void PlayerWorker::adoptKeptMedia(int settledSeconds) {
    if (settledSeconds < 0)
        settledSeconds = KeptMedia::settlingSeconds();
    if (!KeptMedia::waiting(settledSeconds)) {
        KeptMedia::forgetEmptied();
        return;
    }
    const QString folder = StoragePreferences::keptMediaRoot();
    emit keptMediaAdopting(folder); // onto another disk this is a copy, and long
    const KeptMedia::Adopted result = KeptMedia::adopt(settledSeconds);
    emit keptMediaAdopted(folder, result.moved, result.problems);
}

// Every string in `value` that can name a kept file, map keys included (the
// source table is keyed by path).
static void keptMediaNamedIn(const QVariant &value, QSet<QString> *named) {
    const int type = value.metaType().id();
    if (type == QMetaType::QString) {
        const QString text = value.toString();
        if (text.contains(QLatin1String("render-")) || text.contains(QLatin1String(".trf")))
            named->insert(text);
    } else if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
        for (const QVariant &item : value.toList())
            keptMediaNamedIn(item, named);
    } else if (type == QMetaType::QVariantMap) {
        const QVariantMap map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            keptMediaNamedIn(it.key(), named);
            keptMediaNamedIn(it.value(), named);
        }
    }
}

void PlayerWorker::sweepKeptMedia(const QStringList &documents, const QStringList &alsoUsed, bool remove) {
    // A kept file's name is its own, so a project uses the file exactly when
    // its name appears in the project, wherever and however it is written
    // there. Anything that names a file keeps it: being wrong that way costs
    // disk space, the other way a clip.
    QSet<QString> named;
    keptMediaNamedIn(m_timeline.entries(), &named);
    keptMediaNamedIn(m_timeline.mediaSources(), &named);
    for (const Sequence &sequence : std::as_const(m_sequences))
        keptMediaNamedIn(sequence.snapshot, &named);
    for (const QVariantList &snapshot : std::as_const(m_undoStack))
        keptMediaNamedIn(snapshot, &named);
    for (const QVariantList &snapshot : std::as_const(m_redoStack))
        keptMediaNamedIn(snapshot, &named);
    keptMediaNamedIn(alsoUsed, &named);
    QStringList unused;
    for (const QString &path : KeptMedia::files()) {
        // A file still being made is named by nothing yet: this editor's
        // own, or (changed in the last two minutes) another running one's.
        if ((m_render && m_render->path == path) || (m_stabJob && m_stabJob->results == path)
            || QFileInfo(path).lastModified().secsTo(QDateTime::currentDateTime()) < KeptMedia::settlingSeconds())
            continue;
        const QString name = QFileInfo(path).fileName();
        bool used = false;
        for (const QString &text : std::as_const(named))
            if (text.contains(name)) { used = true; break; }
        if (!used)
            unused.append(path);
    }
    // The projects that are not open: the ones the editor named (the recent
    // list, the open project's file as it was last saved), and every copy
    // the application holds itself (saved versions, recovery copies, queued
    // renders' snapshots).
    QStringList files = documents;
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString kept = QDir::cleanPath(StoragePreferences::keptMediaRoot()) + QLatin1Char('/');
    QDirIterator copies(data, QDir::Files, QDirIterator::Subdirectories);
    while (!unused.isEmpty() && copies.hasNext()) {
        const QString path = copies.next();
        if (!path.startsWith(kept) && (ProjectFile::isProject(path) || path.endsWith(QLatin1String(".json"))))
            files.append(path);
    }
    files.removeDuplicates();
    for (const QString &path : std::as_const(files)) {
        if (unused.isEmpty())
            break;
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > ProjectSafety::documentLimit())
            continue;
        const QByteArray text = file.readAll();
        if (!KeptMedia::named(text))
            continue;
        unused.removeIf([&](const QString &candidate) { return text.contains(QFileInfo(candidate).fileName().toUtf8()); });
    }
    int count = 0, failed = 0;
    qint64 bytes = 0;
    for (const QString &path : std::as_const(unused)) {
        const qint64 size = QFileInfo(path).size();
        if (remove && !QFile::remove(path)) {
            ++failed;
            continue;
        }
        ++count;
        bytes += size;
    }
    emit keptMediaSwept({{QStringLiteral("folder"), StoragePreferences::keptMediaRoot()},
                         {QStringLiteral("files"), count}, {QStringLiteral("bytes"), bytes},
                         {QStringLiteral("removed"), remove}, {QStringLiteral("failed"), failed}});
}

void PlayerWorker::loadProject(const QString &path) {
    if (exportActive())
        return;
    setSpeed(0.0);
    QStringList library;
    const QVariantList before = m_timeline.entries();
    const bool ok = m_timeline.loadDocument(path, &library);
    // Only once the file has opened: one that cannot be read leaves the open
    // project as it is, with its clip render, stabilise and tracking jobs.
    const bool replaced = ok || m_timeline.entries() != before;
    if (replaced) {
        ++m_projectGeneration;
        if (m_render) finishRender(false); // a render of the old project has nowhere to land
        dropStabJob();
        dropTrackJob(); // the clip it was for has gone
    }
    const QVariantList records = ok ? ProjectSafety::decodeLibraryState(library) : QVariantList();
    // The file's own timelines come back through the backend a moment after
    // this (setSequences). The one just loaded is its active timeline, so it
    // carries that timeline's id from the start: a window that has shown the
    // clips must not see their timeline change its name afterwards.
    QString storedId;
    for (const QVariant &value : records) {
        const QVariantMap record = value.toMap();
        if (record.value(QStringLiteral("kind")) != QStringLiteral("sequences")) continue;
        const QVariantList list = record.value(QStringLiteral("list")).toList();
        if (!list.isEmpty())
            storedId = list.value(qBound(0, record.value(QStringLiteral("active")).toInt(), int(list.size()) - 1))
                           .toMap().value(QStringLiteral("id")).toString();
    }
    if (ok) {
        m_sourceMode=false; m_preview.close(); m_previewPath.clear(); m_previewPos=0;
        m_loop=false; m_loopIn=m_loopOut=m_sourceLoopIn=m_sourceLoopOut=-1;
        // A new project starts a new history — stale snapshots of the old
        // timeline must not be reachable. (An unparsable file leaves the
        // current timeline, and its history, alone.)
        m_undoStack.clear();
        m_redoStack.clear();
        m_position = 0;
        m_sequences = {Sequence{QStringLiteral("Sequence 1"), {}, 0, storedId.isEmpty() ? newSequenceId() : storedId, {}}};
        m_activeSequence = 0;
        m_sequences[0].configured = true;
        m_sequences[0].settings = m_timeline.projectSettings();
        // Which timeline it is goes out before its clips: a window drops its
        // selection when the timeline changes, and must do that before the
        // new clips can be selected, not after.
        announceSequences();
        announceTimeline();
        refreshFrame();
    } else {
        // The backend showed one tab while it waited — put its list back.
        // The open project keeps its timelines and its history even when its
        // clips could not all be put back: undo can still reach them.
        announceTimeline(replaced);
        announceSequences();
        if (replaced) {
            m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
            refreshFrame();
        }
    }
    emit projectLoaded(ok, path, library, records);
}

void PlayerWorker::createProject(int request, const QString &anchorId, const QVariantMap &settings) {
    const auto fail=[this,request](const QString &error){emit projectCreateFinished(request,false,error);};
    if (exportActive()) { fail(tr("Cannot create a project while exporting.")); return; }
    bool found=false;
    for (const auto &sequence : m_sequences) found|=!anchorId.isEmpty() && sequence.id==anchorId;
    if (!found) { fail(tr("The project changed. Reopen New Project.")); return; }
    if (const auto error=CreationNames::projectProblem(settings); !error.isEmpty()) { fail(error); return; }
    resetTimeline(settings);
    emit projectCreateFinished(request,true,{});
}
void PlayerWorker::newTimeline() {
    if (exportActive()) return;
    ++m_projectGeneration;
    if (m_render) finishRender(false);
    resetTimeline(Timeline::defaultProjectSettings());
}
void PlayerWorker::resetTimeline(const QVariantMap &settings) {
    m_lastEditGesture = 0;
    setSpeed(0.0);
    dropStabJob();
    dropTrackJob();
    m_sourceMode = false;
    m_preview.close(); m_previewPath.clear(); m_previewPos = 0;
    m_loop = false; m_loopIn = m_loopOut = m_sourceLoopIn = m_sourceLoopOut = -1;
    while (!m_stripJobs.isEmpty()) finishStripWork(0, QStringLiteral("cancelled"));
    m_undoStack.clear();
    m_redoStack.clear();
    m_position = 0;
    m_timeline.clear();
    m_timeline.setMasterMixMap({});
    m_timeline.setAdrCues({}); m_timeline.setCaptionSettings({});
    m_timeline.setProjectSettings(settings);
    if (!m_markers.isEmpty()) {
        m_markers.clear();
        emit markersChanged(m_markers);
    }
    m_sequences = {Sequence{QStringLiteral("Sequence 1"), {}, 0, newSequenceId(), {}}};
    m_activeSequence = 0;
    m_sequences[0].settings = m_timeline.projectSettings();
    announceTimeline();
    emit frameReady(QImage(), 0);
    announceSequences();
    // Emitted after the announce, so Backend can wipe the dirty flag the
    // empty-timeline announce just set (same ordering trick as projectLoaded).
    emit timelineCleared();
}
