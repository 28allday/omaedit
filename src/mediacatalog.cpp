#include "mediacatalog.h"
#include "clipcategories.h"
#include <QUuid>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>
#include <functional>

MediaCatalog::MediaCatalog(QObject *parent) : QObject(parent) {
    m_providers = QJsonDocument::fromJson(QSettings().value(QStringLiteral("media/providers")).toByteArray()).toVariant().toList();
    m_locations = QJsonDocument::fromJson(QSettings().value(QStringLiteral("media/locations")).toByteArray()).toVariant().toList();
}
QVariantMap MediaCatalog::state() const { return {{QStringLiteral("bins"), m_bins}, {QStringLiteral("watches"), m_watches}, {QStringLiteral("schema"), m_schema}, {QStringLiteral("categories"), m_categoryEdits}}; }
QVariantList MediaCatalog::categories() const {
    QVariantList rows = ClipCategories::builtIn();
    for (const QVariant &v : m_categoryEdits) {
        const QVariantMap edit = v.toMap();
        bool replaced = false;
        for (QVariant &row : rows)
            if (row.toMap().value("id") == edit.value("id")) {
                QVariantMap merged = row.toMap();
                merged["name"] = edit.value("name");
                merged["color"] = edit.value("color");
                row = merged;
                replaced = true;
            }
        if (!replaced)
            rows.append(edit);
    }
    return rows;
}
QString MediaCatalog::addCategory(const QString &name, const QString &kind, const QString &color) {
    const QString title = name.trimmed();
    if (title.isEmpty() || (kind != "audio" && kind != "video"))
        return {};
    for (const QVariant &v : categories())
        if (v.toMap().value("name").toString().compare(title, Qt::CaseInsensitive) == 0
            && v.toMap().value("kind") == kind) {
            emit error(tr("There is already a category called “%1”.").arg(title));
            return {};
        }
    const QString id = QStringLiteral("c") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    m_categoryEdits.append(QVariantMap{{"id", id}, {"name", title}, {"kind", kind}, {"color", color}});
    emit changed();
    return id;
}
bool MediaCatalog::editCategory(const QString &id, const QString &name, const QString &color) {
    const QString title = name.trimmed();
    if (title.isEmpty())
        return false;
    QVariantMap row;
    for (const QVariant &v : categories())
        if (v.toMap().value("id") == id)
            row = v.toMap();
    if (row.isEmpty())
        return false;
    for (const QVariant &v : categories())
        if (v.toMap().value("id") != id && v.toMap().value("kind") == row.value("kind")
            && v.toMap().value("name").toString().compare(title, Qt::CaseInsensitive) == 0) {
            emit error(tr("There is already a category called “%1”.").arg(title));
            return false;
        }
    row["name"] = title;
    row["color"] = color;
    row.remove("builtIn");
    for (QVariant &v : m_categoryEdits)
        if (v.toMap().value("id") == id) {
            v = row;
            emit changed();
            return true;
        }
    m_categoryEdits.append(row);
    emit changed();
    return true;
}
bool MediaCatalog::removeCategory(const QString &id) {
    for (const QVariant &v : ClipCategories::builtIn())
        if (v.toMap().value("id") == id)
            return false;
    for (int i = 0; i < m_categoryEdits.size(); ++i)
        if (m_categoryEdits.at(i).toMap().value("id") == id) {
            m_categoryEdits.removeAt(i);
            emit changed();
            return true;
        }
    return false;
}
void MediaCatalog::restore(const QVariantMap &state) {
    m_bins.clear();
    m_categoryEdits = state.value(QStringLiteral("categories")).toList();
    m_watches = state.value(QStringLiteral("watches")).toList();
    m_schema.clear();
    setSchema(state.value(QStringLiteral("schema")).toList());
    QSet<QString> ids;
    for (const QVariant &v : state.value(QStringLiteral("bins")).toList()) {
        QVariantMap item = v.toMap();
        const QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || ids.contains(id) || item.value(QStringLiteral("name")).toString().trimmed().isEmpty()) continue;
        ids.insert(id);
        m_bins.append(item);
    }
    // Repair missing parents and cycles from hand-edited/older projects.
    for (int i = 0; i < m_bins.size(); ++i) {
        QVariantMap item = m_bins.at(i).toMap();
        QSet<QString> seen{item.value(QStringLiteral("id")).toString()};
        QString p = item.value(QStringLiteral("parent")).toString();
        while (!p.isEmpty()) {
            if (seen.contains(p) || !ids.contains(p) || bin(p).value(QStringLiteral("smart")).toBool()) {
                item.insert(QStringLiteral("parent"), QString());
                break;
            }
            seen.insert(p);
            p = bin(p).value(QStringLiteral("parent")).toString();
        }
        m_bins[i] = item;
    }
    emit changed();
}
QVariantMap MediaCatalog::bin(const QString &id) const {
    for (const QVariant &v : m_bins) if (v.toMap().value(QStringLiteral("id")).toString() == id) return v.toMap();
    return {};
}
bool MediaCatalog::validName(const QString &name, const QString &parent, const QString &except) {
    if (name.trimmed().isEmpty()) { emit error(tr("Enter a bin name.")); return false; }
    if (!parent.isEmpty() && (bin(parent).isEmpty() || bin(parent).value(QStringLiteral("smart")).toBool())) {
        emit error(tr("Choose an existing regular bin as the parent.")); return false;
    }
    for (const QVariant &v : m_bins) {
        const QVariantMap b = v.toMap();
        if (b.value(QStringLiteral("id")).toString() != except && b.value(QStringLiteral("parent")).toString() == parent
            && b.value(QStringLiteral("name")).toString().compare(name.trimmed(), Qt::CaseInsensitive) == 0) {
            emit error(tr("A bin with that name already exists here.")); return false;
        }
    }
    return true;
}
QString MediaCatalog::create(const QString &name, const QString &parent, const QVariantMap &rules, bool smart) {
    if (!validName(name, parent, {})) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_bins.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name.trimmed()},
        {QStringLiteral("parent"), parent}, {QStringLiteral("smart"), smart}, {QStringLiteral("rules"), rules}});
    emit changed();
    return id;
}
QStringList MediaCatalog::descendants(const QString &id) const {
    QStringList out;
    if (bin(id).isEmpty()) return out;
    out.append(id);
    for (int i = 0; i < out.size(); ++i)
        for (const QVariant &v : m_bins) {
            const QVariantMap b = v.toMap();
            const QString child = b.value(QStringLiteral("id")).toString();
            if (b.value(QStringLiteral("parent")).toString() == out[i] && !out.contains(child)) out.append(child);
        }
    return out;
}
bool MediaCatalog::edit(const QString &id, const QString &name, const QString &parent, const QVariantMap &rules) {
    if (bin(id).isEmpty()) return false;
    if (descendants(id).contains(parent)) { emit error(tr("A bin cannot be moved into itself or its children.")); return false; }
    if (!validName(name, parent, id)) return false;
    for (int i = 0; i < m_bins.size(); ++i) {
        QVariantMap b = m_bins[i].toMap();
        if (b.value(QStringLiteral("id")).toString() != id) continue;
        b.insert(QStringLiteral("name"), name.trimmed()); b.insert(QStringLiteral("parent"), parent);
        if (b.value(QStringLiteral("smart")).toBool()) b.insert(QStringLiteral("rules"), rules);
        if (b == m_bins[i].toMap()) return true;
        m_bins[i] = b; emit changed(); return true;
    }
    return false;
}
bool MediaCatalog::remove(const QString &id) {
    const QStringList ids = descendants(id);
    if (ids.isEmpty()) return false;
    for (int i = m_bins.size() - 1; i >= 0; --i)
        if (ids.contains(m_bins.at(i).toMap().value(QStringLiteral("id")).toString())) m_bins.removeAt(i);
    for (int i = 0; i < m_watches.size(); ++i) {
        QVariantMap watch = m_watches.at(i).toMap();
        if (ids.contains(watch.value(QStringLiteral("binId")).toString())) { watch.insert(QStringLiteral("binId"), QString()); m_watches[i] = watch; }
    }
    emit removed(ids); emit changed(); return true;
}
QVariantList MediaCatalog::tree() const {
    QVariantList result;
    std::function<void(const QString &, int)> visit = [&](const QString &parent, int depth) {
        for (const QVariant &v : m_bins) {
            QVariantMap b = v.toMap();
            if (b.value(QStringLiteral("parent")).toString() != parent) continue;
            b.insert(QStringLiteral("depth"), depth); result.append(b);
            visit(b.value(QStringLiteral("id")).toString(), depth + 1);
        }
    };
    visit({}, 0);
    return result;
}
bool MediaCatalog::matches(const QVariantMap &e, const QVariantMap &r) const {
    const QString query = r.value(QStringLiteral("text")).toString().trimmed();
    if (!query.isEmpty()) {
        QStringList text;
        for (const char *k : {"name", "notes", "comments", "reel", "scene", "shot", "take", "angle"}) text.append(e.value(QLatin1String(k)).toString());
        const QVariantMap custom = e.value(QStringLiteral("customMetadata")).toMap();
        for (const QVariant &value : custom) text.append(value.toString());
        text += e.value(QStringLiteral("keywords")).toStringList(); text += e.value(QStringLiteral("tags")).toStringList();
        for (const QVariant &line : e.value(QStringLiteral("transcript")).toList()) text.append(line.toMap().value(QStringLiteral("text")).toString());
        if (!text.join(' ').contains(query, Qt::CaseInsensitive)) return false;
    }
    const QString kind = r.value(QStringLiteral("kind")).toString();
    const bool video = e.value(QStringLiteral("hasVideo"), true).toBool(), audio = e.value(QStringLiteral("hasAudio"), true).toBool();
    const bool still = video && e.value(QStringLiteral("duration")).toInt() <= 1;
    if (kind == QStringLiteral("video") && (!video || still)) return false;
    if (kind == QStringLiteral("audio") && (video || !audio)) return false;
    if (kind == QStringLiteral("image") && !still) return false;
    if (e.value(QStringLiteral("rating")).toInt() < r.value(QStringLiteral("ratingMin")).toInt()) return false;
    if (r.contains(QStringLiteral("ratingMax")) && e.value(QStringLiteral("rating")).toInt() > r.value(QStringLiteral("ratingMax")).toInt()) return false;
    for (const char *key : {"favorite", "rejected"}) if (r.contains(QLatin1String(key)) && e.value(QLatin1String(key)).toBool() != r.value(QLatin1String(key)).toBool()) return false;
    const QString extension = r.value(QStringLiteral("extension")).toString();
    if (!extension.isEmpty() && QFileInfo(e.value(QStringLiteral("path")).toString()).suffix().compare(extension, Qt::CaseInsensitive)) return false;
    const QString tag = r.value(QStringLiteral("tag")).toString();
    if (!tag.isEmpty() && !e.value(QStringLiteral("tags")).toStringList().contains(tag, Qt::CaseInsensitive)) return false;
    for (const char *key : {"width", "fps"}) {
        const double value = e.value(QLatin1String(key)).toDouble();
        const double min = r.value(QString::fromLatin1(key) + QStringLiteral("Min")).toDouble();
        const double max = r.value(QString::fromLatin1(key) + QStringLiteral("Max")).toDouble();
        if ((min > 0 && value + 0.001 < min) || (max > 0 && value - 0.001 > max)) return false;
    }
    for (const char *key : {"proxy", "offline"}) {
        const QString mode = r.value(QLatin1String(key)).toString();
        const bool on = e.value(QLatin1String(key)).toBool();
        if ((mode == QStringLiteral("yes") && !on) || (mode == QStringLiteral("no") && on)) return false;
    }
    const QDate date = QDateTime::fromString(e.value(QStringLiteral("created")).toString(), Qt::ISODate).date();
    const QDate from = QDate::fromString(r.value(QStringLiteral("dateFrom")).toString(), Qt::ISODate);
    const QDate to = QDate::fromString(r.value(QStringLiteral("dateTo")).toString(), Qt::ISODate);
    if ((!r.value(QStringLiteral("dateFrom")).toString().isEmpty() && !from.isValid()) || (!r.value(QStringLiteral("dateTo")).toString().isEmpty() && !to.isValid())) return false;
    if (from.isValid() && (!date.isValid() || date < from)) return false;
    if (to.isValid() && (!date.isValid() || date > to)) return false;
    return true;
}
void MediaCatalog::saveLocations() {
    QSettings().setValue(QStringLiteral("media/locations"), QJsonDocument::fromVariant(m_locations).toJson(QJsonDocument::Compact));
    emit locationsChanged();
}
bool MediaCatalog::addLocation(const QString &path, const QString &name) {
    const QFileInfo info(path);
    if (!info.isDir()) { emit error(tr("Choose an accessible folder.")); return false; }
    const QString canonical = info.canonicalFilePath();
    for (const QVariant &v : m_locations) if (v.toMap().value(QStringLiteral("path")).toString() == canonical) return false;
    m_locations.append(QVariantMap{{QStringLiteral("path"), canonical}, {QStringLiteral("name"),
        name.trimmed().isEmpty() ? (info.fileName().isEmpty() ? canonical : info.fileName()) : name.trimmed()}});
    saveLocations(); return true;
}
bool MediaCatalog::editLocation(int index, const QString &name) {
    if (index < 0 || index >= m_locations.size() || name.trimmed().isEmpty()) return false;
    QVariantMap location = m_locations[index].toMap(); location.insert(QStringLiteral("name"), name.trimmed());
    m_locations[index] = location; saveLocations(); return true;
}
bool MediaCatalog::removeLocation(int index) {
    if (index < 0 || index >= m_locations.size()) return false;
    m_locations.removeAt(index); saveLocations(); return true;
}
bool MediaCatalog::moveLocation(int index, int delta) {
    const int target = index + delta;
    if (index < 0 || index >= m_locations.size() || target < 0 || target >= m_locations.size()) return false;
    m_locations.swapItemsAt(index, target); saveLocations(); return true;
}

QString MediaCatalog::watch(const QString &path, const QString &binId) {
    const QFileInfo folder(path);
    if (!folder.isDir()) { emit error(tr("Choose an accessible watch folder.")); return {}; }
    if (!binId.isEmpty() && (bin(binId).isEmpty() || bin(binId).value(QStringLiteral("smart")).toBool())) return {};
    const QString canonical = folder.canonicalFilePath();
    for (const QVariant &v : m_watches) if (v.toMap().value(QStringLiteral("path")).toString() == canonical) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_watches.append(QVariantMap{{"id", id}, {"path", canonical}, {"name", folder.fileName()}, {"binId", binId}, {"enabled", true}});
    emit changed(); return id;
}
bool MediaCatalog::enableWatch(const QString &id, bool enabled) {
    for (int i = 0; i < m_watches.size(); ++i) {
        QVariantMap watch = m_watches.at(i).toMap(); if (watch.value(QStringLiteral("id")).toString() != id) continue;
        watch.insert(QStringLiteral("enabled"), enabled); m_watches[i] = watch; emit changed(); return true;
    }
    return false;
}
bool MediaCatalog::removeWatch(const QString &id) {
    for (int i = 0; i < m_watches.size(); ++i) if (m_watches.at(i).toMap().value(QStringLiteral("id")).toString() == id) { m_watches.removeAt(i); emit changed(); return true; }
    return false;
}

bool MediaCatalog::setSchema(const QVariantList &fields) {
    if (fields.size() > 50) { emit error(tr("A metadata schema can contain up to 50 custom fields.")); return false; }
    static const QRegularExpression keyPattern(QStringLiteral("^[A-Za-z][A-Za-z0-9_]{0,31}$"));
    QSet<QString> seen; QVariantList clean;
    for (const QVariant &v : fields) {
        const QVariantMap field = v.toMap(); const QString key = field.value(QStringLiteral("key")).toString().trimmed();
        const QString type = field.value(QStringLiteral("type"), QStringLiteral("text")).toString();
        if (!keyPattern.match(key).hasMatch() || seen.contains(key) || !QStringList{"text", "number", "boolean"}.contains(type)) {
            emit error(tr("Use unique field keys (letters, digits, underscore), and text, number or boolean types.")); return false;
        }
        seen.insert(key); const QString label = field.value(QStringLiteral("label")).toString().trimmed();
        clean.append(QVariantMap{{"key", key}, {"label", label.isEmpty() ? key : label}, {"type", type}});
    }
    if (clean != m_schema) { m_schema = clean; emit changed(); }
    return true;
}

QString MediaCatalog::registerProvider(const QVariantMap &descriptor) {
    const QString adapter = descriptor.value(QStringLiteral("adapter"), QStringLiteral("mounted")).toString();
    const QFileInfo root(descriptor.value(QStringLiteral("root")).toString());
    const QString name = descriptor.value(QStringLiteral("name")).toString().trimmed();
    if (descriptor.value(QStringLiteral("version"), 1).toInt() != 1 || adapter != QStringLiteral("mounted") || name.isEmpty() || !root.isAbsolute() || !root.isDir()) {
        emit error(tr("Provider manifests need a name, adapter 'mounted', and an accessible absolute root folder.")); return {};
    }
    QString id = descriptor.value(QStringLiteral("id")).toString(); if (id.isEmpty()) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QVariantMap provider{{"id", id}, {"name", name}, {"adapter", adapter}, {"root", root.canonicalFilePath()}, {"kind", descriptor.value(QStringLiteral("kind"), QStringLiteral("cloud"))}, {"version", 1}};
    bool replaced = false;
    for (int i = 0; i < m_providers.size(); ++i) if (m_providers.at(i).toMap().value(QStringLiteral("id")).toString() == id) { m_providers[i] = provider; replaced = true; break; }
    if (!replaced) m_providers.append(provider);
    QSettings().setValue(QStringLiteral("media/providers"), QJsonDocument::fromVariant(m_providers).toJson(QJsonDocument::Compact)); emit providersChanged(); return id;
}
bool MediaCatalog::removeProvider(const QString &id) {
    for (int i = 0; i < m_providers.size(); ++i) if (m_providers.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
        m_providers.removeAt(i); QSettings().setValue(QStringLiteral("media/providers"), QJsonDocument::fromVariant(m_providers).toJson(QJsonDocument::Compact)); emit providersChanged(); return true;
    }
    return false;
}
