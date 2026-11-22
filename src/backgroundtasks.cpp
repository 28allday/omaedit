#include "backgroundtasks.h"
#include <QDateTime>
#include <QSet>
#include <algorithm>

BackgroundTasks::BackgroundTasks(QObject *parent) : QObject(parent) {
  m_clock.start();
  connect(&m_tick, &QTimer::timeout, this, [this] {
    if (activeCount())
      emit changed();
  });
  m_tick.start(1000);
}
bool BackgroundTasks::active(const QString &s) {
  return s == "queued" || s == "running" || s == "rendering" || s == "paused" ||
         s == "applying" || s == "waiting";
}
bool BackgroundTasks::lowSpace(qint64 available, qint64 total) {
  // A percentage alone warns forever on large, healthy media volumes.
  return available >= 0 && total > 0 &&
         (available < 1024LL * 1024 * 1024 ||
          (available < 10LL * 1024 * 1024 * 1024 &&
           double(available) / total < .05));
}
int BackgroundTasks::indexOf(const QString &id) const {
  if (m_at.size() != m_jobs.size()) { // jobs were removed: count them again
    m_at.clear();
    for (int i = 0; i < m_jobs.size(); ++i)
      m_at.insert(m_jobs[i].toMap().value("id").toString(), i);
  }
  return m_at.value(id, -1);
}
QVariantMap BackgroundTasks::job(const QString &id) const {
  const int index = indexOf(id);
  return index < 0 ? QVariantMap() : presented(m_jobs[index].toMap());
}
QVariantMap BackgroundTasks::presented(const QVariantMap &row) const {
  auto r = row;
  const QString state = r.value("state").toString();
  const qint64 elapsedMs =
      active(state) ? m_clock.elapsed() - r.value("started").toLongLong()
                    : r.value("elapsedMs").toLongLong();
  const double seconds = qMax(0., elapsedMs / 1000.);
  if (!r.contains("elapsed"))
    r["elapsed"] = int(seconds);
  const double percent = r.value("percent", -1).toDouble();
  const bool working =
      state == "running" || state == "rendering" || state == "applying";
  const double workSeconds =
      (r.value("workMs").toLongLong() +
       (working ? m_clock.elapsed() - r.value("updated").toLongLong() : 0)) /
      1000.;
  if (!r.contains("remaining"))
    r["remaining"] = state != "paused" && state != "waiting" &&
                             state != "queued" && active(state) &&
                             percent > 2 && workSeconds >= 2
                         ? int(workSeconds * (100 - percent) / percent)
                         : -1;
  r["speed"] = (state == "running" || state == "rendering") && percent > 0 &&
                       workSeconds >= 2
                   ? tr("%1%/s").arg(percent / workSeconds, 0, 'f', 1)
                   : QString();
  if (state == "queued")
    r["statusText"] = tr("Queued");
  else if (state == "running" || state == "rendering")
    r["statusText"] = tr("Running");
  else if (state == "paused")
    r["statusText"] = tr("Paused");
  else if (state == "waiting")
    r["statusText"] = tr("Waiting for playback / render");
  else if (state == "applying")
    r["statusText"] = tr("Applying to project");
  else if (state == "completed")
    r["statusText"] = tr("Completed");
  else if (state == "warning")
    r["statusText"] = tr("Completed with warnings");
  else if (state == "partial") // some items failed outright: say so, the summary counts errors
    r["statusText"] = tr("Completed with errors");
  else if (state == "failed")
    r["statusText"] = tr("Failed");
  else if (state == "cancelled")
    r["statusText"] = tr("Cancelled");
  else
    r["statusText"] = state;
  for (const char *key : {"canCancel", "canRetry", "canPause", "canResume"})
    if (!r.contains(key))
      r[key] = false;
  for (const char *key : {"message", "details", "project"})
    if (!r.contains(key))
      r[key] = QString();
  return r;
}
QVariantList BackgroundTasks::jobs() const {
  QVariantList out;
  for (const auto &v : m_jobs)
    if (active(v.toMap().value("state").toString()))
      out.append(presented(v.toMap()));
  int history = 0;
  for (auto i = m_jobs.crbegin(); i != m_jobs.crend() && history < 200; ++i)
    if (!active(i->toMap().value("state").toString())) {
      out.append(presented(i->toMap()));
      ++history;
    }
  return out;
}
int BackgroundTasks::activeCount() const {
  int n = 0;
  for (const auto &v : m_jobs)
    n += active(v.toMap().value("state").toString());
  return n;
}
QVariantMap BackgroundTasks::cache() const {
  const int count = m_cacheActive.size();
  int known = 0;
  double percent = 0;
  for (int value : m_cacheProgress)
    if (value >= 0) {
      ++known;
      percent += value;
    }
  return {{"active", count},
          {"percent", count && known == m_cacheProgress.size() && known
                          ? int(percent / known)
                          : -1},
          {"text", count ? (!m_cacheWaiting.isEmpty() ? tr("Cache waiting")
                                                      : tr("Building cache"))
                         : tr("Cache idle")}};
}
void BackgroundTasks::update(const QVariantMap &row) {
  const QString id = row.value("id").toString();
  if (id.isEmpty())
    return;
  const int index = indexOf(id);
  const QVariantMap previous = index < 0 ? QVariantMap() : m_jobs[index].toMap();
  auto next = row;
  const bool running = active(next.value("state").toString());
  const bool restart = running && !active(previous.value("state").toString());
  const QString priorState = previous.value("state").toString();
  const bool wasWorking = priorState == "running" ||
                          priorState == "rendering" || priorState == "applying";
  next["workMs"] =
      previous.isEmpty() || restart
          ? 0
          : previous.value("workMs").toLongLong() +
                (wasWorking ? m_clock.elapsed() -
                                  previous.value("updated").toLongLong()
                            : 0);
  next["updated"] = m_clock.elapsed();
  if (!running) {
    next["canCancel"] = false;
    next["canPause"] = false;
    next["canResume"] = false;
  }
  next["started"] = previous.isEmpty() || restart
                        ? m_clock.elapsed()
                        : previous.value("started").toLongLong();
  if (!next.contains("project"))
    next["project"] = previous.value("project");
  next["elapsedMs"] =
      running ? 0
              : (active(previous.value("state").toString())
                     ? m_clock.elapsed() - next.value("started").toLongLong()
                     : previous.value("elapsedMs").toLongLong());
  if (next == previous)
    return;
  if (next.value("cache").toBool()) {
    if (running) {
      if (m_cacheActive.isEmpty())
        m_cacheProgress.clear();
      m_cacheActive.insert(id);
      m_cacheProgress[id] = next.value("percent", -1).toInt();
      if (next.value("state") == "waiting")
        m_cacheWaiting.insert(id);
      else
        m_cacheWaiting.remove(id);
    } else {
      m_cacheActive.remove(id);
      m_cacheWaiting.remove(id);
      if (next.value("state") == "completed" && m_cacheProgress.contains(id))
        m_cacheProgress[id] = 100;
      else
        m_cacheProgress.remove(id);
      if (m_cacheActive.isEmpty())
        m_cacheProgress.clear();
    }
  }
  if (index < 0) {
    m_at.insert(id, int(m_jobs.size()));
    m_jobs.append(next);
    m_finished += !running;
  } else {
    m_jobs[index] = next;
    m_finished += int(!running) - int(!active(previous.value("state").toString()));
  }
  if (!previous.isEmpty() && active(previous.value("state").toString()) &&
      !running) {
    const auto display = presented(next);
    const QString state = next.value("state").toString();
    const bool longTask = next.value("elapsedMs").toLongLong() >= 10000;
    if (state != "cancelled" &&
        (next.value("announce", true).toBool() || longTask)) {
      const QString severity = state == "failed" ? "error"
                               : state == "warning" || state == "partial"
                                   ? "warning"
                                   : "success";
      const QString title =
          tr("%1 — %2").arg(next.value("name").toString(),
                            display.value("statusText").toString());
      post(severity, title, next.value("message").toString(), id);
    }
  }
  trim();
  emit changed();
}
void BackgroundTasks::sync(const QString &source, const QVariantList &rows) {
  QSet<QString> seen;
  for (const auto &v : rows) {
    auto row = v.toMap();
    const QString id = source + ":" + row.value("id").toString();
    row["id"] = id;
    seen.insert(id);
    if (m_sourceRows.value(id) != row) {
      m_sourceRows[id] = row;
      update(row);
    }
  }
  for (auto i = m_sourceRows.begin(); i != m_sourceRows.end();) {
    if (!i.key().startsWith(source + ":") || seen.contains(i.key())) {
      ++i;
      continue;
    }
    auto row = job(i.key());
    if (active(row.value("state").toString())) {
      row["state"] = "cancelled";
      row["canCancel"] = false;
      update(row);
    }
    i = m_sourceRows.erase(i);
  }
}
void BackgroundTasks::act(const QString &id, const QString &action) {
  const auto row = job(id);
  const QString capability = action == "cancel"   ? "canCancel"
                             : action == "retry"  ? "canRetry"
                             : action == "pause"  ? "canPause"
                             : action == "resume" ? "canResume"
                                                  : QString();
  if (!capability.isEmpty() && row.value(capability).toBool())
    emit actionRequested(id, action);
}
void BackgroundTasks::trim() {
  // The newest 200 finished rows are the history shown. Older ones go a
  // hundred at a time: dropping one for every task that finishes meant going
  // through the whole list, and finding every row's place again, each time.
  if (m_finished <= 300)
    return;
  m_finished = 200;
  m_at.clear(); // indexOf counts again
  int history = 0;
  for (int i = m_jobs.size() - 1; i >= 0; --i)
    if (!active(m_jobs[i].toMap().value("state").toString()) && ++history > 200)
      m_jobs.removeAt(i);
}
void BackgroundTasks::clearFinished() {
  for (int i = m_jobs.size() - 1; i >= 0; --i)
    if (!active(m_jobs[i].toMap().value("state").toString()))
      m_jobs.removeAt(i);
  m_finished = 0;
  m_at.clear();
  emit changed();
}
void BackgroundTasks::post(const QString &severity, const QString &title,
                           const QString &message, const QString &taskId) {
  if (title.isEmpty() && message.isEmpty())
    return;
  if (!m_notices.isEmpty()) {
    const auto previous = m_notices.first().toMap();
    if (previous.value("title") == title &&
        previous.value("message") == message &&
        m_clock.elapsed() - previous.value("tick").toLongLong() < 5000)
      return;
  }
  const QString level =
      QStringList{"success", "warning", "error", "info"}.contains(severity)
          ? severity
          : "info";
  m_currentNotice = {{"id", QString::number(++m_noticeId)},
             {"severity", level},
             {"title", title},
             {"message", message},
             {"taskId", taskId},
             {"time", QDateTime::currentDateTime().toString(Qt::ISODate)},
             {"tick", m_clock.elapsed()}};
  m_notices.prepend(m_currentNotice);
  while (m_notices.size() > 100)
    m_notices.removeLast();
  emit noticesChanged();
}
void BackgroundTasks::dismissNotice() {
  m_currentNotice.clear();
  emit noticesChanged();
}
void BackgroundTasks::clearNotices() {
  m_notices.clear();
  dismissNotice();
}
