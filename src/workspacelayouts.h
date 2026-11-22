#pragma once
#include <QObject>
#include <QVariantMap>
#include <QWindow>

class WorkspaceLayouts : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
  public:
    explicit WorkspaceLayouts(QObject *parent = nullptr);
    int revision() const { return m_revision; }
    QVariantList screens() const;
    Q_INVOKABLE QVariantMap read(const QString &workspace) const;
    Q_INVOKABLE void write(const QString &workspace, const QVariantMap &state);
    Q_INVOKABLE QStringList presetNames(const QString &workspace) const;
    Q_INVOKABLE bool savePreset(const QString &workspace, const QString &name, const QVariantMap &state,
                                bool replace = false);
    Q_INVOKABLE QVariantMap preset(const QString &workspace, const QString &name) const;
    Q_INVOKABLE void removePreset(const QString &workspace, const QString &name);
    Q_INVOKABLE QVariantMap windowGeometry(QWindow *window) const;
    Q_INVOKABLE void placeWindow(QWindow *window, const QVariantMap &geometry,
                                 const QString &screenName = QString());
    static QRect fitGeometry(const QRect &requested, const QRect &available);
  signals:
    void changed();
    void screensChanged();

  private:
    bool validWorkspace(const QString &workspace) const;
    int m_revision = 0;
};
