#include "backend.h"
#include "projectfile.h"
#include "filepicker.h"
#include "playerworker.h"
#include "projectsafety.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>

void Backend::setupProjectSafety() {
    m_backupLimit = qBound(1, QSettings().value("safety/backupLimit", 20).toInt(), 100);
    m_projectArchive = new ProjectArchive(this);
    resetRecoverySession();
    connect(this, &Backend::saveRecoveryRequested, m_worker,
            [worker=m_worker](const QString &path, const QString &origin, const QVariantMap &state, const QString &token) {
                worker->saveRecovery(path, origin, ProjectSafety::encodeLibraryState(state), token);
            });
    connect(this, &Backend::prepareProjectArchiveRequested, m_worker,
            [worker=m_worker](const QVariantMap &state, const QString &token) {
                worker->prepareProjectArchive(ProjectSafety::encodeLibraryState(state), token);
            });
    connect(m_worker, &PlayerWorker::recoverySaved, this,
            [this](const QString &path, const QString &token, const QString &error) {
                if (path != m_recoveryPath || token != m_recoveryToken) {
                    QFile::remove(path);
                    QFile::remove(path + ".discard");
                    return;
                }
                m_autosavePending = false;
                if (error.isEmpty())
                    m_lastRecoverySave = QDateTime::currentDateTime().toString("HH:mm:ss");
                else {
                    setStatus("Recovery copy could not be saved: " + error);
                    emit failed(m_status);
                }
                emit safetyChanged();
            });
    connect(m_worker, &PlayerWorker::projectArchiveReady, this,
            [this](const QVariantMap &document, const QString &token) {
                if (token != m_archiveToken)
                    return;
                const auto r = m_archiveRequest;
                m_archiveRequest.clear();
                m_projectArchive->start(document, r.value("folder").toString(), r.value("name").toString(),
                                        r.value("unused").toBool(), r.value("trim").toBool(),
                                        r.value("handles").toInt());
            });
    connect(m_projectArchive, &ProjectArchive::finished, this,
            [this](bool ok, const QString &path, const QString &error) {
                setStatus(ok ? "Archive saved to " + path : error);
                if (!ok && !error.contains("cancelled"))
                    emit failed(error);
            });
    QTimer::singleShot(0, this, &Backend::refreshProjectSafety);
}
void Backend::resetRecoverySession() {
    m_recoveryLock.reset();
    const QString folder = ProjectSafety::root() + "/recovery";
    QDir().mkpath(folder);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_recoveryPath = folder + '/' + id + ProjectFile::extension();
    m_recoveryLock = std::make_unique<QLockFile>(m_recoveryPath + ".lock");
    m_recoveryLock->setStaleLockTime(0);
    if (!m_recoveryLock->tryLock(0)) {
        m_recoveryPath.clear();
        m_recoveryLock.reset();
    }
    m_recoveryToken.clear();
    m_autosavePending = false;
    m_lastRecoverySave.clear();
}
void Backend::discardRecovery() {
    if (!m_recoveryPath.isEmpty()) {
        // A queued snapshot may finish after Discard. Its tombstone also
        // survives a process interruption before the worker replies.
        QFile marker(m_recoveryPath + ".discard");
        if (marker.open(QIODevice::WriteOnly))
            marker.close();
        m_discardedRecoveryPaths.append(m_recoveryPath);
        QFile::remove(m_recoveryPath);
    }
    resetRecoverySession();
    emit safetyChanged();
}
void Backend::autosaveNow() {
    if (!m_dirty || m_exporting || m_speed != 0. || projectBusy() || m_autosavePending || m_restoringLibrary)
        return;
    if (m_recoveryPath.isEmpty()) {
        setStatus("Recovery storage is unavailable.");
        return;
    }
    m_autosavePending = true;
    m_recoveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit saveRecoveryRequested(m_recoveryPath, m_projectPath, libraryState(), m_recoveryToken);
}
// A save that cannot start says so and finishes, failed: the window's
// close/quit/new/open-after-save intents must not stay armed for later.
void Backend::refuseProjectSave() {
    setStatus(m_exporting ? tr("Saving waits until the render finishes.")
                          : tr("Wait for the project operation to finish, then save."));
    emit projectSaveFinished(false);
}
void Backend::requestProjectSave(const QString &path) {
    if (projectBusy() || m_exporting) {
        refuseProjectSave();
        return;
    }
    // A save is a queue barrier: include the newest coalesced sound cursor,
    // even when its preceding picture is still decoding.
    if (!m_pendingAudioSeek.isEmpty()) { m_audioSeekInFlight=0; dispatchAudioSeek(); }
    emit saveProjectRequested(path, libraryState());
}
void Backend::setBackupLimit(int limit) {
    limit = qBound(1, limit, 100);
    if (limit == m_backupLimit)
        return;
    m_backupLimit = limit;
    QSettings().setValue("safety/backupLimit", limit);
    emit preferencesChanged();
}
void Backend::startClean() {
    if (m_renderQueue)
        m_renderQueue->clearAll();
    // What a session left behind that nothing can use: a lease whose copy is
    // gone, a discard mark with nothing to discard, a copy marked discarded
    // but never removed. A lease still held belongs to a window that is open.
    const QDir folder(ProjectSafety::root() + "/recovery");
    for (const QFileInfo &file : folder.entryInfoList(QDir::Files)) {
        const QString path = file.absoluteFilePath();
        if (path.startsWith(m_recoveryPath))
            continue;
        if (path.endsWith(QLatin1String(".lock"))) {
            const QString copy = path.chopped(5);
            if (QFileInfo::exists(copy))
                continue;
            QLockFile lease(path);
            lease.setStaleLockTime(0);
            if (lease.tryLock(0)) {
                lease.unlock();
                QFile::remove(path);
            }
        } else if (path.endsWith(QLatin1String(".discard"))) {
            const QString copy = path.chopped(8);
            QLockFile lease(copy + ".lock");
            lease.setStaleLockTime(0);
            if (!lease.tryLock(0))
                continue;
            QFile::remove(copy);
            QFile::remove(path);
            lease.unlock();
            QFile::remove(copy + ".lock");
        }
    }
    // Settings of features gone, and the page the last window ended on (each
    // window opens on its own first page).
    QSettings settings;
    for (const char *retired : {"edit/takes", "shell/workspace"})
        settings.remove(QLatin1String(retired));
    refreshProjectSafety();
}
void Backend::refreshProjectSafety() {
    QVariantList copies;
    const auto folder = QDir(ProjectSafety::root() + "/recovery");
    for (const auto &file : folder.entryInfoList(ProjectFile::openPatterns(), QDir::Files, QDir::Time)) {
        const auto path = file.absoluteFilePath();
        if (path == m_recoveryPath || QFileInfo::exists(path + ".discard"))
            continue;
        QLockFile lease(path + ".lock");
        lease.setStaleLockTime(0);
        if (!lease.tryLock(0))
            continue;
        QString error;
        const auto doc = ProjectSafety::readDocument(path, &error);
        const auto meta = doc.value("recovery").toMap();
        const auto origin = meta.value("origin").toString();
        copies.append(QVariantMap{{"path", path},
                                  {"name", origin.isEmpty() ? QStringLiteral("Untitled project")
                                                            : QFileInfo(origin).completeBaseName()},
                                  {"origin", origin},
                                  {"time", file.lastModified().toUTC().toString(Qt::ISODateWithMs)},
                                  {"valid", !doc.isEmpty()},
                                  {"error", error},
                                  {"bytes", file.size()}});
    }
    m_recoveryCopies = copies;
    m_projectBackups = ProjectSafety::backups(m_projectPath);
    emit safetyChanged();
}
void Backend::restoreProjectCopy(const QString &path) {
    refreshProjectSafety();
    bool found = false;
    for (const auto &v : m_recoveryCopies + m_projectBackups)
        if (v.toMap().value("path").toString() == path)
            found = true;
    if (!found) {
        setStatus("That project copy is no longer available.");
        return;
    }
    QString error;
    if (ProjectSafety::readDocument(path, &error).isEmpty()) {
        setStatus(error);
        emit failed(error);
        return;
    }
    m_recoveryLoadPath = path;
    loadProject(QUrl::fromLocalFile(path));
}
void Backend::removeRecoveryCopy(const QString &path) {
    refreshProjectSafety();
    bool found = false;
    for (const auto &v : m_recoveryCopies)
        if (v.toMap().value("path").toString() == path)
            found = true;
    if (!found)
        return;
    QLockFile lease(path + ".lock");
    lease.setStaleLockTime(0);
    if (!lease.tryLock(0))
        return;
    if (!QFile::remove(path)) {
        setStatus("Could not remove that recovery copy.");
        return;
    }
    refreshProjectSafety();
}
void Backend::chooseArchiveFolder() {
    m_chooserMode = ChooserMode::ArchiveFolder;
    m_chooser->openDirectory("Choose the archive's parent folder");
}
void Backend::archiveProject(const QString &folder, const QString &name, bool includeUnused, bool trim,
                             int handles) {
    if (m_projectArchive->busy() || !m_archiveRequest.isEmpty() || m_exporting || projectBusy()) {
        // The agent CLI reads a quiet return as done: say why nothing started.
        setStatus(m_exporting ? tr("Archiving waits until the render finishes.")
                              : tr("An archive or project operation is already running."));
        emit failed(m_status);
        return;
    }
    m_archiveRequest = {{"folder", folder},
                        {"name", name},
                        {"unused", includeUnused},
                        {"trim", trim},
                        {"handles", qBound(0, handles, 25000)}};
    m_archiveToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit prepareProjectArchiveRequested(libraryState(), m_archiveToken);
}
