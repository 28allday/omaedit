#include "playerworker_p.h"

// Render and Replace's intermediate encoding (startRenderStage).
static constexpr char kRenderSpace[] = "rec2020/pq";

void PlayerWorker::renderAndReplace(int track, int index) {
    if (exportActive() || m_render) {
        emit renderProgress(track, index, -1);
        return;
    }
    const QVariantMap me = m_timeline.entryMapOn(track, index);
    if (me.isEmpty() || me.value(ClipKey::gap).toBool()
        || !me.value(ClipKey::title).toString().isEmpty()
        || me.value(ClipKey::mixIn).toInt() > 0 || me.value(ClipKey::mixOut).toInt() > 0
        || me.value(ClipKey::path).toString().startsWith(QStringLiteral("color:"))) {
        emit renderProgress(track, index, -1);
        return;
    }
    // The clip alone on a scratch timeline: same material, same
    // attributes, at frame 0.
    QVariantMap solo = me;
    solo.insert(ClipKey::lane, 0);
    solo.insert(ClipKey::track, 0);
    solo.insert(ClipKey::index, 0);
    solo.insert(ClipKey::start, 0);
    solo.insert(ClipKey::gapBefore, 0);
    solo.remove(ClipKey::link);
    solo.remove(ClipKey::original);
    solo.remove(ClipKey::markers);
    solo.remove(ClipKey::blend);   // composites against nothing here
    solo.remove(ClipKey::disabled);
    // Linked sound will follow this render. Keep the source sound in the file;
    // the replacement entry retains its original video-only flag below.
    solo.remove(ClipKey::videoOnly);
    if (me.value(ClipKey::audioOnly).toBool())
        solo.remove(ClipKey::audioOnly); // V1 carries it as audio + black
    auto job = std::make_unique<RenderJob>();
    job->track = track;
    job->index = index;
    job->sequenceId = m_activeSequence >= 0 && m_activeSequence < int(m_sequences.size()) ? m_sequences[m_activeSequence].id : QString();
    job->generation = m_projectGeneration;
    job->frames = me.value(ClipKey::frames).toInt();
    job->original = me;
    job->settings = resolvedTimelineSettings();
    job->sources = m_timeline.mediaSources();
    // The project will point at this file: kept, not cached.
    const QDir dir(StoragePreferences::keptMediaRoot()
                   + QStringLiteral("/renders"));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    job->path = dir.absoluteFilePath(
        QStringLiteral("render-%1-%2.mkv")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
            .arg(QFileInfo(me.value(ClipKey::path).toString()).completeBaseName()
                     .left(24).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"))));
    // A frame-blended retime: ffmpeg makes the 1× intermediate first, and
    // the render stage bakes the rest of the attributes onto THAT.
    if (wantsBlendRender(me)) {
        auto sampling = blendIntermediate(me, job->settings, job->sources);
        const QString inter = sampling.out;
        if (inter.isEmpty()) {
            emit frameError(sampling.error);
            emit renderProgress(track, index, -1);
            return;
        }
        solo = sampledEntry(solo, inter, job->settings);
        if (!sampling.args.isEmpty()) {
            job->solo = solo;
            job->sampling = std::move(sampling);
            job->pre = std::make_unique<QProcess>();
            job->pre->setProgram(job->sampling.args.takeFirst());
            job->pre->setArguments(job->sampling.args);
            job->pre->setProcessChannelMode(QProcess::ForwardedErrorChannel);
            RenderJob *raw = job.get();
            connect(job->pre.get(), &QProcess::finished, this,
                    [this, raw](int code, QProcess::ExitStatus status) {
                        if (!m_render || m_render.get() != raw)
                            return;
                        const bool ok = status == QProcess::NormalExit && code == 0
                                        && publishSamplingFile(raw->sampling.stage, raw->sampling.out);
                        if (!ok) {
                            finishRender(false);
                            return;
                        }
                        startRenderStage();
                    });
            connect(job->pre.get(), &QProcess::errorOccurred, this, [this, raw](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart || !m_render || m_render.get() != raw)
                    return; // finished() never comes for a program that could not start
                finishRender(false);
            });
            m_render = std::move(job);
            m_render->pre->start();
            emit renderProgress(track, index, 0);
            m_renderPoll.start(250);
            return;
        }
    }
    job->solo = solo;
    m_render = std::move(job);
    startRenderStage();
}

bool PlayerWorker::wantsBlendRender(const QVariantMap &me) {
    return FrameSampling::interpolates(me.value(ClipKey::frameBlend).toInt())
           && !me.contains(ClipKey::retime)
           && !me.contains(ClipKey::sourceOrigin)
           && !qFuzzyCompare(me.value(ClipKey::speed, 1.0).toDouble(), 1.0)
           && !me.value(ClipKey::path).toString().startsWith(QStringLiteral("color:"))
           && !me.contains(ClipKey::compoundJson)
           && me.value(ClipKey::title).toString().isEmpty()
           && !me.value(ClipKey::audioOnly).toBool()
           && !me.value(ClipKey::gap).toBool();
}

PlayerWorker::PreJob PlayerWorker::blendIntermediate(const QVariantMap &me, const QVariantMap &profile,
                                                      const QVariantMap &sources) const {
    PreJob job;
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        job.error = tr("ffmpeg is needed to bake a frame-blended retime");
        return job;
    }
    const QString path = me.value(ClipKey::path).toString();
    // The material runs at the user's speed times any frame-rate interpretation.
    const double speed = me.value(ClipKey::speed, 1.0).toDouble()
                         * Timeline::interpretationFactor(sources.value(path, me.value(ClipKey::sourceMeta)).toMap());
    const int blend = me.value(ClipKey::frameBlend).toInt();
    const int num = profile.value("fpsNum").toInt(), den = profile.value("fpsDen").toInt();
    const double fps = num > 0 && den > 0 ? double(num) / den : profile.value("fps",25.).toDouble();
    const QString rate = num > 0 && den > 0 ? QString("%1/%2").arg(num).arg(den) : QString::number(fps,'g',17);
    const double a = qAbs(speed);
    const int in = me.value(ClipKey::in).toInt();
    const int out = me.value(ClipKey::out).toInt();
    const int frames = me.value(ClipKey::frames).toInt();
    if (!std::isfinite(a) || a <= 0. || !std::isfinite(fps) || fps <= 0.
        || frames < 1 || in < 0 || out < in
        || (double(out) + 1.) * a > std::numeric_limits<int>::max()) {
        job.error = tr("The clip has invalid timing for frame sampling.");
        return job;
    }
    // Work in original native pictures. Rounded timewarp lengths lose reverse
    // endpoints; rounded trim coordinates lose fractional interpolation phase.
    MediaPlayer original;
    if (!original.open(path)) {
        job.error = tr("The source could not be opened for frame sampling.");
        return job;
    }
    const double sourceFps = original.fps();
    const int sourceFrames = original.frameCount();
    const double step = a * sourceFps / fps;
    if (!std::isfinite(sourceFps) || sourceFps <= 0. || sourceFrames < 1
        || !std::isfinite(step) || step <= 0.) {
        job.error = tr("The clip has invalid timing for frame sampling.");
        return job;
    }
    const auto position = [&](int frame) {
        return qBound(0., speed < 0. ? sourceFrames - 1. - frame * step : frame * step,
                      double(sourceFrames - 1));
    };
    const double first = position(in), last = position(out);
    // Include the neighbouring native picture even when it lies beyond the
    // cut: interpolation needs that handle. Only physical EOF repeats a frame.
    const int srcIn = int(std::floor(qMin(first, last)));
    const int srcOut = int(std::ceil(qMax(first, last)));
    const double sourceOffset = (speed < 0. ? srcOut - first : first - srcIn) / sourceFps;
    const double phase = sourceOffset / a;
    QString vf;
    if (speed < 0.0)
        vf += QStringLiteral("reverse,");
    // scd=none: blend across everything, a hard cut inside the clip too —
    // the detector would otherwise quietly repeat frames there.
    // The interpolator needs future pictures before it releases its final
    // pair. Pad its input with the last SOURCE picture, including for clips
    // shorter than its lookahead. Padding the output repeats an earlier pair
    // and produces no pictures at all for one- and two-frame sources.
    // -frames:v bounds this padding to the requested timeline duration.
    // Drop the negative interpolation preroll before muxing, which would
    // otherwise normalize it back to zero and discard the intended phase.
    vf += QStringLiteral("setpts=(PTS-STARTPTS)/%1-%4/TB,tpad=stop_mode=clone:stop=-1,minterpolate=fps=%2:scd=none:mi_mode=%3,trim=start=0,setpts=PTS-STARTPTS")
              .arg(QString::number(a, 'g', 17), rate,
                   blend == 2 ? QStringLiteral("mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1")
                              : QStringLiteral("blend"), QString::number(phase, 'g', 17));
    // A connected clip may still carry embedded sound; Render & Replace also
    // needs it when the linked audio switches to the completed render. Keep
    // pitch while changing tempo, including reverse and rates outside one
    // atempo stage's interpolation range.
    QStringList audioFilters;
    if (speed < 0.) audioFilters.append(QStringLiteral("areverse"));
    audioFilters.append(QStringLiteral("atrim=start=%1").arg(QString::number(sourceOffset,'g',17)));
    audioFilters.append(QStringLiteral("asetpts=PTS-STARTPTS"));
    double tempo = a;
    while (tempo > 2.) { audioFilters.append(QStringLiteral("atempo=2")); tempo /= 2.; }
    while (tempo < .5) { audioFilters.append(QStringLiteral("atempo=0.5")); tempo *= 2.; }
    audioFilters.append(QStringLiteral("atempo=%1").arg(QString::number(tempo,'g',17)));
    const QString duration = QString::number(frames / fps,'g',17);
    audioFilters.append(QStringLiteral("apad=whole_dur=%1").arg(duration));
    audioFilters.append(QStringLiteral("atrim=duration=%1").arg(duration));
    const QString af = audioFilters.join(',');
    // Replacing media at the same path must not reuse the old picture.
    // Structured values also retain full rate precision in the cache identity.
    const QFileInfo source(path);
    const auto key=QJsonDocument::fromVariant(QVariantMap{
        {"path",path},{"canonical",source.canonicalFilePath()},
        {"bytes",source.size()},{"modified",source.lastModified().toMSecsSinceEpoch()},
        {"in",srcIn},{"out",srcOut},{"speed",speed},{"mode",blend},
        {"fps",fps},{"sourceFps",sourceFps},{"frames",frames},{"filter",vf},{"audioFilter",af}}).toJson(QJsonDocument::Compact);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    const QDir dir(StoragePreferences::cacheRoot()
                   + QStringLiteral("/omaedit-renders"));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    const QString file = dir.absoluteFilePath(QStringLiteral("blend-%1.mov").arg(hash));
    if (completeSamplingFile(file)) { job.out = file; return job; }
    job.directory = std::make_shared<QTemporaryDir>(dir.filePath(QStringLiteral(".omaedit-sampling-XXXXXX")));
    if (!job.directory->isValid()) {
        job.error = tr("Could not create temporary files in the render cache.");
        return job;
    }
    job.out = file;
    job.stage = job.directory->filePath(QStringLiteral("intermediate.mov"));
    job.args = {ffmpeg, QStringLiteral("-y"), QStringLiteral("-nostdin"),
             QStringLiteral("-loglevel"), QStringLiteral("error"),
             QStringLiteral("-progress"), QStringLiteral("pipe:1"),
             QStringLiteral("-ss"), QString::number(qMax(0., srcIn / sourceFps - 1e-7), 'f', 9),
             QStringLiteral("-t"), QString::number((srcOut - srcIn + 1) / sourceFps, 'f', 9),
             QStringLiteral("-i"), path,
             QStringLiteral("-sn"), QStringLiteral("-dn"),
             QStringLiteral("-vf"), vf, QStringLiteral("-af"), af, QStringLiteral("-r"), rate,
             QStringLiteral("-frames:v"), QString::number(frames),
             QStringLiteral("-c:v"), QStringLiteral("prores_ks"),
             QStringLiteral("-profile:v"), QStringLiteral("3"),
             QStringLiteral("-pix_fmt"), QStringLiteral("yuv422p10le"),
             QStringLiteral("-c:a"), QStringLiteral("pcm_s24le"),
             QStringLiteral("-f"), QStringLiteral("mov"), job.stage};
    return job;
}

// ffmpeg -progress on stdout: out_time_us=… lines against the expected length.
int PlayerWorker::ffmpegPercent(QProcess *proc, double seconds, int *last) {
    if (!proc)
        return *last;
    while (proc->canReadLine()) {
        const QByteArray line = proc->readLine().trimmed();
        if (line.startsWith("out_time_us=")) {
            const double us = line.mid(12).toDouble();
            if (seconds > 0.0)
                *last = qBound(*last, int(100.0 * us / 1e6 / seconds), 99);
        }
    }
    return *last;
}

void PlayerWorker::startRenderStage() {
    if (!m_render)
        return;
    RenderJob *job = m_render.get();
    const int track = job->track, index = job->index;
    job->pre.reset();
    job->scratch = std::make_unique<Timeline>();
    job->scratch->setProjectSettings(job->settings);
    job->scratch->setMediaSources(job->sources, QStringLiteral("original"));
    if (!job->scratch->restore({job->solo}) || job->scratch->frameCount() != job->frames) {
        finishRender(false);
        return;
    }
    // Lossless 16-bit RGB in Rec.2020 PQ: the clip's light, far above
    // white and across a wide gamut, survives the integer file. The
    // replacement reads it back with that input space (finishRender).
    const QString problem = job->scratch->prepareLinearOutput(QString::fromLatin1(kRenderSpace));
    if (!problem.isEmpty()) {
        emit editRefused(problem);
        finishRender(false);
        return;
    }
    ExportSpec::EncoderSettings settings;
    settings.muxer = QStringLiteral("matroska");
    settings.vcodec = QStringLiteral("ffv1");
    settings.pixFmt = QStringLiteral("rgba64le");
    settings.videoOptions = {{QStringLiteral("level"), QStringLiteral("3")}, {QStringLiteral("g"), QStringLiteral("1")}};
    settings.primaries = QStringLiteral("bt2020");
    settings.trc = QStringLiteral("smpte2084");
    settings.matrix = QStringLiteral("gbr");
    settings.range = QStringLiteral("pc");
    settings.colorspace = 2020;
    settings.progressive = job->scratch->profile()->progressive();
    settings.sarNum = job->scratch->profile()->sample_aspect_num();
    settings.sarDen = job->scratch->profile()->sample_aspect_den();
    settings.acodec = QStringLiteral("pcm_s16le");
    settings.arate = job->settings.value("sampleRate", 48000).toInt();
    settings.channels = job->settings.value("channels", 2).toInt();
    auto sink = std::make_unique<SinkJob>();
    SinkJob *j = sink.get();
    job->request = sink->request;
    Timeline *scratch = job->scratch.get();
    const QString path = job->path;
    const int last = job->frames - 1;
    sink->thread = std::thread([j, scratch, path, settings, last, request = sink->request] {
        QString error;
        bool renderFailed = false;
        const bool ok = scratch->writeDelivery(path, settings, 0, last, [j](int frame) {
            j->written = frame + 1;
            return !j->stop.load();
        }, &error, &renderFailed, request);
        j->ok = ok;
        j->renderFailed = renderFailed;
        j->error = std::move(error);
        j->done = true;
    });
    job->sink = std::move(sink);
    emit renderProgress(track, index, job->prePercent);
    m_renderPoll.start(250);
}

void PlayerWorker::pollRender() {
    if (!m_render)
        return;
    if (m_render->pre) {
        // The ffmpeg pre-stage: its share is the first half of the bar.
        const double seconds = m_timeline.fps() > 0.0 ? m_render->frames / m_timeline.fps() : 0.0;
        int pc = 0;
        ffmpegPercent(m_render->pre.get(), seconds, &pc);
        m_render->prePercent = qBound(m_render->prePercent, pc / 2, 49);
        emit renderProgress(m_render->track, m_render->index, m_render->prePercent);
        return;
    }
    if (!m_render->sink)
        return;
    if(m_render->request && !m_render->request->error().isEmpty()){finishRender(false);return;}
    if (m_render->sink->done) {
        finishRender(m_render->sink->ok && m_render->sink->written >= m_render->frames);
        return;
    }
    const int base = m_render->prePercent;
    emit renderProgress(m_render->track, m_render->index,
                        qBound(base, base + int((100LL - base) * m_render->sink->written
                                                / qMax(1, m_render->frames)), 99));
}

void PlayerWorker::finishRender(bool ok) {
    m_renderPoll.stop();
    std::unique_ptr<RenderJob> job = std::move(m_render);
    if (!job)
        return;
    if(!ok && job->request)job->request->cancel();
    if (job->pre) {
        job->pre->kill();
        job->pre->waitForFinished(2000);
        job->pre.reset();
    }
    job->sink.reset();  // stops and joins it (abandoning the frame in hand when cancelled)
    job->scratch.reset();
    const int track = job->track, index = job->index;
    const auto problem=job->request?job->request->error():QString();
    if (!ok || !problem.isEmpty() || !QFileInfo::exists(job->path)) {
        QFile::remove(job->path);
        emit renderProgress(track, index, -1);
        if(!problem.isEmpty())emit editRefused(problem);
        return;
    }
    // The editor may have moved on to another sequence or another project
    // while the render ran; the result belongs to the one it started from.
    const QString liveSequence = m_activeSequence >= 0 && m_activeSequence < int(m_sequences.size()) ? m_sequences[m_activeSequence].id : QString();
    if (job->generation != m_projectGeneration || liveSequence != job->sequenceId) {
        QFile::remove(job->path);
        emit renderProgress(track, index, -1);
        return;
    }
    // Edits kept flowing: the clip must still be what it was.
    const QVariantMap now = m_timeline.entryMapOn(track, index);
    const QVariantMap &was = job->original;
    if (MarkerEdit::clipState(now) != MarkerEdit::clipState(was)) {
        QFile::remove(job->path); // nothing will reference it
        emit renderProgress(track, index, -1);
        return;
    }
    const QVariantList before = m_timeline.entries();
    const int start = was.value(ClipKey::start).toInt();
    // A linked (video-only) clip comes back video-only: its sound stays the
    // linked audio clip, which the settle pass re-points at the render.
    if (!m_timeline.overwriteOn(track, job->path, start, 0, job->frames - 1,
                                was.value(ClipKey::audioOnly).toBool(), 1.0,
                                was.value(ClipKey::videoOnly).toBool())) {
        QFile::remove(job->path);
        emit renderProgress(track, index, -1);
        return;
    }
    const int placed = m_timeline.clipIndexAt(track, start);
    if (placed >= 0) {
        // What is not baked in rides: label, category, link, markers, enable.
        QVariantMap carry;
        for (const char *key : {"label", "category", "link", "markers", "disabled"})
            if (was.contains(QLatin1String(key)))
                carry.insert(QLatin1String(key), was.value(QLatin1String(key)));
        if (!carry.isEmpty())
            m_timeline.applyEntryAttributes(track, placed, carry);
        if (was.contains(ClipKey::link))
            m_timeline.setLinkAt(track, placed, was.value(ClipKey::link).toString());
        m_timeline.setOriginalAt(track, placed, was);
        // The file holds the light as Rec.2020 PQ: read it back so.
        QVariantMap grade = Timeline::defaultGrade();
        grade["nodes"] = QVariantList{};
        QVariantMap management = grade.value("management").toMap();
        management["input"] = QStringLiteral("rec2020");
        management["inputGamma"] = QStringLiteral("pq");
        grade["management"] = management;
        m_timeline.setGradeAt(track, placed, grade);
    }
    recordEdit(before, true, tr("Render in place"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
    emit renderProgress(track, index, 100);
}

void PlayerWorker::restoreOriginal(int track, int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantMap me = m_timeline.entryMapOn(track, index);
    const QVariantMap original = me.value(ClipKey::original).toMap();
    if (original.isEmpty())
        return;
    const QVariantList before = m_timeline.entries();
    QVariantMap back = original;
    // Whatever changed on the rendered clip since is the newer truth.
    for (const char *key : {"label", "category", "link", "markers", "disabled"})
        if (me.contains(QLatin1String(key)))
            back.insert(QLatin1String(key), me.value(QLatin1String(key)));
    back.remove(ClipKey::original);
    if (!m_timeline.pasteClip(track, me.value(ClipKey::start).toInt(), back, false))
        return;
    recordEdit(before, true, tr("Restore original"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}
