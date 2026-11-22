#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace RelinkMatcher {
// Pure matching rules: filesystem enumeration, probing and hashing belong to
// the media-task worker. Every enabled criterion must agree.
QVariantMap normalized(const QVariantMap &options);
QString missingFacts(const QVariantMap &source, const QVariantMap &options);
bool fileMatches(const QVariantMap &source, const QString &candidate, qint64 size,
                 const QVariantMap &options);
bool metadataMatches(const QVariantMap &source, const QVariantMap &candidate,
                     const QVariantMap &options);
QStringList evidence(const QVariantMap &source, const QVariantMap &options);
}
