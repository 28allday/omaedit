#include "playerworker_p.h"

namespace {
constexpr int kThumbnailWidth = 220;
}

PlayerWorker::PlayerWorker(QObject *parent, std::unique_ptr<PlaybackAudioOutput> output)
    : QObject(parent), m_nestSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    m_audioOutput=output?std::move(output):PlaybackAudioOutput::create();
    // A device that may be driven from another thread is fed from one of its
    // own, and timeline sound is made ahead of the picture (pump()), so a held
    // worker is not heard. OMAEDIT_SOUND_ON_WORKER=1: the pump feeds the
    // device itself and makes each frame's sound with its picture, as before.
    if (m_audioOutput->feederSafe() && qEnvironmentVariableIntValue("OMAEDIT_SOUND_ON_WORKER") != 1) {
        auto fed=std::make_unique<FedAudioOutput>(std::move(m_audioOutput));
        m_fed=fed.get(); m_audioOutput=std::move(fed);
    }
    m_audioEditSession=QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_cacheFeedbackClock.start();
    m_probePool.setMaxThreadCount(qMax(1, QThread::idealThreadCount() / 4));  // importClip
    m_peakPool.setMaxThreadCount(1);  // makePeaks: one batch at a time, behind everything else
    m_peakPool.setThreadPriority(QThread::LowPriority);
    // makeTiles: likewise, and always the same thread (its engine and its
    // decoders stay with it).
    m_tilePool.setMaxThreadCount(1);
    m_tilePool.setThreadPriority(QThread::LowPriority);
    m_anglePool.setMaxThreadCount(1); // the angle decoders' own thread, kept
    m_anglePool.setExpiryTimeout(-1);
    m_tilePool.setExpiryTimeout(-1);
    // Its copy of the timeline, its engine and its decoders are let go of
    // when no strip has needed them for a while (tilesMade starts this).
    m_tileIdle.setParent(this);
    m_tileIdle.setSingleShot(true);
    m_tileIdle.setInterval(30000);
    connect(&m_tileIdle, &QTimer::timeout, this, [this] {
        for (const StripJob &job : std::as_const(m_stripJobs))
            if (!job.wave && job.sourcePath.isEmpty()) { m_tileIdle.start(); return; }
        endTileThread();
    });
    m_timeline.setAllowOffline(true);
    // Preferences ▸ Storage: both thumbnail caches share one memory limit
    // (KiB of pixels) and the waveform store keeps this many peak values of
    // sounds no picture shows.
    m_stripCache.setMaxCost(StoragePreferences::session().value(QStringLiteral("thumbnailMemoryMiB")).toInt() * 1024);
    WavePeaks::Store::instance()->setSpare(StoragePreferences::session().value(QStringLiteral("waveformSamples")).toInt());
    qRegisterMetaType<QList<QImage>>(); // crosses the worker→GUI queued boundary
    qRegisterMetaType<QList<int>>();    // markersChanged, same boundary
    qRegisterMetaType<QList<float>>();  // trackLevels, same boundary
    // The timers must be children so moveToThread() carries them along with
    // the worker — a parentless member QObject keeps GUI-thread affinity and
    // can never be started from the worker thread.
    m_pump.setParent(this);
    m_exportPoll.setParent(this);
    m_pump.setTimerType(Qt::PreciseTimer);
    m_pump.setSingleShot(true); // pump() books its own next wake-up
    connect(&m_pump, &QTimer::timeout, this, &PlayerWorker::pump);
    connect(&m_exportPoll, &QTimer::timeout, this, &PlayerWorker::pollExport);
    m_renderPoll.setParent(this);
    // Long enough that a drag's pause between two mouse moves does not draw
    // the whole frame, short enough that a picture at rest is whole at once.
    m_settle.setParent(this);
    m_settle.setSingleShot(true);
    m_settle.setInterval(qBound(0, qEnvironmentVariableIntValue("OMAEDIT_SETTLE_MS") > 0 ? qEnvironmentVariableIntValue("OMAEDIT_SETTLE_MS") : 120, 5000));
    connect(&m_settle, &QTimer::timeout, this, &PlayerWorker::settleFrame);
    connect(&m_renderPoll, &QTimer::timeout, this, &PlayerWorker::pollRender);
    m_backgroundRender = new PreviewRenderCache(this);
    m_backgroundRenderEnabled = qEnvironmentVariable("OMAEDIT_BACKGROUND_RENDER") != "0"
        && QSettings().value("playback/backgroundRender", true).toBool();
    m_backgroundRenderLimit = qBound(1, QSettings().value("playback/backgroundRenderGiB", 20).toInt(), 200);
    m_backgroundRenderProcessing = qEnvironmentVariable("OMAEDIT_CACHE_PROCESSING", QSettings().value("hardware/cacheProcessing", "auto").toString()) == "cpu" ? "cpu" : "auto";
    m_backgroundRenderDelay.setParent(this);
    m_backgroundRenderDelay.setSingleShot(true);
    m_backgroundRenderDelay.setInterval(1200);
    connect(&m_backgroundRenderDelay, &QTimer::timeout, this, [this] {
        if (!m_backgroundRenderEnabled || m_timeline.isEmpty()) return;
        if (m_speed != 0 || exportActive() || m_render) { m_backgroundRenderDelay.start(); return; }
        m_backgroundRender->begin(renderSnapshot(), m_position,
            StoragePreferences::cacheRoot() + "/playback-renders-v1", qint64(m_backgroundRenderLimit) * 1024 * 1024 * 1024, m_backgroundRenderProcessing);
    });
    connect(m_backgroundRender, &PreviewRenderCache::changed, this, &PlayerWorker::backgroundRenderChanged);
    connect(m_backgroundRender, &PreviewRenderCache::dependenciesChanged, this, &PlayerWorker::invalidateBackgroundRender);
    // Connect before the GUI subscribes: obsolete pictures are unavailable even
    // to the immediate refresh following an edit, undo, load or track toggle.
    connect(this, &PlayerWorker::timelineChanged, this, &PlayerWorker::picturesMayHaveChanged);
    connect(this, &PlayerWorker::tracksChanged, this, &PlayerWorker::picturesMayHaveChanged);
    connect(this, &PlayerWorker::exportStageChanged, this, &PlayerWorker::updateBackgroundRenderBusy);
    connect(this, &PlayerWorker::renderProgress, this, &PlayerWorker::updateBackgroundRenderBusy);
}

PlayerWorker::~PlayerWorker() {
    // Probes not begun are dropped; one under way ends before anything it
    // would report to is gone (its queued report is never delivered).
    m_probePool.clear();
    m_probePool.waitForDone();
    // Peaks likewise: a batch under way stops at its next peak.
    m_peakPool.clear();
    for (const StripJob &job : std::as_const(m_stripJobs)) if (job.stop) job.stop->store(true);
    m_peakPool.waitForDone();
    // Tiles too: the picture being drawn is given up, and the tile thread's
    // timeline and engine go on that thread, before it does.
    m_tilesHeld->store(true);
    endTileThread();
    m_tilePool.waitForDone();
    // The multicam viewer's grid: one being made is finished, and its
    // decoders go on their own thread.
    m_anglePool.clear();
    m_anglePool.start([bench = std::move(m_angleBench)]() mutable { bench.reset(); });
    m_anglePool.waitForDone();
    m_pump.stop();m_exportPoll.stop();m_renderPoll.stop();m_backgroundRenderDelay.stop();m_settle.stop();
    if(m_exportRenderRequest)m_exportRenderRequest->cancel();
    if(m_render && m_render->request)m_render->request->cancel();
    // waitForFinished can synchronously emit finished. During teardown that
    // must not re-enter endExport or a render callback and touch deleted caches
    // (or start another encoder stage). Stop the owned jobs before their graph.
    const auto stopProcess=[this](QProcess *process) {
        if(!process)return;
        process->disconnect(this);
        if(process->state()==QProcess::NotRunning)return;
        if(process->processId()>0)::kill(pid_t(process->processId()),SIGCONT);
        process->kill();process->waitForFinished(2000);
    };
    if(m_exportProc && m_exportProc->state()!=QProcess::NotRunning)signalExportGroup(m_exportProc.get(),SIGKILL);
    stopProcess(m_exportProc.get());
    if(m_render)stopProcess(m_render->pre.get());
    delete m_backgroundRender; // join its decoder before the live graph is destroyed
    m_backgroundRender=nullptr;
    // This session's nested-timeline files: every saved project, recovery
    // copy and queued render carries its own copy of what it needs of them.
    if (QFileInfo::exists(nestFolder())) QDir(nestFolder()).removeRecursively();
}

// A clip probed for the library: its facts and its poster frame. Runs on a
// thread of the probe pool; touches nothing of the worker.
PlayerWorker::Probed PlayerWorker::probeClip(const QString &path) {
    Probed out;
    MediaPlayer probe;
    if (!probe.open(path))
        return out;
    out.ok = true;
    QVariantMap &meta = out.meta;
    meta.insert(QStringLiteral("name"), QFileInfo(path).fileName());
    meta.insert(QStringLiteral("path"), path);
    meta.insert(QStringLiteral("duration"), probe.frameCount());
    meta.insert(QStringLiteral("fps"), probe.fps());
    meta.insert(QStringLiteral("width"), probe.width());
    meta.insert(QStringLiteral("height"), probe.height());
    // Container metadata (codecs, channels, camera identity when present) and
    // the file's own facts, for the Media page's inspector and filters.
    const QVariantMap container = probe.metadata();
    for (auto it = container.constBegin(); it != container.constEnd(); ++it)
        meta.insert(it.key(), it.value());
    meta.insert(QStringLiteral("hasVideo"), !container.value(QStringLiteral("videoCodec")).toString().isEmpty());
    meta.insert(QStringLiteral("hasAudio"), !container.value(QStringLiteral("audioCodec")).toString().isEmpty());
    const QFileInfo info(path);
    meta.insert(QStringLiteral("size"), info.size());
    meta.insert(QStringLiteral("modified"), info.lastModified().toString(Qt::ISODate));
    if (!meta.contains(QStringLiteral("created"))) {
        const QDateTime birth = info.birthTime();
        meta.insert(QStringLiteral("created"),
                    (birth.isValid() ? birth : info.lastModified()).toString(Qt::ISODate));
    }
    meta.insert(QStringLiteral("imported"), QDateTime::currentDateTime().toString(Qt::ISODate));
    // Only a sound file goes without pictures: one with sound and no picture
    // stream. A generated colour has neither codec and is still a picture.
    out.picture = meta.value(QStringLiteral("hasVideo")).toBool() || !meta.value(QStringLiteral("hasAudio")).toBool();
    if (out.picture && MediaPlayer::isStillPath(path))
        meta.insert(QStringLiteral("still"), true);

    // Poster frame a tenth of the way in — frame 0 is so often black. (The
    // command-line editor shows no library, so it draws none.) A sound file
    // has no picture to show: its card shows that it is sound.
    static const bool agent = qApp && qApp->property("omaedit.agent").toBool();
    out.thumbnail = agent || !out.picture ? QImage() : probe.frameAt(probe.frameCount() / 10);
    if (!out.thumbnail.isNull())
        out.thumbnail = out.thumbnail.scaledToWidth(kThumbnailWidth, Qt::SmoothTransformation);
    out.frames = probe.frameCount();
    out.fps = probe.fps();
    return out;
}

// Opening a project asks for every clip of its library, and an import for
// every file dropped: each was probed here, one after another, in front of
// any seek or play request. Now each is probed on a pool thread (several at
// once, as many as a quarter of the machine's threads) and this thread only
// announces them, in the order they were asked for.
//
// Threads: `m_imports` belongs to this thread. A pool thread writes its own
// Import's `result` and then says so by a queued call; the result is read
// here only after that call (`done`, set here).
void PlayerWorker::importClip(const QString &path) {
    auto import = std::make_shared<Import>();
    import->path = path;
    m_imports.append(import);
    m_probePool.start([this, import] {
        import->result = probeClip(import->path);
        QMetaObject::invokeMethod(this, [this, import] { import->done = true; announceImports(); }, Qt::QueuedConnection);
    });
}

void PlayerWorker::announceImports() {
    while (!m_imports.isEmpty() && m_imports.first()->done) {
        const std::shared_ptr<Import> import = m_imports.takeFirst();
        const Probed &probed = import->result;
        emit clipImported(probed.ok, import->path, probed.meta, probed.thumbnail);
        if (!probed.ok)
            continue;
        if (probed.picture)
            queueLibraryStrip(import->path, probed.frames, probed.fps);
        if (probed.meta.value(QStringLiteral("hasAudio")).toBool())
            queueLibraryWave(import->path, probed.frames);
    }
}

void PlayerWorker::openPreview(const QString &path) {
    if (exportActive()) {
        // Backend already flipped into preview mode optimistically —
        // answer, or its seek bookkeeping wedges waiting for a frame.
        emit previewOpened(false, path, 0, 0.0);
        return;
    }
    setSpeed(0.0);
    flushAudio();
    if (!m_preview.open(m_timeline.playbackPath(path))) {
        // open() closed any previous preview producer on the way in —
        // never stay parked in a source mode with nothing behind it.
        m_sourceMode = false;
        m_previewPath.clear();
        emit previewOpened(false, path, 0, 0.0);
        refreshFrame();
        return;
    }
    m_sourceMode = true;
    m_previewPath = path;
    describePreview();
    m_previewPos = 0;
    resetMediaGangAnchors();
    emit previewOpened(true, path, m_preview.frameCount(), m_preview.fps());
    const auto decoded=sourceShown(0,false);if(!decoded.error.isEmpty())emit frameError(decoded.error);
    emit previewFrameReady(decoded.image,0);
    if(m_settleSource)m_settle.start();
    queueSourceStrip();
    queueLibraryWave(path,m_preview.frameCount());
}

// How the source viewer's clip is read and drawn, as the project has it now:
// its media settings, its input colour space (a clip of it on the timeline
// carries one), and the engine's timeline of it alone (MediaPlayer::drawOnEngine).
void PlayerWorker::describePreview() {
    if (m_previewPath.isEmpty()) return;
    m_preview.setInputTransform(m_timeline.mediaSources().value(m_previewPath).toMap());
    m_preview.setWorkingManagement(m_timeline.linearSourceManagement(m_previewPath));
    m_preview.drawOnEngine(m_previewPath, m_timeline.projectSettings(), m_timeline.mediaSources(), m_timeline.proxyMode());
}

void PlayerWorker::closePreview() {
    if (!m_sourceMode)
        return;
    setSpeed(0.0);
    m_sourceMode = false;
    // A half-decoded source strip is useless once the preview is gone.
    for (int i = m_stripJobs.size() - 1; i >= 0; --i)
        if (!m_stripJobs.at(i).sourcePath.isEmpty())
            finishStripWork(i, QStringLiteral("cancelled"));
    refreshFrame(); // hand the viewer back to the timeline
}

bool PlayerWorker::activeEmpty() const {
    return m_sourceMode ? !m_preview.isOpen() : m_timeline.isEmpty();
}

double PlayerWorker::activeFps() const {
    return m_sourceMode ? m_preview.fps() : m_timeline.fps();
}

QVariantMap PlayerWorker::resolvedTimelineSettings() {
    auto settings = m_timeline.projectSettings();
    if (const auto profile = m_timeline.profile()) {
        settings["custom"] = true;
        settings["width"] = m_timeline.width(); settings["height"] = m_timeline.height();
        settings["fpsNum"] = profile->frame_rate_num(); settings["fpsDen"] = profile->frame_rate_den();
        settings["matchedProfile"] = QVariantMap{{"width",m_timeline.width()},{"height",m_timeline.height()},
            {"sarNum",profile->sample_aspect_num()},{"sarDen",profile->sample_aspect_den()},
            {"progressive",bool(profile->progressive())}};
    }
    return settings;
}

QVariantMap PlayerWorker::audioEditContext() const {
    auto *profile=m_timeline.profile();
    return {{"session",m_audioEditSession},{"generation",m_projectGeneration},
            {"sequenceId",m_activeSequence>=0 && m_activeSequence<m_sequences.size() ? m_sequences[m_activeSequence].id : QString()},
            {"activation",m_audioSequenceActivation},{"revision",m_editRevision},
            {"fpsNum",profile?profile->frame_rate_num():0},{"fpsDen",profile?profile->frame_rate_den():0}};
}
// Keep entries, their exact clock and guarded-edit context in one notification.
// Mixer changes use this lightweight path without rebuilding video decorations.
void PlayerWorker::announceTimelineState(bool contentEdit) {
    if (contentEdit) discardPausedAudio();
    soundMayHaveChanged(); // sound made ahead of an edit is not the timeline's any more
    const auto sequenceId=m_activeSequence>=0 && m_activeSequence<m_sequences.size() ? m_sequences[m_activeSequence].id : QString();
    if(sequenceId!=m_audioEditSequence) {m_audioEditSequence=sequenceId;++m_audioSequenceActivation;}
    auto historyText=[this](const QList<QVariantList> &stack) {
        const auto label=historyLabel(stack);
        if(stack.isEmpty() || stack.last().isEmpty())return label;
        const auto entry=stack.last().last().toMap();
        if(entry.contains("omavid:sequenceState"))return label;
        const auto owner=entry.value("omavid:sequenceId").toString();
        if(m_activeSequence>=0 && m_activeSequence<m_sequences.size() && owner!=m_sequences[m_activeSequence].id)
            for(const auto &sequence:m_sequences)if(sequence.id==owner)return tr("%1 (%2)").arg(label,sequence.name);
        return label;
    };
    emit historyChanged(historyText(m_undoStack), historyText(m_redoStack));
    // Each clip list an undo, a redo or a timeline switch announces has a
    // number of its own: the window answers with the one it has (see
    // acknowledgeReplay), and the last list of the replay is the one that counts.
    if (m_replaying) ++m_replaySerial;
    QVariantMap context = audioEditContext();
    context.insert(QStringLiteral("replay"), m_replaySerial);
    emit timelineChanged(m_timeline.entries(), m_timeline.frameCount(),
                         m_timeline.fps(), m_timeline.width(), m_timeline.height(),
                         !m_undoStack.isEmpty(), !m_redoStack.isEmpty(), contentEdit, resolvedTimelineSettings(), context);
}

void PlayerWorker::announceTimeline(bool contentEdit) {
    bool newlyConfigured = false;
    if (m_activeSequence >= 0 && m_activeSequence < m_sequences.size()) {
        auto &seq = m_sequences[m_activeSequence];
        if (!m_timeline.isEmpty()) {
            newlyConfigured = !seq.configured;
            seq.configured = true;
            // Automatic is a one-time choice for a newly created timeline.
            // Reordering or removing its first clip must not change its timebase.
            if (seq.lockFormat && !m_timeline.projectSettings().value("custom").toBool() && m_timeline.profile()) {
                m_timeline.setProjectSettings(resolvedTimelineSettings());
            }
        }
        seq.settings = m_timeline.projectSettings();
    }
    if (newlyConfigured) announceSequences();
    emit linearPlaybackChanged(m_timeline.originalsOnly());
    announceTimelineState(contentEdit);
    emit tracksChanged(m_timeline.tracks());
    emit masterChanged(m_timeline.masterMix());
    emit adrCuesChanged(m_timeline.adrCues());
    emit projectSettingsChanged(m_timeline.projectSettings(), projectRateLocked());
    queueFilmstrips();
    refreshTwoUp(); // the edit point's neighbours may have changed
}

// Frame numbers belong to this timeline; other timelines can use other rates.
bool PlayerWorker::projectRateLocked() const {
    return !m_timeline.isEmpty();
}

void PlayerWorker::setProjectSettings(const QVariantMap &settings) {
    if (exportActive())
        return;
    QVariantMap next = settings;
    const QVariantMap current = m_timeline.projectSettings();
    if (projectRateLocked()) {
        next.insert(QStringLiteral("fpsNum"), m_timeline.profile() ? m_timeline.profile()->frame_rate_num() : current.value("fpsNum").toInt());
        next.insert(QStringLiteral("fpsDen"), m_timeline.profile() ? m_timeline.profile()->frame_rate_den() : current.value("fpsDen").toInt());
        // Returning to Automatic with clips present would silently re-time the edit.
        if (current.value("custom").toBool()) next["custom"] = true;
    }
    // Media import defaults and the container title remain project-wide.
    for (auto &seq : m_sequences)
        for (const char *key : {"title", "proxyFolder", "importSpace", "importLut"})
            seq.settings[key] = next.value(key);
    const bool shapeChanged = m_timeline.setProjectSettings(next);
    if (shapeChanged && m_timeline.profile() && !m_timeline.isEmpty()) {
        // Every producer reopens against the new profile — the same path
        // an undo takes, so nothing about the cut is special-cased here.
        setSpeed(0.0);
        const QVariantList clips = m_timeline.entries();
        const std::vector<Timeline::TrackFlags> flags =
            Timeline::trackFlagsFromVariant(m_timeline.trackFlagsVariant());
        if (!m_timeline.restore(clips, flags)) {
            // Not every clip came back at the new shape, and this is not an
            // edit undo could take back: the old shape again, with its clips.
            m_timeline.setProjectSettings(current);
            m_timeline.restore(clips, flags);
            emit editRefused(tr("The timeline could not be rebuilt with these settings, so they were not changed."));
        }
        m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    }
    announceTimeline();
    if (shapeChanged || current.value(QStringLiteral("scaling")) != m_timeline.projectSettings().value(QStringLiteral("scaling")))
        refreshFrame();
}

void PlayerWorker::setAudioBufferFrames(int frames) {
    frames = qBound(2, frames, 30);
    if (frames == m_audioBufferFrames)
        return;
    m_audioBufferFrames = frames;
    setAudioOutput(m_audioOutputId, true);
}

void PlayerWorker::setAudioOutput(const QString &deviceId, bool rebuild) {
    if (deviceId == m_audioOutputId && !rebuild)
        return;
    m_audioOutputId = deviceId;
    flushAudio();
    m_audioOutput->close();
    m_audioFailed = false;
}
