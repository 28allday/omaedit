#pragma once
#include "storagepreferences.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QStorageInfo>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Kept media: the files the editor made and a saved project points at: a
// Render and Replace result (`renders/render-<id>-<name>.mkv`) and a
// stabilisation analysis (`stabilise/<key>-<w>x<h>.trf`), both under
// StoragePreferences::keptMediaRoot(). A file's name is its own (a fresh id,
// or a digest of what was analysed), so wherever a project says the file is,
// its name finds it.
//
// The folder has been somewhere else before: builds up to November 2026 wrote
// these files into the cache folder, and the editor may choose another folder
// in Preferences ▸ Storage. `adopt` moves what is still in an earlier place
// into today's folder, and `resolve` turns a path a project saved then into
// where the file is now. A file is never overwritten and never removed from
// its old place before its copy is whole.
namespace KeptMedia {
struct Place { QString folder, kind; };
inline QStringList kinds() { return {QStringLiteral("renders"), QStringLiteral("stabilise")}; }
inline QString folder(const QString &kind) { return StoragePreferences::keptMediaRoot() + QLatin1Char('/') + kind; }
// Only files named as the editor names them are kept media: the cache folder
// holds other things under the same roof (frame-blend intermediates,
// half-written analyses), and a folder the editor chose may hold files of
// their own, which are never moved or removed.
inline bool isKept(const QString &kind, const QString &name) {
    static const QRegularExpression render(QStringLiteral(
        "^render-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}-[A-Za-z0-9_-]*\\.mkv$"));
    static const QRegularExpression analysis(QStringLiteral("^[0-9a-f]{20}(-\\d{1,5}x\\d{1,5})?\\.trf$"));
    if (kind == QStringLiteral("renders")) return render.match(name).hasMatch();
    return kind == QStringLiteral("stabilise") && analysis.match(name).hasMatch();
}
// Which kind a folder of that name holds, today's names and the cache's.
inline QString kindOf(const QString &folderName) {
    if (folderName == QStringLiteral("renders") || folderName == QStringLiteral("omaedit-renders")) return QStringLiteral("renders");
    if (folderName == QStringLiteral("stabilise") || folderName == QStringLiteral("omaedit-stab")) return QStringLiteral("stabilise");
    return {};
}
// Whether two paths name one file or folder: the same device and inode,
// however they are spelt (a link, a bind mount, a home reached by two
// paths). Two that do not both exist are told apart by their clean paths.
inline bool samePlace(const QString &a, const QString &b) {
    struct stat one {}, other {};
    if (::stat(QFile::encodeName(a).constData(), &one) == 0 && ::stat(QFile::encodeName(b).constData(), &other) == 0)
        return one.st_dev == other.st_dev && one.st_ino == other.st_ino;
    return QDir::cleanPath(a) == QDir::cleanPath(b);
}
// Everywhere kept media may still lie that is not today's folder: the cache
// folder (this session's and the built-in one), the built-in kept-media
// folder, and each folder the preference has left.
inline QList<Place> earlierPlaces() {
    QList<Place> places;
    const QString here = QDir::cleanPath(StoragePreferences::keptMediaRoot());
    QStringList seen;
    const auto add = [&](const QString &folder, const QString &kind) {
        const QString clean = QDir::cleanPath(folder);
        // Today's folder by another name holds today's files themselves.
        if (seen.contains(clean) || samePlace(clean, KeptMedia::folder(kind))) return;
        seen.append(clean);
        places.append({clean, kind});
    };
    for (const QString &cache : {StoragePreferences::cacheRoot(), StoragePreferences::defaultCacheRoot()}) {
        add(cache + QStringLiteral("/omaedit-renders"), QStringLiteral("renders"));
        add(cache + QStringLiteral("/omaedit-stab"), QStringLiteral("stabilise"));
    }
    QStringList roots = QSettings().value(QStringLiteral("storage/keptMediaEarlier")).toStringList();
    roots.append(StoragePreferences::defaultKeptMediaRoot());
    for (const QString &root : std::as_const(roots)) {
        if (root.isEmpty() || samePlace(root, here)) continue;
        for (const QString &kind : kinds()) add(root + QLatin1Char('/') + kind, kind);
    }
    return places;
}
inline QStringList keptIn(const Place &place) {
    QStringList names;
    for (const QString &name : QDir(place.folder).entryList(QDir::Files | QDir::NoSymLinks, QDir::Name))
        if (isKept(place.kind, name)) names.append(name);
    return names;
}
// A render is written under its own name. One changed in the last two minutes
// may be another running editor's, still being written into the folder this
// one has left: it is not moved now, and a later launch moves it.
inline int settlingSeconds() { return 120; }
inline QStringList settledIn(const Place &place, int settled) {
    QStringList names;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString &name : keptIn(place))
        if (settled <= 0 || QFileInfo(place.folder + QLatin1Char('/') + name).lastModified().secsTo(now) >= settled) names.append(name);
    return names;
}
// Whether a project's text can name kept media at all: the cheap question
// before its paths are looked at one by one.
inline bool named(const QByteArray &document) { return document.contains("render-") || document.contains(".trf"); }
// Where the kept file a project names is now. A path that is not kept
// media, or whose file is where it says, comes back as it is; so does one
// whose file is nowhere to be found.
inline QString resolve(const QString &path) {
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) return path;
    const QString name = path.mid(slash + 1);
    const qsizetype above = path.lastIndexOf(QLatin1Char('/'), slash - 1);
    const QString kind = kindOf(path.mid(above + 1, slash - above - 1));
    if (kind.isEmpty() || !isKept(kind, name) || QFileInfo::exists(path)) return path;
    const QString here = folder(kind) + QLatin1Char('/') + name;
    if (QFileInfo::exists(here)) return here;
    for (const Place &place : earlierPlaces()) // one that could not be moved yet
        if (place.kind == kind && QFileInfo::exists(place.folder + QLatin1Char('/') + name))
            return place.folder + QLatin1Char('/') + name;
    return path;
}
// Whether anything waits in an earlier place.
inline bool waiting(int settled = settlingSeconds()) {
    for (const Place &place : earlierPlaces())
        if (!settledIn(place, settled).isEmpty()) return true;
    return false;
}
inline bool durable(const QString &path, bool directory) {
    const int handle = ::open(QFile::encodeName(path).constData(), directory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (handle < 0) return false;
    const bool ok = ::fsync(handle) == 0;
    ::close(handle);
    return ok;
}
// One file onto another disk: copied under a working name, checked, written
// out, given its name, and only then is the old one removed. Stopped at any
// point, the file is whole in one place or the other.
inline QString copyAcross(const QString &from, const QString &to) {
    const QString into = QFileInfo(to).absolutePath();
    const qint64 size = QFileInfo(from).size();
    if (QStorageInfo(into).bytesAvailable() < size)
        return QCoreApplication::translate("KeptMedia", "%1 stays where it is: %2 has no room for it.").arg(from, into);
    const QString working = to + QStringLiteral(".moving");
    QFile::remove(working); // what a move that was stopped left
    if (!QFile::copy(from, working) || QFileInfo(working).size() != size || !durable(working, false)
        || QFileInfo::exists(to)
        || std::rename(QFile::encodeName(working).constData(), QFile::encodeName(to).constData()) != 0) {
        QFile::remove(working);
        return QCoreApplication::translate("KeptMedia", "%1 stays where it is: it could not be copied to %2.").arg(from, into);
    }
    durable(into, true);
    return QFile::remove(from) ? QString()
                               : QCoreApplication::translate("KeptMedia", "%1 is now in %2, and the old copy could not be removed.").arg(from, into);
}
// One file to its new folder; the answer is what went wrong, empty when it
// is there. On the same disk it is renamed, onto another it is copied
// (copyAcross). A file already there is never overwritten.
inline QString moveOne(const QString &from, const QString &to) {
    const QFileInfo source(from), target(to);
    const QString into = target.absolutePath();
    if (target.exists()) {
        // The very file, reached by another path: it is where it belongs,
        // and removing `from` would remove it.
        if (samePlace(from, to)) return {};
        // The same name is the same file. Its copy was finished before and
        // only the removal was left; a file that differs is not touched.
        if (target.size() != source.size())
            return QCoreApplication::translate("KeptMedia", "%1 stays where it is: %2 already holds another file of that name.").arg(from, into);
        return QFile::remove(from) ? QString()
                                   : QCoreApplication::translate("KeptMedia", "%1 is now in %2, and the old copy could not be removed.").arg(from, into);
    }
    if (!QDir().mkpath(into))
        return QCoreApplication::translate("KeptMedia", "%1 stays where it is: %2 could not be made.").arg(from, into);
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return {};
    return copyAcross(from, to);
}
// A folder the preference left is remembered only while kept media is in it.
inline void forgetEmptied() {
    QSettings settings;
    const QStringList remembered = settings.value(QStringLiteral("storage/keptMediaEarlier")).toStringList();
    // (Against the saved preference, not this session's folder: a folder
    // saved a moment ago is not in use until the next launch.)
    QString chosen = settings.value(QStringLiteral("storage/keptMedia")).toString();
    if (chosen.isEmpty()) chosen = StoragePreferences::defaultKeptMediaRoot();
    QStringList still;
    for (const QString &root : remembered) {
        bool holds = false;
        for (const QString &kind : kinds())
            holds |= !samePlace(root + QLatin1Char('/') + kind, chosen + QLatin1Char('/') + kind)
                     && !keptIn({root + QLatin1Char('/') + kind, kind}).isEmpty();
        if (holds && !samePlace(root, chosen)) still.append(root);
    }
    if (still == remembered) return;
    if (still.isEmpty()) settings.remove(QStringLiteral("storage/keptMediaEarlier"));
    else settings.setValue(QStringLiteral("storage/keptMediaEarlier"), still);
}
struct Adopted { int moved = 0; QStringList problems; };
// Every kept file of an earlier place into today's folder. Safe to run at
// every launch and to stop half way; an emptied folder the preference left is
// forgotten, and an emptied cache folder of kept media is removed.
inline Adopted adopt(int settled = settlingSeconds()) {
    Adopted result;
    for (const Place &place : earlierPlaces()) {
        for (const QString &name : settledIn(place, settled)) {
            const QString problem = moveOne(place.folder + QLatin1Char('/') + name, folder(place.kind) + QLatin1Char('/') + name);
            if (problem.isEmpty()) ++result.moved;
            else result.problems.append(problem);
        }
        QDir().rmdir(place.folder); // only when nothing at all is left in it
    }
    forgetEmptied();
    return result;
}
// Every kept file in today's folder, as full paths.
inline QStringList files() {
    QStringList paths;
    for (const QString &kind : kinds())
        for (const QString &name : keptIn({folder(kind), kind})) paths.append(folder(kind) + QLatin1Char('/') + name);
    return paths;
}
}
