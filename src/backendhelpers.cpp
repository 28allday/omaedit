// Helpers the Backend sources share: reading and writing what a cut carries.
#include "backend_p.h"

// A timeline used as a clip wears that timeline's name, not its cache file's,
// unless the clip was given one of its own.
bool nameNestedClip(QVariant &clip, const QVariantList &sequences) {
    QVariantMap entry = clip.toMap();
    const QString id = entry.value(ClipKey::nestId).toString();
    if (id.isEmpty()
        || (!entry.value(QStringLiteral("nestNamed")).toBool()
            && entry.value(ClipKey::name).toString() != QFileInfo(entry.value(ClipKey::path).toString()).fileName()))
        return false;
    for (const QVariant &v : sequences) {
        const QVariantMap sequence = v.toMap();
        if (sequence.value(QStringLiteral("id")).toString() != id)
            continue;
        const QString name = sequence.value(QStringLiteral("name")).toString();
        if (entry.value(ClipKey::name).toString() == name)
            return false;
        entry.insert(ClipKey::name, name);
        entry.insert(QStringLiteral("nestNamed"), true);
        clip = entry;
        return true;
    }
    return false;
}

// Values arriving from QML may still be QJSValues (a JS array of objects);
// converting those touches the QML engine, which only this thread may do —
// so flatten to plain QVariantList/Map BEFORE anything crosses to the
// worker (a QVariant::toList() there was a crash).
QVariant plainVariant(const QVariant &v) {
    if (v.userType() == qMetaTypeId<QJSValue>())
        return plainVariant(v.value<QJSValue>().toVariant());
    if (v.typeId() == QMetaType::QVariantMap) {
        QVariantMap out;
        const QVariantMap in = v.toMap();
        for (auto it = in.cbegin(); it != in.cend(); ++it)
            out.insert(it.key(), plainVariant(it.value()));
        return out;
    }
    if (v.typeId() == QMetaType::QVariantList) {
        QVariantList out;
        for (const QVariant &e : v.toList())
            out.append(plainVariant(e));
        return out;
    }
    return v;
}
