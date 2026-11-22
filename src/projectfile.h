#pragma once
#include <QString>
#include <QStringList>

// Projects, nested sequences and the editor's own copies of them are saved as
// .omaedit (ProjectSafety::writeDocument). Earlier builds named the same
// document .mlt; those keep opening, and keep saving under their own name, so
// nothing already on disk is orphaned.
namespace ProjectFile {
inline QString extension() { return QStringLiteral(".omaedit"); }
inline QStringList openPatterns() { return {QStringLiteral("*.omaedit"), QStringLiteral("*.mlt")}; }
inline QStringList savePatterns() { return {QStringLiteral("*.omaedit")}; }
inline bool isProject(const QString &path) {
    return path.endsWith(extension(), Qt::CaseInsensitive)
           || path.endsWith(QStringLiteral(".mlt"), Qt::CaseInsensitive);
}
inline QString withExtension(QString path) {
    if (!isProject(path))
        path += extension();
    return path;
}
}
