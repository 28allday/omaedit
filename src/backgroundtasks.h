#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// Presentation model over the engines' jobs. IDs include the engine namespace;
// actions always return to that engine, never to a visual list index.
class BackgroundTasks : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
  Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)
  Q_PROPERTY(QVariantMap cache READ cache NOTIFY changed)
  Q_PROPERTY(QVariantList notices READ notices NOTIFY noticesChanged)
  Q_PROPERTY(QVariantMap currentNotice READ currentNotice NOTIFY noticesChanged)
public:
  explicit BackgroundTasks(QObject *parent = nullptr);
  QVariantList jobs() const;
  int activeCount() const;
  QVariantMap cache() const;
  QVariantList notices() const { return m_notices; }
  QVariantMap currentNotice() const { return m_currentNotice; }
  QVariantMap job(const QString &id) const;
  static bool active(const QString &state);
  void update(const QVariantMap &row);
  void sync(const QString &source, const QVariantList &rows);
  Q_INVOKABLE void act(const QString &id, const QString &action);
  Q_INVOKABLE void clearFinished();
  Q_INVOKABLE void clearNotices();
  Q_INVOKABLE void dismissNotice();
  void post(const QString &severity, const QString &title,
            const QString &message = {}, const QString &taskId = {});
  static bool lowSpace(qint64 available, qint64 total);
signals:
  void changed();
  void noticesChanged();
  void actionRequested(const QString &id, const QString &action);

private:
  QVariantMap presented(const QVariantMap &row) const;
  void trim();
  QElapsedTimer m_clock;
  QTimer m_tick;
  QVariantList m_jobs, m_notices;
  // Where each job sits in m_jobs, and how many of them have finished: a task
  // reports many times, and looking through the whole list for it each time
  // (and again to see whether old ones should go) took the interface's thread
  // longer the more clips a timeline had.
  int indexOf(const QString &id) const;
  mutable QHash<QString, int> m_at;
  int m_finished = 0;
  QVariantMap m_currentNotice;
  QHash<QString, QVariantMap> m_sourceRows;
  QHash<QString, int> m_cacheProgress;
  QSet<QString> m_cacheActive, m_cacheWaiting;
  quint64 m_noticeId = 0;
};
