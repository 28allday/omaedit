#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

// Project collections and saved search rules. Clip membership stays in the
// library; locations are user preferences and travel between projects.
class MediaCatalog : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList bins READ bins NOTIFY changed)
    Q_PROPERTY(QVariantList locations READ locations NOTIFY locationsChanged)
    Q_PROPERTY(QVariantList watches READ watches NOTIFY changed)
    Q_PROPERTY(QVariantList schema READ schema NOTIFY changed)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
public:
    explicit MediaCatalog(QObject *parent = nullptr);
    QVariantList bins() const { return m_bins; }
    QVariantList locations() const { return m_locations; }
    QVariantList watches() const { return m_watches; }
    QVariantList schema() const { return m_schema; }
    QVariantList providers() const { return m_providers; }
    QVariantList categories() const; // the built-in ones, then the project's own
    Q_INVOKABLE QString addCategory(const QString &name, const QString &kind, const QString &color);
    Q_INVOKABLE bool editCategory(const QString &id, const QString &name, const QString &color);
    Q_INVOKABLE bool removeCategory(const QString &id); // built-in ones stay
    Q_INVOKABLE QString registerProvider(const QVariantMap &descriptor);
    Q_INVOKABLE bool removeProvider(const QString &id);
    Q_INVOKABLE bool setSchema(const QVariantList &fields);
    Q_INVOKABLE QString watch(const QString &path, const QString &binId = QString());
    Q_INVOKABLE bool enableWatch(const QString &id, bool enabled);
    Q_INVOKABLE bool removeWatch(const QString &id);
    QVariantMap state() const;
    void restore(const QVariantMap &state);
    Q_INVOKABLE QString create(const QString &name, const QString &parent = QString(),
                               const QVariantMap &rules = QVariantMap(), bool smart = false);
    Q_INVOKABLE bool edit(const QString &id, const QString &name, const QString &parent,
                         const QVariantMap &rules = QVariantMap());
    Q_INVOKABLE bool remove(const QString &id);
    Q_INVOKABLE QVariantMap bin(const QString &id) const;
    Q_INVOKABLE QStringList descendants(const QString &id) const;
    Q_INVOKABLE QVariantList tree() const;
    Q_INVOKABLE bool matches(const QVariantMap &entry, const QVariantMap &rules) const;
    Q_INVOKABLE bool addLocation(const QString &path, const QString &name = QString());
    Q_INVOKABLE bool editLocation(int index, const QString &name);
    Q_INVOKABLE bool removeLocation(int index);
    Q_INVOKABLE bool moveLocation(int index, int delta);
signals:
    void changed();
    void locationsChanged();
    void providersChanged();
    void removed(const QStringList &ids);
    void error(const QString &message);
private:
    bool validName(const QString &name, const QString &parent, const QString &except);
    void saveLocations();
    QVariantList m_bins;
    QVariantList m_locations;
    QVariantList m_watches;
    QVariantList m_schema;
    QVariantList m_providers;
    QVariantList m_categoryEdits; // changed built-in rows and the project's own
};
