#include "backend_p.h"

// ── Project settings + preferences ────────────────────────────────────────
void Backend::setupProject() {
    m_projectSettings = Timeline::defaultProjectSettings();
    connect(this, &Backend::setProjectSettingsRequested, m_worker, &PlayerWorker::setProjectSettings);
    connect(this, &Backend::setAudioOutputRequested, m_worker, [this](const QString &deviceId) { m_worker->setAudioOutput(deviceId); });
    connect(this, &Backend::setAudioBufferFramesRequested, m_worker, &PlayerWorker::setAudioBufferFrames);
    connect(m_worker, &PlayerWorker::projectSettingsChanged, this, &Backend::onProjectSettings);
    QSettings settings;
    m_audioOutput = settings.value(QStringLiteral("playback/audioOutput")).toString();
    m_audioBufferFrames = qBound(2, settings.value(QStringLiteral("playback/audioBufferFrames"), 6).toInt(), 30);
    if (m_audioBufferFrames != 6)
        emit setAudioBufferFramesRequested(m_audioBufferFrames);
    m_monitorMapping = settings.value(QStringLiteral("playback/monitorMapping"), QStringLiteral("stereo")).toString();
    if (!AudioEngine::channelModes().contains(m_monitorMapping) || m_monitorMapping == QStringLiteral("mono")) m_monitorMapping = QStringLiteral("stereo");
    m_inputChannel = settings.value(QStringLiteral("audio/inputChannel"), QStringLiteral("both")).toString();
    if (!QStringList{"both", "left", "right"}.contains(m_inputChannel)) m_inputChannel = QStringLiteral("both");
    if (m_monitorMapping != QStringLiteral("stereo")) { const QString mode = m_monitorMapping; QMetaObject::invokeMethod(m_worker, [this, mode] { m_worker->setMonitorMapping(mode); }, Qt::QueuedConnection); m_recorder->setMonitorMapping(mode); }
    if (m_inputChannel != QStringLiteral("both")) m_recorder->setInputChannel(m_inputChannel);
    rescanPlugins();
    m_autosaveMinutes = qBound(0, settings.value(QStringLiteral("general/autosaveMinutes"), 5).toInt(), 60);
    m_defaultColorspace = settings.value(QStringLiteral("color/defaultColorspace"), 709).toInt();
    if (m_defaultColorspace != 601 && m_defaultColorspace != 709 && m_defaultColorspace != 2020)
        m_defaultColorspace = 709;
    m_newTimelineStart = startTimecodeText(settings.value(QStringLiteral("general/newTimelineStart")).toString());
    if (!m_audioOutput.isEmpty())
        emit setAudioOutputRequested(m_audioOutput);
    // Recovery writes its own file, including untitled projects. It never
    // clears the dirty flag or emits the manual-save completion signal.
    m_autosave.setInterval(qMax(1, m_autosaveMinutes) * 60 * 1000);
    connect(&m_autosave, &QTimer::timeout, this, &Backend::autosaveNow);
    if (m_autosaveMinutes > 0)
        m_autosave.start();
    // A fresh project takes the preferred colour space.
    connect(m_worker, &PlayerWorker::timelineCleared, this, [this] {
        if (!m_configuredProjectRequest && m_defaultColorspace != 709) {
            QVariantMap s = Timeline::defaultProjectSettings();
            s.insert(QStringLiteral("colorspace"), m_defaultColorspace);
            emit setProjectSettingsRequested(s);
        }
    });
}

void Backend::onProjectSettings(const QVariantMap &settings, bool rateLocked) {
    if (settings == m_projectSettings && rateLocked == m_projectRateLocked)
        return;
    const bool titleChanged=m_projectSettings.value("title")!=settings.value("title");
    m_projectSettings = settings;
    if (titleChanged && m_projectPath.isEmpty()) emit projectChanged();
    m_projectRateLocked = rateLocked;
    emit projectSettingsChanged();
}

QString Backend::colorSpace() const {
    switch (m_projectSettings.value(QStringLiteral("colorspace")).toInt()) {
    case 601: return QStringLiteral("Rec.601");
    case 2020: return QStringLiteral("Rec.2020");
    default: return QStringLiteral("Rec.709");
    }
}

void Backend::setProjectSettings(const QVariantMap &settings) {
    if (m_exporting)
        return;
    emit setProjectSettingsRequested(settings);
}

int Backend::startFrameAtRate(int frame, int rate, double fps) const {
    return Timeline::startFrameAtRate(frame, rate, qRound(fps));
}

QVariantMap Backend::defaultProjectSettings() const {
    QVariantMap s = Timeline::defaultProjectSettings();
    s.insert(QStringLiteral("colorspace"), m_defaultColorspace);
    return s;
}

void Backend::setAudioOutput(const QString &deviceId) {
    if (deviceId == m_audioOutput)
        return;
    m_audioOutput = deviceId;
    QSettings().setValue(QStringLiteral("playback/audioOutput"), deviceId);
    m_recorder->setMonitorOptions(m_monitorSettings.value("muted").toBool(), m_monitorSettings.value("dim").toBool(), m_monitorSettings.value("mono").toBool(), deviceId);
    emit setAudioOutputRequested(deviceId);
    emit preferencesChanged();
}

void Backend::setMonitorMapping(const QString &mode) {
    if (mode == m_monitorMapping || !AudioEngine::channelModes().contains(mode) || mode == QStringLiteral("mono")) return;
    m_monitorMapping = mode;
    QSettings().setValue(QStringLiteral("playback/monitorMapping"), mode);
    QMetaObject::invokeMethod(m_worker, [this, mode] { m_worker->setMonitorMapping(mode); }, Qt::QueuedConnection);
    m_recorder->setMonitorMapping(mode);
    emit preferencesChanged();
}
void Backend::setInputChannel(const QString &mode) {
    if (mode == m_inputChannel || !QStringList{"both", "left", "right"}.contains(mode)) return;
    m_inputChannel = mode;
    QSettings().setValue(QStringLiteral("audio/inputChannel"), mode);
    m_recorder->setInputChannel(mode);
    emit preferencesChanged();
}

// ── Preferences ▸ Plug-ins ────────────────────────────────────────────────
// The tools outside the editor that some features run on: the whisper.cpp
// command (transcripts), the pyaaf2 module (AAF timelines) and
// OpenTimelineIO. Every effect, decoder and encoder is built in.
void Backend::rescanPlugins() {
    QVariantList list;
    const QString whisper = QStandardPaths::findExecutable(m_transcriber->command().section(QLatin1Char(' '), 0, 0));
    list.append(QVariantMap{{"id", "whisper"}, {"name", tr("whisper.cpp transcription")}, {"kind", tr("Command")}, {"core", true}, {"present", !whisper.isEmpty()},
        {"detail", whisper.isEmpty() ? tr("%1 not found on PATH").arg(m_transcriber->command()) : whisper}, {"paths", QStringList{whisper}}, {"enabled", true}, {"quarantined", QVariantList()}});
    // The AAF module is found by starting a Python, which can take seconds
    // on a cold disk: the first look runs on a pool thread and the list is
    // made again when it answers. Until then the row has nothing to say.
    QString aafWhy; bool aaf = false;
    if (m_aafProbed || Interchange::availabilityKnown(QStringLiteral("aaf"))) aaf = Interchange::available(QStringLiteral("aaf"), &aafWhy);
    else if (!m_aafProbing) {
        m_aafProbing = true;
        QPointer<Backend> self(this);
        QThreadPool::globalInstance()->start([self] {
            Interchange::available(QStringLiteral("aaf"));
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self] {
                if (!self) return;
                self->m_aafProbing = false; self->m_aafProbed = true;
                self->rescanPlugins();
            }, Qt::QueuedConnection);
        });
    }
    list.append(QVariantMap{{"id", "pyaaf2"}, {"name", tr("pyaaf2 (AAF timelines)")}, {"kind", tr("Python module")}, {"core", true}, {"present", aaf},
        {"detail", aaf ? tr("AAF import and export available") : aafWhy}, {"paths", QStringList{QStandardPaths::findExecutable(QStringLiteral("python3"))}}, {"enabled", true}, {"quarantined", QVariantList()}});
    list.append(QVariantMap{{"id", "otio"}, {"name", tr("OpenTimelineIO")}, {"kind", tr("Library")}, {"core", true}, {"present", true},
        {"detail", tr("Linked into this build")}, {"paths", QStringList()}, {"enabled", true}, {"quarantined", QVariantList()}});
    m_plugins = list;
    emit pluginsChanged();
}
void Backend::setPluginEnabled(const QString &id, bool enabled) {
    QSet<QString> disabled = Timeline::disabledProviders();
    if (enabled) disabled.remove(id); else disabled.insert(id);
    Timeline::setDisabledProviders(disabled);
    QSettings().setValue(QStringLiteral("plugins/disabled"), QStringList(disabled.begin(), disabled.end()));
    rescanPlugins();
}
void Backend::clearPluginQuarantine(const QString &id) {
    QVariantMap quarantine = Timeline::quarantinedServices();
    if (id.isEmpty()) quarantine.clear(); // everything, including services no provider claims any more
    for (const QVariant &v : m_plugins) {
        const auto p = v.toMap();
        if (p.value(QStringLiteral("id")).toString() != id) continue;
        for (const QVariant &q : p.value(QStringLiteral("quarantined")).toList()) quarantine.remove(q.toMap().value(QStringLiteral("service")).toString());
    }
    Timeline::setQuarantinedServices(quarantine);
    rescanPlugins();
}

void Backend::setAudioBufferFrames(int frames) {
    frames = qBound(2, frames, 30);
    if (frames == m_audioBufferFrames)
        return;
    m_audioBufferFrames = frames;
    QSettings().setValue(QStringLiteral("playback/audioBufferFrames"), frames);
    emit setAudioBufferFramesRequested(frames);
    emit preferencesChanged();
}

// Preferences ▸ Reset. Each category goes back through its own setter so the
// UI, the worker and QSettings all follow; QML-owned bits (density, skimming,
// snapping, shortcuts) are reset by the dialog itself.
void Backend::resetPreferences(const QString &category) {
    const bool all = category == QStringLiteral("all");
    if (all || category == QStringLiteral("general")) { setAutosaveMinutes(5); setBackupLimit(20); setNewTimelineStart(QString()); }
    if (all || category == QStringLiteral("playback")) {
        setBackgroundRenderEnabled(true); setBackgroundRenderLimit(20);
        setAudioOutput(QString()); setAudioBufferFrames(6); setMonitorMapping(QStringLiteral("stereo"));
        const auto next = PlaybackView::normalized({});
        if (next != m_viewerSettings) {
            m_viewerSettings = next;
            QSettings settings; settings.setValue(QStringLiteral("playback/view"), next);
            settings.setValue(QStringLiteral("monitor/safeAreas"), next.value(QStringLiteral("safeAreas")));
            settings.setValue(QStringLiteral("monitor/guides"), next.value(QStringLiteral("guides")));
            emit viewerSettingsChanged();
        }
    }
    if (all || category == QStringLiteral("media")) {
        setMediaPlayback(QStringLiteral("preferred"));
        m_transcriber->setCommand(QStringLiteral("whisper-cli"));
        m_transcriber->setModel(QString()); // the one that comes with the package
        m_transcriber->setLanguage(QStringLiteral("auto"));
    }
    if (all || category == QStringLiteral("storage")) { StoragePreferences::save(StoragePreferences::defaults()); emit storageSettingsChanged(); }
    if ((all || category == QStringLiteral("audio")) && m_recorder) { m_recorder->setInputDevice(QString()); setInputChannel(QStringLiteral("both")); }
    if (all || category == QStringLiteral("plugins")) { Timeline::setDisabledProviders({}); QSettings().remove(QStringLiteral("plugins/disabled")); clearPluginQuarantine(QString()); }
    if (all || category == QStringLiteral("color")) { setDefaultColorspace(709); if (!m_lutLibrary->folder().isEmpty()) m_lutLibrary->setFolder(QString()); }
    if (all || category == QStringLiteral("hardware")) {
        m_hardware->setVideoDecoderPreference(SuiteVideo::normalizedDecodePreference(QStringLiteral("auto")));
        m_hardware->setInterfaceRenderingPreference(QStringLiteral("auto"));
        m_hardware->setViewerProcessingPreference(QStringLiteral("auto"));
        m_hardware->setEngineGpu(QString());
        setBackgroundRenderProcessing(QStringLiteral("auto"));
    }
    emit preferencesChanged();
}

// ── Preferences ▸ Storage ─────────────────────────────────────────────────
QVariantMap Backend::storageSettings() const { return StoragePreferences::configured(); }
QVariantMap Backend::storageDefaults() const { return StoragePreferences::defaults(); }
QVariantMap Backend::storageLimits() const { return StoragePreferences::limits(); }
QVariantMap Backend::storageDefaultPaths() const { return StoragePreferences::defaultPaths(m_projectPath); }
QVariantMap Backend::storageActivePaths() const { return StoragePreferences::activePaths(m_projectPath); }
bool Backend::storageRestartRequired() const { return StoragePreferences::restartRequired(); }
QString Backend::defaultProxyDir() const {
    const QString project = m_projectSettings.value(QStringLiteral("proxyFolder")).toString();
    return project.isEmpty() ? StoragePreferences::proxyRoot(m_projectPath) : project;
}
QString Backend::saveStorageSettings(const QVariantMap &draft) {
    const QString problem = StoragePreferences::save(draft);
    emit storageSettingsChanged();
    return problem;
}
QVariantMap Backend::storageUsage() const {
    return {{QStringLiteral("thumbnailDiskBytes"), ThumbnailDiskCache::bytes()},
            {QStringLiteral("thumbnailDiskLimitBytes"), ThumbnailDiskCache::limit()},
            {QStringLiteral("thumbnailMemoryBytes"), m_thumbs ? m_thumbs->memoryBytes() : 0ll},
            {QStringLiteral("thumbnailMemoryLimitBytes"), qint64(StoragePreferences::session().value(QStringLiteral("thumbnailMemoryMiB")).toInt()) * 1024 * 1024}};
}

void Backend::findUnusedKeptMedia() { sweepKeptMedia(false); }
void Backend::removeUnusedKeptMedia() { sweepKeptMedia(true); }
void Backend::sweepKeptMedia(bool remove) {
    QStringList documents = m_recent, library;
    if (!m_projectPath.isEmpty())
        documents.append(m_projectPath); // as last saved: closing without saving goes back to it
    for (const QVariant &clip : std::as_const(m_library))
        library.append(clip.toMap().value(QStringLiteral("path")).toString());
    emit keptMediaSweepRequested(documents, library, remove);
}

void Backend::setAutosaveMinutes(int minutes) {
    minutes = qBound(0, minutes, 60);
    if (minutes == m_autosaveMinutes)
        return;
    m_autosaveMinutes = minutes;
    QSettings().setValue(QStringLiteral("general/autosaveMinutes"), minutes);
    m_autosave.stop();
    if (minutes > 0) {
        m_autosave.setInterval(minutes * 60 * 1000);
        m_autosave.start();
    }
    emit preferencesChanged();
}

void Backend::setDefaultColorspace(int colorspace) {
    if (colorspace != 601 && colorspace != 709 && colorspace != 2020)
        colorspace = 709;
    if (colorspace == m_defaultColorspace)
        return;
    m_defaultColorspace = colorspace;
    QSettings().setValue(QStringLiteral("color/defaultColorspace"), colorspace);
    emit preferencesChanged();
}

QString Backend::startTimecodeText(const QString &timecode) {
    static const QRegularExpression shape(QStringLiteral("^(\\d\\d):(\\d\\d):(\\d\\d):(\\d\\d)$"));
    const auto match = shape.match(timecode.trimmed());
    if (!match.hasMatch() || match.captured(1).toInt() > 23 || match.captured(2).toInt() > 59
        || match.captured(3).toInt() > 59 || match.captured(4).toInt() >= kNewTimelineStartRate)
        return QStringLiteral("00:00:00:00");
    return match.captured(0);
}

void Backend::setNewTimelineStart(const QString &timecode) {
    const QString next = startTimecodeText(timecode);
    if (next == m_newTimelineStart)
        return;
    m_newTimelineStart = next;
    QSettings().setValue(QStringLiteral("general/newTimelineStart"), next);
    emit preferencesChanged();
}

void Backend::clearRecentProjects() {
    if (m_recent.isEmpty())
        return;
    m_recent.clear();
    QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"))
        .setValue(QStringLiteral("recentProjects"), m_recent);
    emit recentChanged();
}
