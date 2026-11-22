#pragma once
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVariantMap>

// Preferences ▸ Storage. One immutable configuration per process: changing a
// preference cannot move caches or gallery files out from under a running
// render or image request, so saved values apply at the next launch and the
// dialog says so. Empty path = the built-in default for that purpose.
namespace StoragePreferences {
inline QStringList pathKeys() { return {"cache", "keptMedia", "thumbnails", "gallery", "recordings", "proxies"}; }
inline QVariantMap limits() {
    return {{"thumbnailMemoryMiB", QVariantList{16, 512}}, {"thumbnailDiskMiB", QVariantList{0, 16384}},
            {"waveformSamples", QVariantList{1000, 1000000}}};
}
inline QVariantMap defaults() {
    QVariantMap values;
    for (const auto &key : pathKeys()) values[key] = QString();
    values["thumbnailMemoryMiB"] = 64;
    values["thumbnailDiskMiB"] = 512;
    values["waveformSamples"] = 8000;
    return values;
}
inline QVariantMap configured() {
    QSettings settings;
    QVariantMap values = defaults();
    for (const auto &key : pathKeys()) values[key] = settings.value("storage/" + key).toString();
    const auto bounds = limits();
    for (auto i = bounds.begin(); i != bounds.end(); ++i) {
        const auto range = i.value().toList();
        values[i.key()] = qBound(range.at(0).toInt(), settings.value("storage/" + i.key(), values[i.key()]).toInt(), range.at(1).toInt());
    }
    return values;
}
inline const QVariantMap &session() { static const QVariantMap value = configured(); return value; }
inline bool restartRequired() { return configured() != session(); }
inline QString defaultCacheRoot() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation); }
inline QString cacheRoot() {
    const auto custom = session().value("cache").toString();
    return custom.isEmpty() ? defaultCacheRoot() : custom;
}
// What the editor made and a saved project points at: a Render and Replace
// result, a stabilisation analysis. Nothing makes them again when they are
// gone, so they are not cache: they live with the application's data, where
// clearing the cache folder cannot take a project's clips offline, or in the
// folder Preferences ▸ Storage names for them (they are large). keptmedia.h
// brings the files of an earlier folder along when that folder changes.
inline QString defaultKeptMediaRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/kept-media";
}
inline QString keptMediaRoot() {
    const auto custom = session().value("keptMedia").toString();
    return custom.isEmpty() ? defaultKeptMediaRoot() : custom;
}
inline QString thumbnailRoot() {
    const auto custom = session().value("thumbnails").toString();
    return (custom.isEmpty() ? cacheRoot() : custom) + "/thumbnails-v1";
}
inline QString defaultGalleryRoot(const QString &project = {}) {
    return project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/stills"
                             : QFileInfo(project).absolutePath() + "/Stills";
}
inline QString galleryRoot(const QString &project = {}) {
    const auto custom = session().value("gallery").toString();
    return custom.isEmpty() ? defaultGalleryRoot(project) : custom;
}
inline QString defaultRecordingsRoot(const QString &project = {}) {
    if (!project.isEmpty()) return QFileInfo(project).absolutePath() + "/Recordings";
    const auto music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return (music.isEmpty() ? QDir::homePath() : music) + "/OmaEdit Recordings";
}
inline QString recordingsRoot(const QString &project = {}) {
    const auto custom = session().value("recordings").toString();
    return custom.isEmpty() ? defaultRecordingsRoot(project) : custom;
}
inline QString defaultProxyRoot(const QString &project = {}) {
    return project.isEmpty() ? QString() : QFileInfo(project).absolutePath() + "/Proxies";
}
// Empty for an untitled project without a custom folder: Create Proxies then
// asks for a destination instead of inventing one.
inline QString proxyRoot(const QString &project = {}) {
    const auto custom = session().value("proxies").toString();
    return custom.isEmpty() ? defaultProxyRoot(project) : custom;
}
// What each purpose resolves to when its field is empty, for the dialog's
// placeholders; `activePaths` is what this process is really using.
inline QVariantMap defaultPaths(const QString &project = {}) {
    return {{"cache", defaultCacheRoot()}, {"keptMedia", defaultKeptMediaRoot()}, {"thumbnails", defaultCacheRoot() + "/thumbnails-v1"},
            {"gallery", defaultGalleryRoot(project)}, {"recordings", defaultRecordingsRoot(project)},
            {"proxies", defaultProxyRoot(project)}};
}
inline QVariantMap activePaths(const QString &project = {}) {
    return {{"cache", cacheRoot()}, {"keptMedia", keptMediaRoot()}, {"thumbnails", thumbnailRoot()}, {"gallery", galleryRoot(project)},
            {"recordings", recordingsRoot(project)}, {"proxies", proxyRoot(project)}};
}
// Validation precedes all preference writes. A temporary probe checks actual
// access, including ACLs/read-only mounts, without overwriting existing files.
inline QString save(const QVariantMap &draft) {
    QVariantMap clean;
    for (const auto &key : pathKeys()) {
        QString path = draft.value(key).toString().trimmed();
        if (!path.isEmpty()) {
            if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
                return QObject::tr("%1: choose an absolute local folder path.").arg(key);
            path = QDir::cleanPath(path);
            if (!QFileInfo(path).isDir())
                return QObject::tr("%1: the folder must already exist.").arg(key);
            QTemporaryFile probe(QDir(path).filePath(".omaedit-access-XXXXXX"));
            if (!probe.open()) return QObject::tr("%1: the folder is not writable.").arg(key);
        }
        clean[key] = path;
    }
    const auto bounds = limits();
    for (auto i = bounds.begin(); i != bounds.end(); ++i) {
        const auto range = i.value().toList();
        bool ok = false;
        const int number = draft.value(i.key()).toInt(&ok);
        if (!ok || number < range.at(0).toInt() || number > range.at(1).toInt())
            return QObject::tr("%1: enter a value from %2 to %3.").arg(i.key()).arg(range.at(0).toInt()).arg(range.at(1).toInt());
        clean[i.key()] = number;
    }
    QSettings settings;
    // The kept-media folder being left is remembered: its files are a saved
    // project's clips, and the next launch moves them to the new folder
    // (KeptMedia::adopt) and still finds any it could not move.
    const QString leaving = settings.value("storage/keptMedia").toString();
    if (leaving != clean.value("keptMedia").toString()) {
        QStringList earlier = settings.value("storage/keptMediaEarlier").toStringList();
        const QString folder = leaving.isEmpty() ? defaultKeptMediaRoot() : leaving;
        if (!earlier.contains(folder)) earlier.append(folder);
        settings.setValue("storage/keptMediaEarlier", earlier);
    }
    for (auto i = clean.begin(); i != clean.end(); ++i) settings.setValue("storage/" + i.key(), i.value());
    settings.sync();
    return settings.status() == QSettings::NoError ? QString() : QObject::tr("Preferences could not be written.");
}
}
