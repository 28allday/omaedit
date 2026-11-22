#pragma once
#include <QString>
#include <QVariantMap>
namespace RenderSnapshot {
QVariantMap nestedDocuments(const QVariantList &entries);
// Every file `value` names anywhere in it (a compound's clips too), each as
// it is now on disk: whether it is there, its size, its date and where it
// really is. For keys of pictures and peaks kept between runs. Asks the file
// system about each: not for a thread that serves the viewer.
QVariantMap filesNow(const QVariant &value);
QVariantList materialize(const QVariantList &entries,
                         const QVariantMap &documents, const QString &directory,
                         QString *error);
} // namespace RenderSnapshot
