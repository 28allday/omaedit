#pragma once
#include <QJsonDocument>
#include <QStringList>
#include <QVariantMap>

namespace MarkerEdit {
// Only persisted clip state identifies an edit target. Thumbnail and waveform
// delivery can change independently and is not a reason to reject an edit.
inline QString clipState(QVariantMap entry) {
    entry.remove("strip");
    entry.remove("wave");
    return QString::fromUtf8(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact));
}
// The marker types the editor offers.
inline const QStringList &types() {
    static const QStringList known{QStringLiteral("standard"), QStringLiteral("chapter"),
                                   QStringLiteral("todo"), QStringLiteral("done")};
    return known;
}
// The colours a marker can be given instead of its type's own ("" keeps
// the type's colour).
inline const QStringList &colours() {
    static const QStringList known{QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"),
                                   QStringLiteral("green"), QStringLiteral("cyan"), QStringLiteral("blue"),
                                   QStringLiteral("purple"), QStringLiteral("pink"), QStringLiteral("white")};
    return known;
}
// Older builds wrote other types (the command line's "note"); such a marker
// opens as a standard one rather than as a type no dialog can edit.
inline QVariantList knownTypes(QVariantList markers) {
    for (QVariant &v : markers) {
        QVariantMap m = v.toMap();
        if (!types().contains(m.value(QStringLiteral("type")).toString())) {
            m.insert(QStringLiteral("type"), QStringLiteral("standard"));
            v = m;
        }
    }
    return markers;
}
}
