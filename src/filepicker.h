#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QSortFilterProxyModel>
#include <QRegularExpression>

// Asynchronous app-owned chooser, presented with OmaEdit's shared QML controls.
class FileChooser : public QObject {
    Q_OBJECT
public:
    explicit FileChooser(QObject *parent = nullptr) : QObject(parent) {}
    Q_INVOKABLE void openFile(const QString &title, const QString &filterName = {}, const QStringList &patterns = {});
    Q_INVOKABLE void openFiles(const QString &title, const QString &filterName = {}, const QStringList &patterns = {});
    Q_INVOKABLE void openDirectory(const QString &title);
    Q_INVOKABLE void saveFile(const QString &title, const QString &suggestedName,
                              const QString &filterName = {}, const QStringList &patterns = {});
signals:
    void selected(const QUrl &url);
    void selectedMany(const QList<QUrl> &urls);
    void canceled();
    void failed(const QString &message);
private:
    void request(const QString &mode, const QString &title, const QString &filterName = {},
                 const QStringList &patterns = {}, const QString &suggestedName = {});
};

// One host per editor window, scoped to its backend and all that backend's
// children (media tasks and render presets). Request IDs discard late answers.
class FileChooserHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *owner READ owner WRITE setOwner)
public:
    explicit FileChooserHost(QObject *parent = nullptr);
    ~FileChooserHost() override;
    QObject *owner() const { return m_owner; }
    void setOwner(QObject *owner) { m_owner = owner; }
    static FileChooserHost *forChooser(FileChooser *chooser);
    void present(FileChooser *chooser, QVariantMap options);
    Q_INVOKABLE QVariantMap pathInfo(const QString &path, const QUrl &folder, const QString &suffix = {}) const;
    Q_INVOKABLE QVariantList locations() const;
    Q_INVOKABLE void accept(int requestId, const QList<QUrl> &urls);
    Q_INVOKABLE void cancel(int requestId);
signals:
    void requested(const QVariantMap &options);
    void dismissed();
private:
    QPointer<QObject> m_owner;
    QPointer<FileChooser> m_active;
    QMetaObject::Connection m_destroyed;
    int m_requestId = 0;
    QVariantMap m_options;
};

// Filters the asynchronous folder model in memory; typing never rescans disk.
class FileBrowserFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QStringList patterns READ patterns WRITE setPatterns NOTIFY patternsChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    explicit FileBrowserFilterModel(QObject *parent = nullptr);
    void setSourceModel(QAbstractItemModel *model) override;
    QString query() const { return m_query; }
    QStringList patterns() const { return m_patterns; }
    int count() const { return rowCount(); }
    void setQuery(const QString &query);
    void setPatterns(const QStringList &patterns);
    Q_INVOKABLE QVariant get(int row, const QString &role) const;
signals:
    void queryChanged();
    void patternsChanged();
    void countChanged();
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
private:
    QString m_query;
    QStringList m_patterns;
    QList<QRegularExpression> m_matchers;
    int m_nameRole = -1, m_directoryRole = -1;
};
