#pragma once
#include <QVariantMap>
#include <QSet>
#include <QString>
namespace MediaRefs {
// Rewrite source identities in nested snapshots, preserving text/logging fields.
QVariant rewrite(const QVariant &value, const QVariantMap &paths);
QSet<QString> paths(const QVariant &value);
QVariant withSources(const QVariant &value, const QVariantMap &sources);
}
