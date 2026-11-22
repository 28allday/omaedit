#include "backend.h"
#include "projectfile.h"
#include "playbackview.h"
#include "playerworker.h"
#include "scopeprocessor.h"
#include "linearrenderer.h"
#include "audiotimeline.h"
#include "audiotimingedit.h"
#include "filepicker.h"
#include <QFileInfo>
#include <QSettings>
#include <QSaveFile>
#include <QJSValue>
#include <utility>

void Backend::setupPlayback() {
    QSettings settings;
    m_backgroundRenderEnabled = qEnvironmentVariable("OMAEDIT_BACKGROUND_RENDER") != "0"
        && settings.value("playback/backgroundRender", true).toBool();
    m_backgroundRenderLimit = qBound(1, settings.value("playback/backgroundRenderGiB", 20).toInt(), 200);
    m_backgroundRenderProcessing = qEnvironmentVariable("OMAEDIT_CACHE_PROCESSING", settings.value("hardware/cacheProcessing", "auto").toString()) == "cpu" ? "cpu" : "auto";
    connect(m_worker, &PlayerWorker::backgroundRenderChanged, this, [this](const QVariantMap &status) {
        m_backgroundRender = status;
        emit backgroundRenderChanged();
        if (!status.value("id").toString().isEmpty()) {
            auto row = status;
            row["project"] = projectName();
            m_backgroundTasks->update(row);
        }
    });
    auto view=settings.value("playback/view").toMap();
    if(view.isEmpty()) { view["safeAreas"]=settings.value("monitor/safeAreas",false); view["guides"]=settings.value("monitor/guides","none"); }
    m_viewerSettings=PlaybackView::normalized(view);
    connect(this,&Backend::sourceChanged,this,&Backend::syncSourceLoopRange);
    connect(this,&Backend::libraryChanged,this,&Backend::syncSourceLoopRange);
    connect(this,&Backend::sourceChanged,this,&Backend::resetPlaybackStats);
    connect(this,&Backend::playingChanged,this,[this,wasPlaying=false]() mutable {
        if(playing() && !wasPlaying) resetPlaybackStats();
        if(!playing() && wasPlaying) m_playbackElapsedMillis=m_presentationClock.elapsed();
        wasPlaying=playing(); emit playbackStatsChanged();
    });
    connect(&m_presentationTimer,&QTimer::timeout,this,[this] { if(playing()) emit playbackStatsChanged(); });
    m_presentationTimer.start(1000); resetPlaybackStats();
}
void Backend::setBackgroundRenderEnabled(bool enabled) {
    if (m_backgroundRenderEnabled == enabled) return;
    m_backgroundRenderEnabled = enabled;
    QSettings().setValue("playback/backgroundRender", enabled);
    QMetaObject::invokeMethod(m_worker, [this, enabled] { m_worker->setBackgroundRenderEnabled(enabled); }, Qt::QueuedConnection);
    emit preferencesChanged();
}
void Backend::setBackgroundRenderLimit(int gib) {
    gib = qBound(1, gib, 200);
    if (m_backgroundRenderLimit == gib) return;
    m_backgroundRenderLimit = gib;
    QSettings().setValue("playback/backgroundRenderGiB", gib);
    QMetaObject::invokeMethod(m_worker, [this, gib] { m_worker->setBackgroundRenderLimit(gib); }, Qt::QueuedConnection);
    emit preferencesChanged();
}
void Backend::setBackgroundRenderProcessing(const QString &preference) {
    if ((preference != "auto" && preference != "cpu") || preference == m_backgroundRenderProcessing) return;
    m_backgroundRenderProcessing = preference;
    QSettings().setValue("hardware/cacheProcessing", preference);
    QMetaObject::invokeMethod(m_worker, [this, preference] { m_worker->setBackgroundRenderProcessing(preference); }, Qt::QueuedConnection);
    emit preferencesChanged();
}
void Backend::backgroundRenderAction(const QString &action) {
    if (!QStringList{"pause", "resume", "cancel", "clear", "retry"}.contains(action)) return;
    QMetaObject::invokeMethod(m_worker, [this, action] { m_worker->backgroundRenderAction(action); }, Qt::QueuedConnection);
}
void Backend::setViewerOption(const QString &key,const QVariant &value) {
    if(!m_viewerSettings.contains(key)) return;
    auto draft=m_viewerSettings;
    draft[key]=value.metaType()==QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
    const auto next=PlaybackView::normalized(draft);
    if(next==m_viewerSettings) return;
    m_viewerSettings=next;
    QSettings settings; settings.setValue("playback/view",next);
    settings.setValue("monitor/safeAreas",next.value("safeAreas"));
    settings.setValue("monitor/guides",next.value("guides"));
    emit viewerSettingsChanged();
}
void Backend::notePresentationFrame() {
    ++m_presentationSerial;
    if (playing()) m_lastPlaybackSerial = m_presentationSerial;
}
void Backend::framePresented(quint64 serial) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, serial] { framePresented(serial); }, Qt::QueuedConnection);
        return;
    }
    // Rendering can finish after several more frames have been submitted.
    // Only a newer rendered frame proves that the intervening ones were missed.
    // Ignore repeated views, old-session replies and pictures from paused seeks.
    if (serial <= m_presentedSerial || serial > m_lastPlaybackSerial) return;
    m_droppedFrames += int(serial - m_presentedSerial - 1);
    m_presentedSerial = serial;
    ++m_presentedCount;
}
void Backend::resetPlaybackStats() {
    m_presentedSerial = m_lastPlaybackSerial = m_presentationSerial;
    m_droppedFrames = 0; m_presentedCount = 0;
    m_playbackElapsedMillis=0; m_presentationClock.restart(); emit playbackStatsChanged();
}
QVariantMap Backend::playbackStats() const {
    const double seconds=(playing() ? m_presentationClock.elapsed() : m_playbackElapsedMillis)/1000.;
    const double rate=seconds>.2 ? (m_presentedCount)/seconds : 0;
    return {{"displayedFps",rate},{"presentedFrames",m_presentedCount},{"dropped",m_droppedFrames},{"seconds",seconds},
        {"quality",playing() && seconds>=1 && rate < (m_viewingSource ? m_sourceFps : m_fps)*.85 ? tr("Below target rate") : playing() ? tr("Playing") : tr("Paused")}};
}
void Backend::syncSourceLoopRange() {
    const auto e=m_library.value(m_previewLibraryIndex).toMap();
    const int in=e.value("rangeIn",-1).toInt(),out=e.value("rangeOut",-1).toInt();
    QMetaObject::invokeMethod(m_worker,[this,in,out] { m_worker->setSourceLoopRange(in,out); },Qt::QueuedConnection);
}
void Backend::resetProjectPlayback() {
    cancelAudioSeek(true);
    pause();
    m_viewingSource=false; m_previewLibraryIndex=-1;
    m_sourceDuration=0; m_sourcePosition=0; m_sourceFrame={};
    m_previewShownPath.clear();
    if (!hasVideo()) m_beforeFrame={}; // retain a newly loaded comparison frame
    m_pendingSourceSeek=m_pendingSeek=-1; m_seekInFlight=false;
    m_pendingSourceRangeIn=m_pendingSourceRangeOut=-1;
    setLoop(false); setLoopRange(-1,-1); resetPlaybackStats();
    emit sourceFrameChanged(); emit beforeFrameChanged(); emit sourcePositionChanged();
    emit sourceChanged(); emit projectPlaybackReset();
}

int Backend::audioPictureFrame(const QVariantMap &position) const {
    AudioTimeline::Time time;
    const int num=m_audioEditContext.value("fpsNum").toInt(), den=m_audioEditContext.value("fpsDen").toInt();
    if (num<1 || den<1 || !AudioTimeline::readTime(position,&time) || time.ticks<0) return -1;
    const auto frame=__int128(time.ticks)*num/(__int128(time.rate)*den);
    return frame>=0 && frame<m_duration ? int(frame) : -1;
}

QVariantMap Backend::audioPosition() const {
    if (!m_requestedAudioPosition.isEmpty()) return m_requestedAudioPosition;
    if (!m_audioPosition.isEmpty()) return m_audioPosition;
    const int num=m_audioEditContext.value("fpsNum").toInt(), den=m_audioEditContext.value("fpsDen").toInt();
    return num>0 && den>0 ? AudioTimeline::writeTime({qint64(m_position)*den,num}) : QVariantMap{};
}

double Backend::audioPositionFrames() const {
    AudioTimeline::Time time;
    const int num=m_audioEditContext.value("fpsNum").toInt(), den=m_audioEditContext.value("fpsDen").toInt();
    return den>0 && AudioTimeline::readTime(audioPosition(),&time)
        ? double(time.ticks)/time.rate*num/den : m_position;
}

QString Backend::audioPositionText(const QString &unit) const {
    return AudioTimingEdit::formatPosition(audioPosition(),unit,m_projectSettings.value("sampleRate",48000).toInt());
}

void Backend::cancelAudioSeek(bool clearPosition) {
    ++m_audioSeekSerial;
    m_requestedAudioPosition.clear(); m_pendingAudioSeek.clear(); m_speedAfterAudioSeek=0.;
    if (clearPosition) m_audioPosition.clear();
    emit audioPositionChanged();
}

void Backend::syncAudioPosition(int frame) {
    if (!m_requestedAudioPosition.isEmpty()) return;
    // A source preview has its own cursor. Programme refreshes at the parked
    // picture must retain the fractional sound position within that picture.
    if (!m_audioPosition.isEmpty() && audioPictureFrame(m_audioPosition)!=frame) m_audioPosition.clear();
    emit audioPositionChanged();
}

void Backend::dispatchAudioSeek() {
    if (m_audioSeekInFlight || m_pendingAudioSeek.isEmpty()) return;
    const auto next=std::exchange(m_pendingAudioSeek,{});
    m_audioSeekInFlight=next.value("request").toULongLong();
    emit audioPositionRequested(m_audioSeekInFlight,next.value("context").toMap(),next.value("position").toMap(),next.value("audition").toBool());
}

bool Backend::seekAudioPosition(const QVariantMap &position, bool audition) {
    if (projectBusy() || m_exporting || audioPictureFrame(position)<0
        || m_audioEditContext.value("sequenceId").toString().isEmpty()
        || m_audioEditContext.value("sequenceId")!=m_sequences.value(m_activeSequence).toMap().value("id")) {
        setStatus(tr("Choose a sound position inside the active timeline.")); return false;
    }
    viewTimeline();
    applySpeed(0.);
    m_seekInFlight=false; m_pendingSeek=-1;
    m_worker->cancelFrame(); cancelScopes();
    AudioTimeline::Time time; AudioTimeline::readTime(position,&time);
    m_requestedAudioPosition=AudioTimeline::writeTime(time);
    m_pendingAudioSeek={{"request",++m_audioSeekSerial},{"context",m_audioEditContext},
        {"position",m_requestedAudioPosition},{"audition",audition}};
    m_speedAfterAudioSeek=0.;
    emit audioPositionChanged();
    dispatchAudioSeek();
    return true;
}

bool Backend::seekAudioText(const QString &text, const QString &unit) {
    return seekAudioPosition(AudioTimingEdit::parsePosition(text,unit,m_projectSettings.value("sampleRate",48000).toInt()));
}

bool Backend::stepAudioSamples(int samples) {
    AudioTimeline::Time time;
    const int rate=m_projectSettings.value("sampleRate",48000).toInt();
    if (rate<1 || rate>768000 || !AudioTimeline::readTime(audioPosition(),&time)) return false;
    // Step to the next/previous project sample, even from an exact non-grid
    // position such as a video-frame boundary on a fractional picture clock.
    const __int128 scaled=__int128(time.ticks)*rate;
    const auto tick=scaled/time.rate + samples + (samples<0 && scaled%time.rate ? 1 : 0);
    if (tick<0 || tick>9000000000000000LL) return false;
    return seekAudioPosition(AudioTimeline::writeTime({qint64(tick),rate}));
}

void Backend::setupAudioCursor() {
    connect(this,&Backend::audioPositionRequested,m_worker,&PlayerWorker::seekAudioPosition);
    connect(this,&Backend::seekRequested,this,[this] { cancelAudioSeek(!m_viewingSource); });
    connect(this,&Backend::previewRequested,this,[this] { cancelAudioSeek(false); });
    connect(this,&Backend::newProjectRequested,this,[this] { cancelAudioSeek(true); });
    connect(this,&Backend::scrubRequested,this,[this] { cancelAudioSeek(true); });
    connect(this,&Backend::loadProjectRequested,this,[this] { cancelAudioSeek(true); });
    connect(this,&Backend::switchSequenceRequested,this,[this] { cancelAudioSeek(true); });
    connect(m_worker,&PlayerWorker::audioCursorRestored,this,
        [this](const QVariantMap &context,const QVariantMap &position,int frame) {
        if (!m_requestedAudioPosition.isEmpty() || !AudioTimeline::sameEditContext(context,m_audioEditContext,false)) return;
        m_audioPosition=position;
        if (m_position!=frame) { m_position=frame; emit positionChanged(); updateMulticamClip(); }
        emit audioPositionChanged();
    });
    connect(m_worker,&PlayerWorker::audioPositionReady,this,
        [this](quint64 request,const QVariantMap &context,const QVariantMap &position,int frame,const QString &error,const QImage &image,
               const QImage &sourceImage,int sourceFrame,const QString &sourcePath) {
        if (request!=m_audioSeekInFlight) return;
        m_audioSeekInFlight=0;
        if (request==m_audioSeekSerial && !m_viewingSource
            && AudioTimeline::sameEditContext(context,m_audioEditContext,false)) {
            m_requestedAudioPosition.clear();
            if (error.isEmpty()) {
                m_audioPosition=position;
                showFrame(image,frame);
                if (request==m_audioSeekSerial && m_mediaGang && !m_viewingSource
                    && sourcePath==m_previewRequestPath && sourceFrame>=0 && !sourceImage.isNull())
                    onPreviewFrame(sourceImage,sourceFrame,true);
            } else { setStatus(error); emit audioPositionChanged(); }
            const double speed=std::exchange(m_speedAfterAudioSeek,0.);
            if (speed!=0. && error.isEmpty()) { if (speed==1.) play(); else applySpeed(speed); }
        }
        dispatchAudioSeek();
    });
}
void Backend::closeProject() { newProject(); }
QVariantList Backend::recentProjectEntries() const {
    QVariantList rows;
    for(const auto &path:m_recent) rows.append(QVariantMap{{"path",path},{"name",QFileInfo(path).fileName()},{"missing",!QFileInfo::exists(path)}});
    return rows;
}
void Backend::removeRecentProject(const QString &path) {
    if(!m_recent.removeAll(path)) return;
    QSettings("omaedit","omaedit").setValue("recentProjects",m_recent); emit recentChanged();
}
void Backend::locateRecentProject(const QString &path) {
    if(projectBusy() || m_exporting) return;
    pause();
    m_locateRecentPath=path;
    m_chooserMode=ChooserMode::LocateProject;
    m_chooser->openFile(tr("Locate %1").arg(QFileInfo(path).fileName()),tr("OmaEdit project"),ProjectFile::openPatterns());
}
// True for a working frame, and for a picture the engine made for the display
// in one's place (playback's, a seek's before the playhead rests): what is
// saved from it is the working frame it stands for.
static bool savesAsWorking(const QImage &frame) {
    return WorkingFrame::isTagged(frame) || !frame.text(QStringLiteral("omaedit.shown")).isEmpty();
}
void Backend::exportFrameDialog() {
    const auto frame=m_viewingSource ? m_sourceFrame : m_frame;
    if(frame.isNull()) { m_backgroundTasks->post("warning",tr("No frame to export")); return; }
    pause(); m_exportFrameSource=m_viewingSource; m_chooserMode=ChooserMode::ExportFrame;
    m_chooser->saveFile(tr("Export Current Frame"),projectName()+"-frame"+(savesAsWorking(frame)?".exr":".png"),
        tr("PNG or OpenEXR frame"),{"*.png","*.exr"});
}
void Backend::saveProgramFrame(const QString &path) { saveFrameImage(path,false); }
// Waits for the worker, and so for everything queued to it: only for a caller
// that must hand the picture back at once (Grab Still returns the still's ID).
QImage Backend::wholeProgramFrame() const {
    if (m_frame.isNull() || m_frame.text(QStringLiteral("omaedit.fullSize")).isEmpty()) return m_frame;
    QImage whole;
    // (A skimmed picture is of the frame under the pointer, not the playhead's.)
    const int position = m_skimActive && m_skimShown >= 0 ? m_skimShown : m_position;
    QMetaObject::invokeMethod(m_worker, [&whole, worker = m_worker, position] { whole = worker->wholeFrame(position); },
                              Qt::BlockingQueuedConnection);
    return whole.isNull() ? m_frame : whole;
}
void Backend::setViewerDrawSize(const QSize &size) {
    if (size == m_viewerDrawSize) return;
    m_viewerDrawSize = size;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, size] { worker->setPlaybackDrawSize(size); }, Qt::QueuedConnection);
}
void Backend::setSourceViewerDrawSize(const QSize &size) {
    if (size == m_sourceDrawSize) return;
    m_sourceDrawSize = size;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, size] { worker->setSourceDrawSize(size); }, Qt::QueuedConnection);
}
void Backend::setSourceViewerDisplay(const QVariant &asked) {
    // OMAEDIT_VIEWER_SHOWN=0: every picture as a working frame.
    static const bool allowed = qEnvironmentVariable("OMAEDIT_VIEWER_SHOWN") != QLatin1String("0");
    const QVariant display = allowed ? asked : QVariant();
    if (display == m_sourceDisplay) return;
    m_sourceDisplay = display;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, display] { worker->setSourceDisplay(display); }, Qt::QueuedConnection);
}
void Backend::setViewerDisplay(const QVariant &display) {
    if (display == m_viewerDisplay) return;
    m_viewerDisplay = display;
    sendPlaybackDisplay();
}
void Backend::sendPlaybackDisplay() {
    // OMAEDIT_VIEWER_SHOWN=0: every playback picture as a working frame.
    static const bool allowed = qEnvironmentVariable("OMAEDIT_VIEWER_SHOWN") != QLatin1String("0");
    const QVariant display = allowed && (!m_scopesActive || !scopesReadBack()) ? m_viewerDisplay : QVariant();
    if (display != m_playbackDisplay) {
        m_playbackDisplay = display;
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, display] { worker->setPlaybackDisplay(display); }, Qt::QueuedConnection);
    }
    // With the scopes open, pictures made for the display have their scopes
    // counted on the engine as they are drawn.
    const QVariant scopes = m_scopesActive && !scopesReadBack() ? QVariant(measuredScopeOptions()) : QVariant();
    if (scopes != m_playbackScopes) {
        m_playbackScopes = scopes;
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, scopes] { worker->setPlaybackScopes(scopes); }, Qt::QueuedConnection);
    }
}
// OMAEDIT_SCOPES_READBACK=1: with the scopes open every picture is a working
// frame read back and measured on the CPU, as it was before the engine counted.
bool Backend::scopesReadBack() {
    return qEnvironmentVariable("OMAEDIT_SCOPES_READBACK") == QLatin1String("1");
}
QVariantMap Backend::measuredScopeOptions() const {
    return ScopeProcessing::measured(m_scopeOptions, LinearRenderer::outputSpace("rec" + m_projectSettings.value("colorspace", 709).toString()));
}
QString Backend::frameExportProblem(const QString &path,bool source) const {
    const auto suffix=QFileInfo(path).suffix().toLower();
    if(suffix!="png" && suffix!="exr") return tr("Choose a .png or .exr filename.");
    // A float OpenEXR holds scene-linear light, which only a managed frame
    // has; an unmanaged frame is display-encoded video.
    const auto frame=source ? m_sourceFrame : m_frame;
    if(suffix=="exr" && !frame.isNull() && !savesAsWorking(frame))
        return tr("OpenEXR frames need colour management. Switch the viewer's colour engine to OpenColorIO, or save a PNG.");
    return {};
}
void Backend::saveFrameImage(const QString &path,bool source) {
    const auto shown=source ? m_sourceFrame : m_frame;
    // A reduced playback picture is not what is saved: the worker draws the
    // whole one, in the same queued step that writes the file, so the window
    // never waits for whatever the worker is doing first.
    // (The source viewer's the same: its clip's whole working frame.)
    const bool reduced=!shown.isNull() && !shown.text(QStringLiteral("omaedit.fullSize")).isEmpty();
    const int position=source ? m_sourcePosition : m_position;
    if(shown.isNull() || path.trimmed().isEmpty()) {
        setStatus(tr("Open media before exporting a frame."));
        if(source) emit sourceFrameSaved(false,path);
        emit frameExported(false,path); return;
    }
    const QString problem=frameExportProblem(path,source);
    // A PNG is the frame as the project delivers it (as a frame of its
    // export): Rec.709 unless the project says Rec.601 or Rec.2020.
    const int colourspace=m_projectSettings.value(QStringLiteral("colorspace")).toInt();
    const QString delivery=colourspace==2020?QStringLiteral("rec2020/rec709"):colourspace==601?QStringLiteral("rec601/rec709"):QStringLiteral("rec709/rec709");
    QMetaObject::invokeMethod(m_worker,[this,shown,reduced,position,path,source,problem,delivery] {
        QImage frame=shown;
        if(reduced) { const QImage whole=source ? m_worker->wholeSourceFrame(position) : m_worker->wholeFrame(position); if(!whole.isNull()) frame=whole; }
        QString error; const auto suffix=QFileInfo(path).suffix().toLower();
        const bool linear=WorkingFrame::isTagged(frame),exr=suffix=="exr";
        const auto working=linear ? WorkingFrame::fromTagged(frame,&error) : WorkingFrame{};
        bool ok=false;
        if(!problem.isEmpty()) error=problem;
        else if(exr) ok=working.save(path,{},&error);
        else {
            const auto pixels=linear ? working.output(delivery,&error).convertToFormat(QImage::Format_RGBA64) : frame;
            QSaveFile file(path);
            // Quick compression (zlib level 1): the same pixels in a fraction
            // of the default's time, the file a little bigger.
            ok=!pixels.isNull() && file.open(QIODevice::WriteOnly) && pixels.save(&file,"PNG",80) && file.commit();
            if(!ok && error.isEmpty()) error=file.errorString();
        }
        QMetaObject::invokeMethod(this,[this,ok,path,source,error] {
            const auto text=ok ? tr("Frame saved to %1").arg(path) : tr("Could not save frame: %1").arg(error);
            setStatus(text); m_backgroundTasks->post(ok?"success":"error",tr("Export frame"),text);
            if(source) emit sourceFrameSaved(ok,path);
            emit frameExported(ok,path);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
