#include "agentcli.h"
#include "agentskill.h"
#include "backend.h"
#include "colourpipeline.h"
#include "exportspec.h"
#include "hardwareprobe.h"
#include "markeredit.h"
#include "omaedit_version.h"
#include "speedrampedit.h"
#include "playerworker.h"
#include "projectfile.h"
#include "projectsafety.h"
#include "startup.h"
#include "engines.h"
#include "thumbnailprovider.h"
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <unistd.h>

namespace {
struct CliError : std::exception {
  CliError(QString code, QString message) : code(std::move(code)), message(std::move(message)) {}
  QString code, message;
};
[[noreturn]] void fail(const QString &code, const QString &message) {
  throw CliError{code, message};
}
void output(const QVariantMap &value, FILE *stream = stdout) {
  const auto bytes =
      QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact) + '\n';
  fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
  fflush(stream);
}
QString absolute(const QString &path) {
  if (path.isEmpty())
    fail("invalid_argument", "A non-empty local path is required.");
  const QFileInfo info(path);
  return info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}
QByteArray fingerprint(const QString &path) {
  QFile f(path);
  if (!f.exists())
    return {};
  if (!f.open(QIODevice::ReadOnly))
    fail("read_failed", f.errorString());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&f))
    fail("read_failed", f.errorString());
  return hash.result();
}
QVariant json(const QByteArray &bytes) {
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError)
    fail("invalid_json", error.errorString());
  return doc.toVariant();
}
QVariant readJson(const QString &text) {
  if (text == "-") {
    QFile f;
    if (!f.open(stdin, QIODevice::ReadOnly))
      fail("read_failed", f.errorString());
    return json(f.readAll());
  }
  if (text.startsWith('@')) {
    QFile f(text.mid(1));
    if (!f.open(QIODevice::ReadOnly))
      fail("read_failed", f.errorString());
    return json(f.readAll());
  }
  return json(text.toUtf8());
}
QString requiredString(const QVariantMap &args, const QString &name) {
  const auto v = args.value(name);
  if (v.metaType().id() != QMetaType::QString || v.toString().isEmpty())
    fail("invalid_argument", "Expected a non-empty string: " + name);
  return v.toString();
}
QVariant converted(const QVariant &input, QMetaType type, const QString &name) {
  const int id = type.id(), from = input.metaType().id();
  if (id == QMetaType::QVariant)
    return input;
  if (id == QMetaType::QUrl) {
    if (from != QMetaType::QString)
      fail("invalid_argument", name + " must be a path or URL string.");
    const auto s = input.toString();
    return QVariant::fromValue(
        s.contains("://") ? QUrl(s) : QUrl::fromLocalFile(absolute(s)));
  }
  if (id == QMetaType::QString && from != QMetaType::QString)
    fail("invalid_argument", name + " must be a string.");
  if (id == QMetaType::Bool && from != QMetaType::Bool)
    fail("invalid_argument", name + " must be true or false.");
  if (id == QMetaType::Int || id == QMetaType::LongLong ||
      id == QMetaType::UInt || id == QMetaType::ULongLong ||
      id == QMetaType::Double || id == QMetaType::Float) {
    if (from != QMetaType::Double && from != QMetaType::LongLong &&
        from != QMetaType::Int)
      fail("invalid_argument", name + " must be a number.");
    const double d = input.toDouble();
    if (!std::isfinite(d) ||
        ((id != QMetaType::Double && id != QMetaType::Float) &&
         std::floor(d) != d) ||
        (id == QMetaType::Int && (d < -2147483648. || d > 2147483647.)) ||
        ((id == QMetaType::UInt || id == QMetaType::ULongLong) && d < 0))
      fail("invalid_argument", name + " is outside its numeric range.");
  }
  if (id == QMetaType::QVariantMap && from != QMetaType::QVariantMap)
    fail("invalid_argument", name + " must be an object.");
  if ((id == QMetaType::QVariantList || id == QMetaType::QStringList ||
       type == QMetaType::fromType<QList<QUrl>>()) &&
      from != QMetaType::QVariantList && from != QMetaType::QStringList)
    fail("invalid_argument", name + " must be an array.");
  if (id == QMetaType::QStringList ||
      type == QMetaType::fromType<QList<QUrl>>()) {
    QStringList strings;
    QList<QUrl> urls;
    for (const auto &v : input.toList()) {
      if (v.metaType().id() != QMetaType::QString)
        fail("invalid_argument", name + " must contain strings.");
      strings << v.toString();
      if (type == QMetaType::fromType<QList<QUrl>>())
        urls << converted(v, QMetaType::fromType<QUrl>(), name).toUrl();
    }
    return id == QMetaType::QStringList ? QVariant(strings)
                                        : QVariant::fromValue(urls);
  }
  QVariant value = input;
  if (!value.convert(type))
    fail("invalid_argument", "Cannot convert " + name + " to " + type.name());
  return value;
}
// A misspelt setting would otherwise be dropped silently and its default
// used instead.
void knownKeys(const QVariantMap &map, const QStringList &allowed,
               const QString &name) {
  for (auto i = map.begin(); i != map.end(); ++i)
    if (!allowed.contains(i.key()))
      fail("invalid_argument", "Unknown key in " + name + ": " + i.key() +
                                   ". See omaedit defaults.");
}
// Render settings: every key of a complete spec, plus the audio track
// selection the Deliver page adds to it.
QStringList renderKeys() {
  return ExportSpec::normalized({}).keys() +
         QStringList{"audioSelection", "audioTracks", "audioTrackMap",
                     "vaapiDevice"};
}
QVariantMap range(double min, double max, double neutral,
                  const QString &meaning = {}) {
  QVariantMap r{{"min", min}, {"max", max}, {"neutral", neutral}};
  if (!meaning.isEmpty())
    r["meaning"] = meaning;
  return r;
}
// Value ranges of numeric colour and audio parameters, keyed "op:parameter".
// They are the limits the engine clamps to (Timeline::setColorAt,
// Backend::setClipVolume/setClipPan, Timeline::setClipSpeedOn and the
// audio engine's fader), so an agent can choose values without guessing.
const QVariantMap &parameterRanges() {
  static const QVariantMap mixValue{
      {"byKey",
       QVariantMap{{"volume", range(-100, 36, 0, "dB gain; -100 is silent")},
                   {"pan", range(-1, 1, 0, "-1 left, 1 right")}}}};
  static const QVariantMap ranges{
      {"editor.setClipColor:brightness",
       range(-1, 1, 0, "offset added to brightness; 0 leaves the picture "
                       "unchanged")},
      {"editor.setClipColor:contrast", range(0, 2, 1, "multiplier")},
      {"editor.setClipColor:saturation", range(0, 3, 1, "multiplier")},
      {"editor.setConnectedColor:brightness",
       range(-1, 1, 0, "offset added to brightness; 0 leaves the picture "
                       "unchanged")},
      {"editor.setConnectedColor:contrast", range(0, 2, 1, "multiplier")},
      {"editor.setConnectedColor:saturation", range(0, 3, 1, "multiplier")},
      {"editor.setClipVolume:db", range(-60, 12, 0, "dB; -60 silences")},
      {"editor.setConnectedVolume:db", range(-60, 12, 0, "dB; -60 silences")},
      {"editor.adjustVolume:deltaDb",
       range(-72, 72, 0, "dB added to the clip's volume, which stays within "
                         "-60 to 12")},
      {"editor.setClipPan:pan", range(-1, 1, 0, "-1 left, 1 right")},
      {"editor.setConnectedPan:pan", range(-1, 1, 0, "-1 left, 1 right")},
      {"editor.setClipSpeed:speed",
       range(-20, 20, 1, "rate; negative plays in reverse, magnitude at "
                         "least 0.05")},
      {"editor.setConnectedSpeed:speed",
       range(-20, 20, 1, "rate; negative plays in reverse, magnitude at "
                         "least 0.05")},
      {"loudness.normalize:targetLufs",
       range(-60, -1, -23, "integrated loudness target in LUFS")},
      {"loudness.normalize:truePeak",
       range(-20, 0, -1, "true-peak ceiling in dBTP the result is checked "
                         "against; optional")},
      {"editor.setTrackMix:value", mixValue},
      {"editor.setMasterMix:value", mixValue}};
  return ranges;
}
// A value outside a documented range is refused rather than clamped or
// stored as given, so the reply never claims an edit the engine changed.
void checkRanges(const QString &op, const QVariantMap &args) {
  for (auto i = args.begin(); i != args.end(); ++i) {
    auto limits = parameterRanges().value(op + ':' + i.key()).toMap();
    QString name = i.key();
    if (limits.contains("byKey")) {
      name = args.value("key").toString();
      limits = limits.value("byKey").toMap().value(name).toMap();
    }
    const auto id = i.value().metaType().id();
    if (limits.isEmpty() || (id != QMetaType::Double &&
                             id != QMetaType::LongLong && id != QMetaType::Int))
      continue;
    const double value = i.value().toDouble(),
                 min = limits.value("min").toDouble(),
                 max = limits.value("max").toDouble();
    if (value < min || value > max)
      fail("invalid_argument", QString("%1 must be between %2 and %3.")
                                   .arg(name)
                                   .arg(min)
                                   .arg(max));
  }
}
bool supported(QMetaType type) {
  return QList<int>{QMetaType::Void,         QMetaType::Bool,
                    QMetaType::Int,          QMetaType::UInt,
                    QMetaType::LongLong,     QMetaType::ULongLong,
                    QMetaType::Double,       QMetaType::Float,
                    QMetaType::QString,      QMetaType::QStringList,
                    QMetaType::QUrl,         QMetaType::QVariant,
                    QMetaType::QVariantList, QMetaType::QVariantMap}
             .contains(type.id()) ||
         type == QMetaType::fromType<QList<QUrl>>();
}
// Names that used to be the editor's own and now belong to one of its
// objects. The old spelling keeps working (editor.rescanLuts, get
// transcriptModel) so that a script written before the move still runs.
// A name leaving the editor gets a row here.
void resolveMoved(QString &target, QString &name) {
  static const QHash<QString, std::pair<QString, QString>> moved = {
      {"toggleEffectFavorite", {"effects", "toggleFavorite"}},
      {"clearRecentEffects", {"effects", "clearRecents"}},
      {"addEffectBin", {"effects", "addBin"}},
      {"renameEffectBin", {"effects", "renameBin"}},
      {"deleteEffectBin", {"effects", "deleteBin"}},
      {"addToEffectBin", {"effects", "addToBin"}},
      {"removeFromEffectBin", {"effects", "removeFromBin"}},
      {"effectBinKeys", {"effects", "binKeys"}},
      {"effectFavorites", {"effects", "favorites"}},
      {"recentEffects", {"effects", "recents"}},
      {"effectBins", {"effects", "bins"}},
      {"setVideoDecoderPreference", {"hardware", "setVideoDecoderPreference"}},
      {"setVideoDecoderDevice", {"hardware", "setVideoDecoderDevice"}},
      {"retestVideoDecoder", {"hardware", "retestVideoDecoder"}},
      {"setInterfaceRenderingPreference", {"hardware", "setInterfaceRenderingPreference"}},
      {"setViewerProcessingPreference", {"hardware", "setViewerProcessingPreference"}},
      {"setEngineGpu", {"hardware", "setEngineGpu"}},
      {"probeEncoders", {"hardware", "probeEncoders"}},
      {"encoders", {"hardware", "encoders"}},
      {"videoDecoderPreference", {"hardware", "videoDecoderPreference"}},
      {"videoDecoderDevice", {"hardware", "videoDecoderDevice"}},
      {"videoDecoderDevices", {"hardware", "videoDecoderDevices"}},
      {"videoDecoderRestartRequired", {"hardware", "videoDecoderRestartRequired"}},
      {"viewerProcessingPreference", {"hardware", "viewerProcessingPreference"}},
      {"interfaceRenderingPreference", {"hardware", "interfaceRenderingPreference"}},
      {"engineGpu", {"hardware", "engineGpu"}},
      {"engineGpus", {"hardware", "engineGpus"}},
      {"engineGpuRestartRequired", {"hardware", "engineGpuRestartRequired"}},
      {"interfaceRenderingRestartRequired", {"hardware", "interfaceRenderingRestartRequired"}},
      {"probeHardware", {"hardware", "probe"}},
      {"hardwareInfo", {"hardware", "info"}},
      {"cancelTranscription", {"transcriber", "cancel"}},
      {"transcriptCommand", {"transcriber", "command"}},
      {"transcriptModel", {"transcriber", "model"}},
      {"transcriptLanguage", {"transcriber", "language"}},
      {"transcriptAvailable", {"transcriber", "available"}},
      {"transcriptStatus", {"transcriber", "status"}},
      {"transcriptJob", {"transcriber", "job"}},
      {"setLutFolder", {"luts", "setFolder"}},
      {"rescanLuts", {"luts", "rescan"}},
      {"addLutFile", {"luts", "addFile"}},
      {"setLutFavorite", {"luts", "setFavorite"}},
      {"luts", {"luts", "luts"}},
      {"lutFolder", {"luts", "folder"}},
      {"refreshDrives", {"volumes", "refreshDrives"}},
      {"drives", {"volumes", "drives"}},
      {"diskFree", {"volumes", "diskFree"}},
      {"diskLow", {"volumes", "diskLow"}},
      {"audioLeft", {"meters", "left"}},
      {"audioRight", {"meters", "right"}},
      {"trackLevels", {"meters", "trackLevels"}},
      {"audioRoutingLevels", {"meters", "routingLevels"}},
      {"loudness", {"meters", "loudness"}},
      {"loudnessHistory", {"meters", "loudnessHistory"}},
      {"spectrum", {"meters", "spectrum"}},
      {"analyzeLoudness", {"loudness", "analyze"}},
      {"cancelLoudnessAnalysis", {"loudness", "cancel"}},
      {"normalizeLoudness", {"loudness", "normalize"}},
      {"loudnessAnalysis", {"loudness", "result"}}};
  if (target != "editor")
    return;
  const auto it = moved.constFind(name);
  if (it == moved.constEnd())
    return;
  target = it->first;
  name = it->second;
}
bool permitted(const QString &target, const QString &name) {
  // Dialogs, clipboard, desktop launch and internal UI bookkeeping have no
  // headless meaning. Explicit path-taking wrappers replace file dialogs.
  static const QSet<QString> excluded = {"saveProject",
                                         "saveProjectAs",
                                         "loadProject",
                                         "openRecent",
                                         "newProject",
                                         "closeProject",
                                         "createProject",
                                         "createSequence",
                                         "addSequence",
                                         "enableTimelineCreationPrompt",
                                         "cancelTimelinePlacement",
                                         "announce",
                                         "saveStill",
                                         "copySystemInformation",
                                         "quitWhenRendersFinish",
                                         "exportWithPreset",
                                         "exportTo",
                                         "enqueueRender",
                                         "openRenderOutput",
                                         "revealRenderOutput",
                                         "revealClip",
                                         "revealMediaPath",
                                         "locateRecentProject",
                                         "setTimelineDecorationWindow",
                                         "timelineDecoration",
                                         "beginEditGesture",
                                         "endEditGesture",
                                         "prepareSpeedRamp",
                                         "beginAudioGesture",
                                         "endAudioGesture",
                                         "noteUsed",
                                         "refreshRecentProjects",
                                         "image",
                                         "cachePath",
                                         "saveSourceFrame",
                                         "saveProgramFrame",
                                         "open",
                                         "importUrls",
                                         "restoreProjectCopy",
                                         // Asks, then waits for the editor's Ok in the window.
                                         "findUnusedKeptMedia",
                                         "removeUnusedKeptMedia"};
  if (name.contains("Dialog") || name.startsWith("choose") ||
      name == "browse" || excluded.contains(name))
    return false;
  // Not offered in this version (Backend::kMaskProviderOffered).
  if (!Backend::kMaskProviderOffered &&
      QStringList{"generateColorMask", "cancelColorMask"}.contains(name))
    return false;
  if (target == "queue" && name == "enqueue")
    return false;
  return true;
}
// The external mask provider's settings and progress, while it is not
// offered (Backend::kMaskProviderOffered), are neither listed nor reached.
bool shownProperty(const QString &name) {
  return Backend::kMaskProviderOffered ||
         !QStringList{"maskProvider", "maskJob", "maskProviderOffered"}.contains(name);
}
} // namespace

// Uses the exact editor Backend. The friend access is only for worker fencing,
// asynchronous job completion and a non-interactive save path.
class AgentSession : public QObject {
public:
  // A one-shot command takes --overwrite; a session request overwrite:true.
  AgentSession(int timeout, bool oneShot)
      : m_timeout(timeout), m_oneShot(oneShot) {
    m_thumbs = std::make_unique<ThumbnailProvider>();
    m_backend = std::make_unique<Backend>();
    auto *b = m_backend.get();
    b->setThumbnailProvider(m_thumbs.get());
    b->setAutosaveMinutes(0);
    b->soundLibrary()->cancelScan();
    m_objects = {{"editor", b},
                 {"media", b->mediaCatalog()},
                 {"mediaTasks", b->mediaTasks()},
                 {"queue", b->renderQueue()},
                 {"presets", b->renderPresets()},
                 {"sounds", b->soundLibrary()},
                 {"recorder", b->recorder()},
                 {"stills", b->stills()},
                 {"tasks", b->backgroundTasks()},
                 {"effects", b->effectShelf()},
                 {"hardware", b->hardware()},
                 {"transcriber", b->transcriber()},
                 {"luts", b->lutLibrary()},
                 {"volumes", b->volumes()},
                 {"meters", b->meters()},
                 {"loudness", b->loudnessPass()}};
    connect(b, &Backend::failed, this, [this](const QString &s) { error(s); });
    connect(b, &Backend::statusChanged, this, [this] {
      m_statusBefore = {m_statusChanged, m_status};
      m_statusChanged = true;
      m_status = m_backend->status();
    });
    // A queued render starting or ending in the background sets a status
    // that belongs to its job, not to whatever request is running; only a
    // render or queue request reports it. The backend announced it first.
    const auto background = [this] {
      if (m_op != "render" && !m_op.startsWith("queue."))
        std::tie(m_statusChanged, m_status) = m_statusBefore;
    };
    connect(b->renderQueue(), &RenderQueue::startRequested, this, background);
    connect(b->renderQueue(), &RenderQueue::jobFinished, this, background);
    auto *w = b->m_worker;
    connect(w, &PlayerWorker::audioOperationFinished, this,
            [this](const QString &s) { error(s); });
    connect(w, &PlayerWorker::gradeVersionFinished, this,
            [this](const QString &s) {
              if (!s.isEmpty())
                m_gradeMessages << s;
            });
    connect(w, &PlayerWorker::editOperationFinished, this,
            [this](const QString &s) {
              if (s.startsWith("No edit"))
                error(s);
              else if (!s.isEmpty())
                m_messages << s;
            });
    connect(w, &PlayerWorker::editRefused, this,
            [this](const QString &s) { error(s); });
    connect(w, &PlayerWorker::compoundEditFinished, this,
            [this](bool, int, const QString &s) { error(s); });
    connect(w, &PlayerWorker::markerEditFinished, this,
            [this](int, bool, const QString &s) { error(s); });
    connect(w, &PlayerWorker::retimeFinished, this,
            [this](int, bool, const QString &s) { error(s); });
    connect(b, &Backend::audioEditFinished, this,
            [this](int, bool, const QString &s) { error(s); });
    connect(b, &Backend::audioGestureFinished, this,
            [this](int, bool, const QString &s, const QVariantMap &) { error(s); });
    connect(b, &Backend::clipboardPasteFinished, this,
            [this](int, bool changed, const QString &s, const QVariantList &) {
              ++m_replies;
              error(!changed && s.isEmpty() ? QString("Nothing was pasted.") : s);
            });
    // These report their outcome through the node they produced; the message
    // accompanies both a success and a refusal.
    connect(w, &PlayerWorker::colorPasteFinished, this,
            [this](int, int, int node, const QString &s) {
              ++m_replies;
              if (node >= 0)
                m_messages << s;
              else
                error(s.isEmpty() ? "Nothing was pasted." : s);
            });
    connect(w, &PlayerWorker::autoBalanceFinished, this,
            [this](int, int, int node, const QString &s) {
              ++m_replies;
              if (node >= 0)
                m_messages << s;
              else
                error(s.isEmpty() ? "Auto Balance was not applied." : s);
            });
    connect(w, &PlayerWorker::nodeLutFinished, this,
            [this](bool changed, const QString &s) {
              ++m_replies;
              if (changed || s == "LUT is unchanged")
                m_messages << s;
              else
                error(s);
            });
    connect(w, &PlayerWorker::colorMatteReady, this,
            [this](int, const QImage &, const QString &s) { error(s); });
    connect(w, &PlayerWorker::lutPreviewReady, this,
            [this](int, const QImage &, const QImage &, const QString &s) {
              error(s);
            });
    connect(w, &PlayerWorker::sequenceOpenFailed, this,
            [this](const QString &name) {
              error("Could not open timeline: " + name);
            });
    connect(b, &Backend::frameExported, this, [this](bool ok, const QString &) {
      if (!ok)
        error(m_backend->status());
    });
    connect(b->recorder(), &Recorder::recordingFailed, this,
            [this](const QString &, const QString &s) { error(s); });
    connect(b->renderPresets(), &RenderPresets::failed, this,
            [this](const QString &s) { error(s); });
    connect(b->stills(), &StillsGallery::failed, this,
            [this](const QString &s) { error(s); });
    connect(b, &Backend::projectCreateFinished, this,
            [this](int, bool, const QString &s) { error(s); });
    connect(b, &Backend::sequenceCreateFinished, this,
            [this](int, bool, const QString &s) { error(s); });
    connect(b->renderQueue(), &RenderQueue::jobFinished, this,
            [this](int, bool ok, const QString &, const QString &s) {
              if (!ok && s != "Cancelled")
                error(s);
            });
    connect(b->mediaTasks(), &MediaTasks::finished, this,
            [this](const QString &, const QVariantMap &r) {
              if (r.value("state") == "failed")
                error(r.value("message").toString());
            });
    connect(b->projectArchive(), &ProjectArchive::finished, this,
            [this](bool ok, const QString &, const QString &s) {
              if (!ok)
                error(s);
            });
    connect(b->renderQueue(), &RenderQueue::jobsChanged, this, [this] {
      if (!m_progress)
        return;
      const auto j =
          m_backend->renderQueue()->job(m_backend->renderQueue()->activeJob());
      const auto key = j.value("id").toString() + ":" +
                       j.value("percent").toString() + ":" +
                       j.value("stage").toString();
      if (!j.isEmpty() && key != m_lastProgress) {
        m_lastProgress = key;
        output({{"event", "render.progress"}, {"job", j}}, stderr);
      }
    });
    settle(true);
    for (const auto &v : b->renderQueue()->jobs())
      if (waiting(v.toMap()))
        m_earlierJobs << v.toMap().value("id").toInt();
  }
  ~AgentSession() override {
    // Finalize devices while Backend is still fully alive; its callbacks can
    // publish a partial take or a failure and must not enter its destructor.
    if (m_backend->recordingTake())
      m_backend->stopRecording();
    else if (m_backend->recorder()->recording())
      m_backend->recorder()->stop();
    m_backend->pause();
    m_backend->discardRecovery();
  }
  bool dirty() const { return m_backend->dirty(); }
  // Work this session leaves undone: anything running, and renders it
  // queued but never ran. Jobs already waiting when it began are not its own.
  bool unfinished() const {
    if (busy() || m_backend->recorder()->recording() ||
        m_backend->renderQueue()->active())
      return true;
    for (const auto &v : m_backend->renderQueue()->jobs())
      if (waiting(v.toMap()) &&
          !m_earlierJobs.contains(v.toMap().value("id").toInt()))
        return true;
    return false;
  }
  QVariantMap request(const QVariantMap &request) {
    m_errors.clear();
    m_messages.clear();
    m_gradeMessages.clear();
    m_replies = 0;
    m_statusChanged = false;
    m_status.clear();
    const auto beforeRevision = m_backend->m_projectRevision;
    m_progress = request.value("progress", false).toBool();
    const auto op = request.value("op").toString();
    m_op = op;
    try {
      for (auto it = request.begin(); it != request.end(); ++it)
        if (!QStringList{"id", "op", "args", "wait", "progress"}.contains(
                it.key()))
          fail("invalid_request", "Unknown request field: " + it.key());
      for (const auto &key : QStringList{"wait", "progress"})
        if (request.contains(key))
          converted(request[key], QMetaType::fromType<bool>(), key);
      if (op.isEmpty())
        fail("invalid_request", "A request requires an op string.");
      const auto args = request.value("args", QVariantMap{});
      if (args.metaType().id() != QMetaType::QVariantMap)
        fail("invalid_argument", "args must be an object.");
      const bool control = op.startsWith("queue.pause") ||
                           op.startsWith("queue.cancel") ||
                           op.startsWith("queue.resume");
      const auto result =
          execute(op, args.toMap(), request.value("wait", !control).toBool());
      if (!m_gradeMessages.isEmpty()) {
        if (m_backend->m_projectRevision == beforeRevision)
          m_errors.append(m_gradeMessages);
        else
          m_messages.append(m_gradeMessages);
      }
      if (!m_errors.isEmpty())
        fail("operation_failed", m_errors.join('\n'));
      return {
          {"ok", true},
          {"id", request.value("id")},
          {"op", op},
          {"result", result},
          {"dirty", m_backend->dirty()},
          // Only this request's own status; an older one would misdescribe it.
          {"status", m_statusChanged ? m_status : QString()},
          {"projectChanged", m_backend->m_projectRevision != beforeRevision},
          {"busy", busy()},
          {"messages", m_messages}};
    } catch (const CliError &e) {
      return {{"ok", false},
              {"id", request.value("id")},
              {"op", op},
              {"error", QVariantMap{{"code", e.code}, {"message", e.message}}},
              {"dirty", m_backend->dirty()},
              {"busy", busy()}};
    }
  }
  QVariantMap catalogue(const QString &filter = {}) const {
    QVariantList operations, properties;
    const QVariantList builtins = {
        QVariantMap{{"op", "project.new"},
                    {"args", QVariantMap{{"settings", "object (optional)"},
                                         {"discard", "bool (optional)"}}}},
        QVariantMap{{"op", "project.open"},
                    {"args", QVariantMap{{"path", "string"},
                                         {"discard", "bool (optional)"}}}},
        QVariantMap{
            {"op", "project.save"},
            {"args", QVariantMap{{"path", "string (optional after open/save)"},
                                 {"overwrite", "bool (optional)"}}}},
        QVariantMap{{"op", "timeline.create"},
                    {"args", QVariantMap{{"name", "string"},
                                         {"settings", "object (optional)"}}}},
        QVariantMap{{"op", "media.import"},
                    {"args", QVariantMap{{"paths", "array of local paths"}}}},
        QVariantMap{{"op", "render"},
                    {"args", QVariantMap{{"path", "string"},
                                         {"spec", "object (optional)"},
                                         {"preset", "preset ID (optional)"},
                                         {"overwrite", "bool (optional)"}}}},
        QVariantMap{{"op", "render.enqueue"},
                    {"args", QVariantMap{{"path", "string"},
                                         {"spec", "object (optional)"},
                                         {"preset", "preset ID (optional)"},
                                         {"overwrite", "bool (optional)"}}}},
        QVariantMap{{"op", "frame"},
                    {"args", QVariantMap{{"path", "string"},
                                         {"frame", "integer"},
                                         {"overwrite", "bool (optional)"}}}},
        QVariantMap{{"op", "stills.import"},
                    {"args", QVariantMap{{"path", "string"}}}},
        QVariantMap{{"op", "stills.export"},
                    {"args", QVariantMap{{"id", "string"},
                                         {"path", "string"},
                                         {"overwrite", "bool (optional)"}}}},
        QVariantMap{{"op", "marker.set"},
                    {"args", QVariantMap{{"frame", "integer"},
                                         {"text", "string"},
                                         {"type", "standard (default), chapter, "
                                                  "todo or done"},
                                         {"notes", "string (optional)"}}}},
        QVariantMap{{"op", "color.manage"},
                    {"args", QVariantMap{{"track", "integer"},
                                         {"index", "integer"},
                                         {"enabled", "bool (optional, true)"}}}},
        QVariantMap{{"op","beats.map"},{"args",QVariantMap{{"libraryIndex","integer"}}}},
        QVariantMap{{"op","beats.clear"},{"args",QVariantMap{{"libraryIndex","integer"}}}},
        QVariantMap{{"op","speed.inspect"},{"args",QVariantMap{{"track","integer"},{"index","integer"}}}},
        QVariantMap{{"op","speed.edit"},{"args",QVariantMap{{"track","integer"},{"index","integer"},
            {"action","set, split, join, reverse, replay, rewind, hold or normal"},{"segment","integer (optional, 0)"},{"options","object (optional)"}}}},
        QVariantMap{{"op","speed.apply"},{"args",QVariantMap{{"track","integer"},{"index","integer"},
            {"definition","visible speed map object"},{"normal","bool (optional, false)"}}}},
        QVariantMap{
            {"op", "image.save"},
            {"args", QVariantMap{{"source", "image://thumbs URL or image key"},
                                 {"path", "PNG output path"},
                                 {"overwrite", "bool (optional)"}}}},
        QVariantMap{
            {"op", "timeline.select"},
            {"args", QVariantMap{{"sequence",
                                  "timeline ID, name or index as a string"}}}},
        QVariantMap{{"op", "inspect"}, {"args", QVariantMap{}}},
        QVariantMap{
            {"op", "get"},
            {"args", QVariantMap{{"target", "object name (optional, editor)"},
                                 {"property", "string (optional, all)"}}}},
        QVariantMap{
            {"op", "set"},
            {"args", QVariantMap{{"target", "object name (optional, editor)"},
                                 {"property", "string"},
                                 {"value", "typed JSON"}}}},
        QVariantMap{
            {"op", "wait"},
            {"args",
             QVariantMap{
                 {"milliseconds",
                  "integer (optional delay for live recording/playback)"}}}},
        QVariantMap{{"op", "ops"},
                    {"args", QVariantMap{{"filter", "string (optional)"}}}},
        QVariantMap{{"op", "defaults"}, {"args", QVariantMap{}}}};
    for (const auto &v : builtins)
      if (filter.isEmpty() || v.toMap().value("op").toString().contains(
                                  filter, Qt::CaseInsensitive))
        operations << v;
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
      const auto *meta = it.value()->metaObject();
      QMap<QString, QVariantList> methods;
      for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const auto m = meta->method(i);
        if (m.access() != QMetaMethod::Public ||
            m.methodType() == QMetaMethod::Signal ||
            !permitted(it.key(), m.name()) || !supported(m.returnMetaType()) ||
            m.parameterCount() > 10)
          continue;
        QVariantList params;
        bool valid = true;
        const auto opName = it.key() + '.' + m.name();
        for (int n = 0; n < m.parameterCount(); ++n) {
          valid &= supported(m.parameterMetaType(n));
          const auto name = QString::fromUtf8(m.parameterNames()[n]);
          QVariantMap param{
              {"name", name},
              {"type", QString::fromUtf8(m.parameterTypes()[n])}};
          const auto range = parameterRanges().value(opName + ':' + name);
          if (!range.isNull())
            param["range"] = range;
          params << param;
        }
        if (valid)
          methods[QString::fromUtf8(m.name())]
              << QVariantMap{{"parameters", params},
                             {"returns", QString::fromUtf8(m.typeName())}};
      }
      for (auto m = methods.begin(); m != methods.end(); ++m) {
        const auto name = it.key() + '.' + m.key();
        if (filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive))
          operations << QVariantMap{{"op", name}, {"overloads", m.value()}};
      }
      for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const auto p = meta->property(i);
        if (!supported(p.metaType()) || !shownProperty(p.name()))
          continue;
        if (filter.isEmpty() || QString(it.key() + '.' + p.name())
                                    .contains(filter, Qt::CaseInsensitive))
          properties << QVariantMap{
              {"target", it.key()},
              {"name", p.name()},
              {"type", p.typeName()},
              {"writable",
               p.isWritable() && QString(p.name()) != "afterCompletion"}};
      }
    }
    return {{"apiVersion", 1},
            {"operations", operations},
            {"properties", properties},
            {"objects", m_objects.keys()},
            {"units", "Timeline positions and inclusive in/out ranges use "
                      "frames in the active timeline rate. Indices are zero "
                      "based; refresh inspect after structural edits."}};
  }

private:
  std::unique_ptr<ThumbnailProvider> m_thumbs; // outlives Backend callbacks
  std::unique_ptr<Backend> m_backend;
  QMap<QString, QObject *> m_objects;
  int m_timeout;
  bool m_oneShot;
  QStringList m_errors, m_messages, m_gradeMessages;
  QString m_path, m_lastProgress;
  QByteArray m_hash;
  QSet<int> m_earlierJobs;
  bool m_progress = false, m_statusChanged = false;
  // The status this request set, and the one before the latest change.
  QString m_status, m_op;
  std::pair<bool, QString> m_statusBefore;
  int m_replies = 0;
  void error(const QString &s) {
    if (!s.isEmpty() && !m_errors.contains(s))
      m_errors << s;
  }
  [[noreturn]] void refuseExisting(const QString &path) const {
    fail("output_exists", path + " exists. Pass " +
                                (m_oneShot ? "--overwrite" : "overwrite: true") +
                                " to replace it.");
  }
  static bool waiting(const QVariantMap &job) {
    return QStringList{"queued", "rendering", "paused"}.contains(
        job.value("status").toString());
  }
  QObject *object(const QString &name) const {
    auto *o = m_objects.value(name);
    if (!o)
      fail("unknown_object", "Unknown object: " + name);
    return o;
  }
  // The editor owns the list of its asynchronous work; see
  // Backend::hasPendingWork.
  bool busy() const { return m_backend->hasPendingWork(); }
  void delay(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  }
  void fence(QDeadlineTimer &deadline) {
    if (deadline.hasExpired())
      fail("timeout", "The operation timed out. Inspect jobs before retrying; "
                      "work may still be running.");
    // The worker answers when it has run everything queued before this, and
    // the answer ends the wait at once. (Looking every 10 ms instead made
    // every command, however small, take 40 ms.)
    const auto done = std::make_shared<std::atomic_bool>(false);
    const auto waiting = std::make_shared<QEventLoop>();
    QMetaObject::invokeMethod(
        m_backend->m_worker,
        [this, done, waiting] {
          QMetaObject::invokeMethod(
              this,
              [done, waiting] {
                done->store(true);
                waiting->quit();
              },
              Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    while (!done->load()) {
      if (deadline.hasExpired())
        fail("timeout", "The editor worker did not finish before the timeout.");
      QTimer::singleShot(int(qMin<qint64>(250, deadline.remainingTime())), waiting.get(), &QEventLoop::quit);
      waiting->exec();
    }
  }
  void settle(bool wait) {
    QDeadlineTimer deadline(m_timeout);
    // Each round drains worker commands and their main-thread replies,
    // including follow-up commands posted by import/project callbacks.
    int quiet = 0;
    do {
      fence(deadline);
      QCoreApplication::sendPostedEvents();
      if (!wait || !busy())
        ++quiet;
      else
        quiet = 0;
      if (quiet < 3)
        delay(5);
    } while (quiet < 3);
  }
  QVariantMap properties(QObject *o) const {
    QVariantMap result;
    const auto *meta = o->metaObject();
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
      const auto p = meta->property(i);
      if (p.isReadable() && supported(p.metaType()) && shownProperty(p.name()))
        result[p.name()] = p.read(o);
    }
    return result;
  }
  QVariantMap inspect() const {
    auto result = properties(m_backend.get());
    result["projectPath"] = m_path;
    result["sha256"] = QString::fromLatin1(m_hash.toHex());
    QVariantMap related;
    for (auto i = m_objects.begin(); i != m_objects.end(); ++i)
      if (i.key() != "editor")
        related[i.key()] = properties(i.value());
    result["objects"] = related;
    // Every timeline with its own format. A media-only project holds one
    // unconfigured placeholder, which is not a timeline and is left out.
    QVariantList sequences;
    const auto &stored = m_backend->m_sequences;
    for (int i = 0; i < stored.size(); ++i) {
      const auto row = stored[i].toMap();
      if (!row.value("configured", true).toBool())
        continue;
      auto settings = row.value("settings").toMap();
      if (settings.isEmpty() || i == m_backend->activeSequence())
        settings = m_backend->projectSettings();
      sequences << QVariantMap{{"index", i},
                               {"id", row.value("id")},
                               {"name", row.value("name")},
                               {"active", i == m_backend->activeSequence()},
                               {"settings", settings},
                               {"entries", m_backend->sequenceEntries(i)}};
    }
    result["sequences"] = sequences;
    result["images"] =
        QVariantMap{{"program", !m_backend->programFrame().isNull()},
                    {"source", !m_backend->sourceFrame().isNull()},
                    {"matte", !m_backend->colorMatteFrame().isNull()},
                    {"before", !m_backend->beforeFrame().isNull()},
                    {"trim.incoming", !m_backend->twoUpIncoming().isNull()},
                    {"trim.outgoing", !m_backend->twoUpOutgoing().isNull()}};
    return result;
  }
  void validateAddress(const QString &op, const QVariantMap &args) const {
    // Every queue operation that takes a job names it by id; one that is not
    // in the queue would otherwise do nothing and report success.
    if (op.startsWith("queue.") && args.contains("id")) {
      const int id =
          converted(args["id"], QMetaType::fromType<int>(), "id").toInt();
      if (m_backend->renderQueue()->job(id).isEmpty())
        fail("not_found", QString("No render job %1 in the queue. Its jobs are "
                                  "the queue target's jobs property.")
                              .arg(id));
    }
    if (!op.startsWith("editor."))
      return;
    const QSet<QString> placements = {"editor.appendClipAt",
                                      "editor.dropClipAt",
                                      "editor.insertClipAt",
                                      "editor.overwriteClipAt",
                                      "editor.connectClipAt",
                                      "editor.connectAudioClipAt",
                                      "editor.addTitle",
                                      "editor.addTitleAt",
                                      "editor.addGenerator",
                                      "editor.addGeneratorAt",
                                      "editor.insertGapAtPlayhead",
                                      "editor.pasteClips",
                                      "editor.pasteClip",
                                      "editor.pasteClipboardSnapshot",
                                      "editor.duplicateClips",
                                      "editor.placeEntryAt",
                                      "editor.importCaptions"};
    if (placements.contains(op) && !m_backend->hasTimeline())
      fail("timeline_required", "Create a timeline with timeline.create and "
                                "its settings before placing clips.");
    const auto bounded = [&](const QString &key, int count) {
      if (!args.contains(key))
        return;
      const auto n =
          converted(args[key], QMetaType::fromType<int>(), key).toInt();
      if (n < 0 || n >= count)
        fail("invalid_address",
             key + " is outside the current project. Refresh inspect.");
    };
    bounded("libraryIndex", m_backend->library().size());
    bounded("timelineIndex", m_backend->timeline().size());
    bounded("connectedIndex", m_backend->connected().size());
    if (QStringList{"editor.switchSequence", "editor.renameSequence",
                    "editor.duplicateSequence", "editor.closeSequence",
                    "editor.nestSequence", "editor.sequenceEntries",
                    "editor.collapseMulticam", "editor.multicamPiecesIn"}
            .contains(op))
      bounded("index", m_backend->sequences().size());
    if (args.contains("track") && args.contains("index") &&
        op != "editor.markerEditTarget") {
      const int track =
          converted(args["track"], QMetaType::fromType<int>(), "track").toInt();
      const int index =
          converted(args["index"], QMetaType::fromType<int>(), "index").toInt();
      if (m_backend->entryOn(track, index).isEmpty())
        fail("invalid_address",
             "No clip at this track/index. Refresh inspect.");
      if (!QStringList{"editor.retimeTarget", "editor.markerEditTarget", "editor.audioEditTarget"}
               .contains(op) &&
          m_backend->tracks().value(track).toMap().value("locked").toBool())
        fail("track_locked", "Unlock the track before editing it.");
    }
  }
  QVariant invoke(const QString &op, const QVariantMap &args) {
    validateAddress(op, args);
    checkRanges(op, args);
    if (op == "editor.setMarkerInfo" &&
        !MarkerEdit::types().contains(args.value("type").toString()))
      fail("invalid_argument",
           "type must be one of: " + MarkerEdit::types().join(", ") + '.');
    const auto dot = op.indexOf('.');
    if (dot <= 0)
      fail("unknown_operation", "Unknown operation: " + op);
    auto target = op.left(dot), name = op.mid(dot + 1);
    resolveMoved(target, name);
    QObject *o = object(target);
    if (!permitted(target, name))
      fail("unavailable_operation",
           "This GUI-only/internal operation is unavailable. See ops for CLI "
           "alternatives.");
    QVariantMap call = args;
    if (op == "editor.exportTimeline") {
      // Like every other output: an existing file, under the name the
      // format's suffix gives it, is replaced only on request.
      const bool overwrite = call.contains("overwrite") &&
          converted(call.take("overwrite"), QMetaType::fromType<bool>(), "overwrite").toBool();
      QString path = call.contains("url")
          ? converted(call.value("url"), QMetaType::fromType<QUrl>(), "url").toUrl().toLocalFile() : QString();
      const QString format = call.value("format").toString();
      const QString suffix = format == "xmeml" ? QStringLiteral("xml") : format;
      if (!path.isEmpty() && !suffix.isEmpty() && !path.endsWith('.' + suffix, Qt::CaseInsensitive))
        path += '.' + suffix;
      if (!overwrite && !path.isEmpty() && QFileInfo::exists(path))
        refuseExisting(path);
    }
    const auto *meta = o->metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
      const auto m = meta->method(i);
      if (m.name() != name.toUtf8() || m.access() != QMetaMethod::Public ||
          m.methodType() == QMetaMethod::Signal ||
          m.parameterCount() != call.size() || m.parameterCount() > 10 ||
          !supported(m.returnMetaType()))
        continue;
      const auto names = m.parameterNames(), types = m.parameterTypes();
      bool match = true;
      for (const auto &n : names)
        match &= call.contains(QString::fromUtf8(n));
      if (!match)
        continue;
      QVariant values[10];
      QGenericArgument passed[10];
      for (int n = 0; n < m.parameterCount(); ++n) {
        const auto type = m.parameterMetaType(n);
        if (!supported(type))
          fail("unsupported_type", type.name());
        values[n] = converted(call.value(QString::fromUtf8(names[n])), type,
                              QString::fromUtf8(names[n]));
        passed[n] = QGenericArgument(types[n].constData(),
                                     type.id() == QMetaType::QVariant
                                         ? &values[n]
                                         : values[n].constData());
      }
      QVariant result(m.returnMetaType());
      QGenericReturnArgument returned;
      if (m.returnMetaType().id() != QMetaType::Void)
        returned = QGenericReturnArgument(
            m.typeName(), m.returnMetaType().id() == QMetaType::QVariant
                              ? &result
                              : result.data());
      if (!m.invoke(o, Qt::DirectConnection, returned, passed[0], passed[1],
                    passed[2], passed[3], passed[4], passed[5], passed[6],
                    passed[7], passed[8], passed[9]))
        fail("invoke_failed", op);
      return result;
    }
    fail("invalid_operation",
         "Unknown operation or argument names/count. Inspect ops for " + op);
  }
  void save(const QVariantMap &args) {
    settle(true);
    const auto path = absolute(args.value("path", m_path).toString());
    const bool same = path == m_path;
    const auto previousPath = m_backend->m_projectPath;
    const bool previousDirty = m_backend->dirty();
    bool published = false;
    const auto restore = qScopeGuard([&] {
      if (!published) {
        m_backend->m_projectPath = previousPath;
        m_backend->setDirty(previousDirty);
        emit m_backend->projectChanged();
      }
    });
    QLockFile lock(path + ".agent.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock())
      fail(lock.error() == QLockFile::LockFailedError ? "project_locked"
                                                      : "write_failed",
           lock.error() == QLockFile::LockFailedError
               ? "Another CLI process is saving this project."
               : "The destination folder is missing or not writable.");
    const auto before = fingerprint(path);
    if (same && before != m_hash)
      fail("project_changed",
           "The project changed on disk. Reopen it before saving.");
    if (!same && !before.isEmpty() && !args.value("overwrite", false).toBool())
      refuseExisting(path);
    // Stage a hidden file in the destination folder itself, so the saved
    // document's root is the project's folder, exactly as a desktop save.
    // The name is reserved and then freed: the editor keeps a backup of any
    // file it saves over, and an empty placeholder is not a previous version.
    QString staged;
    {
      QTemporaryFile stage(QFileInfo(path).absolutePath() +
                           "/.omaedit-save-XXXXXX.omaedit");
      if (!stage.open())
        fail("write_failed",
             "Cannot create a temporary save in the destination folder.");
      staged = stage.fileName();
    }
    const auto discard = qScopeGuard([&] {
      QFile::remove(staged);
      m_backend->removeRecentProject(staged);
    });
    m_backend->requestProjectSave(staged);
    settle(true);
    if (!m_errors.isEmpty())
      fail("save_failed", m_errors.join('\n'));
    QFile source(staged);
    if (!source.open(QIODevice::ReadOnly))
      fail("save_failed", "The editor did not produce a project file.");
    if (fingerprint(path) != before)
      fail("project_changed",
           "The destination changed while saving. It has been left untouched.");
    QSaveFile dest(path);
    if (!dest.open(QIODevice::WriteOnly))
      fail("write_failed", dest.errorString());
    while (!source.atEnd()) {
      const auto bytes = source.read(1024 * 1024);
      if (bytes.isEmpty() && source.error() != QFile::NoError)
        fail("read_failed", source.errorString());
      if (dest.write(bytes) != bytes.size())
        fail("write_failed", dest.errorString());
    }
    if (!before.isEmpty()) {
      QString error;
      ProjectSafety::makeBackup(path, &error);
      if (!error.isEmpty())
        fail("backup_failed", error);
    }
    if (fingerprint(path) != before)
      fail("project_changed",
           "The destination changed while saving. It has been left untouched.");
    if (!dest.commit())
      fail("write_failed", dest.errorString());
    ProjectSafety::pruneBackups(path, m_backend->backupLimit());
    published = true;
    m_path = path;
    m_hash = fingerprint(path);
    m_backend->m_projectPath = path;
    m_backend->setDirty(false);
    emit m_backend->projectChanged();
    // The editor described its save under the staged name.
    m_backend->removeRecentProject(staged);
    m_backend->addRecent(path);
    m_backend->refreshProjectSafety();
    m_backend->setStatus(Backend::tr("Saved %1").arg(QFileInfo(path).fileName()));
  }
  QVariant execute(const QString &op, const QVariantMap &args, bool wait) {
    const QMap<QString, QStringList> keys = {
        {"image.save", {"source", "path", "overwrite"}},
        {"timeline.select", {"sequence"}},
        {"ops", {"filter"}},
        {"inspect", {}},
        {"defaults", {}},
        {"get", {"target", "property"}},
        {"set", {"target", "property", "value"}},
        {"wait", {"milliseconds"}},
        {"project.new", {"settings", "discard"}},
        {"project.open", {"path", "discard"}},
        {"project.save", {"path", "overwrite"}},
        {"timeline.create", {"name", "settings"}},
        {"media.import", {"paths"}},
        {"frame", {"path", "frame", "overwrite"}},
        {"render", {"path", "spec", "preset", "overwrite"}},
        {"render.enqueue", {"path", "spec", "preset", "overwrite"}},
        {"stills.import", {"path"}},
        {"stills.export", {"id", "path", "overwrite"}},
        {"marker.set", {"frame", "text", "type", "notes"}},
        {"beats.map", {"libraryIndex"}},
        {"beats.clear", {"libraryIndex"}},
        {"speed.inspect", {"track","index"}},
        {"speed.edit", {"track","index","action","segment","options"}},
        {"speed.apply", {"track","index","definition","normal"}},
        {"color.manage", {"track", "index", "enabled"}}};
    if (keys.contains(op)) {
      for (auto i = args.begin(); i != args.end(); ++i)
        if (!keys[op].contains(i.key()))
          fail("invalid_argument", "Unknown argument: " + i.key());
      for (const auto &key : QStringList{"overwrite", "discard"})
        if (args.contains(key))
          converted(args[key], QMetaType::fromType<bool>(), key);
      for (const auto &key : QStringList{"settings", "spec"})
        if (args.contains(key))
          converted(args[key], QMetaType::fromType<QVariantMap>(), key);
    }
    // The beats of a library clip's music: found (this waits for it), written
    // onto the clip as `beatMap`, and given back as {found, bpm, beats, bar}; the
    // beats are seconds of the file's own sound, and the bar is {beats: how
    // many it has, first: which of the beats is the first beat of one}. Sound with no steady beat is
    // `found: false`, not an error.
    if (op == "beats.map" || op == "beats.clear") {
      const int index = converted(args.value("libraryIndex"), QMetaType::fromType<int>(), "libraryIndex").toInt();
      if (index < 0 || index >= m_backend->library().size())
        fail("invalid_address", "libraryIndex is outside the current project. Refresh inspect.");
      const auto path = m_backend->library().at(index).toMap().value("path").toString();
      if (op == "beats.clear") {
        m_backend->clearBeatMap(index);
        settle(true);
        return QVariantMap{{"cleared", true}};
      }
      bool done = false, found = false;
      QString message;
      const auto link = QObject::connect(m_backend.get(), &Backend::beatMapFinished, m_backend.get(),
                                         [&](const QString &mapped, bool has, const QString &said) {
        if (mapped == path) { done = true; found = has; message = said; }
      });
      const auto unlink = qScopeGuard([&] { QObject::disconnect(link); });
      if (!m_backend->mapBeats(index))
        fail("operation_failed", m_backend->status());
      QDeadlineTimer deadline(m_timeout);
      while (!done) {
        if (deadline.hasExpired()) {
          m_backend->cancelBeatMap(path);
          fail("timeout", "The beats were not mapped in time; nothing was stored.");
        }
        delay(5);
      }
      settle(true);
      const auto map = m_backend->library().value(index).toMap().value("beatMap").toMap();
      return QVariantMap{{"found", found}, {"message", message}, {"bpm", found ? map.value("bpm") : QVariant(0)},
                         {"beats", found ? map.value("beats") : QVariant(QVariantList{})},
                         {"bar", found ? map.value("bar") : QVariant(QVariantMap{})}};
    }
    if(op=="speed.inspect" || op=="speed.edit" || op=="speed.apply") {
      const int track=converted(args.value("track"),QMetaType::fromType<int>(),"track").toInt();
      const int index=converted(args.value("index"),QMetaType::fromType<int>(),"index").toInt();
      settle(true);
      const auto target=m_backend->retimeTarget(track,index);
      if(target.isEmpty())fail("invalid_address","No clip at this track/index. Refresh inspect.");
      QVariantMap state;bool prepared=false;int preparation=0;
      const auto connection=connect(m_backend.get(),&Backend::speedRampPrepared,this,[&](int id,const QVariantMap &value){if(id==preparation){state=value;prepared=true;}});
      const auto disconnectPrepare=qScopeGuard([&]{disconnect(connection);});
      preparation=m_backend->prepareSpeedRamp(target);settle(true);
      if(!prepared)fail("timeout","The speed draft was not returned by the worker.");
      if(!state.value("valid").toBool())fail("operation_failed",state.value("error").toString());
      state.remove("parent");
      if(op=="speed.inspect")return state;
      bool normal=false;QVariantMap definition;
      if(op=="speed.edit") {
        const auto action=converted(args.value("action"),QMetaType::fromType<QString>(),"action").toString();
        const int segment=converted(args.value("segment",0),QMetaType::fromType<int>(),"segment").toInt();
        const auto options=converted(args.value("options",QVariantMap{}),QMetaType::fromType<QVariantMap>(),"options").toMap();
        const auto result=SpeedRampEdit::edit(state.value("definition").toMap(),action,segment,options);
        if(!result.value("valid").toBool())fail("invalid_argument",result.value("error").toString());
        definition=result.value("definition").toMap();normal=action=="normal";
      } else {
        definition=converted(args.value("definition"),QMetaType::fromType<QVariantMap>(),"definition").toMap();
        normal=converted(args.value("normal",false),QMetaType::fromType<bool>(),"normal").toBool();
      }
      bool completed=false,changed=false;QString problem;int application=0;
      const auto applied=connect(m_backend.get(),&Backend::retimeFinished,this,[&](int id,bool didChange,const QString &error){if(id==application){completed=true;changed=didChange;problem=error;}});
      const auto disconnectApply=qScopeGuard([&]{disconnect(applied);});
      application=m_backend->applySpeedRamp(target,definition,normal);settle(true);
      if(!completed)fail("timeout","The speed edit was not completed by the worker.");
      if(!problem.isEmpty())fail("operation_failed",problem);
      return QVariantMap{{"changed",changed},{"entry",m_backend->entryOn(track,index)}};
    }
    if (op == "ops")
      return catalogue(args.value("filter").toString());
    if (op == "inspect") {
      settle(wait);
      return inspect();
    }
    if (op == "defaults")
      return QVariantMap{{"project", m_backend->newProjectDefaults()},
                         {"timeline", m_backend->newTimelineDefaults()},
                         {"grade", m_backend->defaultGrade()},
                         {"gradeNode", m_backend->defaultGradeNode("Node")},
                         {"colourManagement", ColourPipeline::defaults()},
                         {"render", ExportSpec::normalized({})},
                         {"audioEffects", m_backend->mixEffects()},
                         {"videoEffects", m_backend->videoEffectCatalogue()}};
    if (op == "get") {
      auto target = args.value("target", "editor").toString();
      if (!args.contains("property"))
        return properties(object(target));
      auto name = requiredString(args, "property");
      resolveMoved(target, name);
      auto *o = object(target);
      const auto index = o->metaObject()->indexOfProperty(name.toUtf8());
      if (index < 0 || !supported(o->metaObject()->property(index).metaType()) ||
          !shownProperty(name))
        fail("unknown_property", name);
      return o->property(name.toUtf8());
    }
    if (op == "set") {
      auto target = args.value("target", "editor").toString();
      auto name = requiredString(args, "property");
      resolveMoved(target, name);
      auto *o = object(target);
      const auto index = o->metaObject()->indexOfProperty(name.toUtf8());
      if (index < 0 || !shownProperty(name))
        fail("unknown_property", name);
      const auto p = o->metaObject()->property(index);
      if (!p.isWritable() || !supported(p.metaType()) ||
          name == "afterCompletion")
        fail("read_only", name);
      if (!args.contains("value"))
        fail("invalid_argument", "value is required.");
      if (!p.write(o, converted(args["value"], p.metaType(), name)))
        fail("write_failed", name);
      settle(wait);
      return p.read(o);
    }
    if (op == "wait") {
      const int ms = converted(args.value("milliseconds", 0),
                               QMetaType::fromType<int>(), "milliseconds")
                         .toInt();
      if (ms < 0 || ms > m_timeout)
        fail("invalid_argument",
             "milliseconds must be between zero and the timeout.");
      if (ms)
        delay(ms);
      settle(true);
      return inspect();
    }
    if (op == "project.new") {
      if (m_backend->dirty() && !args.value("discard", false).toBool())
        fail("unsaved_changes", "Save first or set discard: true.");
      auto settings = m_backend->newProjectDefaults();
      const auto patch = args.value("settings").toMap();
      knownKeys(patch, settings.keys(), "settings");
      for (auto i = patch.begin(); i != patch.end(); ++i)
        settings[i.key()] = i.value();
      // A refused creation leaves the open project open: it keeps its path
      // and fingerprint, so a later save is still checked against its file.
      int request = -1;
      bool refused = false;
      QString problem;
      const auto answer = connect(m_backend.get(), &Backend::projectCreateFinished, this,
                                  [&](int id, bool ok, const QString &why) {
                                    if (id == request && !ok) { refused = true; problem = why; }
                                  });
      request = m_backend->createProject(m_backend->creationContext(), settings);
      settle(true);
      disconnect(answer);
      if (refused)
        fail("operation_failed", problem);
      m_path.clear();
      m_hash.clear();
      return inspect();
    }
    if (op == "project.open") {
      if (m_backend->dirty() && !args.value("discard", false).toBool())
        fail("unsaved_changes", "Save first or set discard: true.");
      const auto path = absolute(requiredString(args, "path"));
      if (!QFileInfo(path).isFile())
        fail("not_found", path);
      const auto hash = fingerprint(path);
      m_backend->loadProject(QUrl::fromLocalFile(path));
      settle(true);
      if (m_backend->projectPath() != path)
        fail("open_failed", m_backend->status());
      if (fingerprint(path) != hash)
        fail("project_changed",
             "The project changed while opening. Retry project.open.");
      m_path = path;
      m_hash = hash;
      return inspect();
    }
    if (op == "project.save") {
      save(args);
      return QVariantMap{{"path", m_path},
                         {"sha256", QString::fromLatin1(m_hash.toHex())}};
    }
    if (op == "timeline.create") {
      auto settings = m_backend->newTimelineDefaults();
      const auto patch = args.value("settings").toMap();
      knownKeys(patch, settings.keys(), "settings");
      for (auto i = patch.begin(); i != patch.end(); ++i)
        settings[i.key()] = i.value();
      m_backend->createSequence(m_backend->creationContext(),
                                requiredString(args, "name"), settings);
      settle(true);
      return m_backend->sequences();
    }
    if (op == "media.import") {
      const auto paths = converted(args.value("paths"),
                                   QMetaType::fromType<QStringList>(), "paths")
                             .toStringList();
      if (paths.isEmpty())
        fail("invalid_argument", "paths must not be empty.");
      QList<QUrl> urls;
      for (const auto &p : paths) {
        const auto path = absolute(p);
        if (ProjectFile::isProject(path))
          fail("invalid_argument", "Use project.open to open a project file.");
        if (!QFileInfo::exists(path))
          fail("not_found", path);
        urls << QUrl::fromLocalFile(path);
      }
      m_backend->importUrls(urls);
      settle(wait);
      return m_backend->library();
    }
    if (op == "stills.import") {
      QString error;
      const auto id = m_backend->stills()->importStill(
          absolute(requiredString(args, "path")), &error);
      if (id.isEmpty())
        fail("import_failed", error);
      return id;
    }
    if (op == "stills.export") {
      const auto path = absolute(requiredString(args, "path"));
      if (QFileInfo::exists(path) && !args.value("overwrite", false).toBool())
        refuseExisting(path);
      QString error;
      if (!m_backend->stills()->exportStill(requiredString(args, "id"), path,
                                            &error))
        fail("export_failed", error);
      return QVariantMap{{"path", path}};
    }
    if (op == "color.manage") {
      // The Colour Management dialog's OpenColorIO choice: the bundled
      // configuration and the suite's scene-linear working space. A managed
      // grade is also what gives a frame the light an OpenEXR file holds.
      for (const auto &key : {"track", "index"})
        if (!args.contains(key))
          fail("invalid_argument", QString(key) + " is required.");
      const bool enabled =
          converted(args.value("enabled", true), QMetaType::fromType<bool>(),
                    "enabled")
              .toBool();
      validateAddress("editor.setGrade", args);
      const int track = args["track"].toInt(), index = args["index"].toInt();
      auto grade = m_backend->entryOn(track, index).value("grade").toMap();
      if (grade.isEmpty())
        grade = m_backend->defaultGrade();
      auto management = grade.value("management").toMap();
      if (enabled) {
        const auto bundled = ColourPipeline::defaults();
        if (const auto problem =
                ColourPipeline::info().value("error").toString();
            !problem.isEmpty())
          fail("operation_failed", problem);
        for (auto i = bundled.begin(); i != bundled.end(); ++i)
          if (!management.contains(i.key()) ||
              QStringList{"engine", "config", "configHash", "working",
                          "workingGamma"}
                  .contains(i.key()))
            management[i.key()] = i.value();
      }
      management["enabled"] = enabled;
      grade["management"] = management;
      m_backend->setGrade(track, index, grade);
      settle(true);
      if (!m_errors.isEmpty())
        fail("operation_failed", m_errors.join('\n'));
      return m_backend->entryOn(track, index).value("grade").toMap().value(
          "management");
    }
    if (op == "marker.set") {
      const int frame =
          converted(args.value("frame"), QMetaType::fromType<int>(), "frame")
              .toInt();
      if (frame < 0 || frame >= m_backend->duration())
        fail("invalid_argument", "Frame is outside the active timeline.");
      // The marker types the editor's marker dialog accepts.
      const QStringList &types = MarkerEdit::types();
      const auto type =
          args.contains("type") ? requiredString(args, "type") : types.first();
      if (!types.contains(type))
        fail("invalid_argument",
             "type must be one of: " + types.join(", ") + '.');
      // Every argument is read before the timeline is touched: a request
      // that is refused leaves no empty marker behind.
      const auto text = requiredString(args, "text");
      bool found = false;
      for (const auto &m : m_backend->markers())
        found |= m.toMap().value("frame").toInt() == frame;
      if (!found) {
        m_backend->viewTimeline();
        m_backend->seek(frame);
        settle(true);
        m_backend->toggleMarker();
        settle(true);
      }
      m_backend->setMarkerInfo(frame, text, type,
                               args.value("notes").toString());
      settle(true);
      return m_backend->markers();
    }
    if (op == "image.save") {
      const auto source = requiredString(args, "source");
      const auto url = QUrl(source);
      const auto key =
          source.startsWith("image://thumbs/")
              ? url.path().mid(1) +
                    (url.hasQuery() ? "?" + url.query() : QString())
              : source;
      const auto path = absolute(requiredString(args, "path"));
      if (QFileInfo::exists(path) && !args.value("overwrite", false).toBool())
        refuseExisting(path);
      settle(true);
      QSize size;
      const auto image =
          source == "program"         ? m_backend->programFrame()
          : source == "source"        ? m_backend->sourceFrame()
          : source == "matte"         ? m_backend->colorMatteFrame()
          : source == "before"        ? m_backend->beforeFrame()
          : source == "trim.incoming" ? m_backend->twoUpIncoming()
          : source == "trim.outgoing" ? m_backend->twoUpOutgoing()
                                      : m_thumbs->requestImage(key, &size, {});
      if (image.isNull())
        fail("not_found", "The requested image is unavailable or has expired.");
      if (QFileInfo(path).suffix().toLower() != "png")
        fail("invalid_argument", "Use .png for provider images.");
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") ||
          !file.commit())
        fail("write_failed", file.errorString());
      return QVariantMap{
          {"path", path}, {"width", image.width()}, {"height", image.height()}};
    }
    if (op == "timeline.select") {
      const auto selector = requiredString(args, "sequence");
      int index = -1;
      const auto seqs = m_backend->sequences();
      for (int i = 0; i < seqs.size(); ++i) {
        const auto row = seqs[i].toMap();
        if (row.value("id").toString() == selector ||
            row.value("name").toString() == selector) {
          index = i;
          break;
        }
      }
      if (index < 0) {
        bool ok = false;
        index = selector.toInt(&ok);
        if (!ok)
          index = -1;
      }
      if (index < 0 || index >= seqs.size())
        fail("invalid_address", "Unknown timeline: " + selector);
      m_backend->switchSequence(index);
      settle(true);
      return seqs[index];
    }
    if (op == "frame") {
      const auto path = absolute(requiredString(args, "path"));
      if (QFileInfo::exists(path) && !args.value("overwrite", false).toBool())
        refuseExisting(path);
      const int frame =
          converted(args.value("frame", 0), QMetaType::fromType<int>(), "frame")
              .toInt();
      if (frame < 0 || frame >= m_backend->duration())
        fail("invalid_argument", "Frame is outside the active timeline.");
      m_backend->viewTimeline();
      m_backend->seek(frame);
      settle(true);
      if (const QString problem = m_backend->frameExportProblem(path); !problem.isEmpty())
        fail("invalid_argument",
             QFileInfo(path).suffix().compare("exr", Qt::CaseInsensitive)
                 ? problem
                 : QStringLiteral("OpenEXR frames need colour management. "
                                  "Turn it on for the clip at this frame "
                                  "with color.manage, or save a PNG."));
      // The editor says why a frame was refused (an OpenEXR frame needs
      // OpenColorIO colour management, say); pass that on.
      QString refusal;
      const auto connection = QObject::connect(
          m_backend.get(), &Backend::frameExported,
          [this, &refusal](bool ok, const QString &) {
            if (!ok)
              refusal = m_backend->status();
          });
      m_backend->saveProgramFrame(path);
      settle(true);
      QObject::disconnect(connection);
      if (!QFileInfo(path).isFile())
        fail("write_failed", refusal.isEmpty() ? QStringLiteral("No frame image was written.")
                                               : refusal);
      return QVariantMap{{"path", path}, {"frame", frame}};
    }
    if (op == "render" || op == "render.enqueue") {
      const auto path = absolute(requiredString(args, "path"));
      knownKeys(args.value("spec").toMap(), renderKeys(), "spec");
      QVariantMap spec;
      const auto preset = args.value("preset").toString();
      if (!preset.isEmpty()) {
        const auto p = m_backend->renderPresets()->preset(preset);
        if (p.isEmpty())
          fail("unknown_preset", preset);
        spec = p.value("spec").toMap();
      }
      const auto patch = args.value("spec").toMap();
      for (auto i = patch.begin(); i != patch.end(); ++i)
        spec[i.key()] = i.value();
      spec = ExportSpec::normalized(spec);
      QString err;
      const auto jobs = ExportSpec::jobs(
          m_backend->projectName(), path, spec, m_backend->duration(),
          m_backend->fps(), m_backend->audioRenderTracks(), &err,
          m_backend->audioCategoriesInUse()); // (stems by category are jobs of this render too)
      if (!err.isEmpty())
        fail("invalid_render", err);
      QStringList approved;
      const bool overwrite = args.value("overwrite", false).toBool();
      for (const auto &v : jobs) {
        auto j = v.toMap();
        for (const auto &p : ExportSpec::outputPaths(j.value("path").toString(),
                                                     j.value("spec").toMap())) {
          if (!overwrite && QFileInfo::exists(p))
            refuseExisting(p);
          approved << ExportSpec::outputKey(p);
        }
      }
      spec["overwritePaths"] = overwrite ? approved : QStringList();
      const auto checks = m_backend->validateRender(spec, path);
      for (const auto &v : checks)
        if (v.toMap().value("level") == "error")
          fail("invalid_render", v.toMap().value("text").toString());
      const int first = m_backend->enqueueRender(m_backend->projectName(), path,
                                                 spec, preset);
      if (first < 0)
        fail("render_failed", m_backend->status());
      // A render runs exactly the jobs it created. Jobs queued earlier, in
      // this session or one that ended, wait for queue.renderAll.
      QList<int> created;
      QVariantList createdIds;
      for (int id = first; id < first + jobs.size(); ++id) {
        created << id;
        createdIds << id;
      }
      if (op == "render")
        m_backend->renderQueue()->renderJobs(created);
      settle(wait);
      return QVariantMap{{"firstJob", first},
                         {"jobIds", createdIds},
                         {"jobs", m_backend->renderQueue()->jobs()},
                         {"validation", checks}};
    }
    const auto result = invoke(op, args);
    settle(wait);
    return outcome(op, args, result);
  }
  // Why an operation was refused: its own failure reports first, then the
  // status it set, then a plain statement naming it.
  QString refusal(const QString &op, const QString &what) const {
    if (!m_errors.isEmpty())
      return m_errors.join('\n');
    if (m_statusChanged && !m_status.isEmpty())
      return m_status;
    return op + ' ' + what;
  }
  // Engine methods report success in different ways; each is classified by
  // what its return value means rather than by the text it carries.
  QVariant outcome(const QString &op, const QVariantMap &args,
                   const QVariant &result) {
    // A description of the problem, empty on success.
    static const QSet<QString> problems = {
        "editor.setClipAttributes", "editor.setMediaRaw", "editor.importTimeline",
        "editor.exportTimeline", "editor.saveStorageSettings",
        "editor.splitPhotoshopLayers", "sounds.setCategory"};
    // The ID or path of what was created or started, empty when refused.
    // Media tasks then report their own failure when they finish.
    static const QSet<QString> identifiers = {
        "editor.relinkMedia",      "editor.attachMediaProxy",
        "editor.exportMediaCsv",   "editor.importMediaCsv",
        "editor.startMediaTask",   "editor.grabGradeStill",
        "editor.saveNodePreset",   "editor.stopRecording",
        "media.create",            "media.watch",
        "media.registerProvider",  "mediaTasks.submit",
        "mediaTasks.retry",        "presets.savePreset",
        "presets.duplicatePreset", "recorder.start",
        "recorder.stop"};
    // A new job's ID, negative when refused.
    static const QSet<QString> jobs = {"queue.duplicateJob"};
    // How many captions an edit changes, negative when refused.
    static const QSet<QString> counts = {
        "editor.replaceCaptionText", "editor.snapCaptionsToCuts", "editor.extendCaptionsToNext",
        "editor.closeCaptionGaps", "editor.shiftCaptions"};
    // Void methods whose outcome arrives as a worker reply; a refusal before
    // the worker is asked leaves only a status message.
    static const QSet<QString> replies = {
        "editor.pasteColor", "editor.autoBalance", "editor.setNodeLut"};
    // True/false answers to a question, not success flags.
    static const QSet<QString> questions = {"editor.clipIsUsed",
                                            "editor.timelineFormatAvailable",
                                            "presets.nameTaken", "media.matches"};
    if (problems.contains(op) && !result.toString().isEmpty())
      fail("operation_failed", result.toString());
    if (identifiers.contains(op) && result.toString().isEmpty())
      fail("operation_failed", refusal(op, "was refused."));
    if (identifiers.contains(op))
      taskOutcome(result.toString());
    if (jobs.contains(op) && result.toInt() < 0)
      fail("operation_failed", refusal(op, "was refused."));
    if (counts.contains(op) && result.toInt() < 0)
      fail("operation_failed", refusal(op, "was refused."));
    // The files written, none when refused.
    if (op == "editor.exportCaptionFiles" && result.toStringList().isEmpty())
      fail("operation_failed", refusal(op, "wrote no file."));
    if (replies.contains(op) && !m_replies && m_errors.isEmpty())
      fail("operation_failed", refusal(op, "made no change."));
    if ((op == "editor.pasteClipboardSnapshot" || op == "editor.duplicateClips") && result.toInt() > 0 && !m_replies && m_errors.isEmpty())
      fail("operation_failed", refusal(op, "did not finish."));
    if (result.metaType().id() == QMetaType::Bool && !result.toBool() &&
        !questions.contains(op))
      fail("operation_failed", refusal(op, "returned false."));
    if (op == "editor.setLibraryRange")
      return libraryRange(args);
    return result;
  }
  // A media task that finished within the request's wait decides its
  // reply: a failed or cancelled task fails the request with the reasons from
  // its report, and a partial one lists them. A task still running (a request
  // sent with wait: false, or one past the timeout) was only accepted; its
  // outcome is mediaTasks.record with the returned ID.
  void taskOutcome(const QString &id) {
    const auto task = m_backend->mediaTasks()->record(id);
    const auto state = task.value("state").toString();
    if (!QStringList{"failed", "cancelled", "partial"}.contains(state))
      return;
    QStringList reasons;
    for (const auto &v : task.value("report").toList()) {
      const auto row = v.toMap();
      const auto message = row.value("message").toString();
      if (!QStringList{"completed", "match", "validated", "duplicate"}.contains(
              row.value("state").toString()) &&
          !message.isEmpty() && !reasons.contains(message))
        reasons << message;
    }
    if (state == "partial") {
      m_messages << reasons;
      return;
    }
    fail("operation_failed",
         QStringList{task.value("message").toString(), reasons.join('\n')}
             .filter(QRegularExpression("\\S"))
             .join('\n'));
  }
  // The media range is limited to the clip's frames. Say what was applied
  // so a clamped request is visible.
  QVariant libraryRange(const QVariantMap &args) {
    const auto entry =
        m_backend->library().value(args.value("libraryIndex").toInt()).toMap();
    const int in = entry.value("rangeIn", -1).toInt(),
              out = entry.value("rangeOut", -1).toInt();
    const int askedIn = args.value("in").toInt(),
              askedOut = args.value("out").toInt();
    const bool clamped = (askedIn >= 0 && askedIn != in && askedIn != out) ||
                         (askedOut >= 0 && askedOut != out && askedOut != in);
    if (clamped)
      m_messages << QString("The range was limited to the clip's %1 frames: "
                            "in %2, out %3.")
                        .arg(entry.value("duration").toInt())
                        .arg(in)
                        .arg(out);
    return QVariantMap{{"in", in}, {"out", out}, {"clamped", clamped}};
  }
};

namespace {
// The CLI keeps its own settings, queues and recovery files by pointing the
// XDG folders at its state directory. The user's fonts, font configuration
// and downloaded speech models are shared resources, not editor state:
// link them into the state folders so renders match the desktop editor.
void shareUserResources(const QString &state) {
  const QString home = QDir::homePath();
  const QString data = qEnvironmentVariable("XDG_DATA_HOME", home + "/.local/share");
  const QString config = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config");
  const QList<QPair<QString, QString>> links = {
      {data + "/fonts", state + "/data/fonts"},
      {config + "/fontconfig", state + "/config/fontconfig"},
      {data + "/omaedit/whisper", state + "/data/omaedit/whisper"}};
  for (const auto &[target, link] : links) {
    const QFileInfo existing(link);
    if (existing.isSymLink()) {
      if (existing.symLinkTarget() == QFileInfo(target).absoluteFilePath())
        continue;
      QFile::remove(link); // the user's folder moved; follow it
    } else if (existing.exists())
      continue; // a real folder of the CLI's own is never replaced
    if (!QFileInfo(target).isDir() || QFileInfo(target).absoluteFilePath() == QFileInfo(link).absoluteFilePath())
      continue;
    QDir().mkpath(QFileInfo(link).absolutePath());
    QFile::link(target, link);
  }
}
} // namespace

namespace {
// Options the CLI documents, split by whether they take a value. They may
// come before or after the command word (`omaedit [options] command`).
const QStringList valueOptions = {"output",   "ops",       "args",    "spec",
                                  "settings", "preset",    "timeline", "frame",
                                  "state-dir", "timeout",  "sequence"};
const QStringList flagOptions = {"in-place", "overwrite", "progress", "link",
                                 "force",    "help",      "help-all", "version"};
const QStringList shortFlags = {"-h", "-v", "-?"};
const QStringList commandWords = {"new",   "inspect",  "apply", "call",
                                  "export", "frame",   "ops",   "defaults",
                                  "serve", "guide",    "help",  "skill"};

enum class Invocation { Editor, Cli, Unknown };
// Decides who handles this command line before any application object
// exists. The first word that is neither a CLI option nor an option's value
// is the command. Qt's own options (-platform, -style, --reverse) and file
// names still start the editor; a word that is neither a command nor an
// existing path is a mistyped command and must never open a window.
Invocation classify(int argc, char **argv, QString *command) {
  bool cliOption = false, foreign = false;
  for (int i = 1; i < argc; ++i) {
    const auto arg = QString::fromLocal8Bit(argv[i]);
    if (arg.startsWith("--")) {
      const auto name = arg.mid(2).section('=', 0, 0);
      if (valueOptions.contains(name)) {
        cliOption = true;
        if (!arg.contains('='))
          ++i; // its value, never the command
      } else if (flagOptions.contains(name))
        cliOption = true;
      else
        foreign = true;
      continue;
    }
    if (arg.startsWith('-') && arg.size() > 1) {
      if (shortFlags.contains(arg))
        cliOption = true;
      else
        foreign = true;
      continue;
    }
    *command = arg;
    if (commandWords.contains(arg))
      return Invocation::Cli;
    // Qt option values (-platform offscreen) cannot be told from words, and
    // files, folders and URLs are what the editor opens.
    if (foreign || arg.contains("://") || QFileInfo::exists(arg))
      return Invocation::Editor;
    return Invocation::Unknown;
  }
  return cliOption && !foreign ? Invocation::Cli : Invocation::Editor;
}
} // namespace

int agentCliMain(int argc, char **argv) {
  QString word;
  const auto invocation = classify(argc, argv, &word);
  if (invocation == Invocation::Editor)
    return -1;
  if (invocation == Invocation::Unknown) {
    output({{"ok", false},
            {"error",
             QVariantMap{{"code", "usage"},
                         {"message", "Unknown command or missing file: " +
                                         word + ". Commands: " +
                                         commandWords.join(", ") +
                                         ". See omaedit --help."}}}});
    return 2;
  }
  if (word == "skill")
    return agentSkillMain(argc, argv);
  // Do this before constructing QGuiApplication or any QSettings. GUI queues,
  // preferences and recovery files are never inherited by an agent session.
  QString state = qEnvironmentVariable("OMAEDIT_CLI_STATE_DIR");
  for (int i = 1; i < argc; ++i) {
    const auto arg = QString::fromLocal8Bit(argv[i]);
    if (arg.startsWith("--state-dir="))
      state = arg.mid(12);
    else if (arg == "--state-dir" && i + 1 < argc)
      state = QString::fromLocal8Bit(argv[++i]);
  }
  if (state.isEmpty())
    state = qEnvironmentVariable("XDG_DATA_HOME",
                                 QDir::homePath() + "/.local/share") +
            "/omaedit/cli";
  state = QFileInfo(state).absoluteFilePath();
  shareUserResources(state);
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QPA_PLATFORMTHEME", "");
  qunsetenv("QT_STYLE_OVERRIDE");
  qputenv("OMAEDIT_BACKGROUND_RENDER", "0");
  qputenv("OMAEDIT_VIEWER_PROCESSING", "cpu");
  qputenv("XDG_CONFIG_HOME", (state + "/config").toUtf8());
  qputenv("XDG_DATA_HOME", (state + "/data").toUtf8());
  qputenv("XDG_CACHE_HOME", (state + "/cache").toUtf8());
  qputenv("XDG_STATE_HOME", (state + "/state").toUtf8());
  QGuiApplication app(argc, argv);
  app.setOrganizationName("omaedit");
  app.setApplicationName("omaedit");
  app.setApplicationVersion(QStringLiteral(OMAEDIT_VERSION));
  app.setQuitOnLastWindowClosed(false);
  app.setProperty("omaedit.agent", true);
  // (The GPU that draws is the first one tried for decoding.)
  {
    const int drawing = render::engineGpu();
    SuiteVideo::configureDecoding(drawing >= 0 ? QString::number(drawing) : QString());
  }
  QCommandLineParser parser;
  parser.setApplicationDescription(
      "OmaEdit agent CLI — JSON results on stdout; diagnostics and progress on "
      "stderr.\n\n"
      "Commands:\n"
      "  new PROJECT            create a project (--timeline, --settings)\n"
      "  inspect PROJECT        report the project's state\n"
      "  apply PROJECT          run an --ops batch; save with --output or "
      "--in-place\n"
      "  call PROJECT OP        run one operation with --args\n"
      "  export PROJECT         render to --output (--spec, --preset)\n"
      "  frame PROJECT          save --frame N of the timeline to --output\n"
      "  ops [FILTER]           operation catalogue\n"
      "  defaults               project/timeline/grade/render/audio templates\n"
      "  serve [PROJECT]        JSON-lines session on stdin/stdout\n"
      "  guide                  the complete manual as JSON text\n"
      "  skill [--link [--force]]  report or register the agent skill; "
      "--force backs up conflicting entries first\n\n"
      "Run `omaedit guide` for the full manual and `omaedit ops` for every "
      "operation.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("command", "CLI command (see above)");
  parser.addPositionalArgument("arguments",
                               "Project path, operation or catalogue filter",
                               "[arguments...]");
  const QList<QPair<QString, QString>> described = {
      {"output", "Destination: saved project (apply/call), video (export) or "
                 "PNG/EXR image (frame)."},
      {"ops", "JSON array of requests for apply: a file path, or - for "
              "stdin."},
      {"args", "Operation arguments for call: inline JSON object or "
               "@file.json."},
      {"spec", "Render settings for export: JSON object or @file.json (see "
               "defaults)."},
      {"settings", "Project and first-timeline settings for new: JSON object "
                   "or @file.json."},
      {"preset", "Render preset ID for export."},
      {"timeline", "Name of the first timeline new creates; omit it for a "
                   "media-only project."},
      {"frame", "Timeline frame number for frame."},
      {"state-dir", "CLI settings, queue, cache and recovery folder "
                    "(default ~/.local/share/omaedit/cli)."},
      {"timeout", "Per-operation wait limit in milliseconds, 100 to "
                  "86400000 (default 600000)."},
      {"sequence", "Timeline name or ID for inspect, apply, call, frame, "
                   "export or serve."}};
  for (const auto &[name, description] : described)
    parser.addOption(QCommandLineOption(name, description, name));
  parser.addOption(
      QCommandLineOption("in-place", "Save edits back to the input project."));
  parser.addOption(QCommandLineOption(
      "overwrite", "Allow replacement of explicitly requested output files."));
  parser.addOption(
      QCommandLineOption("progress", "Write JSON progress events to stderr."));
  if (!parser.parse(app.arguments())) {
    output({{"ok", false},
            {"error",
             QVariantMap{{"code", "usage"}, {"message", parser.errorText()}}}});
    return 2;
  }
  const auto pos = parser.positionalArguments();
  const QString command = pos.value(0);
  if (command == "help" || parser.isSet("help") || parser.isSet("help-all")) {
    fputs(parser.helpText().toUtf8().constData(), stdout);
    return 0;
  }
  if (command == "guide") {
    QFile guide(":/agent/guide.md");
    if (!guide.open(QIODevice::ReadOnly)) {
      output({{"ok", false},
              {"error", QVariantMap{{"code", "read_failed"},
                                    {"message",
                                     "The built-in guide is unavailable."}}}});
      return 1;
    }
    output({{"ok", true},
            {"apiVersion", 1},
            {"result", QString::fromUtf8(guide.readAll())}});
    return 0;
  }
  if (parser.isSet("version")) {
    output({{"ok", true},
            {"version", app.applicationVersion()},
            {"apiVersion", 1}});
    return 0;
  }
  try {
    bool valid = true;
    const int timeout = parser.isSet("timeout")
                            ? parser.value("timeout").toInt(&valid)
                            : 600000;
    if (!valid || timeout < 100 || timeout > 86400000)
      fail("usage", "--timeout is milliseconds between 100 and 86400000.");
    const bool argsOk = command == "ops" || command == "serve"
                            ? pos.size() >= 1 && pos.size() <= 2
                        : command == "defaults" ? pos.size() == 1
                        : command == "call"     ? pos.size() == 3
                                                : pos.size() == 2;
    if (!argsOk)
      fail("usage", "Wrong number of arguments. See omaedit --help.");
    AgentSession session(timeout, command != "serve");
    const auto run = [&](const QString &op,
                         const QVariantMap &args = QVariantMap{}) {
      auto r = session.request(
          {{"op", op}, {"args", args}, {"progress", parser.isSet("progress")}});
      if (!r.value("ok").toBool()) {
        const auto e = r.value("error").toMap();
        fail(e.value("code").toString(), e.value("message").toString());
      }
      return r;
    };
    if (command == "serve") {
      if (pos.size() > 1)
        run("project.open", {{"path", pos[1]}});
      if (parser.isSet("sequence"))
        run("timeline.select", {{"sequence", parser.value("sequence")}});
      QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
      QByteArray pending;
      QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        input.setEnabled(false);
        char buffer[16384];
        auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        while (count < 0 && errno == EINTR)
          count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          // A spurious wake on non-blocking input: stdin is still open.
          input.setEnabled(true);
          return;
        }
        if (count > 0)
          pending.append(buffer, int(count));
        if (pending.size() > 16 * 1024 * 1024) {
          output(
              {{"ok", false},
               {"error",
                QVariantMap{{"code", "request_too_large"},
                            {"message", "Requests must fit within 16 MiB."}}}});
          app.exit(2);
          return;
        }
        if (count == 0 && !pending.trimmed().isEmpty())
          pending.append('\n');
        while (pending.contains('\n')) {
          const auto at = pending.indexOf('\n');
          const auto line = pending.left(at);
          pending.remove(0, at + 1);
          if (line.trimmed().isEmpty())
            continue;
          try {
            const auto v = json(line);
            if (v.metaType().id() != QMetaType::QVariantMap)
              fail("invalid_request", "Each line must contain an object.");
            output(session.request(v.toMap()));
          } catch (const CliError &e) {
            output({{"ok", false},
                    {"error",
                     QVariantMap{{"code", e.code}, {"message", e.message}}}});
          }
        }
        if (count <= 0) {
          if (session.dirty()) {
            output({{"ok", false},
                    {"event", "session.closed"},
                    {"error",
                     QVariantMap{{"code", "unsaved_changes"},
                                 {"message",
                                  "The session ended with unsaved edits. Send "
                                  "project.save before closing stdin."}}}});
            app.exit(1);
          } else if (session.unfinished()) {
            output(
                {{"ok", false},
                 {"event", "session.closed"},
                 {"error",
                  QVariantMap{
                      {"code", "unfinished_jobs"},
                      {"message",
                       "The session ended with active work or renders it "
                       "queued but did not run. Stop recording, and finish, "
                       "run or cancel jobs before closing stdin."}}}});
            app.exit(1);
          } else
            app.quit();
          return;
        }
        input.setEnabled(true);
      });
      return app.exec();
    }
    QVariantMap result;
    if (command == "ops")
      result = run("ops", {{"filter", pos.value(1)}});
    else if (command == "defaults")
      result = run("defaults");
    else {
      if (pos.size() < 2)
        fail("usage", "This command requires a project path.");
      if (command == "new") {
        QVariantMap settings;
        if (parser.isSet("settings"))
          settings = converted(readJson(parser.value("settings")),
                               QMetaType::fromType<QVariantMap>(), "settings")
                         .toMap();
        if (!settings.contains("title"))
          settings["title"] = QFileInfo(pos[1]).completeBaseName();
        run("project.new", {{"settings", settings}});
        if (parser.isSet("timeline"))
          run("timeline.create",
              {{"name", parser.value("timeline")}, {"settings", settings}});
        result =
            run("project.save",
                {{"path", pos[1]}, {"overwrite", parser.isSet("overwrite")}});
      } else {
        run("project.open", {{"path", pos[1]}});
        if (parser.isSet("sequence"))
          run("timeline.select", {{"sequence", parser.value("sequence")}});
        if (command == "inspect")
          result = run("inspect");
        else if (command == "export" || command == "frame") {
          QVariantMap a{{"path", parser.value("output")},
                        {"overwrite", parser.isSet("overwrite")}};
          if (command == "export") {
            if (parser.isSet("spec"))
              a["spec"] = readJson(parser.value("spec"));
            if (parser.isSet("preset"))
              a["preset"] = parser.value("preset");
          } else {
            bool ok;
            const int frame = parser.value("frame").toInt(&ok);
            if (!ok)
              fail("usage", "--frame requires an integer.");
            a["frame"] = frame;
          }
          result = run(command == "export" ? "render" : "frame", a);
        } else {
          if (parser.isSet("output") && parser.isSet("in-place"))
            fail("usage", "Choose --output or --in-place.");
          if (command == "apply") {
            if (!parser.isSet("output") && !parser.isSet("in-place"))
              fail("usage", "apply requires --output or --in-place.");
            const auto path = parser.value("ops");
            if (path.isEmpty())
              fail("usage", "apply requires --ops FILE (or - for stdin).");
            auto operations = readJson(path == "-" ? path : "@" + path);
            if (operations.metaType().id() != QMetaType::QVariantList)
              fail("invalid_json",
                   "The operations file must be an array of requests.");
            for (const auto &v : operations.toList()) {
              const auto r = v.toMap();
              if (r.value("op").toString().startsWith("project."))
                fail("invalid_batch",
                     "Project open/new/save belongs outside an apply batch. "
                     "apply publishes the project once at the end.");
              if (r.contains("wait") && !r.value("wait").toBool())
                fail("invalid_batch",
                     "apply operations must wait for completion. Use serve for "
                     "asynchronous requests.");
            }
            QVariantList results;
            for (const auto &v : operations.toList()) {
              if (v.metaType().id() != QMetaType::QVariantMap)
                fail("invalid_request", "Each operation must be an object.");
              auto r = session.request(v.toMap());
              results << r;
              if (!r.value("ok").toBool()) {
                output({{"ok", false},
                        {"error", r.value("error")},
                        {"operationIndex", results.size() - 1},
                        {"results", results},
                        {"projectSaved", false}});
                return 1;
              }
            }
            result = {{"ok", true}, {"results", results}};
          } else {
            if (pos.size() != 3)
              fail("usage", "call requires PROJECT OPERATION --args JSON.");
            result =
                run(pos[2],
                    parser.isSet("args")
                        ? converted(readJson(parser.value("args")),
                                    QMetaType::fromType<QVariantMap>(), "args")
                              .toMap()
                        : QVariantMap{});
          }
          if (parser.isSet("output") || parser.isSet("in-place"))
            result["saved"] =
                run("project.save", {{"path", parser.isSet("in-place")
                                                  ? pos[1]
                                                  : parser.value("output")},
                                     {"overwrite", parser.isSet("overwrite")}})
                    .value("result");
          else if (result.value("dirty").toBool())
            fail("save_required", "The operation changed the project. Use "
                                  "--output or --in-place to retain edits.");
        }
      }
    }
    output(result);
    return 0;
  } catch (const CliError &e) {
    output({{"ok", false},
            {"error", QVariantMap{{"code", e.code}, {"message", e.message}}}});
    return 1;
  }
}
