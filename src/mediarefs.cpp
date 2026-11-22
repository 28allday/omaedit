#include "mediarefs.h"
#include <QJsonDocument>
#include <QStringList>
namespace {
bool sourceKey(const QString &key) {
    return QStringList{"path", "resource", "proxyPath", "originalPath", "source"}.contains(key);
}
bool jsonKey(const QString &key) { return key == QStringLiteral("compoundJson"); }
}
QVariant MediaRefs::rewrite(const QVariant &value, const QVariantMap &paths) {
    if (value.metaType().id() == QMetaType::QVariantList) {
        QVariantList out; for (const QVariant &v : value.toList()) out.append(rewrite(v, paths)); return out;
    }
    if (value.metaType().id() != QMetaType::QVariantMap) return value;
    QVariantMap out = value.toMap();
    for (auto it = out.begin(); it != out.end(); ++it) {
        if (sourceKey(it.key()) && it.value().metaType().id()==QMetaType::QString && paths.contains(it.value().toString())) it.value() = paths.value(it.value().toString());
        else if (it.key() == QStringLiteral("omavid:sourceState")) {
            auto state = rewrite(it.value(), paths).toMap();
            const auto sources = state.value("sources").toMap();
            QVariantMap relocated;
            for (auto source=sources.begin();source!=sources.end();++source)
                relocated[paths.value(source.key(),source.key()).toString()]=source.value();
            state["sources"]=relocated;it.value()=state;
        } else if (jsonKey(it.key())) {
            const QJsonDocument json = QJsonDocument::fromJson(it.value().toString().toUtf8());
            if (!json.isNull()) it.value() = QString::fromUtf8(QJsonDocument::fromVariant(rewrite(json.toVariant(), paths)).toJson(QJsonDocument::Compact));
        } else it.value() = rewrite(it.value(), paths);
    }
    return out;
}
QSet<QString> MediaRefs::paths(const QVariant &value) {
    QSet<QString> out;
    if (value.metaType().id() == QMetaType::QVariantList) {
        for (const QVariant &v : value.toList()) out.unite(paths(v));
    } else if (value.metaType().id() == QMetaType::QVariantMap) {
        const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (sourceKey(it.key()) && it.value().metaType().id()==QMetaType::QString && !it.value().toString().isEmpty()) out.insert(it.value().toString());
            else if (jsonKey(it.key())) out.unite(paths(QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant()));
            else out.unite(paths(it.value()));
        }
    }
    return out;
}

QVariant MediaRefs::withSources(const QVariant &value, const QVariantMap &sources) {
    if (value.metaType().id() == QMetaType::QVariantList) {
        QVariantList out; for (const QVariant &v : value.toList()) out.append(withSources(v, sources)); return out;
    }
    if (value.metaType().id() != QMetaType::QVariantMap) return value;
    QVariantMap out = value.toMap();
    for (auto it = out.begin(); it != out.end(); ++it) {
        if (it.key() == QStringLiteral("sourceMeta")) continue;
        // A reversible source-attribute operation owns the old clocks and
        // sequence arrangements. Proxy/availability refreshes must not rewrite it.
        if (it.key() == QStringLiteral("omavid:sourceState")) continue;
        if (jsonKey(it.key())) {
            const QJsonDocument json = QJsonDocument::fromJson(it.value().toString().toUtf8());
            if (!json.isNull()) it.value() = QString::fromUtf8(QJsonDocument::fromVariant(withSources(json.toVariant(), sources)).toJson(QJsonDocument::Compact));
        } else it.value() = withSources(it.value(), sources);
    }
    const QString path = out.value(QStringLiteral("path")).toString();
    if (sources.contains(path)) out.insert(QStringLiteral("sourceMeta"), sources.value(path));
    return out;
}
