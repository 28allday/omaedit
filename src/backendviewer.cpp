#include "backend_p.h"

void Backend::setCompareMode(bool on) {
    if (m_compareMode == on)
        return;
    m_compareMode = on;
    emit compareModeChanged();
    emit compareRequested(on);
    setStatus(on ? tr("Before / after: drag the divider")
                 : tr("Before / after off"));
}

void Backend::setTwoUpEdge(int edge) {
    edge = edge < 0 ? -1 : edge;
    if (m_twoUpEdge == edge)
        return;
    m_twoUpEdge = edge;
    emit twoUpEdgeChanged();
    emit twoUpEdgeRequested(edge);
}

void Backend::onBeforeFrame(const QImage &image) {
    m_beforeFrame = image;
    emit beforeFrameChanged();
}

void Backend::previewClip(int libraryIndex) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    pause();
    m_previewLibraryIndex = libraryIndex;
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    m_previewRequestPath = entry.value(QStringLiteral("multicam")).toBool()
                               ? angleSource(entry, entry.value(QStringLiteral("activeAngle")).toInt())
                                     .value(QStringLiteral("path")).toString()
                               : entry.value(QStringLiteral("path")).toString();
    emit previewRequested(m_previewRequestPath);
}

void Backend::viewTimeline() {
    if (!m_viewingSource)
        return;
    pause();
    m_viewingSource = false;
    // Any seek still aimed at the source is stale, and its answer may come
    // back as a timeline frame (the worker routes by ITS current mode) —
    // dropping the flags beats wedging on a tag that never matches.
    m_seekInFlight = false;
    m_pendingSeek = -1;
    emit sourceChanged();
    emit closePreviewRequested();
}

void Backend::onPreviewOpened(bool ok, const QString &path, int duration, double fps) {
    const QString name = QFileInfo(path).fileName();
    if (!ok && !m_previewRequestPath.isEmpty() && path != m_previewRequestPath) {
        // A newer preview was requested while this one failed: its reply is
        // on the way and must not be reset to the timeline view behind it.
        setStatus(tr("Could not preview %1.").arg(name));
        return;
    }
    if (!ok) {
        // The worker dropped source mode (or never had it): fall back to
        // the timeline view and clear any seek left waiting on a preview
        // frame that will never come.
        m_previewLibraryIndex = -1;
        m_previewShownPath.clear();
        m_viewingSource = false;
        m_seekInFlight = false;
        m_pendingSeek = -1;
        m_pendingSourceSeek = -1;
        m_pendingSourceRangeIn = m_pendingSourceRangeOut = -1;
        emit sourceChanged();
        setStatus(tr("Could not preview %1.").arg(name));
        return;
    }
    m_viewingSource = true;
    m_previewShownPath = path;
    // Timeline-aimed seeks queued before the flip must not fire into the
    // source view (mirrors viewTimeline's reset).
    m_seekInFlight = false;
    m_pendingSeek = -1;
    m_sourceDuration = duration;
    m_sourceFps = fps;
    m_sourcePosition = 0;
    emit sourceChanged();
    emit sourcePositionChanged();
    setStatus(tr("Viewing %1 — I/O mark a range").arg(name));
    if (m_pendingSourceSeek >= 0) { // Match Frame: park on the frame, mark the span
        const int seekTo = m_pendingSourceSeek;
        m_pendingSourceSeek = -1;
        if (m_pendingSourceRangeIn >= 0 || m_pendingSourceRangeOut >= 0)
            setRange(m_pendingSourceRangeIn, m_pendingSourceRangeOut);
        m_pendingSourceRangeIn = m_pendingSourceRangeOut = -1;
        seek(seekTo);
        setStatus(tr("Match frame in %1").arg(name));
    }
}

void Backend::onPreviewFrame(const QImage &image, int position, bool placed) {
    // As showFrame: a picture for an overtaken seek, or one shown while a
    // newer seek waits to go out, leaves the playhead where it was put. So
    // does a picture that answers no seek while one is still unanswered: the
    // source's first picture can arrive after the playhead was moved, and put
    // it back at the start. A picture a linked seek placed is not one of those.
    const bool unanswered = !placed && !m_sentSeeks[true].isEmpty() && !m_sentSeeks[true].contains(position);
    const bool stale = m_speed == 0.0 && overtaken(true, position);
    const bool waiting = m_speed == 0.0 && !stale && (unanswered || (m_pendingSeek >= 0 && m_pendingSeekSource));
    m_sourceFrame = image;
    if (m_viewingSource && !image.isNull()) notePresentationFrame();
    if (!stale && !waiting && m_sourcePosition != position) {
        m_sourcePosition = position;
        emit sourcePositionChanged();
    }
    emit sourceFrameChanged();
    if (!stale) flushPendingSeek(true);
}

void Backend::markRangeIn() {
    setRangeMark(true);
}

void Backend::markRangeOut() {
    setRangeMark(false);
}

void Backend::setRangeMark(bool markIn) {
    if (!m_viewingSource || m_previewLibraryIndex < 0
        || m_previewLibraryIndex >= m_library.size())
        return;
    QVariantMap entry = m_library.at(m_previewLibraryIndex).toMap();
    int in = entry.value(QStringLiteral("rangeIn"), -1).toInt();
    int out = entry.value(QStringLiteral("rangeOut"), -1).toInt();
    if (markIn) {
        in = m_sourcePosition;
        if (out >= 0 && out < in)
            out = -1; // an in past the old out invalidates it
    } else {
        out = m_sourcePosition;
        if (in >= 0 && in > out)
            in = -1;
    }
    entry.insert(QStringLiteral("rangeIn"), in);
    entry.insert(QStringLiteral("rangeOut"), out);
    m_library[m_previewLibraryIndex] = entry;
    emit libraryChanged();
    setDirty(true); // ranges are saved in the project file
    setStatus(tr("Range %1 → %2")
                  .arg(in >= 0 ? timecodeFor(in, m_sourceFps) : QStringLiteral("start"),
                       out >= 0 ? timecodeFor(out, m_sourceFps) : QStringLiteral("end")));
}

void Backend::setLibraryRange(int libraryIndex, int in, int out) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    QVariantMap entry = m_library.at(libraryIndex).toMap();
    // Marks always address recorded pictures, including when Clip Attributes
    // changes playback speed. The effective duration may be shorter or longer.
    const int last = qMax(0, entry.value("sourceDuration", entry.value("duration")).toInt() - 1);
    in = in < 0 ? -1 : qBound(0, in, last);
    out = out < 0 ? -1 : qBound(0, out, last);
    if (in >= 0 && out >= 0 && out < in)
        qSwap(in, out);
    entry.insert(QStringLiteral("rangeIn"), in);
    entry.insert(QStringLiteral("rangeOut"), out);
    m_library[libraryIndex] = entry;
    emit libraryChanged();
    setDirty(true);
}

void Backend::setRange(int in, int out) {
    if (m_previewLibraryIndex < 0 || m_previewLibraryIndex >= m_library.size())
        return;
    const int last = qMax(0, m_sourceDuration - 1);
    in = in < 0 ? -1 : qBound(0, in, last);
    out = out < 0 ? -1 : qBound(0, out, last);
    if (in >= 0 && out >= 0 && out < in)
        qSwap(in, out);
    QVariantMap entry = m_library.at(m_previewLibraryIndex).toMap();
    if (entry.value(QStringLiteral("rangeIn"), -1).toInt() == in
        && entry.value(QStringLiteral("rangeOut"), -1).toInt() == out)
        return;
    entry.insert(QStringLiteral("rangeIn"), in);
    entry.insert(QStringLiteral("rangeOut"), out);
    m_library[m_previewLibraryIndex] = entry;
    emit libraryChanged();
    setDirty(true); // ranges are saved in the project file
    setStatus(tr("Range %1 → %2")
                  .arg(in >= 0 ? timecodeFor(in, m_sourceFps) : QStringLiteral("start"),
                       out >= 0 ? timecodeFor(out, m_sourceFps) : QStringLiteral("end")));
}

void Backend::clearRange() {
    if (m_previewLibraryIndex < 0 || m_previewLibraryIndex >= m_library.size())
        return;
    QVariantMap entry = m_library.at(m_previewLibraryIndex).toMap();
    const bool hadRange = entry.value(QStringLiteral("rangeIn"), -1).toInt() >= 0
                          || entry.value(QStringLiteral("rangeOut"), -1).toInt() >= 0;
    entry.insert(QStringLiteral("rangeIn"), -1);
    entry.insert(QStringLiteral("rangeOut"), -1);
    m_library[m_previewLibraryIndex] = entry;
    emit libraryChanged();
    if (hadRange)
        setDirty(true); // ranges are saved in the project file
    setStatus(tr("Range cleared"));
}


// A picture sent straight away answers something other than playback (a
// seek, an edit, a load): anything still scheduled is out of date.
void Backend::onFrame(const QImage &image, int position) {
    if (!m_requestedAudioPosition.isEmpty()) return;
    m_scheduledFrames.clear();
    m_scheduledTimer.stop();
    showFrame(image, position);
}

void Backend::onFrameScheduled(const QImage &image, int position, qint64 dueNs) {
    if (!playing() || m_viewingSource)
        return; // made before a pause reached the worker; the picture on screen stands
    m_scheduledFrames.append({image, position, dueNs});
    presentScheduled();
}

// Shows the newest scheduled picture whose time has come; older ones that
// were never shown are dropped frames. Then sleeps until the next is due.
void Backend::presentScheduled() {
    const qint64 now = PlayerWorker::steadyNs();
    int due = -1;
    for (int i = 0; i < m_scheduledFrames.size() && m_scheduledFrames.at(i).dueNs <= now; ++i)
        due = i;
    if (due >= 0) {
        if (playing()) m_droppedFrames += due;
        const ScheduledFrame frame = m_scheduledFrames.at(due);
        m_scheduledFrames.remove(0, due + 1);
        showFrame(frame.image, frame.position);
    }
    if (m_scheduledFrames.isEmpty()) { m_scheduledTimer.stop(); return; }
    const qint64 wait = m_scheduledFrames.first().dueNs - now;
    m_scheduledTimer.start(int((qMax<qint64>(0, wait) + 999999) / 1000000));
}

void Backend::onScheduleCleared(int settled) {
    for (const ScheduledFrame &frame : std::as_const(m_scheduledFrames))
        if (frame.position == settled && settled != m_position) { showFrame(frame.image, frame.position); break; }
    m_scheduledFrames.clear();
    m_scheduledTimer.stop();
}

void Backend::showFrame(const QImage &image, int position) {
    // A picture for a seek a newer one has overtaken: the playhead stays where
    // it was put, and the newest seek is still to come. While a seek is out or
    // waits to go out, no picture that answers none (an edit's, say, made at
    // the old playhead) moves the playhead either.
    const bool seekOut = m_seekInFlight && !m_seekInFlightSource && !m_sentSeeks[false].isEmpty()
                         && !m_sentSeeks[false].contains(position);
    const bool stale = !playing() && overtaken(false, position);
    const bool waiting = !playing() && !stale && (seekOut || (m_pendingSeek >= 0 && !m_pendingSeekSource));
    if (m_skimActive) m_skimHome = image;  // shown when the skim ends
    else m_frame = image;
    if (!m_viewingSource && !image.isNull()) notePresentationFrame();
    // The last picture shown for a playhead parked past the end leaves it there.
    const bool pastEnd = !playing() && m_position >= m_duration && position == m_duration - 1;
    if (!stale && !waiting && m_position != position && !pastEnd) {
        m_position = position;
        emit positionChanged();
        updateMulticamClip();
    }
    if (!m_skimActive) emit frameChanged();
    if (stale) return;
    if (waiting) { flushPendingSeek(false); return; }
    syncAudioPosition(position);
    flushPendingSeek(false);
}

void Backend::sentSeek(bool source, int frame) {
    m_sentSeeks[source].append(frame);
    if (m_sentSeeks[source].size() > 64) m_sentSeeks[source].removeFirst();
}

bool Backend::overtaken(bool source, int position) {
    auto &sent = m_sentSeeks[source];
    const qsizetype at = sent.indexOf(position);
    if (at < 0) return false;  // not a seek's picture (an edit's, say)
    if (at == sent.size() - 1) { sent.clear(); return false; }  // the newest: it settles the playhead
    sent.remove(0, at + 1);
    return true;
}

void Backend::skim(int frame) {
    if (m_viewingSource || playing() || m_duration < 1) return;
    if (!m_skimActive) { m_skimActive = true; m_skimHome = m_frame; }
    const int clamped = qBound(0, frame, m_duration - 1);
    if (m_skimInFlight) { m_pendingSkim = clamped; return; }
    m_skimInFlight = true;
    emit skimRequested(clamped);
}

void Backend::endSkim() {
    m_pendingSkim = -1;
    if (!m_skimActive) return;
    m_skimActive = false;
    m_skimShown = -1;
    m_frame = m_skimHome;
    m_skimHome = {};
    emit frameChanged();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->endSkim(); }, Qt::QueuedConnection);
}

// The whole working frame of the skimmed frame on screen, which was first
// shown as playback draws (PlayerWorker::settleFrame). Late for a skim that
// has moved on or ended, it is dropped.
void Backend::onSkimSettled(const QImage &image, int frame) {
    if (!m_skimActive || frame != m_skimShown || image.isNull()) return;
    m_frame = image;
    emit frameChanged();
}

void Backend::onSkimFrame(const QImage &image, int frame) {
    m_skimInFlight = false;
    if (m_skimActive && !image.isNull()) { m_frame = image; m_skimShown = qBound(0, frame, qMax(0, m_duration - 1)); emit frameChanged(); }
    if (m_skimActive && m_pendingSkim >= 0) {
        const int next = m_pendingSkim;
        m_pendingSkim = -1;
        m_skimInFlight = true;
        emit skimRequested(next);
    }
}

void Backend::flushPendingSeek(bool fromSource) {
    // A frame from the other monitor doesn't answer this seek — a preview's
    // frame 0 must not flush a timeline skim into the source (or vice versa).
    if (m_seekInFlight && m_seekInFlightSource != fromSource)
        return;
    // A pending seek aimed at a view we've since left is stale.
    if (m_pendingSeek >= 0 && m_pendingSeekSource != m_viewingSource)
        m_pendingSeek = -1;
    if (m_pendingSeek >= 0) {
        const int next = m_pendingSeek;
        m_pendingSeek = -1;
        m_seekInFlight = true;
        m_seekInFlightSource = m_pendingSeekSource;
        sentSeek(m_pendingSeekSource, next);
        emit seekRequested(next);
    } else {
        m_seekInFlight = false;
        m_sentSeeks[fromSource].clear();
    }
}

void Backend::play() {
    endSkim();  // playback starts from the playhead
    if (!m_requestedAudioPosition.isEmpty()) { m_speedAfterAudioSeek=1.; return; }
    const int total = m_viewingSource ? m_sourceDuration : m_duration;
    const int pos = m_viewingSource ? m_sourcePosition : m_position;
    if (total < 1 || m_speed == 1.0)
        return;
    if (pos >= total - 1 && (m_viewingSource || m_audioPosition.isEmpty()))
        emit seekRequested(0);
    applySpeed(1.0);
}

void Backend::pause() {
    m_speedAfterAudioSeek=0.;
    if (!m_pendingAudioSeek.isEmpty()) m_pendingAudioSeek["audition"]=false;
    applySpeed(0.0);
}

void Backend::togglePlay() {
    playing() ? pause() : play();
}

void Backend::shuttle(int direction) {
    if (!m_requestedAudioPosition.isEmpty()) {
        m_speedAfterAudioSeek=nextShuttleSpeed(m_speedAfterAudioSeek,direction); return;
    }
    if ((m_viewingSource ? m_sourceDuration : m_duration) < 1)
        return;
    applySpeed(nextShuttleSpeed(m_speed, direction));
}

void Backend::seek(int frame) {
    const int total = m_viewingSource ? m_sourceDuration : m_duration;
    if (total < 1)
        return;
    cancelAudioSeek(!m_viewingSource);
    // On the timeline the playhead runs on into the empty tracks after the
    // edit; there the viewer keeps the last picture.
    const int placed = qBound(0, frame, total - 1 + (m_viewingSource ? 0 : m_pastEndFrames));
    const int clamped = qMin(placed, total - 1);
    endSkim();
    // The playhead goes where it was put at once; the picture follows.
    if (m_viewingSource) {
        if (m_sourcePosition != clamped) { m_sourcePosition = clamped; emit sourcePositionChanged(); }
    } else {
        if (playing()) { m_scheduledFrames.clear(); m_scheduledTimer.stop(); }  // pictures from before it
        const bool wasPast = m_position >= total;
        if (m_position != placed) { m_position = placed; emit positionChanged(); updateMulticamClip(); }
        // Along the empty tracks the last picture is already up.
        if (wasPast && placed >= total && !playing()) return;
    }
    // Dragging fires seeks faster than heavy codecs decode; keep one in
    // flight and remember only the newest — the rest were already stale.
    if (m_seekInFlight) {
        m_pendingSeek = clamped;
        m_pendingSeekSource = m_viewingSource;
        m_worker->cancelFrame();
        return;
    }
    m_seekInFlight = true;
    m_seekInFlightSource = m_viewingSource;
    m_worker->cancelFrame();
    sentSeek(m_viewingSource, clamped);
    emit seekRequested(clamped);
}

void Backend::step(int frames) {
    pause();
    seek((m_viewingSource ? m_sourcePosition : m_position) + frames);
}

QString Backend::timecode(int frame) const {
    // Added wide (two ints can pass the int range) and taken round the day,
    // which the readout does anyway.
    const qint64 at = qint64(frame) + startFrame();
    const qint64 day = 86400LL * qMax(1, qRound(m_fps));
    return timecodeFor(at < 0 ? -1 : int(at % day), m_fps);
}

QString Backend::frameTimecode(int frame) const {
    return timecodeFor(frame, m_fps);
}

QString Backend::sourceTimecode(int frame) const {
    return timecodeFor(frame, m_sourceFps);
}

void Backend::applySpeed(double speed) {
    const bool stopping = speed == 0.0 && m_speed != 0.0;
    if (stopping) { m_worker->cancelFrame();m_scheduledFrames.clear(); m_scheduledTimer.stop(); }
    emit speedRequested(speed);
    // The picture on screen is where playback stopped; the worker may have
    // counted one more frame as shown than the viewer got round to.
    if (stopping && !m_viewingSource)
        QMetaObject::invokeMethod(m_worker, [this, at = m_position] { m_worker->settleAt(at); }, Qt::QueuedConnection);
    if (!m_viewingSource && (stopping || (speed!=0. && speed!=1.))) {
        m_audioPosition.clear(); emit audioPositionChanged();
    }
    if (m_speed == speed)
        return;
    m_speed = speed;
    emit playingChanged();
}

void Backend::setMonitorOptions(bool muted, bool dim, bool mono) {
    m_monitorSettings={{"muted",muted},{"dim",dim},{"mono",mono}};
    QMetaObject::invokeMethod(m_worker,[this,muted,dim,mono]{m_worker->setMonitorOptions(muted,dim,mono);},Qt::QueuedConnection);
    m_recorder->setMonitorOptions(muted,dim,mono,m_audioOutput);
    emit monitorVolumeChanged();
}
