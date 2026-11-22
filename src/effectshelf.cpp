#include "effectshelf.h"
#include <QJsonDocument>
#include <QSettings>
#include <QVariantMap>

EffectShelf::EffectShelf(QObject *parent) : QObject(parent) {
    QSettings settings(QStringLiteral("omaedit"), QStringLiteral("omaedit"));
    m_favorites = settings.value(QStringLiteral("effectFavorites")).toStringList();
    m_recents = settings.value(QStringLiteral("recentEffects")).toStringList();
    m_bins = QJsonDocument::fromJson(settings.value(QStringLiteral("effectBins")).toString().toUtf8())
                 .toVariant().toList();
}

void EffectShelf::store() {
    QSettings settings(QStringLiteral("omaedit"), QStringLiteral("omaedit"));
    settings.setValue(QStringLiteral("effectFavorites"), m_favorites);
    settings.setValue(QStringLiteral("recentEffects"), m_recents);
    settings.setValue(QStringLiteral("effectBins"),
                      QString::fromUtf8(QJsonDocument::fromVariant(m_bins).toJson(QJsonDocument::Compact)));
    emit changed();
}

void EffectShelf::toggleFavorite(const QString &key) {
    if (key.isEmpty())
        return;
    if (m_favorites.removeAll(key) == 0) {
        m_favorites.append(key);
        emit status(tr("Added to favourite effects"));
    } else {
        emit status(tr("Removed from favourite effects"));
    }
    store();
}

void EffectShelf::noteUsed(const QString &key) {
    if (key.isEmpty())
        return;
    m_recents.removeAll(key);
    m_recents.prepend(key);
    while (m_recents.size() > 8)
        m_recents.removeLast();
    store();
}

void EffectShelf::clearRecents() {
    if (m_recents.isEmpty())
        return;
    m_recents.clear();
    store();
}

static int binIndex(const QVariantList &bins, const QString &name) {
    for (int i = 0; i < bins.size(); ++i)
        if (bins.at(i).toMap().value(QStringLiteral("name")).toString() == name)
            return i;
    return -1;
}

bool EffectShelf::addBin(const QString &name) {
    const QString title = name.trimmed();
    if (title.isEmpty() || binIndex(m_bins, title) >= 0)
        return false;
    m_bins.append(QVariantMap{{QStringLiteral("name"), title},
                              {QStringLiteral("keys"), QStringList()}});
    store();
    emit status(tr("Effect bin “%1” added").arg(title));
    return true;
}

bool EffectShelf::renameBin(const QString &name, const QString &to) {
    const QString title = to.trimmed();
    const int at = binIndex(m_bins, name);
    if (at < 0 || title.isEmpty() || (title != name && binIndex(m_bins, title) >= 0))
        return false;
    QVariantMap bin = m_bins.at(at).toMap();
    bin.insert(QStringLiteral("name"), title);
    m_bins[at] = bin;
    store();
    return true;
}

void EffectShelf::deleteBin(const QString &name) {
    const int at = binIndex(m_bins, name);
    if (at < 0)
        return;
    m_bins.removeAt(at);
    store();
    emit status(tr("Effect bin “%1” deleted").arg(name));
}

void EffectShelf::addToBin(const QString &binName, const QString &key) {
    const int at = binIndex(m_bins, binName);
    if (at < 0 || key.isEmpty())
        return;
    QVariantMap bin = m_bins.at(at).toMap();
    QStringList keys = bin.value(QStringLiteral("keys")).toStringList();
    if (keys.contains(key))
        return;
    keys.append(key);
    bin.insert(QStringLiteral("keys"), keys);
    m_bins[at] = bin;
    store();
    emit status(tr("Added to “%1”").arg(binName));
}

void EffectShelf::removeFromBin(const QString &binName, const QString &key) {
    const int at = binIndex(m_bins, binName);
    if (at < 0)
        return;
    QVariantMap bin = m_bins.at(at).toMap();
    QStringList keys = bin.value(QStringLiteral("keys")).toStringList();
    if (keys.removeAll(key) == 0)
        return;
    bin.insert(QStringLiteral("keys"), keys);
    m_bins[at] = bin;
    store();
}

QStringList EffectShelf::binKeys(const QString &binName) const {
    const int at = binIndex(m_bins, binName);
    return at < 0 ? QStringList() : m_bins.at(at).toMap().value(QStringLiteral("keys")).toStringList();
}

void EffectShelf::dropKey(const QString &key) {
    bool changed = m_favorites.removeAll(key) > 0;
    changed = m_recents.removeAll(key) > 0 || changed;
    for (int i = 0; i < m_bins.size(); ++i) {
        QVariantMap bin = m_bins.at(i).toMap();
        QStringList keys = bin.value(QStringLiteral("keys")).toStringList();
        if (keys.removeAll(key) == 0)
            continue;
        bin.insert(QStringLiteral("keys"), keys);
        m_bins[i] = bin;
        changed = true;
    }
    if (changed)
        store();
}
