#include "backend.h"
#include "playerworker.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace {
QString mediaType(const QString &kind) {
  if (kind == "import" || kind == "watch")
    return Backend::tr("Import");
  if (kind == "proxy" || kind == "attach")
    return Backend::tr("Proxy");
  if (kind == "relink" || kind == "relinkApply")
    return Backend::tr("Relink");
  if (kind == "clone")
    return Backend::tr("Copy media");
  if (kind == "verify")
    return Backend::tr("Verify media");
  if (kind == "duplicates")
    return Backend::tr("Find duplicates");
  if (kind == "csvImport" || kind == "csvExport")
    return Backend::tr("Metadata CSV");
  return Backend::tr("Storage provider");
}
} // namespace
void Backend::setupFeedback() {
  m_feedbackSync.setSingleShot(true);
  m_feedbackSync.setInterval(0);
  auto schedule = [this] { m_feedbackSync.start(); };
  connect(&m_feedbackSync, &QTimer::timeout, this, &Backend::syncFeedback);
  connect(m_mediaTasks, &MediaTasks::changed, this, schedule);
  connect(m_renderQueue, &RenderQueue::jobsChanged, this, schedule);
  connect(this, &Backend::projectChanged, this, schedule);
  connect(this, &Backend::libraryChanged, this, &Backend::refreshMediaHealth);
  connect(this, &Backend::timelineEntriesChanged, this,
          &Backend::refreshMediaHealth);
  connect(this, &Backend::mediaPlaybackChanged, this,
          &Backend::refreshMediaHealth);
  connect(m_worker, &PlayerWorker::linearPlaybackChanged, this,
          [this](bool on) {
            if (m_linearPlayback == on)
              return;
            m_linearPlayback = on;
            refreshMediaHealth();
          });
  connect(this, &Backend::failed, this, [this](const QString &message) {
    m_backgroundTasks->post("error", tr("Operation failed"), message);
  });
  // A routine save is announced in the status line only ("Saved <name>");
  // notices stay in the inline info area. A failed save raises `failed`.
  connect(m_worker, &PlayerWorker::exportFallbackUsed, this,
          [this](const QString &message) {
            m_backgroundTasks->post("warning", tr("Software encoder fallback"),
                                    message);
          });
  // Cache creation can precede projectLoaded while the worker restores a file.
  // Capture the requested project label without changing the open project's
  // name.
  auto loadingProject = std::make_shared<QString>();
  connect(this, &Backend::loadProjectRequested, this,
          [loadingProject](const QString &path) {
            *loadingProject = QFileInfo(path).completeBaseName();
          });
  connect(m_worker, &PlayerWorker::projectLoaded, this,
          [loadingProject](bool, const QString &, const QStringList &) {
            loadingProject->clear();
          });
  auto cacheUpdate = [this, loadingProject](QVariantMap row) {
    const QString id = row.value("id").toString();
    if (m_backgroundTasks->job(id).isEmpty())
      row["project"] =
          loadingProject->isEmpty() ? projectName() : *loadingProject;
    m_backgroundTasks->update(row);
  };
  connect(m_worker, &PlayerWorker::cacheTaskChanged, this, cacheUpdate);
  connect(m_beatMapper, &BeatMapper::taskChanged, this, cacheUpdate);
  connect(m_worker, &PlayerWorker::renderProgress, this,
          [this](int, int, int percent) {
            workerTaskProgress("replace", tr("Render in Place"), percent);
          });
  connect(m_worker, &PlayerWorker::stabProgress, this,
          [this](int, int, int percent) {
            workerTaskProgress("stabilize", tr("Stabilisation"), percent);
          });
  connect(m_worker, &PlayerWorker::trackProgress, this,
          [this](int, int, int, int percent) {
            workerTaskProgress("track", tr("Window tracking"), percent);
          });
  // Kept media moving to its folder as the editor starts (keptmedia.h): said
  // in the status line, and kept in Background Tasks with what could not be
  // moved and why.
  const auto keptMove = [this](const QString &state, const QString &message) {
    m_backgroundTasks->update({{"id", "kept-media:move"},
                               {"name", tr("Move kept media")},
                               {"type", tr("Storage")},
                               {"canCancel", false},
                               {"state", state},
                               {"percent", state == "running" ? 0 : 100},
                               {"message", message}});
  };
  connect(m_worker, &PlayerWorker::keptMediaAdopting, this,
          [this, keptMove](const QString &folder) {
            setStatus(tr("Moving kept media to %1…").arg(folder));
            keptMove("running", {});
          });
  connect(m_worker, &PlayerWorker::keptMediaAdopted, this,
          [this, keptMove](const QString &folder, int moved,
                           const QStringList &problems) {
            if (problems.isEmpty()) {
              setStatus(tr("Kept media moved to %1: %n file(s).", "", moved).arg(folder));
              keptMove("completed", {});
              return;
            }
            setStatus(tr("Not all kept media could be moved to %1. Nothing is lost: see Background Tasks.").arg(folder));
            keptMove("failed", problems.join(QLatin1Char('\n')));
          });
  connect(m_projectArchive, &ProjectArchive::changed, this, [this] {
    if (!m_projectArchive->busy())
      return;
    if (m_archiveTaskId.isEmpty())
      m_archiveTaskId =
          "archive:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto state = m_projectArchive->state();
    m_backgroundTasks->update({{"id", m_archiveTaskId},
                               {"name", tr("Archive project")},
                               {"project", projectName()},
                               {"type", tr("Archive")},
                               {"state", "running"},
                               {"percent", state.value("progress", -1)},
                               {"message", state.value("message")},
                               {"canCancel", true}});
  });
  connect(m_projectArchive, &ProjectArchive::finished, this,
          [this](bool ok, const QString &path, const QString &error) {
            if (m_archiveTaskId.isEmpty())
              return;
            auto row = m_backgroundTasks->job(m_archiveTaskId);
            row["state"] = ok ? "completed"
                           : error.contains("cancel", Qt::CaseInsensitive)
                               ? "cancelled"
                               : "failed";
            row["percent"] = ok ? 100 : row.value("percent");
            row["message"] = ok ? path : error;
            row["canCancel"] = false;
            m_backgroundTasks->update(row);
            m_archiveTaskId.clear();
          });
  connect(m_backgroundTasks, &BackgroundTasks::actionRequested, this,
          [this](const QString &id, const QString &action) {
            const QString key = id.section(':', 1);
            if (id.startsWith("media:")) {
              const auto job = m_mediaTasks->record(key);
              if (action == "cancel")
                m_mediaTasks->cancel(key);
              else if (action == "retry" && mediaTaskCurrent(key)) {
                // Rebuild current catalogue facts, never replay stale
                // known-file or generation data.
                startMediaTask(job.value("kind").toString(),
                               job.value("paths").toStringList(),
                               job.value("options").toMap());
              }
            } else if (id.startsWith("render:")) {
              const int number = key.toInt();
              if (action == "cancel")
                m_renderQueue->cancelJob(number);
              else if (action == "retry")
                m_renderQueue->retryJob(number);
              else if (action == "pause")
                m_renderQueue->pauseJob(number);
              else if (action == "resume")
                m_renderQueue->resumeJob(number);
            } else if (id.startsWith("preview:") && id == m_backgroundRender.value("id").toString()) {
              backgroundRenderAction(action);
            } else if (id.startsWith("cache:") && action == "cancel") {
              QMetaObject::invokeMethod(
                  m_worker, [this, id] { m_worker->cancelCacheTask(id); },
                  Qt::QueuedConnection);
            } else if (id.startsWith("beats:") && action == "cancel") {
              m_beatMapper->cancel(
                  m_backgroundTasks->job(id).value("token").toString());
            } else if (id == m_workerTaskIds.value("track") &&
                       action == "cancel")
              stopWindowTracking(false);
            else if (id == m_archiveTaskId && action == "cancel")
              m_projectArchive->cancel();
            else if (id == m_aafTaskId && action == "cancel" && m_aafCancel)
              m_aafCancel->store(true);
          });
  syncFeedback();
  refreshMediaHealth();
}
void Backend::workerTaskProgress(const QString &kind, const QString &name,
                                 int percent) {
  QString id = m_workerTaskIds.value(kind);
  if (id.isEmpty()) {
    id = kind + ":" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_workerTaskIds[kind] = id;
  }
  m_backgroundTasks->update(
      {{"id", id},
       {"name", name},
       {"project", projectName()},
       {"type", kind == "replace" ? tr("Render cache") : tr("Analysis")},
       {"cache", kind == "replace"},
       {"canCancel", kind == "track" && percent >= 0 && percent < 100},
       {"state", percent == -2    ? "cancelled"
                 : percent < 0    ? "failed"
                 : percent >= 100 ? "completed"
                                  : "running"},
       {"percent", percent},
       {"message", percent < 0 ? status() : QString()}});
  if (percent < 0 || percent >= 100)
    m_workerTaskIds.remove(kind);
}
void Backend::syncFeedback() {
  QVariantList media;
  for (const auto &v : m_mediaTasks->jobs()) {
    const auto j = v.toMap();
    const QString state = j.value("state").toString(),
                  kind = j.value("kind").toString();
    const bool current = mediaTaskCurrent(j.value("id").toString());
    media.append(QVariantMap{
        {"id", j.value("id")},
        {"name", j.value("label")},
        {"type", mediaType(kind)},
        {"project",
         j.value("options").toMap().value("projectName", projectName())},
        {"state", state},
        {"percent", j.value("percent", -1)},
        {"message", j.value("message")},
        {"details",
         QString::fromUtf8(QJsonDocument::fromVariant(j.value("report"))
                               .toJson(QJsonDocument::Indented))},
        {"canCancel", state == "running" || state == "queued"},
        {"canRetry", current && (state == "failed" || state == "partial" ||
                                 state == "cancelled")},
        {"announce",
         current && (kind != "watch" || j.value("completed").toInt() > 0 ||
                     state == "failed" || state == "partial")}});
  }
  m_backgroundTasks->sync("media", media);
  QVariantList renders;
  for (const auto &v : m_renderQueue->jobs()) {
    const auto j = v.toMap();
    const QString state = j.value("status").toString();
    const QString id = "render:" + j.value("id").toString();
    auto existing = m_backgroundTasks->job(id);
    QString details = j.value("error").toString() + "\n" +
                      j.value("warnings").toStringList().join('\n') + "\n" +
                      j.value("log").toStringList().join('\n');
    renders.append(QVariantMap{
        {"id", j.value("id")},
        {"name", j.value("name")},
        {"type", tr("Render")},
        {"project", existing.isEmpty()
                        ? j.value("spec").toMap().value("projectName",
                                                        tr("Queued project"))
                        : existing.value("project")},
        {"state", state},
        {"percent", j.value("percent", -1)},
        {"elapsed", j.value("elapsed", 0)},
        {"remaining", j.value("remaining", -1)},
        {"message",
         j.value("error").toString().isEmpty()
             ? j.value("stage").toString() + "\n" + j.value("path").toString()
             : j.value("error")},
        {"details", details.trimmed()},
        {"canCancel", BackgroundTasks::active(state)},
        {"canRetry", state == "failed" || state == "cancelled"},
        {"canPause", state == "rendering"},
        {"canResume", state == "paused"}});
  }
  m_backgroundTasks->sync("render", renders);
}
void Backend::refreshMediaHealth() {
  int total = 0, wanted = 0, ready = 0, offline = 0, missingAttached = 0;
  QSet<QString> seen;
  QVariantList sources = m_library;
  sources.append(m_timeline);
  sources.append(m_connected);
  for (const auto &v : sources) {
    const auto entry = v.toMap();
    const QString path = entry.value("path").toString();
    if (entry.value("multicam").toBool() || !QFileInfo(path).isAbsolute() ||
        seen.contains(path))
      continue;
    seen.insert(path);
    ++total;
    const auto facts = m_lastMediaSources.value(path).toMap();
    // The fallback stat only runs when the media sources have not reported.
    const bool available = facts.contains("originalAvailable")
                               ? facts.value("originalAvailable").toBool()
                               : QFileInfo::exists(path);
    const bool proxy =
        facts.value("proxyAvailable", entry.value("proxy")).toBool();
    if (!available)
      ++offline;
    if (!entry.value("proxyPath").toString().isEmpty() && !proxy)
      ++missingAttached;
    const bool use =
        !m_linearPlayback &&
        (m_mediaPlayback == "proxy" ||
         (m_mediaPlayback == "preferred" &&
          (entry.value("preferProxy", true).toBool() || !available)));
    if (use) {
      ++wanted;
      if (proxy)
        ++ready;
    }
  }
  offline = missingMediaEntries().size();
  QString label = m_linearPlayback ? tr("Originals · OCIO")
                  : total == 0 && m_timeline.isEmpty() && m_connected.isEmpty()
                      ? tr("No media")
                  : wanted ? tr("Proxies %1/%2").arg(ready).arg(wanted)
                           : tr("Originals");
  if (offline)
    label = tr("%1 offline · %2").arg(offline).arg(label);
  else if (missingAttached)
    label = tr("Missing proxy · %1").arg(label);
  QStringList details;
  if (m_linearPlayback)
    details << tr("OCIO processing uses original media to preserve precision.");
  if (wanted > ready)
    details
        << tr("%1 requested proxies unavailable; available originals are used.")
               .arg(wanted - ready);
  if (missingAttached)
    details << tr("%1 attached proxy files are missing.").arg(missingAttached);
  if (offline)
    details << tr("%1 originals offline — relink required for original "
                  "playback and export.")
                   .arg(offline);
  if (details.isEmpty())
    details << tr("All requested media is available.");
  const auto previous = m_mediaHealth;
  m_mediaHealth = {
      {"label", label},
      {"originalsOnly", m_linearPlayback},
      {"ready", offline == 0 && wanted == ready && missingAttached == 0},
      {"offline", offline},
      {"missingProxies", qMax(missingAttached, wanted - ready)},
      {"message", details.join(' ')}};
  if (previous == m_mediaHealth)
    return;
  emit mediaHealthChanged();
  if (offline > previous.value("offline").toInt())
    m_backgroundTasks->post("warning", tr("Offline media — relink required"),
                            details.join(' '));
  else if (missingAttached > previous.value("missingProxies").toInt())
    m_backgroundTasks->post("warning", tr("Missing proxy media"),
                            details.join(' '));
}
