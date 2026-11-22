#include "playerworker_p.h"
#include "sink.h"

// ── Export (the Deliver page's render job) ───────────────────────────────
// Stages: pre (frame-blended retimes bake through ffmpeg) → render (the
// encoder sink on a SCRATCH restore of the timeline: the delivery file) →
// concat (segments of a paused-and-resumed render stage) → post (frame-rate
// conversion) → mux (embedded captions). Every ffmpeg stage pauses with
// SIGSTOP; the render stage checkpoints: the sink is stopped, the finished
// part kept as a segment, and the rest rendered
// from the next frame on resume, then stream-copied together.

namespace {
// What marks an export's working folder as in use, inside it.
QString exportOwnerFile(const QString &folder) { return folder + QStringLiteral("/.owner"); }

// Working folders beside the delivery whose render was killed (the process
// that held the folder's owner lock is gone) are removed. One without the
// lock file is left alone: nothing says whose it is.
void removeAbandonedExportFolders(const QString &beside) {
    const QDir folder(beside);
    for (const QString &name : folder.entryList({QStringLiteral(".omaedit-export-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const QString path = folder.filePath(name);
        if (!QFileInfo::exists(exportOwnerFile(path))) continue;
        QLockFile owner(exportOwnerFile(path));
        owner.setStaleLockTime(0);  // stale only when its process is gone
        if (!owner.tryLock(0)) continue;
        owner.unlock();
        QDir(path).removeRecursively();
    }
}
}  // namespace

void PlayerWorker::exportTimeline(const QString &path, const QString &preset) {
    exportTimeline(path, ExportSpec::fromLegacyPreset(preset.isEmpty() ? QStringLiteral("h264-high") : preset));
}

QVariantMap PlayerWorker::renderSnapshot() {
    const auto *p=m_timeline.profile();
    QVariantMap profile;
    if(p)profile={{"width",p->width()},{"height",p->height()},{"fpsNum",p->frame_rate_num()},{"fpsDen",p->frame_rate_den()},
        {"sarNum",p->sample_aspect_num()},{"sarDen",p->sample_aspect_den()},{"darNum",p->display_aspect_num()},{"darDen",p->display_aspect_den()},{"progressive",p->progressive()},{"colorspace",p->colorspace()}};
    return {{"entries",m_timeline.entries()},{"nested",RenderSnapshot::nestedDocuments(m_timeline.entries())},{"tracks",m_timeline.trackFlagsVariant()},
            {"master",m_timeline.masterMix()},{"project",m_timeline.projectSettings()},
            {"mediaSources",m_timeline.mediaSources()},{"proxyMode",m_timeline.proxyMode()},{"fps",m_timeline.fps()},
            {"frames",m_timeline.frameCount()},{"profile",profile},{"markers",m_markers}};
}
void PlayerWorker::prepareRenderSnapshot(int jobId) { emit renderSnapshotReady(jobId,renderSnapshot()); }

void PlayerWorker::exportTimeline(const QString &path, const QVariantMap &spec) {
    if (m_exportStage != ExportStage::Idle)
        return; // a real export is running — leave its state alone
    m_exportContext = spec.value("snapshot").toMap();
    if(spec.contains("snapshotFile")) {
        QFile file(spec.value("snapshotFile").toString());
        if(file.size()>ProjectSafety::documentLimit()||!file.open(QIODevice::ReadOnly)) {emit exportEnded(false,path,"The queued project snapshot is unavailable.",{});emit exportFinished(false,path);return;}
        const QByteArray queued=file.readAll();
        m_exportContext=QJsonDocument::fromJson(queued).toVariant().toMap();
        // Queued before the kept-media folder moved: its kept files are where they are now.
        if(KeptMedia::named(queued))m_exportContext=ProjectSafety::withKeptMedia(m_exportContext);
        if(m_exportContext.isEmpty()){emit exportEnded(false,path,"The queued project snapshot is invalid.",{});emit exportFinished(false,path);return;}
    }
    if(m_exportContext.isEmpty())m_exportContext=renderSnapshot();
    m_exportFps=m_exportContext.value("fps",25.).toDouble();
    if (m_exportContext.value("entries").toList().isEmpty()) {
        // Backend latched exporting=true before asking; unwind it or the
        // whole session wedges (e.g. Ctrl+N emptied us mid-save-dialog).
        emit exportEnded(false, path, QStringLiteral("The timeline is empty"), {});
        emit exportFinished(false, path);
        return;
    }

    QSet<QString> checkedNests;
    std::function<QString(const QVariantList &)> missing = [&](const QVariantList &entries) -> QString {
        for (const QVariant &v : entries) {
            const QVariantMap entry = v.toMap();
            const QString source = entry.value(ClipKey::path).toString();
            const QString compound = entry.value(ClipKey::compoundJson).toString();
            if (!compound.isEmpty()) { const QString problem = missing(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList()); if (!problem.isEmpty()) return problem; }
            const auto documents=m_exportContext.value("nested").toMap();
            if(documents.contains(source)) {
                if(!checkedNests.contains(source)) { checkedNests.insert(source);const QString problem=missing(documents.value(source).toMap().value("entries").toList());if(!problem.isEmpty())return problem; }
            } else if (QFileInfo(source).isAbsolute() && !QFileInfo::exists(source)) return source;
            const QString lut = entry.value(ClipKey::sourceMeta).toMap().value(QStringLiteral("inputLut")).toString();
            if (!lut.isEmpty() && !QFileInfo::exists(lut)) return lut;
            const QStringList problems = Timeline::gradeLutProblems(entry);
            if (!problems.isEmpty()) return problems.first();
        }
        return {};
    };
    const QString unavailable = missing(m_exportContext.value("entries").toList());
    if (!unavailable.isEmpty()) {
        emit exportEnded(false, path, tr("Required media, LUT or colour configuration is unavailable: %1").arg(unavailable), {});
        emit exportFinished(false, path); return;
    }
    setSpeed(0.0);
    m_exportPath = path;
    {
        // A spec without its own audio format takes the project's.
        QVariantMap filled = spec;
        const QVariantMap project = m_exportContext.value("project").toMap();
        if (!filled.contains(QStringLiteral("arate")))
            filled.insert(QStringLiteral("arate"), project.value(QStringLiteral("sampleRate"), 48000));
        if (!filled.contains(QStringLiteral("channels")))
            filled.insert(QStringLiteral("channels"), project.value(QStringLiteral("channels"), 2));
        m_exportSpec = ExportSpec::normalized(filled);
    }
    m_exportScratch.reset();
    m_exportSegments.clear();
    m_exportSegmentFrames.clear();
    m_exportAudioPass = false;
    m_exportLoudnessGain = std::numeric_limits<double>::quiet_NaN();
    m_exportLoudnessCeiling = std::numeric_limits<double>::quiet_NaN();
    m_exportAudio.clear();
    m_exportCheckpoint = 0;
    m_exportPaused = false;
    m_exportCancelled = false;
    m_exportLog.clear();
    m_exportProcPercent = 0;
    m_exportReportedPercent = 0;
    removeAbandonedExportFolders(QFileInfo(path).absolutePath());
    m_exportDirectory = std::make_unique<QTemporaryDir>(QFileInfo(path).absolutePath() + "/.omaedit-export-XXXXXX");
    if (!m_exportDirectory->isValid()) {
        endExport(false, tr("Could not create temporary files in the export folder."));
        return;
    }
    m_exportOwner = std::make_unique<QLockFile>(exportOwnerFile(m_exportDirectory->path()));
    m_exportOwner->setStaleLockTime(0);
    m_exportOwner->tryLock(0);
    m_exportWorkingPath = m_exportDirectory->filePath(QFileInfo(path).fileName());

    // Frame-blended retimes bake first; the export then runs from a
    // scratch copy with those clips swapped for their intermediates.
    m_exportPre.clear();
    m_exportPreDone = 0;
    m_exportEntries = m_exportContext.value("entries").toList();
    if(m_exportSpec.value("video").toBool() && LinearRenderer::enabled(m_exportEntries,m_exportContext.value("project").toMap())) {
        LinearRenderer check;
        const auto profile=m_exportContext.value("profile").toMap();
        const QSize size(profile.value("width").toInt(),profile.value("height").toInt());
        check.configure(m_exportEntries,m_exportContext.value("tracks").toList(),m_exportContext.value("project").toMap(),size,m_exportFps);
        // The engine draws the export when it can; the CPU renderer's own limits otherwise.
        const bool drawn=render::engineRenderingRequested()
            && render::engineUnsupported(m_exportEntries,m_exportContext.value("tracks").toList(),m_exportContext.value("project").toMap(),size,m_exportFps).isEmpty();
        const auto error=drawn?QString():check.validate();if(!error.isEmpty()){endExport(false,error);return;}
    }
    auto interpolationProfile = m_exportContext.value("profile").toMap();
    interpolationProfile["fps"] = m_exportFps; // legacy snapshots may lack the rational profile
    for (int i = 0; i < m_exportEntries.size(); ++i) {
        const QVariantMap e = m_exportEntries.at(i).toMap();
        if (!m_exportSpec.value("video").toBool() || !wantsBlendRender(e))
            continue;
        PreJob job = blendIntermediate(e, interpolationProfile, m_exportContext.value("mediaSources").toMap());
        job.at = i;
        if (job.out.isEmpty()) {
            endExport(false, job.error);
            return;
        }
        m_exportPre.append(job);
    }
    // An image sequence has no encoder to check.
    if (!ExportSpec::sequenceFormat(m_exportSpec).isEmpty()) {
        m_exportStage = ExportStage::Pre;
        emit exportStageChanged(QStringLiteral("pre"));
        emit exportProgress(0);
        continueExportPre();
        return;
    }
    // An encoder that fails to initialise can take the process with it.
    // Check the exact codec/container settings in a separate process.
    auto probe = std::make_shared<QTemporaryDir>();
    if (!probe->isValid()) { endExport(false, tr("Could not create the encoder check folder")); return; }
    const auto profile = m_exportContext.value("profile").toMap();
    m_exportStage = ExportStage::Check;
    emit exportStageChanged(QStringLiteral("check"));
    emit exportProgress(0);
    // This program checks itself (the sink and the libraries the delivery will
    // use); one that cannot falls back on the ffmpeg program.
    const QString checked = probe->filePath("check" + ExportSpec::extension(m_exportSpec));
    const QString space = LinearRenderer::outputSpace(m_exportSpec.value("colorSpace").toString(), m_exportSpec.value("toneMap").toString());
    QStringList check = deliver::probeCommand(m_exportSpec, profile.value("width").toInt(), profile.value("height").toInt(),
        profile.value("fpsNum").toInt(), profile.value("fpsDen").toInt(),
        space.startsWith(QLatin1String("rec2020/")) ? 2020 : space.startsWith(QLatin1String("rec601/")) ? 601 : 709, checked);
    if (check.isEmpty())
        check = ExportSpec::encoderProbeArgs(m_exportSpec, profile.value("width").toInt(), profile.value("height").toInt(), m_exportFps, checked);
    runExportProcess(check,
        0, [this, probe] {
            m_exportStage = ExportStage::Pre;
            emit exportStageChanged(QStringLiteral("pre"));
            continueExportPre();
        });
}

// Weighted progress across the stages, so the bar never runs backwards.
void PlayerWorker::reportExportProgress(int stagePercent) {
    const bool post = false;  // the sink writes the delivery itself
    const bool mux = ExportSpec::embedsSubtitles(m_exportSpec) && m_exportSpec.value(QStringLiteral("video")).toBool();
    const bool pre = !m_exportPre.isEmpty() || m_exportStage == ExportStage::Pre;
    const int preEnd = pre ? 12 : 0;
    const int renderEnd = post ? 70 : (mux ? 96 : 100);
    const int postEnd = mux ? 96 : 100;
    int overall = 0;
    const int p = qBound(0, stagePercent, 100);
    switch (m_exportStage) {
    case ExportStage::Check: overall = 0; break;
    case ExportStage::Pre: overall = preEnd * p / 100; break;
    case ExportStage::Render:
    case ExportStage::Concat: overall = preEnd + (renderEnd - preEnd) * p / 100; break;
    case ExportStage::Post: overall = renderEnd + (postEnd - renderEnd) * p / 100; break;
    case ExportStage::Mux: overall = postEnd + (100 - postEnd) * p / 100; break;
    case ExportStage::Idle: overall = p; break;
    }
    m_exportReportedPercent = qMax(m_exportReportedPercent, qBound(0, overall, 99));
    emit exportProgress(m_exportReportedPercent);
}

// One ffmpeg stage. `seconds` sizes -progress output; `done` runs on a
// clean exit (a failed one ends the export with the process's stderr).
void PlayerWorker::runExportProcess(QStringList args, double seconds, const std::function<void()> &done) {
    if (args.isEmpty()) {
        endExport(false, QStringLiteral("ffmpeg is not installed"));
        return;
    }
    m_exportProc = std::make_unique<QProcess>();
    // Its own process group: a stage can be a pipeline (APV), and pause,
    // resume and cancel must reach every process in it.
    m_exportProc->setChildProcessModifier([] { ::setpgid(0, 0); });
    m_exportProc->setProgram(args.takeFirst());
    m_exportProc->setArguments(args);
    m_exportProc->setProcessChannelMode(QProcess::SeparateChannels);
    m_exportProcPercent = 0;
    QProcess *proc = m_exportProc.get();
    connect(proc, &QProcess::started, this, [this, proc] {
        if (m_exportProc.get() == proc && m_exportPaused)
            signalExportGroup(proc, SIGSTOP);
    });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, seconds] {
        if (m_exportProc.get() != proc)
            return;
        reportExportProgress(ffmpegPercent(proc, seconds, &m_exportProcPercent));
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc] {
        if (m_exportProc.get() != proc)
            return;
        const QList<QByteArray> lines = proc->readAllStandardError().split('\n');
        for (const QByteArray &l : lines)
            if (!l.trimmed().isEmpty())
                appendExportLog(QStringLiteral("[ffmpeg] ") + QString::fromUtf8(l.trimmed()));
    });
    connect(proc, &QProcess::finished, this, [this, proc, done](int code, QProcess::ExitStatus status) {
        if (m_exportProc.get() != proc)
            return;
        const bool ok = status == QProcess::NormalExit && code == 0;
        m_exportProc->deleteLater();
        (void)m_exportProc.release();
        if (m_exportCancelled)
            return; // cancelExport() already answered
        if (!ok) {
            endExport(false, QStringLiteral("ffmpeg exited with code %1").arg(code));
            return;
        }
        done();
    });
    // finished() is never emitted when the program cannot start at all.
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || m_exportProc.get() != proc) return;
        m_exportProc->deleteLater();
        (void)m_exportProc.release();
        if (!m_exportCancelled) endExport(false, QStringLiteral("ffmpeg could not be started: %1").arg(proc->errorString()));
    });
    appendExportLog(QStringLiteral("$ ") + proc->program() + QLatin1Char(' ') + proc->arguments().join(QLatin1Char(' ')));
    proc->start();
}

void PlayerWorker::appendExportLog(const QString &line) {
    m_exportLog.append(line);
    if (m_exportLog.size() > 400)
        m_exportLog.removeFirst();
}

// The scratch timeline the render stage renders: built once the intermediates
// exist, and again for each resumed segment. A producer that already ran
// ahead of the checkpoint answers a backward seek with silence for the
// frames it had read, so a resume never reuses the paused one.
bool PlayerWorker::buildExportScratch() {
    m_exportScratch.reset();
    QVariantList entries = m_exportEntries;
    // Header flags ride along; captions hide unless they are burnt in.
    std::vector<Timeline::TrackFlags> flags = Timeline::trackFlagsFromVariant(m_exportContext.value("tracks").toList());
    const bool burnCaptions=m_exportSpec.value("subtitles").toString()=="burn";
    entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const QVariant &v){
        const auto e=v.toMap();if(!e.value(ClipKey::caption).toBool())return false;
        const int track=e.value(ClipKey::lane).toInt();
        return !burnCaptions || (track>=0 && track<int(flags.size()) && !flags[size_t(track)].mix.value("captionBurnIn",true).toBool());
    }),entries.end());
    const auto selected=m_exportSpec.value("audioTracks").toList();
    if(!selected.isEmpty())for(int t=0;t<int(flags.size());++t){
        bool include=false;for(const auto &v:selected)include=include||v.toInt()==t;
        flags[t].solo=false;if(!include){flags[t].muted=true;flags[t].mix["muted"]=true;}
    }
    // A category stem: everything else stays in place, silent. (A sound with
    // no category is in none of them: it has the stem of its own.) Every clip
    // the mix plays sound from is a sound here, a picture that sounds as much
    // as a sound clip: a nested sequence, a compound clip, a clip that still
    // holds its own sound (one under a transition) is in its category's stem,
    // which for a picture's is the Uncategorised one; silenced in every stem,
    // it was in the mix and in no stem.
    if(const auto stem=m_exportSpec.value("audioCategory").toString();!stem.isEmpty()) {
        const auto categories=m_exportSpec.value("categoryList").toList();
        for(auto &value:entries) {
            auto entry=value.toMap();
            if(entry.value(ClipKey::gap).toBool())continue;
            if(!sound::Mixer::sounds(entry) || ClipCategories::stemOf(entry,categories)!=stem) {
                entry[ClipKey::disabled]=true;value=entry;
            }
        }
    }
    auto nests=std::make_unique<QTemporaryDir>();
    QString nestError;
    entries=RenderSnapshot::materialize(entries,m_exportContext.value("nested").toMap(),nests->path(),&nestError);
    if(!nests->isValid()||!nestError.isEmpty()){endExport(false,nestError.isEmpty()?"Could not prepare render files.":nestError);return false;}
    m_exportScratch = std::make_unique<Timeline>();
    m_exportScratch->setExportAlpha(m_exportSpec.value("alpha").toBool());
    {
        const int fw=m_exportSpec.value("width").toInt(),fh=m_exportSpec.value("height").toInt();
        m_exportScratch->setExportFraming(fw>0&&fh>0?QSize(fw,fh):QSize(),m_exportSpec.value("framing").toString());
    }
    auto project=m_exportContext.value("project").toMap();const auto resolved=m_exportContext.value("profile").toMap();
    if(!resolved.isEmpty()){project["custom"]=true;for(const auto &key:QStringList{"width","height","fpsNum","fpsDen"})project[key]=resolved.value(key);}
    m_exportScratch->setProjectSettings(project);
    m_exportScratch->setMediaSources(m_exportContext.value("mediaSources").toMap(),"original");
    if (!m_exportScratch->restore(entries, flags) || m_exportScratch->frameCount() != m_exportContext.value("frames").toInt()) {
        m_exportScratch.reset();
        endExport(false, QStringLiteral("The timeline could not be rebuilt for export (a clip is missing?)"));
        return false;
    }
    if(!resolved.isEmpty()) {auto *p=m_exportScratch->profile();p->set_sample_aspect(resolved.value("sarNum",1).toInt(),resolved.value("sarDen",1).toInt());p->set_display_aspect(resolved.value("darNum",16).toInt(),resolved.value("darDen",9).toInt());p->set_progressive(resolved.value("progressive",1).toInt());}
    QVariantMap master=m_exportSpec.value("includeMaster",true).toBool() ? m_exportContext.value("master").toMap() : QVariantMap{{"buses",m_exportContext.value("master").toMap().value("buses")}};
    if(!m_exportContext.value("master").toMap().contains("buses"))master.remove("buses");
    master["channelMap"]=m_exportSpec.value("audioTrackMap").toMap();
    if(!AudioEngine::validateRouting(m_exportScratch->tracks(),master).isEmpty()) {endExport(false,"The queued audio routing is invalid.");return false;}
    m_exportScratch->setMasterMixMap(master);
    m_exportNests = std::move(nests);  // the nested sequences' files, while the export reads them
    return true;
}

void PlayerWorker::continueExportPre() {
    // Next intermediate still to make (cached ones need nothing).
    while (m_exportPreDone < m_exportPre.size()) {
        auto &job = m_exportPre[m_exportPreDone];
        if (!job.args.isEmpty() && !completeSamplingFile(job.out)) break;
        job.directory.reset();
        job.args.clear();
        ++m_exportPreDone;
    }
    if (m_exportPreDone < m_exportPre.size()) {
        const PreJob &job = m_exportPre.at(m_exportPreDone);
        const double fps = m_exportFps > 0.0 ? m_exportFps : 25.0;
        const int frames = m_exportEntries.at(job.at).toMap().value(QStringLiteral("frames")).toInt();
        const QString out = job.out, stage = job.stage;
        runExportProcess(job.args, frames / fps, [this, out, stage] {
            if (!publishSamplingFile(stage, out)) {
                endExport(false, QStringLiteral("Could not keep the frame-blend intermediate"));
                return;
            }
            ++m_exportPreDone;
            continueExportPre();
        });
        return;
    }
    // Every intermediate exists: the scratch timeline plays them at 1×.
    QVariantList entries = m_exportEntries;
    for (const PreJob &job : std::as_const(m_exportPre)) {
        entries[job.at] = sampledEntry(entries.at(job.at).toMap(), job.out, m_exportContext.value("profile").toMap());
    }
    m_exportEntries = entries; // a resumed segment rebuilds from these
    m_exportPre.clear();
    if (!buildExportScratch())
        return;
    const int total = m_exportScratch->frameCount();
    int rin = m_exportSpec.value(QStringLiteral("rangeIn"), -1).toInt();
    int rout = m_exportSpec.value(QStringLiteral("rangeOut"), -1).toInt();
    if (rin < 0)
        rin = 0;
    if (rout < 0 || rout >= total)
        rout = total - 1;
    if (rout < rin) {
        endExport(false, QStringLiteral("The render range is empty"));
        return;
    }
    m_exportRangeIn = rin;
    m_exportRangeOut = rout;
    // The timeline's chapter markers inside the range become the movie's chapters.
    m_exportSpec[QStringLiteral("chapters")] = ExportSpec::chaptersFromMarkers(m_exportContext.value(QStringLiteral("markers")).toList(),
                                                                              rin, rout, m_exportScratch->fps());
    m_exportFrames = rout - rin + 1;
    m_exportCheckpoint = 0;
    m_exportStage = ExportStage::Render;
    emit exportStageChanged(QStringLiteral("render"));
    startExportStage();
}

// What the render stage writes: the delivery file.
QString PlayerWorker::exportStageTarget() const {
    return m_exportWorkingPath;
}

// The first frame, within the range, the current render pass starts from.
int PlayerWorker::exportStageStart() const {
    return m_exportAudioPass ? 0 : m_exportCheckpoint;
}

void PlayerWorker::startExportStage() {
    const QString target = exportStageTarget() + (m_exportAudioPass ? QStringLiteral(".audio") : QString());
    const QString part = target + QStringLiteral(".part");
    const QString linearProblem=m_exportSpec.value("video").toBool() && !m_exportAudioPass
        ? m_exportScratch->prepareLinearOutput(LinearRenderer::outputSpace(m_exportSpec.value("colorSpace").toString(),m_exportSpec.value("toneMap").toString())) : QString();
    if(!linearProblem.isEmpty()){endExport(false,linearProblem);return;}
    // An image sequence's working folder is added to after a pause; anything else starts its file afresh.
    if (!ExportSpec::sequenceFormat(m_exportSpec).isEmpty()) { if (m_exportCheckpoint == 0) QDir(part).removeRecursively(); }
    else QFile::remove(part);
    if (const QString why = exportProblem(); !why.isEmpty()) {
        endExport(false, why);
        return;
    }
    startExportSink(part);
}

// Why this export's files cannot be made (the mixer cannot make its sound), or nothing.
QString PlayerWorker::exportProblem() {
    if (m_exportSpec.value(QStringLiteral("audio"), true).toBool()) {
        const QString why = m_exportScratch->soundUnsupported();
        if (!why.isEmpty()) return tr("The sound cannot be made: %1").arg(why);
    }
    return {};
}

// The stage's file from the editor's own encoder sink, on a thread of its
// own: true when it started.
void PlayerWorker::startExportSink(const QString &part) {
    const bool pictures = m_exportSpec.value(QStringLiteral("video")).toBool() && !m_exportAudioPass;
    QVariantMap spec = m_exportSpec;
    if (!pictures)
        spec[QStringLiteral("video")] = false;  // the sound alone, in the delivery's codec
    // An image sequence's files carry the delivery's own name.
    if (!ExportSpec::sequenceFormat(spec).isEmpty())
        spec[QStringLiteral("sequenceStem")] = QFileInfo(m_exportPath).fileName();
    // A segment of a paused render starts part-way: the join writes the chapters.
    if (exportStageStart() != 0)
        spec.remove(QStringLiteral("chapters"));
    const int first = m_exportRangeIn + exportStageStart(), last = m_exportRangeOut;
    auto job = std::make_unique<SinkJob>();
    SinkJob *j = job.get();
    Timeline *scratch = m_exportScratch.get();
    m_exportRenderFailed = false;
    // Levelled sound: measured once over the whole range (first stage), the
    // same gain for every stage after.
    const bool level = spec.value(QStringLiteral("loudness")).toBool() && spec.value(QStringLiteral("audio"), true).toBool();
    const double known = m_exportLoudnessGain, knownCeiling = m_exportLoudnessCeiling;
    const int whole = m_exportRangeIn, wholeLast = m_exportRangeOut;
    if (level && !std::isfinite(known))
        appendExportLog(QStringLiteral("Measuring the sound's loudness"));
    job->thread = std::thread([j, scratch, part, spec, first, last, level, known, knownCeiling, whole, wholeLast,
                               request = job->request]() mutable {
        QString error;
        bool renderFailed = false;
        // Asked to stop where it is (a pause): nothing is written yet and
        // nothing went wrong. Anything else is a failure.
        const auto stopped = [&] {
            j->ok = j->stop.load() && !request->cancelled();
            j->renderFailed = !j->ok;
            j->error = j->ok ? QString() : std::move(error);
            j->done = true;
        };
        const auto going = [j](int) { return !j->stop.load(); };
        if (level) {
            double gain = known, ceiling = knownCeiling;
            if (!std::isfinite(gain)) {
                const int rate = spec.value(QStringLiteral("arate"), 48000).toInt(), channels = spec.value(QStringLiteral("channels"), 2).toInt();
                QVariantMap reading;
                if (!scratch->measureSound(whole, wholeLast, rate, channels, going, &reading, &error)) return stopped();
                const double integrated = reading.value(QStringLiteral("integrated"), -100.0).toDouble();
                const double target = spec.value(QStringLiteral("loudnessTarget"), -14.0).toDouble();
                const double promised = ExportSpec::promisedPeak(spec);
                const bool lossy = ExportSpec::lossySound(spec) && ExportSpec::sequenceFormat(spec).isEmpty();
                ceiling = ExportSpec::levellingCeiling(spec);
                // Silence (or all but) stays as it is; otherwise at most
                // 24 dB up, the master fader's own reach.
                gain = integrated > -70.0 ? qBound(-40.0, target - integrated, 24.0) : 0.0;
                // Where the limiter holds peaks down it takes some of the
                // loudness with it (a quiet mix with loud moments, raised):
                // the sound measured again as it will be made, and the gain
                // made up until it meets the target (a few passes of the
                // sound alone, only when it is limited at all). A decibel
                // more gain is less than a decibel louder where the limiter
                // holds much of the sound: how much less, from the last two
                // passes.
                const double peak = qMax(reading.value(QStringLiteral("truePeak"), -100.0).toDouble(),
                                         reading.value(QStringLiteral("reconstructedPeak"), -100.0).toDouble());
                double offset = 0.0;  // the codec's own change of loudness (heard less made)
                QVariantMap made;
                const auto makeUp = [&] {
                    made.clear();
                    // Untouched by the limiter, the sound is exactly the gain louder.
                    if (peak + gain <= ceiling - 0.5) {
                        gain = qBound(-40.0, target - offset - integrated, 24.0);
                        if (peak + gain <= ceiling - 0.5) return true;
                    }
                    double slope = 1.0, lastGain = qQNaN(), lastLoudness = qQNaN();
                    for (int pass = 0;; ++pass) {
                        if (!scratch->measureSound(whole, wholeLast, rate, channels, going, &made, &error, gain, ceiling)) return false;
                        const double loudness = made.value(QStringLiteral("integrated"), -100.0).toDouble(), under = target - offset - loudness;
                        if (std::abs(under) < 0.1 || pass == 5 || gain >= 24.0) return true;
                        if (std::isfinite(lastGain) && std::abs(gain - lastGain) > 0.05)
                            slope = qBound(0.1, (loudness - lastLoudness) / (gain - lastGain), 1.0);
                        lastGain = gain; lastLoudness = loudness;
                        gain = qBound(-40.0, gain + qBound(-6.0, under / slope, 6.0), 24.0);
                    }
                };
                // Then what the listener gets is checked, and the limiter's
                // ceiling (and the gain with it) set again until it meets
                // both the target and the peak promised. A lossy codec
                // rebuilds the sound with peaks over what went in, by as
                // much as a few decibels on sharp transients and the more
                // the lower its bitrate, and changes the loudness a little
                // with the highs it leaves out: a lossy delivery's sound is
                // always checked as encoded and decoded again. A lossless
                // one gives back what was made: that is checked, wherever
                // the limiter has work to do. Peaks are read as a BS.1770
                // meter and a careful reconstruction read them, whichever
                // is higher, and a lossy codec's are held a tenth of a
                // decibel under the promise (meters read its rebuilt highs a
                // little differently).
                const QString heardPath = part + QStringLiteral(".sound-check");
                const double aim = lossy ? promised - 0.1 : promised;
                int passes = 0;
                bool met = false, peaksMet = false;
                QVariantMap heard;
                while (integrated > -70.0 && passes < 10) {
                    // Past six passes only the ceiling moves, the gain as it is.
                    if (passes < 6) {
                        if (!makeUp()) return stopped();
                    } else {
                        made.clear();
                    }
                    if (lossy) {
                        if (!scratch->hearDeliveredSound(heardPath, spec, whole, wholeLast, gain, ceiling, going, &heard, &error, request))
                            return stopped();
                    } else if (!made.isEmpty()) {
                        heard = made;
                    } else if (peak + gain <= ceiling - 0.5) {
                        break;  // untouched by the limiter: exactly the gain louder, its peaks under the ceiling
                    } else if (!scratch->measureSound(whole, wholeLast, rate, channels, going, &heard, &error, gain, ceiling)) {
                        return stopped();
                    }
                    ++passes;
                    const double loudness = heard.value(QStringLiteral("integrated"), -100.0).toDouble();
                    const double over = qMax(heard.value(QStringLiteral("truePeak"), -100.0).toDouble(),
                                             heard.value(QStringLiteral("reconstructedPeak"), -100.0).toDouble()) - aim;
                    peaksMet = over <= 0.0;
                    met = peaksMet && std::abs(target - loudness) <= 0.2;
                    // The peaks are the promise: past six passes the loudness
                    // is left as near as it has got, the ceiling still lowered
                    // until they are under.
                    // (Nor can the gain go further than its reach.)
                    const bool reached = (loudness < target && gain >= 24.0) || (loudness > target && gain <= -40.0);
                    if (met || (peaksMet && (passes >= 6 || reached))) break;
                    if (lossy) offset = loudness - (made.isEmpty() ? integrated + gain : made.value(QStringLiteral("integrated")).toDouble());
                    // Under the promise by what it went over and a little more
                    // (the codec's rebuilding moves a little with it).
                    if (!peaksMet) ceiling = qMax(promised - 24.0, ceiling - over - 0.1);
                }
                if (passes > 0)
                    j->checked = QStringLiteral("Sound checked as delivered: %1 LUFS integrated, %2 dBTP true peak, limiter at %3 dBTP (%4 pass%5%6)")
                                     .arg(heard.value(QStringLiteral("integrated")).toDouble(), 0, 'f', 1)
                                     .arg(qMax(heard.value(QStringLiteral("truePeak")).toDouble(), heard.value(QStringLiteral("reconstructedPeak")).toDouble()), 0, 'f', 1)
                                     .arg(ceiling, 0, 'f', 1).arg(passes).arg(passes == 1 ? QString() : QStringLiteral("es"))
                                     .arg(met ? QString() : peaksMet ? QStringLiteral(", the loudness as near as the peaks allow")
                                                                     : QStringLiteral(", the peaks still over: delivered with the last correction"));
                j->measured = reading;
                j->gain = gain;
                j->ceiling = ceiling;
            }
            spec[QStringLiteral("loudnessGain")] = gain;
            spec[QStringLiteral("loudnessCeiling")] = ceiling;
        }
        const bool ok = scratch->writeDelivery(part, spec, false, first, last, [j, first](int frame) {
            j->written = frame - first + 1;
            return !j->stop.load();
        }, &error, &renderFailed, request);
        j->ok = ok;
        j->renderFailed = renderFailed;
        j->error = std::move(error);
        j->delivered = scratch->deliveredSound();
        j->done = true;
    });
    m_exportSink = std::move(job);
    appendExportLog(QStringLiteral("Encoding with the editor's own sink"));
    reportExportProgress(100 * exportStageStart() / qMax(1, m_exportFrames));
    m_exportPoll.start(250);

}

// The sink asked to stop where it is (its file finished), and joined.
void PlayerWorker::stopExportSink(bool cancel) {
    if (!m_exportSink)
        return;
    m_exportSink->stop = true;
    if (cancel)
        m_exportSink->request->cancel();
    if (m_exportSink->thread.joinable())
        m_exportSink->thread.join();
}

void PlayerWorker::pollExport() {
    if (m_exportSink && !m_exportPaused) {
        if (!m_exportSink->done) {
            if (m_exportFrames > 0)
                reportExportProgress(int(100LL * (exportStageStart() + m_exportSink->written) / m_exportFrames));
            return;
        }
        stopExportSink();
        const auto job = std::move(m_exportSink);
        if (std::isfinite(job->gain)) {
            m_exportLoudnessGain = job->gain;
            m_exportLoudnessCeiling = job->ceiling;
            appendExportLog(QStringLiteral("Sound measured %1 LUFS integrated, %2 dBTP true peak: levelling by %3 dB")
                                .arg(job->measured.value(QStringLiteral("integrated")).toDouble(), 0, 'f', 1)
                                .arg(job->measured.value(QStringLiteral("truePeak")).toDouble(), 0, 'f', 1)
                                .arg(job->gain, 0, 'f', 1));
            if (!job->checked.isEmpty()) appendExportLog(job->checked);
        }
        if (job->ok && !job->delivered.isEmpty())
            appendExportLog(QStringLiteral("Sound delivered at %1 LUFS integrated, %2 dBTP true peak (limiter at most %3 dB)")
                                .arg(job->delivered.value(QStringLiteral("integrated")).toDouble(), 0, 'f', 1)
                                .arg(job->delivered.value(QStringLiteral("truePeak")).toDouble(), 0, 'f', 1)
                                .arg(job->delivered.value(QStringLiteral("limited")).toDouble(), 0, 'f', 1));
        if (!job->ok) {
            m_exportRenderFailed = job->renderFailed;
            m_exportSinkFailed = !job->renderFailed;
            endExport(false, job->error.isEmpty() ? QStringLiteral("The encoder stopped before the end") : job->error);
            return;
        }
        finishExportStage(job->written >= m_exportFrames - exportStageStart());
    }
}

// Frames in a finished segment, from its packets — the consumer's
// position is one frame behind what it wrote, at best.
int PlayerWorker::countVideoFrames(const QString &file) {
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty())
        return -1;
    QProcess p;
    p.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                      QStringLiteral("-count_packets"), QStringLiteral("-show_entries"), QStringLiteral("stream=nb_read_packets"),
                      QStringLiteral("-of"), QStringLiteral("csv=p=0"), file});
    if (!p.waitForFinished(60000))
        return -1;
    // A file with a timecode track lists its picture stream twice (once with
    // the track it belongs to): the first count is the one.
    bool ok = false;
    const int n = QString::fromUtf8(p.readAllStandardOutput()).trimmed().section(QLatin1Char('\n'), 0, 0).trimmed().toInt(&ok);
    return ok ? n : -1;
}

void PlayerWorker::finishExportStage(bool ok) {
    m_exportPoll.stop();
    stopExportSink();
    m_exportSink.reset();
    const auto renderProblem=m_exportRenderRequest?m_exportRenderRequest->error():QString();
    if(!renderProblem.isEmpty()){endExport(false,renderProblem);return;}
    if (m_exportCancelled)
        return;
    const QString target = exportStageTarget();
    if (!ok) {
        endExport(false, QStringLiteral("The encoder stopped before the end"));
        return;
    }
    if (m_exportAudioPass) {
        m_exportAudioPass = false;
        m_exportAudio = target + QStringLiteral(".audio");
        QFile::remove(m_exportAudio);
        if (!QFile::rename(m_exportAudio + QStringLiteral(".part"), m_exportAudio)) {
            endExport(false, QStringLiteral("Could not keep the rendered sound"));
            return;
        }
        joinExportSegments();
        return;
    }
    const QString part = target + QStringLiteral(".part");
    if (!m_exportSegments.isEmpty()) {
        // Paused and resumed: keep the last segment, then join them.
        const QString last = target + QStringLiteral(".seg%1").arg(m_exportSegments.size() + 1);
        QFile::remove(last);
        if (!QFile::rename(part, last)) {
            endExport(false, QStringLiteral("Could not keep the last render segment"));
            return;
        }
        m_exportSegments.append(last);
        m_exportSegmentFrames.append(m_exportFrames - std::accumulate(m_exportSegmentFrames.cbegin(), m_exportSegmentFrames.cend(), 0));
        if (m_exportSpec.value(QStringLiteral("audio")).toBool()) {
            // Pictures stream-copy together frame-exactly; sound does not: each
            // segment's encoder primes its start and pads its end to a whole
            // codec frame, a click or a gap at every seam. Render the sound
            // again whole, from a fresh timeline, and join that instead.
            appendExportLog(QStringLiteral("Rendering the sound in one pass for a seamless join"));
            m_exportAudioPass = true;
            emit exportStageChanged(QStringLiteral("join"));
            if (!buildExportScratch())
                return;
            startExportStage();
            return;
        }
        joinExportSegments();
        return;
    }
    afterRenderStage(part);
}

// The picture segments stream-copied into one file, with the one-pass sound
// when there is one.
void PlayerWorker::joinExportSegments() {
    const QString part = exportStageTarget() + QStringLiteral(".part");
    const QString list = exportStageTarget() + QStringLiteral(".segments.txt");
    QFile f(list);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        endExport(false, QStringLiteral("Could not write the segment list"));
        return;
    }
    // Each segment's exact length places the next: its own duration also
    // counts its sound's priming and padding, a frame's drift at a seam.
    const double fps = m_exportFps > 0.0 ? m_exportFps : 25.0;
    for (int i = 0; i < m_exportSegments.size(); ++i) {
        f.write(QStringLiteral("file '%1'\n").arg(QString(m_exportSegments.at(i)).replace(QLatin1Char('\''), QStringLiteral("'\\''"))).toUtf8());
        f.write(QStringLiteral("duration %1\n").arg(m_exportSegmentFrames.value(i) / fps, 0, 'f', 9).toUtf8());
    }
    f.close();
    m_exportStage = ExportStage::Concat;
    emit exportStageChanged(QStringLiteral("join"));
    // The join keeps no chapters of its segments: they go in from a file of their own.
    QString chapters;
    if (const QString text = ExportSpec::chapterMetadata(m_exportSpec); !text.isEmpty()) {
        chapters = exportStageTarget() + QStringLiteral(".chapters.txt");
        QFile c(chapters);
        if (!c.open(QIODevice::WriteOnly | QIODevice::Truncate) || c.write(text.toUtf8()) < 0) {
            endExport(false, QStringLiteral("Could not write the chapter list"));
            return;
        }
    }
    // Nor the timecode it starts at: the whole range's, not a segment's.
    QStringList start;
    if (m_exportScratch) {
        ExportSpec::EncoderSettings whole; int rateNum = 0, rateDen = 1; QString unused;
        if (m_exportScratch->deliverySettings(m_exportSpec, false, m_exportRangeIn, &whole, &rateNum, &rateDen, &unused))
            start = ExportSpec::startArgs(whole);
    }
    runExportProcess(ExportSpec::ffmpegConcatArgs(list, part, m_exportAudio, ExportSpec::normalized(m_exportSpec).value(QStringLiteral("metadata")).toMap(), chapters, start), m_exportFrames / fps, [this, list, part, chapters] {
        if (!chapters.isEmpty()) QFile::remove(chapters);
        for (const QString &s : std::as_const(m_exportSegments))
            QFile::remove(s);
        m_exportSegments.clear();
        m_exportSegmentFrames.clear();
        QFile::remove(list);
        if (!m_exportAudio.isEmpty())
            QFile::remove(m_exportAudio);
        m_exportAudio.clear();
        afterRenderStage(part);
    });
}

// The render stage's file is complete: the post-encode, the caption mux,
// or straight to the delivery name.
void PlayerWorker::afterRenderStage(const QString &part) {
    muxOrFinish(part);
}

// Caption cues clipped to the render range, timed from its start.
QVariantList PlayerWorker::exportCues() const {
    QVariantList cues;
    const QVariantList all = m_exportSpec.value(QStringLiteral("captionCues")).toList();
    for (const QVariant &v : all) {
        QVariantMap c = v.toMap();
        int start = c.value(QStringLiteral("start")).toInt();
        int end = start + c.value(QStringLiteral("frames")).toInt();
        start = qMax(start, m_exportRangeIn);
        end = qMin(end, m_exportRangeOut + 1);
        if (end <= start)
            continue;
        c.insert(QStringLiteral("start"), start - m_exportRangeIn);
        c.insert(QStringLiteral("frames"), end - start);
        cues.append(c);
    }
    return cues;
}

void PlayerWorker::muxOrFinish(const QString &part) {
    const QString subs = m_exportSpec.value(QStringLiteral("subtitles")).toString();
    const QVariantList cues = exportCues();
    const QString container = m_exportSpec.value(QStringLiteral("container")).toString();
    const bool canEmbed = m_exportSpec.value(QStringLiteral("video")).toBool()
                          && (container == QStringLiteral("mp4") || container == QStringLiteral("mov")
                              || container == QStringLiteral("mkv") || container == QStringLiteral("webm"));
    const double fps = m_exportFps > 0.0 ? m_exportFps : 25.0;
    if (subs == QStringLiteral("embed") && !cues.isEmpty() && canEmbed) {
        const QString srt = m_exportWorkingPath + QStringLiteral(".captions.srt");
        QFile f(srt);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            endExport(false, QStringLiteral("Could not write the caption file"));
            return;
        }
        f.write(Timeline::formatSrt(cues, fps).toUtf8());
        f.close();
        const QString muxed = m_exportWorkingPath + QStringLiteral(".mux.part");
        m_exportStage = ExportStage::Mux;
        emit exportStageChanged(QStringLiteral("captions"));
        runExportProcess(ExportSpec::ffmpegMuxSubtitleArgs(m_exportSpec, part, srt, muxed), m_exportFrames / fps,
                         [this, part, srt, muxed] {
            QFile::remove(srt);
            QFile::remove(part);
            finishDelivery(muxed);
        });
        return;
    }
    if ((subs == QStringLiteral("sidecar") || (subs == QStringLiteral("embed") && !canEmbed)) && !cues.isEmpty()) {
        const QString srt = QFileInfo(m_exportPath).path() + QLatin1Char('/')
                            + QFileInfo(m_exportPath).completeBaseName() + QStringLiteral(".srt");
        // Written now, published after the main file (so a refused delivery
        // leaves the existing .srt alone), readable like any other output.
        const QString pending = QFileInfo(srt).absolutePath() + QStringLiteral("/.omaedit-caption-")
                                + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".srt");
        QFile f(pending);
        const auto text = Timeline::formatSrt(cues, fps).toUtf8();
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write(text) != text.size() || !f.flush()) {
            f.close(); QFile::remove(pending);
            endExport(false, QStringLiteral("Could not write subtitle sidecar: %1").arg(srt)); return;
        }
        f.close();
        QFile::setPermissions(pending, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        m_exportSidecarPending = pending;
        m_exportSidecarTarget = srt;
    }
    finishDelivery(part);
}

// The finished pictures of an image sequence in `folder` (not one still being written).
int PlayerWorker::sequenceFilesIn(const QString &folder) {
    int count = 0;
    for (const QString &name : QDir(folder).entryList(QDir::Files | QDir::NoDotAndDotDot))
        count += name.startsWith(QLatin1Char('.')) ? 0 : 1;
    return count;
}

// An image sequence's files into the folder it is delivered as. The folder
// may be there already (an earlier delivery's): a file of the same name is
// replaced, anything else in it is left alone.
bool PlayerWorker::publishSequence(const QString &part, QString *error) {
    const QDir from(part);
    const QStringList names = from.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    const bool replace = !m_exportSpec.contains("overwritePaths")
                         || m_exportSpec.value("overwritePaths").toStringList().contains(ExportSpec::outputKey(m_exportPath));
    if (QFileInfo(m_exportPath).exists() && !QFileInfo(m_exportPath).isDir()) {
        *error = tr("Could not move the render into place: %1 is a file, not a folder.").arg(m_exportPath);
        return false;
    }
    if (names.isEmpty() || !QDir().mkpath(m_exportPath)) {
        *error = tr("Could not make the image sequence's folder: %1").arg(m_exportPath);
        return false;
    }
    for (const QString &name : names) {
        if (name.startsWith(QLatin1Char('.'))) continue;
        const QString to = m_exportPath + QLatin1Char('/') + name;
        if (!replace && QFileInfo::exists(to)) {
            *error = tr("Could not move the render into place: %1. The destination may already exist; review it before retrying.").arg(to);
            return false;
        }
        if (std::rename(QFile::encodeName(from.filePath(name)).constData(), QFile::encodeName(to).constData()) != 0) {
            *error = tr("Could not move the render into place: %1").arg(to);
            return false;
        }
    }
    QDir(part).removeRecursively();
    return true;
}

void PlayerWorker::finishDelivery(const QString &part) {
    QString error;
    if (!ExportSpec::sequenceFormat(m_exportSpec).isEmpty()) {
        if (!publishSequence(part, &error)) { endExport(false, error); return; }
        endExport(true, QString());
        return;
    }
    if (!ExportSpec::publishOutput(part, m_exportPath, m_exportSpec, &error)) {
        endExport(false, error);
        return;
    }
    if (!m_exportSidecarPending.isEmpty()) {
        const QString pending = std::exchange(m_exportSidecarPending, QString());
        if (!ExportSpec::publishOutput(pending, m_exportSidecarTarget, m_exportSpec, &error)) {
            QFile::remove(pending);
            endExport(false, QStringLiteral("Could not write subtitle sidecar: %1").arg(m_exportSidecarTarget));
            return;
        }
        appendExportLog(QStringLiteral("Captions written to %1").arg(m_exportSidecarTarget));
    }
    endExport(true, QString());
}

void PlayerWorker::endExport(bool ok, const QString &reason) {
    if (!m_exportSidecarPending.isEmpty()) // a delivery that never published
        QFile::remove(std::exchange(m_exportSidecarPending, QString()));
    const bool fallback=!ok && !m_exportCancelled && m_exportSpec.value("softwareFallback",true).toBool()
        && (!m_exportRenderRequest || m_exportRenderRequest->error().isEmpty()) && !m_exportRenderFailed
        && m_exportSpec.value("hw").toString()!="software"
        // Only where the hardware encoder ran: the check, the post stage, or
        // the render stage when it was the sink's encoder that stopped it (a
        // caption file, a segment or a sidecar that cannot be kept is no
        // encoder's doing). A file that cannot be written (a full disk, a
        // quota, a failing drive) is not cured by a software retry.
        && (m_exportStage==ExportStage::Check || m_exportStage==ExportStage::Post
            || (m_exportStage==ExportStage::Render && m_exportSinkFailed))
        && !reason.contains("No space left", Qt::CaseInsensitive)
        && !reason.startsWith("Writing the file failed") && !reason.startsWith("The file cannot be written")
        && !reason.startsWith("The file could not be finished")
        && !reason.contains("move the render");
    m_exportPoll.stop();
    stopExportSink(true);
    m_exportSink.reset();
    m_exportRenderFailed = false;
    m_exportSinkFailed = false;
    if(m_exportRenderRequest)m_exportRenderRequest->cancel();
    m_exportRenderRequest.reset();
    if (m_exportProc) {
        QProcess *p = m_exportProc.release();
        p->disconnect(this);
        signalExportGroup(p, SIGCONT);
        signalExportGroup(p, SIGKILL);
        p->kill();
        p->waitForFinished(3000);
        p->deleteLater();
    }
    // The directory is exclusively ours. Never infer ownership from an output
    // basename: adjacent .part/.seg files may belong to a user or another app.
    // Interpolation jobs also own private directories; clearing the jobs below
    // removes their unfinished files after the process has stopped.
    m_exportSegments.clear();
    m_exportSegmentFrames.clear();
    m_exportAudioPass = false;
    m_exportAudio.clear();
    m_exportPre.clear();
    m_exportScratch.reset();
    m_exportNests.reset();  // the nested sequences' files went with the scratch timeline that read them
    m_exportOwner.reset();
    m_exportDirectory.reset();
    m_exportWorkingPath.clear();
    if (!ok && !reason.isEmpty())
        appendExportLog(reason);
    m_exportStage = ExportStage::Idle;
    m_exportPaused = false;
    if(fallback) {
        auto retry=m_exportSpec;retry["hw"]="software";retry["snapshot"]=m_exportContext;retry.remove("snapshotFile");
        const QString message="Hardware encoding failed; restarting this job in software.";
        const auto firstLog=m_exportLog;
        emit exportFallbackUsed(message);
        exportTimeline(m_exportPath,retry);
        m_exportLog=firstLog+QStringList{message}+m_exportLog;
        return;
    }
    if (m_mediaSourcesDeferred) {
        m_mediaSourcesDeferred = false;
        setMediaSources(m_deferredMediaSources, m_deferredMediaMode);
        m_deferredMediaSources.clear();
    }
    emit exportStageChanged(QString());
    if (ok)
        emit exportProgress(100); // a failed or cancelled job keeps where it got to
    emit exportEnded(ok, m_exportPath, reason, m_exportLog);
    emit exportFinished(ok, m_exportPath);
    refreshFrame(); // the consumer moved the playlist position; repaint ours
    scheduleStripWork();
}

void PlayerWorker::pauseExport() {
    if (m_exportStage == ExportStage::Idle || m_exportPaused)
        return;
    if (m_exportProc && m_exportProc->state() != QProcess::NotRunning) {
        if (m_exportProc->state() == QProcess::Running)
            signalExportGroup(m_exportProc.get(), SIGSTOP);
        m_exportPaused = true;
        emit exportPausedChanged(true);
        return;
    }
    if (m_exportStage == ExportStage::Render && m_exportSink) {
        m_exportPoll.stop();
        // The sink finishes its file where it stops: a segment, joined later.
        stopExportSink();
        if (m_exportSink && m_exportSink->done && !m_exportSink->ok) {
            const auto job = std::move(m_exportSink);
            m_exportRenderFailed = job->renderFailed;
            m_exportSinkFailed = !job->renderFailed;
            endExport(false, job->error.isEmpty() ? QStringLiteral("The encoder stopped before the end") : job->error);
            return;
        }
        // The loudness measured before the pause holds for every stage after it.
        if (m_exportSink && std::isfinite(m_exportSink->gain)) {
            m_exportLoudnessGain = m_exportSink->gain;
            m_exportLoudnessCeiling = m_exportSink->ceiling;
        }
        m_exportSink.reset();
        const QString target = exportStageTarget();
        const QString part = target + (m_exportAudioPass ? QStringLiteral(".audio.part") : QStringLiteral(".part"));
        if (!ExportSpec::sequenceFormat(m_exportSpec).isEmpty()) {
            // An image sequence: the finished files stay where they are, and the next pass adds to them.
            m_exportCheckpoint = qMin(m_exportFrames, sequenceFilesIn(part));
            appendExportLog(QStringLiteral("Paused after frame %1 of %2").arg(m_exportCheckpoint).arg(m_exportFrames));
        } else if (m_exportSpec.value(QStringLiteral("video")).toBool() && !m_exportAudioPass) {
            const int done = countVideoFrames(part);
            if (done > 0) {
                const QString seg = target + QStringLiteral(".seg%1").arg(m_exportSegments.size() + 1);
                QFile::remove(seg);
                if (QFile::rename(part, seg)) {
                    m_exportSegments.append(seg);
                    m_exportSegmentFrames.append(done);
                    m_exportCheckpoint = qMin(m_exportFrames, m_exportCheckpoint + done);
                    appendExportLog(QStringLiteral("Paused after frame %1 of %2").arg(m_exportCheckpoint).arg(m_exportFrames));
                } else {
                    QFile::remove(part);
                }
            } else {
                QFile::remove(part);
            }
        } else {
            // An audio-only render restarts: PCM/AAC frames do not
            // checkpoint cleanly.
            QFile::remove(part);
            appendExportLog(QStringLiteral("Paused; audio renders restart from the top"));
        }
        m_exportPaused = true;
        emit exportPausedChanged(true);
    }
}

void PlayerWorker::resumeExport() {
    if (m_exportStage == ExportStage::Idle || !m_exportPaused)
        return;
    m_exportPaused = false;
    emit exportPausedChanged(false);
    if (m_exportProc && m_exportProc->state() == QProcess::Running) {
        signalExportGroup(m_exportProc.get(), SIGCONT);
        return;
    }
    if (m_exportStage == ExportStage::Render) {
        if (m_exportCheckpoint >= m_exportFrames && !m_exportAudioPass) {
            // Everything was already written when it paused.
            const QString target = exportStageTarget();
            if (m_exportSegments.isEmpty()) {  // an image sequence keeps no segments
                afterRenderStage(target + QStringLiteral(".part"));
                return;
            }
            const QString last = m_exportSegments.takeLast();
            m_exportSegmentFrames.removeLast();
            if (m_exportSegments.isEmpty()) {
                QFile::rename(last, target + QStringLiteral(".part"));
                afterRenderStage(target + QStringLiteral(".part"));
            } else {
                QFile::rename(last, target + QStringLiteral(".part"));
                finishExportStage(true);
            }
            return;
        }
        if (!buildExportScratch())
            return;
        startExportStage();
    }
}

void PlayerWorker::cancelExport() {
    if (m_exportStage == ExportStage::Idle)
        return;
    m_exportCancelled = true;
    endExport(false, QStringLiteral("Cancelled"));
}

bool PlayerWorker::exportActive() const {
    return m_exportStage != ExportStage::Idle;
}
