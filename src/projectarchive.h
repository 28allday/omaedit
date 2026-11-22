#pragma once
#include <QObject>
#include <QThread>
#include <QVariantMap>
#include <atomic>
#include <memory>

class ProjectArchive : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
  public:
    explicit ProjectArchive(QObject *parent = nullptr) : QObject(parent) {}
    ~ProjectArchive() override;
    bool busy() const { return m_thread != nullptr; }
    QVariantMap state() const { return m_state; }
    void start(const QVariantMap &document, const QString &folder, const QString &name, bool includeUnused,
               bool trim, int handles);
    Q_INVOKABLE void cancel();
  signals:
    void changed();
    void finished(bool ok, const QString &path, const QString &error);

  private:
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic_bool> m_stop;
    QVariantMap m_state;
};
