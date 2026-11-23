#include "backend_p.h"
#include <QJsonValue>

// ── Deliver: render queue, presets, pre-flight ───────────────────────────

void Backend::setupDeliver() {
    m_renderPresets = new RenderPresets(this);
    m_renderQueue = new RenderQueue(this);
    connect(m_renderQueue, &RenderQueue::failed, this, [this](const QString &message) { setStatus(message); emit failed(message); });
    connect(m_worker,&PlayerWorker::exportFallbackUsed,this,[this](const QString &message){m_renderQueue->addWarning(message);setStatus(message);});
    connect(m_renderQueue,&RenderQueue::snapshotNeeded,this,[this](int id){QMetaObject::invokeMethod(m_worker,[this,id]{m_worker->prepareRenderSnapshot(id);},Qt::QueuedConnection);});
    connect(m_worker,&PlayerWorker::renderSnapshotReady,this,[this](int id,const QVariantMap &snapshot){m_renderQueue->attachSnapshot(id,snapshot);});
    connect(m_renderPresets, &RenderPresets::failed, this, [this](const QString &m) { setStatus(m); emit failed(m); });
    connect(m_renderPresets, &RenderPresets::imported, this, [this](int n) {
        setStatus(tr("%n preset(s) imported", nullptr, n));
    });
    connect(m_renderPresets, &RenderPresets::exported, this, [this](const QString &path) {
        setStatus(tr("Preset written to %1").arg(QFileInfo(path).fileName()));
    });

    // The queue drives the worker; the worker answers the queue.
    connect(m_renderQueue, &RenderQueue::startRequested, this, [this](const QString &path, const QVariantMap &spec) {
        pause();
        m_exporting = true;
        m_exportPercent = 0;
        emit exportingChanged();
        setStatus(tr("Rendering %1…").arg(QFileInfo(path).fileName()));
        emit exportRequested(path, spec);
    });
    connect(m_renderQueue, &RenderQueue::pauseRequested, this, &Backend::exportPauseRequested);
    connect(m_renderQueue, &RenderQueue::resumeRequested, this, &Backend::exportResumeRequested);
    connect(m_renderQueue, &RenderQueue::cancelRequested, this, &Backend::exportCancelRequested);
    connect(m_renderQueue, &RenderQueue::jobFinished, this,
            [this](int, bool ok, const QString &path, const QString &reason) {
        const QString name = QFileInfo(path).fileName();
        if (ok) {
            setStatus(tr("Rendered %1").arg(name));
            emit exportDone(path);
        } else if (reason == QStringLiteral("Cancelled")) {
            setStatus(tr("Render of %1 cancelled").arg(name));
        } else {
            setStatus(tr("Render of %1 failed: %2").arg(name, reason));
            emit failed(m_status);
        }
    });
    connect(m_renderQueue, &RenderQueue::queueDrained, this, [this](bool allOk) {
        if (m_quitWhenIdle) {
            m_quitWhenIdle = false;
            QCoreApplication::quit();
            return;
        }
        const QString after = m_renderQueue->afterCompletion();
        if (allOk && after != QStringLiteral("none") && !QStandardPaths::isTestModeEnabled()) {
            m_renderQueue->setAfterCompletion(QStringLiteral("none"));
            QProcess::startDetached(QStringLiteral("systemctl"),
                                    {after == QStringLiteral("shutdown") ? QStringLiteral("poweroff") : QStringLiteral("suspend")});
        }
    });
    connect(this, &Backend::exportRequested, m_worker,
            qOverload<const QString &, const QVariantMap &>(&PlayerWorker::exportTimeline));
    connect(this, &Backend::exportPauseRequested, m_worker, &PlayerWorker::pauseExport);
    connect(this, &Backend::exportResumeRequested, m_worker, &PlayerWorker::resumeExport);
    connect(this, &Backend::exportCancelRequested, m_worker, &PlayerWorker::cancelExport);
    connect(m_worker, &PlayerWorker::exportProgress, this, [this](int percent) {
        m_renderQueue->onProgress(percent);
        if (m_exportPercent == percent)
            return;
        m_exportPercent = percent;
        emit exportingChanged();
    });
    connect(m_worker, &PlayerWorker::exportStageChanged, m_renderQueue, &RenderQueue::onStage);
    connect(m_worker, &PlayerWorker::exportPausedChanged, m_renderQueue, &RenderQueue::onPaused);
    connect(m_worker, &PlayerWorker::exportEnded, this,
            [this](bool ok, const QString &path, const QString &reason, const QStringList &log) {
        m_exporting = false;
        emit exportingChanged();
        m_renderQueue->onEnded(ok, path, reason, log);
    });
}

QVariantList Backend::captionCuesForRender() const {
    return captionCues();
}

int Backend::captionTrackIndex() const {
    for (const QVariant &v : m_tracks) {
        const QVariantMap t = v.toMap();
        if (t.value("mix").toMap().value("editKind")=="caption" || t.value(QStringLiteral("name")).toString() == QStringLiteral("Captions"))
            return t.value(QStringLiteral("index")).toInt();
    }
    return -1;
}

// Whether a job in the queue was made from the settings the panel shows: every
// setting the panel has is the job's too. (What queueing adds to a job, and
// what it may settle for it, is left out: the files it may replace, the
// encoder it fell back to, its captions' track.)
static bool sameRenderSettings(const QVariantMap &panel, const QVariantMap &queued) {
    for (auto it = panel.begin(); it != panel.end(); ++it) {
        if (it.key() == QLatin1String("overwritePaths") || it.key() == QLatin1String("hw") || it.key() == QLatin1String("captionTrack")) continue;
        if (QJsonValue::fromVariant(it.value()) != QJsonValue::fromVariant(queued.value(it.key()))) return false;
    }
    return true;
}

QStringList Backend::reservedRenderOutputs() const {
    QStringList paths;
    for (const auto &value : m_renderQueue->jobs()) {
        const auto job = value.toMap();
        if (QStringList{"queued", "rendering", "paused"}.contains(job.value("status").toString()))
            paths.append(ExportSpec::outputPaths(job.value("path").toString(), job.value("spec").toMap()));
    }
    return paths;
}

int Backend::enqueueRender(const QString &name, const QString &path, const QVariantMap &specIn,
                           const QString &presetName, const QStringList &warnings) {
    if (path.isEmpty())
        return -1;
    QVariantMap spec = m_hardware->resolvedSpec(ExportSpec::normalized(plainVariant(specIn).toMap()));
    spec.insert(QStringLiteral("captionTrack"), captionTrackIndex());
    spec.insert(QStringLiteral("captionCues"), captionCuesForRender());
    spec.insert(QStringLiteral("captionTracks"), captionTrackCues());
    QString error;const auto jobs=ExportSpec::jobs(name,path,spec,m_duration,m_fps,audioRenderTracks(),&error,audioCategoriesInUse());
    if(!error.isEmpty()){setStatus(error);return -1;}
    QStringList approved;
    for (const auto &p : spec.value("overwritePaths").toStringList()) approved.append(ExportSpec::outputKey(p));
    const auto checks = ExportSpec::checkOutputs(jobs, reservedRenderOutputs());
    for (const auto &value : checks) {
        const auto check = value.toMap();
        if (check.value("level") == "error" || (check.contains("overwritePath") && !approved.contains(check.value("overwritePath").toString()))) {
            const auto message = check.value("text").toString() + tr(". Review the render destinations before adding this job.");
            setStatus(message); emit failed(message); return -1;
        }
    }
    int first=-1;
    for(const auto &v:jobs){const auto job=v.toMap();auto settings=job.value("spec").toMap();settings["snapshotReady"]=false;
        QStringList overwrite;
        for (const auto &p : ExportSpec::outputPaths(job.value("path").toString(), settings))
            if (approved.contains(ExportSpec::outputKey(p))) overwrite.append(ExportSpec::outputKey(p));
        settings["overwritePaths"] = overwrite;
        auto jobWarnings=warnings;
        if(settings.value("hw").toString()!="software" && !m_hardware->encodersForSpec(settings).value(ExportSpec::videoEncoder(settings),true).toBool() && settings.value("softwareFallback",true).toBool()) {
            settings["hw"]="software";jobWarnings.append("Hardware encoding is unavailable; this job will use software.");
        }
        settings["projectName"]=projectName();
        const int id=m_renderQueue->enqueue(job.value("name").toString(),job.value("path").toString(),settings,presetName,jobWarnings);
        if(first<0)first=id;
        if(id>=0)QMetaObject::invokeMethod(m_worker,[this,id]{m_worker->prepareRenderSnapshot(id);},Qt::QueuedConnection);
    }
    return first;
}

QStringList Backend::missingMedia() const {
    QStringList names;
    QSet<QString> seen;
    const QList<QVariantList> lists{m_timeline, m_connected};
    for (const QVariantList &list : lists) {
        for (const QVariant &v : list) {
            const QVariantMap e = v.toMap();
            const QString path = e.value(ClipKey::path).toString();
            names.append(Timeline::gradeLutProblems(e));
            // Only absolute paths are files: generators (color:, noise,
            // count…) and a title's "#00000000" colour resource are not media.
            if (path.isEmpty() || !path.startsWith(QLatin1Char('/')))
                continue;
            if (seen.contains(path))
                continue;
            seen.insert(path);
            if (e.value(QStringLiteral("missing")).toBool() || !QFileInfo::exists(path))
                names << QFileInfo(path).fileName();
        }
    }
    return names;
}

QStringList Backend::missingServices() const {
    QSet<QString> needed;
    const QList<QVariantList> lists{m_timeline, m_connected};
    for (const QVariantList &list : lists) {
        for (const QVariant &v : list) {
            const QVariantMap e = v.toMap();
            if (e.value(ClipKey::cropFeather).toDouble() > 0.0 || e.value(ClipKey::cropInvert).toBool())
                needed << QStringLiteral("filter:frei0r.alphaspot");
            if (e.value(ClipKey::generator).toString() == QStringLiteral("bars"))
                needed << QStringLiteral("producer:frei0r.test_pat_B");
            if (e.value(ClipKey::generator).toString() == QStringLiteral("noise"))
                needed << QStringLiteral("producer:noise");
            if (e.value(ClipKey::generator).toString() == QStringLiteral("count"))
                needed << QStringLiteral("producer:count");
            if (e.contains(ClipKey::stab) && e.value(ClipKey::stab).toBool())
                needed << QStringLiteral("filter:vidstab");
            if (e.value(ClipKey::lensK1).toDouble() != 0.0 || e.value(ClipKey::lensK2).toDouble() != 0.0)
                needed << QStringLiteral("filter:avfilter.lenscorrection");
            if (!e.value(ClipKey::blend).toString().isEmpty() && e.value(ClipKey::blend).toString() != QStringLiteral("normal"))
                needed << QStringLiteral("filter:qtblend_mode");
            if (!e.value(ClipKey::title).toString().isEmpty())
                needed << QStringLiteral("filter:dynamictext");
            if (e.contains(ClipKey::speed) && qAbs(e.value(ClipKey::speed).toDouble() - 1.0) > 0.0001)
                needed << QStringLiteral("producer:timewarp");
            if (e.contains(ClipKey::freeze))
                needed << QStringLiteral("filter:freeze");
            if (e.value(ClipKey::flipH).toBool())
                needed << QStringLiteral("filter:avfilter.hflip");
            if (e.value(ClipKey::flipV).toBool())
                needed << QStringLiteral("filter:avfilter.vflip");
            if (e.value(ClipKey::normalize).toBool())
                needed << QStringLiteral("filter:dynamic_loudness");
        }
    }
    QStringList missing;
    for (const QString &s : needed) {
        const int colon = s.indexOf(QLatin1Char(':'));
        if (!Timeline::serviceAvailable(s.left(colon), s.mid(colon + 1)))
            missing << s.mid(colon + 1);
    }
    missing.sort();
    return missing;
}

QVariantMap Backend::renderContext(const QVariantMap &specIn, const QString &path) const {
    QVariantMap spec = m_hardware->resolvedSpec(ExportSpec::normalized(plainVariant(specIn).toMap()));
    spec["captionCues"] = captionCuesForRender();
    spec["captionTracks"] = captionTrackCues();
    QVariantMap ctx;
    ctx.insert(QStringLiteral("path"), path);
    ctx.insert(QStringLiteral("reframing"), reframing());
    ctx.insert("tracks",m_tracks);
    const QFileInfo info(path);
    const QDir dir = info.dir();
    ctx.insert(QStringLiteral("dirExists"), !path.isEmpty() && dir.exists());
    ctx.insert(QStringLiteral("dirWritable"), !path.isEmpty() && dir.exists() && QFileInfo(dir.absolutePath()).isWritable());
    ctx.insert(QStringLiteral("fileExists"), !path.isEmpty() && info.exists());
    ctx.insert(QStringLiteral("hasVideo"), hasVideo());
    ctx.insert(QStringLiteral("fps"), m_fps);
    ctx.insert(QStringLiteral("width"), m_videoWidth);
    ctx.insert(QStringLiteral("height"), m_videoHeight);
    int rin = spec.value(QStringLiteral("rangeIn"), -1).toInt();
    int rout = spec.value(QStringLiteral("rangeOut"), -1).toInt();
    if (rin < 0) rin = 0;
    if (rout < 0 || rout >= m_duration) rout = m_duration - 1;
    const int frames = qMax(0, rout - rin + 1);
    ctx.insert(QStringLiteral("seconds"), m_fps > 0.0 ? frames / m_fps : 0.0);
    const RenderTimelineChecks &timeline = renderTimelineChecks();
    ctx.insert(QStringLiteral("hasAudio"), timeline.hasAudio);
    ctx.insert(QStringLiteral("hasCaptions"), !captionCues().isEmpty());
    ctx.insert(QStringLiteral("captionTracks"), spec.value(QStringLiteral("captionTracks")));
    // (Files come and go whatever the timeline does: looked at every time.)
    QStringList missing = timeline.clipProblems;
    for (const auto &[file, name, flagged] : timeline.media)
        if (flagged || !QFileInfo::exists(file)) missing << name;
    ctx.insert(QStringLiteral("missingMedia"), missing);
    ctx.insert(QStringLiteral("missingServices"), timeline.missingServices);
    // The caption checks' findings, said when captions are delivered.
    ctx.insert(QStringLiteral("captionChecks"), captionCheckWarnings());
    if (spec.value(QStringLiteral("video")).toBool())
        ctx.insert(QStringLiteral("eightBitEffects"), timeline.eightBitEffects);
    ctx["managedOutput"] = timeline.managedOutput;
    if (timeline.managedOutput)
        ctx["linearError"] = timeline.linearError;
    ctx.insert(QStringLiteral("encoders"), m_hardware->encodersForSpec(spec));
    qint64 freeBytes = -1; // unknown: no folder to measure yet
    if (dir.exists()) {
        const QStorageInfo volume(dir.absolutePath());
        if (volume.isValid() && volume.isReady())
            freeBytes = volume.bytesAvailable();
    }
    ctx.insert(QStringLiteral("freeBytes"), freeBytes);
    qint64 estimate=0;QString batchError;
    const auto batch=ExportSpec::jobs("Estimate",path,spec,m_duration,m_fps,timeline.audioTracks,&batchError,timeline.categoriesInUse);
    if (!batchError.isEmpty())
        ctx["outputChecks"] = QVariantList{QVariantMap{{"level", "error"}, {"text", batchError}}};
    else {
        // Settings just queued still stand in the panel, aimed at the file
        // their job will write: that is the step just taken, not two renders
        // after one file, and is said so. A job waiting with other settings
        // for the same file is the clash it always was; and adding a job is
        // refused for either (enqueueRender), so one file is never queued twice.
        QStringList reserved; QVariantList checks;
        for (const auto &value : m_renderQueue->jobs()) {
            const auto job = value.toMap();
            if (!QStringList{"queued", "rendering", "paused"}.contains(job.value("status").toString())) continue;
            bool same = false;
            for (const auto &b : batch)
                same |= ExportSpec::outputKey(b.toMap().value("path").toString()) == ExportSpec::outputKey(job.value("path").toString())
                        && sameRenderSettings(b.toMap().value("spec").toMap(), job.value("spec").toMap());
            if (same) checks.append(QVariantMap{{"level", "ok"}, {"queued", true},
                                               {"text", tr("In the queue with these settings: %1").arg(job.value("path").toString())}});
            else reserved.append(ExportSpec::outputPaths(job.value("path").toString(), job.value("spec").toMap()));
        }
        ctx["outputChecks"] = ExportSpec::checkOutputs(batch, reserved) + checks;
    }
    for(const auto &v:batch){const auto settings=v.toMap().value("spec").toMap();const double seconds=(settings.value("rangeOut").toInt()-settings.value("rangeIn").toInt()+1)/qMax(1.,m_fps);estimate+=ExportSpec::estimateBytes(settings,seconds,m_videoWidth,m_videoHeight,m_fps);}
    if(batchError.isEmpty())ctx["estimatedBytes"]=estimate;
    return ctx;
}

const Backend::RenderTimelineChecks &Backend::renderTimelineChecks() const {
    RenderTimelineChecks &c = m_renderTimelineChecks;
    const bool engine = render::engineRenderingRequested();
    const QVariantList categories = m_mediaCatalog->categories();
    if (c.valid && c.timeline == m_timeline && c.connected == m_connected && c.tracks == m_tracks && c.project == m_projectSettings
        && c.categories == categories && c.width == m_videoWidth && c.height == m_videoHeight && c.fps == m_fps && c.engine == engine)
        return c;
    c = RenderTimelineChecks{};
    c.valid = true;
    c.timeline = m_timeline; c.connected = m_connected; c.tracks = m_tracks; c.project = m_projectSettings; c.categories = categories;
    c.width = m_videoWidth; c.height = m_videoHeight; c.fps = m_fps; c.engine = engine;
    c.audioTracks = audioRenderTracks();
    c.categoriesInUse = audioCategoriesInUse();
    const QList<QVariantList> lists{m_timeline, m_connected};
    // As missingMedia() looks at the clips, the files' own check left for each time.
    QSet<QString> seen;
    for (const QVariantList &list : lists)
        for (const QVariant &v : list) {
            const QVariantMap e = v.toMap();
            const QString file = e.value(ClipKey::path).toString();
            c.clipProblems.append(Timeline::gradeLutProblems(e));
            if (file.isEmpty() || !file.startsWith(QLatin1Char('/')) || seen.contains(file))
                continue;
            seen.insert(file);
            c.media.append({file, QFileInfo(file).fileName(), e.value(QStringLiteral("missing")).toBool()});
        }
    for (const QVariantList &list : lists)
        for (const QVariant &v : list)
            if (v.toMap().value(QStringLiteral("audio"), true).toBool() && !v.toMap().value(QStringLiteral("videoOnly")).toBool())
                c.hasAudio = true;
    c.missingServices = missingServices();
    for (const QVariantList &list : lists)
        for (const QVariant &v : list) {
            const QVariantMap e = v.toMap();
            if (e.value(ClipKey::disabled).toBool() || e.value(ClipKey::audioOnly).toBool()
                || e.value(ClipKey::bypass).toStringList().contains(QStringLiteral("effects")))
                continue;
            const QStringList effects = VideoEffects::eightBit(e.value(ClipKey::videoEffects).toList());
            if (effects.isEmpty())
                continue;
            const QString label = e.value(ClipKey::label).toString();
            c.eightBitEffects << QVariantMap{{QStringLiteral("clip"), label.isEmpty() ? e.value(ClipKey::name).toString() : label},
                                             {QStringLiteral("effects"), effects}};
        }
    QVariantList entries = m_timeline; entries.append(m_connected);
    c.managedOutput = LinearRenderer::enabled(entries, m_projectSettings);
    if (c.managedOutput) {
        // The engine draws the export when it can; the CPU renderer's own limits otherwise.
        if (engine && render::engineUnsupported(entries, m_tracks, m_projectSettings, QSize(m_videoWidth, m_videoHeight), m_fps).isEmpty())
            c.linearError = QString();
        else {
            LinearRenderer renderer; renderer.configure(entries, m_tracks, m_projectSettings, QSize(m_videoWidth, m_videoHeight), m_fps);
            c.linearError = renderer.validate();
        }
    }
    return c;
}

QVariantList Backend::renderStems(const QVariantMap &spec, const QString &path) const {
    QVariantList stems;
    QString error;
    const auto jobs = ExportSpec::jobs(QStringLiteral("Stems"), path.isEmpty() ? QStringLiteral("render.wav") : path, plainVariant(spec).toMap(),
                                       m_duration, m_fps, audioRenderTracks(), &error, audioCategoriesInUse());
    for (int i = 1; i < jobs.size(); ++i) {
        const QVariantMap job = jobs.at(i).toMap();
        if (!job.contains("stem")) break; // the next part's main file
        stems.append(QVariantMap{{"name", job.value("stem")}, {"file", QFileInfo(job.value("path").toString()).fileName()}});
    }
    return stems;
}

QVariantList Backend::validateRender(const QVariantMap &spec, const QString &path) const {
    return ExportSpec::validate(spec, renderContext(spec, path));
}

QVariantMap Backend::renderEstimate(const QVariantMap &spec, const QString &path) const {
    return renderEstimateFrom(spec, renderContext(spec, path));
}

QVariantMap Backend::renderChecks(const QVariantMap &spec, const QString &path) const {
    const QVariantMap ctx = renderContext(spec, path);
    return {{QStringLiteral("validation"), ExportSpec::validate(spec, ctx)}, {QStringLiteral("estimate"), renderEstimateFrom(spec, ctx)}};
}

QString Backend::tidyRenderName(const QString &name) const {
    return ExportSpec::tidyFileName(name);
}

QVariantMap Backend::renderEstimateFrom(const QVariantMap &spec, const QVariantMap &ctx) const {
    QVariantMap out;
    const qint64 bytes = ctx.value("estimatedBytes").toLongLong();
    out.insert(QStringLiteral("bytes"), bytes);
    out.insert(QStringLiteral("freeBytes"), ctx.value(QStringLiteral("freeBytes")));
    out.insert(QStringLiteral("seconds"), ctx.value(QStringLiteral("seconds")));
    out.insert(QStringLiteral("bytesText"), QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeSIFormat));
    // An unknown figure stays blank, never a misleading "0 bytes".
    const qint64 freeBytes = ctx.value(QStringLiteral("freeBytes")).toLongLong();
    out.insert(QStringLiteral("freeText"), freeBytes < 0 ? QString() : QLocale().formattedDataSize(freeBytes, 2, QLocale::DataSizeSIFormat));
    // The timecode the file will say it starts at: the timeline's, at the
    // first frame of the range. Blank for a kind of file that holds none.
    if (m_fps > 0) {
        const QVariantMap normal = ExportSpec::normalized(plainVariant(spec).toMap());
        const auto [fpsNum, fpsDen] = timelineRate();
        int toNum = fpsNum, toDen = fpsDen;
        if (const QString converted = ExportSpec::sequenceFormat(normal).isEmpty() ? ExportSpec::convertedRate(normal, m_fps) : QString(); !converted.isEmpty()) {
            const QStringList parts = converted.split(QLatin1Char('/'));
            toNum = parts.size() == 2 ? parts.at(0).toInt() : qRound(converted.toDouble() * 1000);
            toDen = parts.size() == 2 ? parts.at(1).toInt() : 1000;
        }
        ExportSpec::EncoderSettings settings = ExportSpec::encoderSettings(normal, double(toNum) / qMax(1, toDen), false);
        const qint64 first = qint64(startFrame()) + qMax(0, normal.value(QStringLiteral("rangeIn")).toInt());
        ExportSpec::stampStart(settings, first, fpsNum, fpsDen, toNum, toDen);
        out.insert(QStringLiteral("startText"), !settings.timecode.isEmpty() ? settings.timecode
            : settings.timeReference >= 0 ? tr("%1 (time reference)").arg(ExportSpec::startTimecode(first, fpsNum, fpsDen, fpsNum, fpsDen)) : QString());
    }
    return out;
}

QString Backend::renderSummary(const QVariantMap &spec) const {
    return ExportSpec::summary(spec, m_videoWidth, m_videoHeight, m_fps);
}

QString Backend::renderExtension(const QVariantMap &spec) const {
    return ExportSpec::extension(ExportSpec::normalized(spec));
}

QString Backend::renderFileName(const QString &pattern, const QString &presetName, const QString &dir,
                                const QVariantMap &specIn) const {
    const QVariantMap spec = ExportSpec::normalized(plainVariant(specIn).toMap());
    QVariantMap tokens;
    tokens.insert(QStringLiteral("project"), projectName());
    QString sequence = QStringLiteral("Sequence 1");
    if (m_activeSequence >= 0 && m_activeSequence < m_sequences.size())
        sequence = m_sequences.at(m_activeSequence).toMap().value(QStringLiteral("name")).toString();
    tokens.insert(QStringLiteral("sequence"), sequence);
    tokens.insert(QStringLiteral("preset"), presetName.isEmpty() ? QStringLiteral("Custom") : presetName);
    const int w = spec.value(QStringLiteral("width")).toInt() > 0 ? spec.value(QStringLiteral("width")).toInt() : m_videoWidth;
    const int h = spec.value(QStringLiteral("height")).toInt() > 0 ? spec.value(QStringLiteral("height")).toInt() : m_videoHeight;
    tokens.insert(QStringLiteral("resolution"), w > 0 ? QStringLiteral("%1x%2").arg(w).arg(h) : QString());
    const double f = spec.value(QStringLiteral("fps")).toDouble() > 0.0 ? spec.value(QStringLiteral("fps")).toDouble() : m_fps;
    tokens.insert(QStringLiteral("fps"), f > 0.0 ? QString::number(f, 'g', 6) + QStringLiteral("fps") : QString());
    return ExportSpec::expandFileName(pattern, tokens, dir, ExportSpec::extension(spec));
}

QString Backend::defaultRenderDir() const {
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return movies.isEmpty() ? QDir::homePath() : movies;
}

void Backend::chooseRenderFolder() {
    m_chooserMode = ChooserMode::RenderFolder;
    m_chooser->openDirectory(QStringLiteral("Save Location"));
}

void Backend::quitWhenRendersFinish() {
    if (!m_renderQueue->active() && m_renderQueue->pendingCount() == 0) {
        QCoreApplication::quit();
        return;
    }
    m_quitWhenIdle = true;
}

void Backend::openRenderOutput(int id) {
    const QString path = m_renderQueue->job(id).value(QStringLiteral("path")).toString();
    if (!path.isEmpty() && QFileInfo::exists(path) && !QStandardPaths::isTestModeEnabled())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Backend::revealRenderOutput(int id) {
    const QString path = m_renderQueue->job(id).value(QStringLiteral("path")).toString();
    if (path.isEmpty() || QStandardPaths::isTestModeEnabled())
        return;
    // The portal's "show in folder" when the desktop offers it; the folder otherwise.
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    msg << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 1500);
    if (reply.type() == QDBusMessage::ErrorMessage)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
