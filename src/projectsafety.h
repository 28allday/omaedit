#pragma once
#include <QSet>
#include <QString>
#include <QVariantMap>

namespace ProjectSafety {
QString root();
// The editor's project document, JSON (earlier builds' XML-wrapped copies read too).
QVariantMap readDocument(const QString &path, QString *error = nullptr);
// A Render and Replace file or a stabilisation analysis the document names
// comes back as where that file is now (KeptMedia::resolve): the kept-media
// folder has moved since older projects were saved.
// `withKeptMedia` does the same for any saved snapshot of a timeline.
QVariantMap withKeptMedia(const QVariantMap &document);
// Whether the file holds one, from its first few kilobytes.
bool isDocument(const QString &path);
// Nothing larger than documentLimit() is written, so nothing the editor
// saved is ever too large for it to open again.
bool writeDocument(const QString &path, const QVariantMap &document, QString *error = nullptr);
qint64 documentLimit();
QVariantMap freezeNested(QVariantMap document);
QVariantMap resolveDocument(QVariantMap document, const QString &directory, QString *error);
QVariantMap rewriteDocument(const QVariantMap &document, const QVariantMap &paths);
QVariantMap usedDocument(QVariantMap document, bool includeUnused);
QSet<QString> assets(const QVariantMap &document);
QVariantList clipEntries(const QVariant &value);
// The library line format is tab-separated and newline-delimited; a path
// holding either would split the record. Tagged percent encoding preserves
// literal percent sequences; the reader also supports legacy %09/%0A/%0D.
QString encodeLibraryPath(const QString &path);
QString decodeLibraryPath(const QString &field);
QVariantList libraryRecords(const QStringList &library);
QStringList encodeLibrary(const QVariantList &records);
// The saved library lines decoded for the editor, in file order: {kind:
// "clip", path, rangeIn, rangeOut, logging, exists}, {kind: "markers",
// markers} (file order), {kind: "sequences", list, active} and {kind:
// "media", document}. Percent decoding, JSON parsing and the file checks
// belong on the project worker.
QVariantList decodeLibraryState(const QStringList &library);
// Raw GUI snapshots: perform JSON and percent encoding on the project worker.
QVariantMap libraryLogging(const QVariantMap &entry);
QStringList encodeLibraryState(const QVariantMap &state);
// The library's "::markers=" line for a marker list; empty for no markers.
QString encodeMarkers(const QVariantList &markers);
bool isFileReference(const QString &path);
QString backupDirectory(const QString &project);
QString makeBackup(const QString &project, QString *error);
void pruneBackups(const QString &project, int limit);
QVariantList backups(const QString &project);
} // namespace ProjectSafety
