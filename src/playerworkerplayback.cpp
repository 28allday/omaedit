#include "stagetimer.h"
#include "playerworker_p.h"

namespace {
constexpr int kBytesPerSample = 2; // s16
// Sound ahead of the picture (see "Sound ahead of the picture" below).
// How far ahead of the playhead timeline sound is mixed: a worker held for
// less than this (less the block or two in the making) is not heard. A second
// is what the audit asked for; it costs 190 kB and a few milliseconds of mixing
// after each start, and nothing while the editor is changing the mix.
constexpr qint64 kSoundAheadNs = 1000000000;
// After a change to what the mixer makes, sound is made no further ahead than
// its picture for this long: a fader being moved, or an automation pass (a
// change a frame), then costs no mixing that the next change throws away, and
// is made just as it was when every frame's sound was made with its picture.
constexpr qint64 kSoundShallowNs = 250000000;
// The join where sound mixed before a change gives way to sound mixed after it.
constexpr int kSpliceSamples = 480; // 10 ms
}

// True when a seek or a skim is drawn the way playback draws its frames: at
// the size the viewer shows, for its display, read back as 8 bits. Only
// while paused, with a display given (a single channel or unmanaged viewing
// give none) and nothing compared beside it: otherwise the request is the
// working frame, as it always was. With the scopes open the picture's scopes
// are counted on the engine as it is drawn (setPlaybackScopes).
bool PlayerWorker::interactiveShown() const {
    return m_playbackDisplay.isValid() && !m_compare && !m_sourceMode && m_speed == 0.;
}

// After a seek's or a skim's picture went out. One that stands for a whole
// working frame (drawn small, or for the display only) is owed that frame
// once the playhead rests; a whole one settles the matter.
void PlayerWorker::settleDue(const QImage &shown, int skim) {
    const bool owed = !shown.isNull() && !shown.text(QStringLiteral("omaedit.fullSize")).isEmpty();
    if (skim >= 0) m_settleSkim = owed ? skim : -1;
    else m_settleSeek = owed;
    if (m_settleSeek || m_settleSkim >= 0) m_settle.start();
    else if (!m_settleSource) m_settle.stop();
}

// The playhead has rested: the whole working frame of what is on screen, as a
// seek gave before seeks were drawn the quick way. Stills, saved frames,
// scopes and colour picks read this one. It answers no seek, so a newer
// request that overtakes it only puts it off (nothing is told of a discard).
void PlayerWorker::settleFrame() {
    settleSource();
    if (!m_settleSeek && m_settleSkim < 0) return;
    if (m_timeline.isEmpty()) { m_settleSeek = false; m_settleSkim = -1; return; }
    if (m_speed != 0.) {
        // Playing the timeline: its pause draws the whole frame. A source
        // playing leaves the timeline's picture owed until it stops.
        if (!m_sourceMode) { m_playedSmall |= m_settleSeek; m_settleSeek = false; m_settleSkim = -1; }
        else m_settle.start();
        return;
    }
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    if (m_settleSeek) {
        const auto decoded = decodePlaybackFrame(m_position, false);
        if (decoded.cancelled || RenderScope::cancelled()) { m_settle.start(); return; }
        m_settleSeek = false;
        if (!decoded.error.isEmpty()) emit frameError(decoded.error);
        emit frameReady(decoded.image, m_position);
    }
    if (m_settleSkim >= 0) {
        const int frame = qBound(0, m_settleSkim, m_timeline.frameCount() - 1);
        const auto decoded = decodePlaybackFrame(frame, false);
        if (decoded.cancelled || RenderScope::cancelled()) { m_settle.start(); return; }
        const int settled = std::exchange(m_settleSkim, -1);
        if (!decoded.image.isNull()) emit skimFrameSettled(decoded.image, settled);
    }
}

void PlayerWorker::endSkim() {
    m_settleSkim = -1;
    if (!m_settleSeek && !m_settleSource) m_settle.stop();
}

// The source viewer's picture at `position` (with its sound, when asked):
// drawn for the viewer when it has given its display, as the timeline's
// played and scrubbed pictures are, else the working frame. One drawn for the
// viewer is owed its working frame (settleSource).
MediaPlayer::Decoded PlayerWorker::sourceShown(int position, bool audio) {
    const bool quick = m_sourceDisplay.isValid();
    if (quick) m_preview.setViewerDraw(m_sourceDrawSize, m_sourceDisplay);
    MediaPlayer::Decoded decoded = m_preview.decodeAt(position, audio, kFrequency, kChannels);
    if (quick) m_preview.setViewerDraw({}, {});
    // One given up for a newer request (a pause takes back the picture being
    // made ahead) shows nothing: what the picture on screen is owed stands.
    if (decoded.cancelled || RenderScope::cancelled()) return decoded;
    m_settleSource = !decoded.image.isNull() && !decoded.image.text(QStringLiteral("omaedit.fullSize")).isEmpty();
    return decoded;
}

// The source viewer's playhead has rested, or its clip has stopped playing:
// the whole working frame of the picture on screen, which a saved frame and
// whatever else reads the source's picture take. It answers no seek; a newer
// request that overtakes it only puts it off.
void PlayerWorker::settleSource() {
    if (!m_settleSource) return;
    if (!m_sourceMode || !m_preview.isOpen()) { m_settleSource = false; return; }
    if (m_speed != 0.) return;  // playing: its pause asks again
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    const auto decoded = m_preview.decodeAt(m_previewPos, false);
    if (decoded.cancelled || RenderScope::cancelled()) { m_settle.start(); return; }
    m_settleSource = false;
    if (!decoded.error.isEmpty()) emit frameError(decoded.error);
    else emit previewFrameReady(decoded.image, m_previewPos);
}

void PlayerWorker::refreshFrame(bool interactive) {
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    emit linearPlaybackChanged(m_timeline.originalsOnly());
    describePreview();
    if (m_timeline.isEmpty()) {
        emit frameReady(QImage(), 0);
        if (m_compare)
            emit beforeFrameReady(QImage());
        return;
    }
    // While playing, the next queued frame is at most a frame away; decoding
    // here would pull the decoder back behind the frames already queued.
    if (m_speed != 0.0 && !m_sourceMode && m_playAnchored)
        return;
    const bool quick = interactive && interactiveShown();
    if (quick) {
        m_timeline.setDrawSize(m_playbackDrawSize);
        m_timeline.setShownAs(m_playbackDisplay);
        m_timeline.setScopesAs(m_playbackScopes);
    }
    const auto decoded=decodePlaybackFrame(m_position,false);
    if (quick) {
        m_timeline.setDrawSize({});
        m_timeline.setShownAs({});
        m_timeline.setScopesAs({});
    }
    if(decoded.cancelled || RenderScope::cancelled()){emit frameDiscarded(false);return;}
    if(!decoded.error.isEmpty())emit frameError(decoded.error);
    emit frameReady(decoded.image,m_position);
    settleDue(decoded.image);
    if (m_compare)
        emit beforeFrameReady(m_timeline.decodeBefore(m_position).image);
}

void PlayerWorker::skimTo(int frame) {
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    if (m_sourceMode || m_timeline.isEmpty()) { emit skimFrameReady(QImage(), frame); return; }
    // Skimmed frames are drawn as a seek's are (see refreshFrame).
    const bool quick = interactiveShown();
    const int clamped = qBound(0, frame, m_timeline.frameCount() - 1);
    if (quick) {
        m_timeline.setDrawSize(m_playbackDrawSize);
        m_timeline.setShownAs(m_playbackDisplay);
        m_timeline.setScopesAs(m_playbackScopes);
    }
    const auto decoded = decodePlaybackFrame(clamped, false);
    if (quick) {
        m_timeline.setDrawSize({});
        m_timeline.setShownAs({});
        m_timeline.setScopesAs({});
    }
    const bool dropped = decoded.cancelled || RenderScope::cancelled();
    emit skimFrameReady(dropped ? QImage() : decoded.image, frame);
    if (!dropped) settleDue(decoded.image, clamped);
}

void PlayerWorker::refreshTwoUp() {
    if (m_twoUpEdge < 0)
        return;
    const int total = m_timeline.frameCount();
    if (m_timeline.isEmpty() || total < 1) {
        emit twoUpReady(QImage(), QImage());
        return;
    }
    const QImage outgoing = m_twoUpEdge >= 1 && m_twoUpEdge - 1 < total
                                ? m_timeline.decodeAt(m_twoUpEdge - 1, false).image : QImage();
    const QImage incoming = m_twoUpEdge < total ? m_timeline.decodeAt(m_twoUpEdge, false).image
                                                : QImage();
    emit twoUpReady(outgoing, incoming);
}

void PlayerWorker::setTwoUpEdge(int edge) {
    if (m_twoUpEdge == edge)
        return;
    m_twoUpEdge = edge;
    if (edge < 0)
        emit twoUpReady(QImage(), QImage());
    else
        refreshTwoUp();
}

void PlayerWorker::setCompare(bool on) {
    if (m_compare == on)
        return;
    m_compare = on;
    if (on)
        refreshFrame();
    else
        emit beforeFrameReady(QImage());
}

void PlayerWorker::seek(int frame) {
    // A render reads its own scratch timeline, so a seek is as free as
    // playback is; dropped here, the window would wait for its picture until
    // the render ended.
    if (activeEmpty())
        return;
    mediaGangReady();
    if (m_sourceMode) {
        RenderScope render(frameRequest());
        ViewerDraw viewerDraw(this);  // filmstrip tiles wait
        m_previewPos = qBound(0, frame, m_preview.frameCount() - 1);
        flushAudio();
        const auto decoded=sourceShown(m_previewPos,false);
        if(decoded.cancelled || RenderScope::cancelled()){emit frameDiscarded(true);return;}
        if(!decoded.error.isEmpty())emit frameError(decoded.error);
        emit previewFrameReady(decoded.image, m_previewPos);
        if(m_settleSource)m_settle.start();
        refreshMediaGang();
        return;
    }
    const int clamped = qBound(0, frame, m_timeline.frameCount() - 1);
    m_fineAudioPosition.clear(); m_fineAudioContext.clear(); m_fineAudioFrame=-1;
    m_position = clamped;
    flushAudio();
    refreshFrame(true);
    refreshMediaGang();
}

void PlayerWorker::requestSpeed(double speed, quint64 request) {
    m_speedRequest = request;
    if (m_speed == speed) {
        if (speed==0. && m_pausedContext.isEmpty()) flushAudio(); // Pause also stops an audition queued while parked.
        emit playbackSpeedChanged(m_speed, request);
    }
    else setSpeed(speed);
}

void PlayerWorker::setPlaybackDrawSize(const QSize &size) { m_playbackDrawSize = size; }
void PlayerWorker::setPlaybackDisplay(const QVariant &display) {
    m_playbackDisplay = display;
    // No display any more (a single channel chosen): whatever
    // reads the picture now wants the working frame, so one owed comes at once.
    if (!display.isValid() && (m_settleSeek || m_settleSkim >= 0)) {
        m_settle.stop();
        QMetaObject::invokeMethod(this, &PlayerWorker::settleFrame, Qt::QueuedConnection);
    }
}
void PlayerWorker::setPlaybackScopes(const QVariant &options) {
    const bool opened = options.isValid() && !m_playbackScopes.isValid();
    m_playbackScopes = options;
    // Opened over a picture drawn for the viewer alone: that one has no
    // counts, so the working frame it is owed comes at once to be measured.
    if (opened && (m_settleSeek || m_settleSkim >= 0)) {
        m_settle.stop();
        QMetaObject::invokeMethod(this, &PlayerWorker::settleFrame, Qt::QueuedConnection);
    }
}
void PlayerWorker::setSourceDrawSize(const QSize &size) { m_sourceDrawSize = size; }
void PlayerWorker::setSourceDisplay(const QVariant &display) {
    m_sourceDisplay = display;
    // No display any more: what reads the source's picture now wants the
    // working frame, so one owed comes at once.
    if (!display.isValid() && m_settleSource) QMetaObject::invokeMethod(this, &PlayerWorker::settleSource, Qt::QueuedConnection);
}
QImage PlayerWorker::wholeSourceFrame(int position) {
    if (!m_preview.isOpen() || m_preview.frameCount() < 1) return {};
    return m_preview.decodeAt(qBound(0, position, m_preview.frameCount() - 1), false).image;
}
QImage PlayerWorker::wholeFrame(int position) {
    if (m_timeline.isEmpty()) return {};
    return m_timeline.decodeAt(qBound(0, position, m_timeline.frameCount() - 1), false).image;
}

void PlayerWorker::setSpeed(double speed) {
    // Rendering reads a scratch timeline, so playback stays free.
    if (m_speed == speed) {
        if (speed==0. && m_pausedContext.isEmpty()) flushAudio();
        return;
    }
    if (speed == 0.) {
        m_automationWrites.clear(); m_automationRecorded=false;
        // The keys written since the pass's first went out as tracks only.
        if (std::exchange(m_automationUnannounced, false)) announceMix();
    }
    else if (m_speed == 0. && !m_sourceMode) {
        m_automationRecorded=false;
        for(int t=0;t<m_timeline.trackCount();++t){const auto mix=m_timeline.trackMix(t);
            if(mix.value("automation")=="write" && !m_timeline.trackLocked(t))
                for(const auto &key:QStringList{"volume","pan"})m_automationWrites[QString::number(t)+":"+key]={t,key,mix.value(key).toDouble(),mix};}
    }
    const bool parkedFine=speed==0. && m_speed==1. && !m_sourceMode && parkFinePlayback();
    if (!parkedFine) settlePlayback();
    if (m_speed!=0. && !m_sourceMode && !parkedFine) { m_fineAudioPosition.clear(); m_fineAudioContext.clear(); m_fineAudioFrame=-1; }
    m_speed = speed;
    updateBackgroundRenderBusy();
    if (m_speed == 0.0 || activeEmpty()) {
        m_speed = 0.0;
        m_pump.stop();
        flushAudio(parkedFine);
        emit audioLevels(-100.0f, -100.0f); // meters fall silent
        m_timeline.takeTrackPeaks();
        emit trackLevels(QList<float>());
        emit audioRoutingLevels(QVariantList());
        emit spectrumChanged(QList<float>());
        scheduleStripWork(); // paused decoding may resume now
        emit playbackSpeedChanged(m_speed, m_speedRequest);
        // The source's paused picture as a working frame again.
        if (m_sourceMode && m_settleSource) QMetaObject::invokeMethod(this, &PlayerWorker::settleSource, Qt::QueuedConnection);
        if (parkedFine) {
            emit audioCursorRestored(m_fineAudioContext,m_fineAudioPosition,m_position);
            refreshFrame();
            refreshMediaGang();
            m_playedSmall = false;
        } else if (m_playedSmall) {
            // The paused picture whole again, once the viewer has said where
            // playback stopped (settleAt is already on its way).
            QTimer::singleShot(0, this, [this] {
                if (m_speed != 0. || !m_playedSmall) return;
                m_playedSmall = false;
                refreshFrame();
            });
        }
        return;
    }
    // A new speed restarts the clock from the frame on screen, with an empty
    // sink (a scrub may have left sound in it; shuttle is silent).
    const bool retained=m_speed==1. && !m_sourceMode && !m_pausedContext.isEmpty()
        && m_pausedContext==audioEditContext();
    flushAudio(retained);
    emit playbackSpeedChanged(m_speed, m_speedRequest);
}

// Playback. Frames are produced a few ahead of the picture and queued with a
// due time one exact frame period after the last, so the cadence comes from
// the clock, not from how long a frame took. (A timeline frame's layers are
// decoded on several threads at once, and the next frame's begun while this
// one is finished: "layers fetched side by side" in timelinerenderer.cpp.) Timeline
// pictures go to the viewer as soon as they exist (frameScheduled) and are
// shown there when due; here, a frame's due time moves the playhead, meters
// and automation. At normal speed the due times are steered gently towards
// the sink's clock so picture and sound stay together over a long play. A
// frame that can no longer be ready in time is skipped before it is rendered
// — only its sound is pulled — so the timeline keeps real time.
void PlayerWorker::pump() {
    if (m_speed==0.) {
        feedSink();
        if (!m_audioPending.isEmpty() && m_audioOutput->isOpen()) m_pump.start(2);
        return;
    }
    if (activeEmpty())
        return;
    const bool audible = m_speed == 1.0 && ensureAudio();
    const int current = m_sourceMode ? m_previewPos : m_position;
    // Something else moved the playhead (an edit, a sequence switch): start
    // again from there rather than show frames queued for the old place.
    if (m_playAnchored && (m_playSourceMode != m_sourceMode || current != m_playShown))
        flushAudio();
    if (!m_playAnchored) {
        m_playAnchored = true;
        m_playSourceMode = m_sourceMode;
        m_playContext = audioEditContext();
        m_playShown = current;
        m_playNext = nextPlayPosition(current);
        m_playFirstAudioSample=-1;
        m_playFineAudio=!m_sourceMode && m_speed==1. && fineAudioSample(&m_playFirstAudioSample);
        if (m_playFineAudio)
            m_playNext=current;
        if (!m_pausedContext.isEmpty()) {
            m_playFirstAudioSample=-1; // retained blocks already start at the parked sample
            m_playNext=m_pausedAudio.isEmpty()?m_pausedNext:m_pausedAudio.first().position;
            if (m_pausedAudio.isEmpty()) m_pausedContext.clear();
        }
        m_playNextDueNs = -1;
        m_playSyncNs = 0;
        m_playSyncBase = -1;
        m_playSyncPlayed = -1;
        m_playHeldNs = m_pumpNs = 0;
        // The timeline at normal speed, into a device fed from its own thread:
        // its sound is made ahead. Held back until the first picture is ready,
        // so the two start together as they always have.
        m_soundAhead = m_fed && audible && !m_sourceMode;
        if (m_soundAhead) {
            m_soundNext = m_playNext;
            m_soundShallowUntilNs = 0;
            m_soundGeneration = m_timeline.soundGeneration();
            m_soundDry = m_fed->dryTimes();
            m_timeline.rememberSound(true);
            m_fed->hold();
            m_fed->setLead(qint64(frameAudioBytes()) * playDepth(true));
        }
    }
    // The device stopped taking sound (feedSink closed it): start again, silent.
    if (m_soundAhead && !audible) { flushAudio(); return; }
    const qint64 period = framePeriodNs();
    if (audible)
        feedSink();

    qint64 now = steadyNs();
    // The worker was away from the pump (an edit, a slow picture): remembered
    // for the check on sound and picture having come apart (below).
    if (m_pumpNs > 0 && now - m_pumpNs > 150000000) m_playHeldNs = now;
    m_pumpNs = now;
    // A stopped/slow device must not cause an unbounded decoded-audio queue.
    // Keep the prepared sound and effect state, and wait without treating the
    // device's delay as a decoder stall or skipping sound to catch up afterward.
    if (audible && audioBackpressured()) {
        if (!m_audioWaitNs) {
            m_audioWaitNs = now;
            if (!m_playSourceMode) emit scheduleCleared(m_position);
        }
        m_pump.start(2);
        return;
    }
    if (m_audioWaitNs) {
        const qint64 delay = now - m_audioWaitNs;
        m_audioWaitNs = 0;
        if (m_playNextDueNs >= 0) m_playNextDueNs += delay;
        for (auto &frame : m_playQueue) {
            frame.dueNs += delay;
            if (frame.scheduled) emit frameScheduled(frame.decoded.image, frame.position, frame.dueNs);
        }
    }
    // A long stall (an edit rebuilt the timeline mid-play) has run the sink
    // dry; catching up would be a burst of skipped frames with the sound
    // behind them. Start again from the picture on screen instead.
    const qint64 stall = qMax(qMax<qint64>(4 * period, 150000000), qint64(2 * m_playFrameNs));
    if (m_soundAhead) {
        // With sound made ahead a stall is only heard once it has outlasted
        // that sound: the feeder then found the device and the ring empty with
        // more to come. Short of that the sound never stopped, and the pictures
        // passed over are skipped below (skipLateAhead).
        const quint64 dry = m_fed->dryTimes();
        if (dry != m_soundDry && m_soundNext >= 0) {
            settlePlayback();
            flushAudio();
            return; // flushAudio re-armed the pump
        }
        m_soundDry = dry;
    }
    // A device taking the final queued bytes is not a decoder stall.
    else if (m_playQueue.isEmpty() && m_playNext>=0 && m_playNextDueNs >= 0 && now - m_playNextDueNs > stall) {
        settlePlayback();
        flushAudio();
        return; // flushAudio re-armed the pump
    }

    // Everything due goes; the newest of it is the frame on screen. (The
    // viewer holds its own copies of timeline pictures and counts the ones it
    // never showed; a source frame passed over here is counted now.)
    if (!m_playQueue.isEmpty() && m_playQueue.first().dueNs <= now + 1000000) {
        int passed = 0;
        while (m_playQueue.size() > 1 && m_playQueue.at(1).dueNs <= now + 1000000)
            passed += m_playQueue.takeFirst().scheduled ? 0 : 1;
        if (passed)
            emit framesSkipped(passed);
        QueuedFrame frame = m_playQueue.takeFirst();
        if (audible) {
            // Sound still to play before this frame's own: > 0 means the
            // picture is early, < 0 late. Filter out the sink's chunky
            // consumption and nudge the clock by at most 1 ms a frame.
            const qint64 played = m_soundAhead ? m_fed->played() : m_audioWritten - m_audioOutput->bufferedBytes();
            qint64 waiting = frame.audioOffset - played;
            const double bytesPerNs = kFrequency * kChannels * kBytesPerSample / 1e9;
            // A picture shown late (the worker was held while its sound played
            // on) says nothing against the clock: its sound began when it was due.
            if (m_soundAhead) waiting += qint64(double(steadyNs() - frame.dueNs) * bytesPerNs);
            // The first frame's sound starts just as it shows, so whatever
            // reads as waiting then is the sink's own bookkeeping.
            if (m_playSyncBase < 0)
                m_playSyncBase = waiting;
            waiting -= m_playSyncBase;
            // Sound made ahead plays on whatever the pictures do, so after the
            // worker was held the two can be further apart than the gentle
            // steering below is for (the device ran dry behind the hold and did
            // not say so). Further apart than the pictures in hand, with the
            // device playing: start again from the picture. A device that
            // itself stopped taking sound is another matter (audioBackpressured):
            // the worker was not held then, and nothing is thrown away for it.
            const bool playing = m_playSyncPlayed >= 0 && played - m_playSyncPlayed >= frameAudioBytes() / 2;
            m_playSyncPlayed = played;
            if (m_soundAhead && playing && m_playHeldNs > 0 && steadyNs() - m_playHeldNs < 2000000000
                && qAbs(double(waiting) / bytesPerNs) > double(qMax<qint64>(5 * period, 200000000))) {
                m_playQueue.prepend(std::move(frame));
                settlePlayback();
                flushAudio();
                return; // flushAudio re-armed the pump
            }
            m_playSyncNs += (waiting / bytesPerNs - m_playSyncNs) * 0.1;
            const qint64 nudge = qint64(qBound(-1e6, m_playSyncNs * 0.05, 1e6));
            m_playNextDueNs += nudge;
            for (QueuedFrame &q : m_playQueue)
                q.dueNs += nudge; // the viewer holds its copies; a millisecond apart at most
        }
        presentFrame(frame, audible);
        if (m_speed == 0.0 || !m_playAnchored)
            return; // stopped on an error, or something reset the clock
        soundMayHaveChanged(); // an automation pass has just written this frame's key
    }
    if (m_playQueue.isEmpty() && m_playNext < 0) {
        // The last picture becoming due is the START of its sound interval.
        // Keep the sink alive until the final interval and queued PCM finish.
        if (audible) feedSink();
        const qint64 remaining=m_playNextDueNs-steadyNs();
        if (remaining>0 || (audible && (!m_audioPending.isEmpty() || m_audioOutput->bufferedBytes()>0))) {
            m_pump.start(int(qBound(qint64(1),remaining>0?(remaining+999999)/1000000:qint64(2),qint64(20))));
            return;
        }
        setSpeed(0.0);
        emit stopped();
        return;
    }

    // One frame per pass, so seeks and edits get in between frames.
    if (m_playQueue.size() < playDepth(audible) && m_playNext >= 0) {
        if (m_soundAhead) {
            if (!skipLateAhead()) return;
            if (m_playNext >= 0 && !audioBackpressured()) produceAheadFrame();
        } else {
            if (!skipLate(audible)) return; // an interrupted read belongs to the pending transport action
            if (m_playNext >= 0 && (!audible || !audioBackpressured())) produceFrame(audible);
        }
        if (m_speed == 0.0 || !m_playAnchored)
            return;
    }
    if (m_soundAhead) {
        fillSoundAhead();
        if (m_speed == 0.0 || !m_playAnchored)
            return;
    }
    if (audible)
        feedSink();

    // Sleep until the next frame is due, or go straight on if there is room
    // to produce more.
    now = steadyNs();
    qint64 wake = m_playQueue.isEmpty() ? now : m_playQueue.first().dueNs;
    if ((m_playQueue.size() < playDepth(audible) && m_playNext >= 0) || (m_soundAhead && soundWanted()))
        wake = now;
    m_pump.start(int(qBound(qint64(0), (wake - now) / 1000000, qint64(1000))));
}

qint64 PlayerWorker::steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PlayerWorker::resetPlayback(bool keepPaused) {
    m_audioWaitNs = 0;
    if (m_playAnchored && !m_playSourceMode)
        emit scheduleCleared(m_position);
    m_playQueue.clear();
    m_playAnchored = false;
    m_playFineAudio = false;
    m_audioSpans.clear();
    m_audioHistory.clear();m_playContext.clear();
    if (!keepPaused) {m_pausedAudio.clear();m_pausedContext.clear();}
    m_audioPending.clear();
    m_audioQueued = m_audioWritten = 0;
    m_playFirstAudioSample=-1;
    if (m_soundAhead) m_timeline.rememberSound(false);
    m_soundAhead = false;
    m_soundQueue.clear();
    m_soundNext = -1;
    m_spliceOld.clear();
    m_splicePosition = -1;
    m_pump.stop();
    if (m_speed != 0.0)
        m_pump.start(0);
}

// Stopping: the viewer may already be showing a queued picture whose time
// has come but that the worker has not reached (it was busy producing).
// Settle on that one, so the playhead is where the picture is.
void PlayerWorker::settlePlayback() {
    if (!m_playAnchored)
        return;
    const qint64 now = steadyNs() + 2000000;
    for (const QueuedFrame &q : std::as_const(m_playQueue))
        if (q.scheduled && q.dueNs <= now)
            m_position = q.position;
}

void PlayerWorker::settleAt(int position) {
    if (m_speed != 0.0 || m_sourceMode || exportActive() || activeEmpty())
        return;
    qint64 sample=0;
    if (fineAudioSample(&sample)) return; // a fine pause already chose its audible position
    m_position = qBound(0, position, m_timeline.frameCount() - 1);
}

// Frames produced ahead of the picture: enough in hand to ride out one slow
// frame (a cut opening the next clip), capped because a UHD frame is big.
int PlayerWorker::playDepth(bool audible) const {
    return audible ? 4 : 2;
}

int PlayerWorker::nextPlayPosition(int from) const {
    const int stride = m_speed > 0 ? qMax(1, qRound(m_speed)) : -qMax(1, qRound(-m_speed));
    const int total = m_playSourceMode ? m_preview.frameCount() : m_timeline.frameCount();
    int next = from + stride;
    if (m_loop && total > 0) {
        // Source and timeline have independent inclusive In/Out ranges. The
        // wrap is seamless: the sound queued before it plays out.
        const int start = m_playSourceMode ? m_sourceLoopIn : m_loopIn;
        const int end = m_playSourceMode ? m_sourceLoopOut : m_loopOut;
        const int lo = start >= 0 ? qMin(start, total - 1) : 0;
        const int hi = end >= lo ? qMin(end, total - 1) : total - 1;
        if (next > hi || next < lo)
            next = stride > 0 ? lo : hi;
    }
    return next < 0 || next >= total ? -1 : next;
}

// The frame to produce next would not be ready by its due time: skip it,
// pulling only its sound, until the next one can be.
bool PlayerWorker::skipLate(bool audible) {
    if (m_playNextDueNs < 0)
        return true;
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    const qint64 ready = steadyNs() + qint64(m_playFrameNs);
    int skipped = 0;
    while (m_playNext >= 0 && m_playNextDueNs < ready) {
        if (audible && audioBackpressured()) break;
        QueuedFrame frame;
        frame.position = m_playNext;
        const bool retained=!m_pausedAudio.isEmpty();
        if (retained) frame=m_pausedAudio.first();
        Timeline::MeterScope meters(m_timeline,frame.position,m_playSourceMode?-1:m_playFirstAudioSample,kFrequency,
                                      -1,m_playFineAudio && audible && !retained && !m_playSourceMode);
        if (audible && !retained)
            frame.decoded = m_playSourceMode
                ? m_preview.decodeAt(m_playNext, true, kFrequency, kChannels, false)
                : m_timeline.decodeAt(m_playNext, true, kFrequency, kChannels, false);
        if (frame.decoded.cancelled || RenderScope::cancelled()) return false;
        if (!frame.decoded.error.isEmpty()) {
            emit frameError(frame.decoded.error);
            setSpeed(0.);
            return false;
        }
        if (audible && !retained && !m_playSourceMode) {
            frame.trackPeaks=m_timeline.takeTrackPeaks();
            frame.routing=m_timeline.audioRoutingMeters();
            frame.meterCapture=meters.capture();
        }
        if (retained) m_pausedAudio.removeFirst();
        queueAudio(frame, audible);
        m_playNext = nextAudioPosition(m_playNext,retained);
        m_playNextDueNs += frame.durationNs;
        ++skipped;
    }
    if (skipped)
        emit framesSkipped(skipped);
    return true;
}

bool PlayerWorker::cachedPlaybackFrame(int position, bool audio, MediaPlayer::Decoded *out) {
    if (!m_backgroundRenderEnabled)
        return false;
    MediaPlayer::Decoded decoded;
    {
        StageTimer::Scope timed("cache.read");
        // Playing: the picture as the viewer draws it, at the size it plays at.
        // (Not with the scopes open: a kept picture has no counts to go with
        // it, so the kept working frame is measured, as it always was.)
        if (m_timeline.shownAs().isValid() && !m_timeline.scopesAs().isValid())
            decoded.image = m_backgroundRender->shown(position, m_timeline.drawSize(), m_timeline.shownAs().toMap());
        if (decoded.image.isNull()) decoded.image = m_backgroundRender->image(position);
    }
    if (decoded.image.isNull())
        return false;
    if (WorkingFrame::isTagged(decoded.image)) {
        decoded.working = WorkingFrame::fromTagged(decoded.image, &decoded.error);
        if (!decoded.error.isEmpty())
            return false;
    }
    // Only pictures are cached. Audio automation, routing and per-track meters
    // must always read the current mix without invoking video filters.
    if (audio) decoded.audio = m_timeline.audioAt(position, kFrequency, kChannels);
    emit backgroundFrameUsed(position);
    *out = std::move(decoded);
    return true;
}

// Produces the next frame onto the queue, with its due time.
void PlayerWorker::produceFrame(bool audible) {
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    const qint64 started = steadyNs();
    QueuedFrame frame;
    frame.position = m_playNext;
    const bool retained=!m_pausedAudio.isEmpty();
    const bool separateAudio=m_playFineAudio && audible && !retained && !m_playSourceMode;
    bool preparedAudio=false;
    if (retained) frame=m_pausedAudio.first();
    Timeline::MeterScope meters(m_timeline,frame.position,m_playSourceMode?-1:m_playFirstAudioSample,kFrequency,
                                  -1,m_playFineAudio && audible && !retained && !m_playSourceMode);
    if (m_playSourceMode) {
        // Drawn as the source viewer shows it.
        frame.decoded = sourceShown(frame.position, audible);
    } else {
        // Keep completed DSP separate from an interruptible picture. A decoder
        // may finish the sound before its picture is cancelled; reading that
        // same sound again on resume would reset stateful effects at this frame.
        if (separateAudio) {
            StageTimer::Scope timed("play.sound");
            frame.decoded=m_timeline.decodeAt(frame.position,true,kFrequency,kChannels,false);
            frame.trackPeaks = m_timeline.takeTrackPeaks();
            frame.routing = m_timeline.audioRoutingMeters();
            frame.meterCapture = meters.capture();
            preparedAudio=!frame.decoded.cancelled && !RenderScope::cancelled() && frame.decoded.error.isEmpty();
        }
        if (!separateAudio || preparedAudio) {
            // Drawn at the size the viewer shows it.
            m_timeline.setDrawSize(m_playbackDrawSize);
            // And for its display, unless the unprocessed picture is wanted beside it.
            m_timeline.setShownAs(m_compare ? QVariant() : m_playbackDisplay);
            m_timeline.setScopesAs(m_compare ? QVariant() : m_playbackScopes);
            StageTimer::Sequence timed;
            timed.next("play.picture");
            auto decoded = decodePlaybackFrame(frame.position, audible && !retained && !separateAudio);
            timed.finish();
            m_timeline.setDrawSize({});
            m_timeline.setShownAs({});
            m_timeline.setScopesAs({});
            m_playedSmall |= !decoded.image.text(QStringLiteral("omaedit.fullSize")).isEmpty();
            if (retained || separateAudio) decoded.audio=std::move(frame.decoded.audio);
            frame.decoded=std::move(decoded);
            if (audible && !retained && !separateAudio) {
                frame.trackPeaks = m_timeline.takeTrackPeaks();
                frame.routing = m_timeline.audioRoutingMeters();
                frame.meterCapture = meters.capture();
            }
        }
        if (m_compare)
            frame.before = m_timeline.decodeBefore(frame.position).image;
    }
    if(frame.decoded.cancelled || RenderScope::cancelled()) {
        if (preparedAudio && !frame.decoded.audio.isEmpty()) {
            frame.decoded.image={};frame.decoded.working={};frame.decoded.cancelled=false;frame.decoded.error.clear();frame.before={};
            frame.firstSample=sound::Mixer::frameStart(frame.position,m_timeline.fps(),kFrequency);
            if (m_playFirstAudioSample>=0) {
                frame.firstSample=qMax(frame.firstSample,m_playFirstAudioSample);
                trimAudioStart(frame,m_playFirstAudioSample);m_playFirstAudioSample=-1;
            }
            m_pausedAudio.append(std::move(frame));m_pausedContext=m_playContext;
            m_pausedNext=nextPlayPosition(m_playNext);
        }
        return;
    }
    if (separateAudio && !frame.decoded.error.isEmpty()) frame.decoded.audio.clear();
    if (retained) m_pausedAudio.removeFirst();
    queueAudio(frame, audible);
    const bool failed = !frame.decoded.error.isEmpty();
    m_playNext = failed ? -1 : nextAudioPosition(frame.position,retained); // show up to a failure

    const qint64 finished = steadyNs();
    const double cost = double(finished - started);
    // What a frame takes to make, for telling whether the next one can be
    // ready in time (skipLate). It follows the ordinary frames. The first
    // frame of a run opens every track's decoder and finds its place, and a
    // frame after skipped ones decodes through them: either can take ten
    // times what the frames after it do, and taken at its word it had the
    // next dozen frames skipped, each skip costing as much again, for a
    // second or more after every start. So the first frame is not counted,
    // and one frame moves the figure up by no more than a fifth of itself.
    if (m_playNextDueNs < 0) {
        // (Not counted.)
    } else if (m_playFrameNs <= 0) {
        m_playFrameNs = std::min(cost, 2.0 * double(frame.durationNs > 0 ? frame.durationNs : framePeriodNs()));
    } else {
        m_playFrameNs = m_playFrameNs * 0.8 + std::min(cost, 2.0 * m_playFrameNs) * 0.2;
    }
    if (m_playNextDueNs < 0)
        m_playNextDueNs = finished; // the first frame shows at once; its sound goes out now
    frame.dueNs = m_playNextDueNs;
    m_playNextDueNs += frame.durationNs;
    if (!m_playSourceMode && !failed && !frame.decoded.image.isNull()) {
        frame.scheduled = true;
        emit frameScheduled(frame.decoded.image, frame.position, frame.dueNs);
    }
    m_playQueue.append(std::move(frame));
}

// Sound ahead of the picture.
//
// With the device fed from a thread of its own (m_fed), the timeline's sound at
// normal speed is mixed up to kSoundAheadNs ahead of the playhead, in play
// order, and waits in the feeder's ring; the pictures follow, each taking its
// block from m_soundQueue (the sound, its track peaks, routing meters and
// meter capture travel together, so what is shown when a picture comes due is
// what is heard then). A worker held for a while (an edit rebuilding the
// timeline, a slow picture after a cut) therefore leaves the device fed: when
// it returns, the pictures it missed are skipped and their sound, already
// played, is simply dropped from the queue.
//
// Everything here runs on the worker's thread. The mixer, its inserts' state,
// the meters, automation and the retained blocks of a fine pause keep their one
// owner; only bytes cross to the feeder, through its ring.
//
// What keeps the editor's hands on the sound:
//  - The device holds no more than playDepth frames (FedAudioOutput::setLead),
//    which is what it held when sound was made with its picture.
//  - A change to what the mixer makes (soundChanged) takes back everything the
//    device has not been given, and it is mixed again from there.
//  - Pause, seek and a change of speed reset the feeder: the ring goes with
//    the device's buffer (flushAudio), or is kept as retained blocks by a fine
//    pause, exactly as the blocks waiting for the device always were.

// The next block of sound, into the ring and onto m_soundQueue: a block kept
// over a fine pause first, then the timeline mixed at m_soundNext. False when
// none was made. The read is not one a seek can interrupt: a block is a
// millisecond or two of mixing, and half a block would leave the inserts
// between two states.
bool PlayerWorker::produceSound() {
    if (m_soundNext < 0) return false;
    QueuedFrame frame;
    frame.position = m_soundNext;
    const bool retained = !m_pausedAudio.isEmpty();
    if (retained) frame = m_pausedAudio.first();
    {
        StageTimer::Scope timed("play.sound");
        Timeline::MeterScope meters(m_timeline, frame.position, m_playFirstAudioSample, kFrequency, -1, m_playFineAudio && !retained);
        if (!retained) {
            frame.decoded = m_timeline.decodeAt(frame.position, true, kFrequency, kChannels, false);
            if (frame.decoded.cancelled) return false;
            frame.trackPeaks = m_timeline.takeTrackPeaks();
            frame.routing = m_timeline.audioRoutingMeters();
            frame.meterCapture = meters.capture();
            frame.mixMemory = m_timeline.takeSoundMemory();
        }
    }
    if (retained) m_pausedAudio.removeFirst();
    const bool failed = !frame.decoded.error.isEmpty();
    if (failed) frame.decoded.audio.clear();
    queueAudio(frame, true);
    m_soundNext = failed ? -1 : nextAudioPosition(frame.position, retained); // sound up to a failure
    m_soundQueue.append(std::move(frame));
    feedSink();
    return true;
}

// The picture stage's next position: the oldest block still without a picture,
// or where the sound will go on.
void PlayerWorker::takePlayNext() {
    m_playNext = m_soundQueue.isEmpty() ? m_soundNext : m_soundQueue.first().position;
}

bool PlayerWorker::soundWanted() const {
    if (m_soundNext < 0 || steadyNs() < m_soundShallowUntilNs) return false;
    const qint64 ahead = m_audioQueued - (m_audioWritten - m_audioOutput->bufferedBytes());
    return ahead < qint64(double(kSoundAheadNs) * kFrequency * kChannels * kBytesPerSample / 1e9);
}

// A block or two a pass, so seeks and edits get in between and the pictures
// are not kept waiting while the sound in hand builds up after a start.
void PlayerWorker::fillSoundAhead() {
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    for (int made = 0; made < 2 && soundWanted(); ++made)
        if (!produceSound()) break;
    takePlayNext();
}

// skipLate, with the sound already made: a picture that cannot be ready by
// its due time is passed over, and its block with it.
bool PlayerWorker::skipLateAhead() {
    if (m_playNextDueNs < 0)
        return true;
    const qint64 ready = steadyNs() + qint64(m_playFrameNs);
    int skipped = 0;
    while (m_playNext >= 0 && m_playNextDueNs < ready) {
        if (m_soundQueue.isEmpty() && (audioBackpressured() || !produceSound())) break;
        const QueuedFrame frame = m_soundQueue.takeFirst();
        takePlayNext();
        if (!frame.decoded.error.isEmpty()) {
            emit frameError(frame.decoded.error);
            setSpeed(0.);
            return false;
        }
        m_playNextDueNs += frame.durationNs;
        ++skipped;
    }
    if (skipped)
        emit framesSkipped(skipped);
    return true;
}

// produceFrame, with the sound already made: the oldest block's picture, onto
// the queue with its due time. A picture given up for a seek or a pause leaves
// its block where it is: the sound is on its way already, and the transport
// action that interrupted decides what becomes of it.
void PlayerWorker::produceAheadFrame() {
    if (m_soundQueue.isEmpty() && !produceSound()) return;
    // Before the first picture (often the slowest: it opens the decoders) the
    // feed is held, so the sound of the pictures the queue will hold is made
    // now and the start does not hang on the second picture being quick.
    while (m_playNextDueNs < 0 && m_soundQueue.size() < playDepth(true) && produceSound()) {}
    RenderScope render(frameRequest());
    ViewerDraw viewerDraw(this);  // filmstrip tiles wait
    const qint64 started = steadyNs();
    QueuedFrame frame = m_soundQueue.first();
    if (frame.decoded.error.isEmpty()) {
        m_timeline.setDrawSize(m_playbackDrawSize);
        m_timeline.setShownAs(m_compare ? QVariant() : m_playbackDisplay);
        m_timeline.setScopesAs(m_compare ? QVariant() : m_playbackScopes);
        StageTimer::Sequence timed;
        timed.next("play.picture");
        auto decoded = decodePlaybackFrame(frame.position, false);
        timed.finish();
        m_timeline.setDrawSize({});
        m_timeline.setShownAs({});
        m_timeline.setScopesAs({});
        m_playedSmall |= !decoded.image.text(QStringLiteral("omaedit.fullSize")).isEmpty();
        decoded.audio = std::move(frame.decoded.audio);
        frame.decoded = std::move(decoded);
        if (m_compare)
            frame.before = m_timeline.decodeBefore(frame.position).image;
    }
    if (frame.decoded.cancelled || RenderScope::cancelled())
        return;
    m_soundQueue.removeFirst();
    const bool failed = !frame.decoded.error.isEmpty();
    if (failed) { m_soundQueue.clear(); m_soundNext = -1; } // show up to a failure
    takePlayNext();

    const qint64 finished = steadyNs();
    const double cost = double(finished - started);
    // (What a frame takes to make: see produceFrame.)
    if (m_playNextDueNs < 0) {
        // (Not counted.)
    } else if (m_playFrameNs <= 0) {
        m_playFrameNs = std::min(cost, 2.0 * double(frame.durationNs > 0 ? frame.durationNs : framePeriodNs()));
    } else {
        m_playFrameNs = m_playFrameNs * 0.8 + std::min(cost, 2.0 * m_playFrameNs) * 0.2;
    }
    if (m_playNextDueNs < 0) {
        m_playNextDueNs = finished; // the first frame shows at once; its sound goes out now
        m_fed->release();
    }
    frame.dueNs = m_playNextDueNs;
    m_playNextDueNs += frame.durationNs;
    if (!failed && !frame.decoded.image.isNull()) {
        frame.scheduled = true;
        emit frameScheduled(frame.decoded.image, frame.position, frame.dueNs);
    }
    m_playQueue.append(std::move(frame));
}

// What the timeline sounds like has changed while sound made before the change
// waits in the ring (a level, a pan, a mute or solo, an automation key, an
// edit, the loop, the monitor's channels). All of it the device has not been
// given is taken back and will be mixed again, starting with a short fade from
// what it was; and for kSoundShallowNs sound is made no further ahead than its
// picture (a fader in motion would have it all mixed again a frame later).
void PlayerWorker::soundChanged() {
    if (!m_soundAhead || !m_playAnchored) return;
    m_soundShallowUntilNs = steadyNs() + kSoundShallowNs;
    if (m_soundQueue.isEmpty() && m_pausedAudio.isEmpty()) return;
    feedSink();
    QList<qint64> starts;
    for (const QueuedFrame &block : std::as_const(m_soundQueue)) starts.append(block.audioOffset);
    const qint64 cut = starts.isEmpty() ? -1 : m_fed->truncate(starts);
    int from = m_soundQueue.size();
    for (int i = 0; cut >= 0 && i < m_soundQueue.size(); ++i)
        if (m_soundQueue.at(i).audioOffset == cut) { from = i; break; }
    if (from == m_soundQueue.size() && m_pausedAudio.isEmpty()) return; // the device has it all already
    const QueuedFrame first = from < m_soundQueue.size() ? m_soundQueue.at(from) : m_pausedAudio.first();
    if (from < m_soundQueue.size()) {
        m_spliceOld = first.decoded.audio.left(kSpliceSamples * kChannels * kBytesPerSample);
        m_splicePosition = first.position;
        m_audioQueued = m_audioWritten = cut;
        while (!m_audioSpans.isEmpty() && m_audioSpans.last().offset >= cut) m_audioSpans.removeLast();
        if (!m_audioSpans.isEmpty()) m_audioSpans.last().bytes = qMin(m_audioSpans.last().bytes, cut - m_audioSpans.last().offset);
        while (!m_audioHistory.isEmpty() && m_audioHistory.last().audioOffset >= cut) m_audioHistory.removeLast();
    }
    // The mixer's inserts are where the last block it made left them; what it
    // makes next starts earlier. Each block taken back gives the inserts back
    // what they held before it, newest first, so the block mixed again follows
    // on from the one before it as it did the first time.
    for (int i = m_pausedAudio.size() - 1; i >= 0; --i)
        if (m_pausedAudio.at(i).mixMemory) m_timeline.recallSound(*m_pausedAudio.at(i).mixMemory);
    for (int i = m_soundQueue.size() - 1; i >= from; --i)
        if (m_soundQueue.at(i).mixMemory) m_timeline.recallSound(*m_soundQueue.at(i).mixMemory);
    const bool whole = first.firstSample <= sound::Mixer::frameStart(first.position, m_timeline.fps(), kFrequency);
    m_soundQueue.erase(m_soundQueue.begin() + from, m_soundQueue.end());
    m_pausedAudio.clear();
    m_pausedContext.clear();
    m_soundNext = first.position;
    m_playFirstAudioSample = whole ? -1 : first.firstSample;
    takePlayNext();
}

// Asks the timeline whether its sound is still what the queued sound was mixed
// from (Timeline::soundGeneration), and takes the queued sound back if not.
void PlayerWorker::soundMayHaveChanged() {
    if (!m_soundAhead || !m_playAnchored) return;
    const quint64 generation = m_timeline.soundGeneration();
    if (generation == m_soundGeneration) return;
    m_soundGeneration = generation;
    soundChanged();
}

// Sound joins the stream in position order, skipped frames' included; the
// sink takes it as it has room (feedSink).
void PlayerWorker::queueAudio(QueuedFrame &frame, bool audible) {
    if (frame.durationNs<=0) frame.durationNs=framePeriodNs();
    if (frame.firstSample<0)
        frame.firstSample=sound::Mixer::frameStart(frame.position,m_timeline.fps(),kFrequency);
    if (m_playFirstAudioSample>=0) {
        frame.firstSample=qMax(frame.firstSample,m_playFirstAudioSample);
        trimAudioStart(frame,m_playFirstAudioSample);
        m_playFirstAudioSample=-1;
    }
    frame.audioOffset = m_audioQueued;
    if (!audible || frame.decoded.audio.isEmpty())
        return;
    if (!m_spliceOld.isEmpty() && frame.position == m_splicePosition) {
        // The block a change cut away, mixed again: faded in from what it was,
        // so the join is no step in the sound.
        auto *fresh = reinterpret_cast<qint16 *>(frame.decoded.audio.data());
        const auto *old = reinterpret_cast<const qint16 *>(m_spliceOld.constData());
        const int samples = int(qMin(frame.decoded.audio.size(), m_spliceOld.size()) / (kChannels * kBytesPerSample));
        for (int i = 0; i < samples; ++i)
            for (int c = 0; c < kChannels; ++c) {
                const int at = i * kChannels + c;
                fresh[at] = qint16(std::lrint(old[at] + (double(fresh[at]) - old[at]) * (i + 1) / (samples + 1)));
            }
        m_spliceOld.clear();
        m_splicePosition = -1;
    }
    const QByteArray mapped = AudioEngine::mapChannels(frame.decoded.audio, kChannels, m_monitorMono ? QStringLiteral("mono") : m_monitorMapping);
    if (m_playFineAudio) {
        const qint64 played=qMax<qint64>(0,m_audioWritten-m_audioOutput->bufferedBytes());
        while (m_audioSpans.size()>1 && m_audioSpans[1].offset<=played) m_audioSpans.removeFirst();
        while (!m_audioHistory.isEmpty() && m_audioHistory.first().audioOffset+m_audioHistory.first().decoded.audio.size()<=played)
            m_audioHistory.removeFirst();
        auto remembered=frame;remembered.decoded.image={};remembered.decoded.working={};remembered.before={};remembered.scheduled=false;
        m_audioHistory.append(std::move(remembered));
        // Adjacent intervals share one span. Keep separate spans across loops
        // so queued bytes still identify the actual timeline sample on pause.
        if (!m_audioSpans.isEmpty() && m_audioSpans.last().offset+m_audioSpans.last().bytes==m_audioQueued
            && m_audioSpans.last().firstSample+m_audioSpans.last().bytes/(kChannels*kBytesPerSample)==frame.firstSample)
            m_audioSpans.last().bytes+=mapped.size();
        else m_audioSpans.append({m_audioQueued,frame.firstSample,mapped.size()});
    }
    m_audioPending.append(mapped);
    m_audioQueued += mapped.size();
}

void PlayerWorker::feedSink() {
    if (!m_audioOutput->isOpen() || m_audioPending.isEmpty())
        return;
    const int frameBytes = kChannels * kBytesPerSample;
    const qint64 room = qMin<qint64>(m_audioOutput->writableBytes(), m_audioPending.size()) / frameBytes * frameBytes;
    if (room <= 0)
        return;
    const qint64 written = m_audioOutput->write(m_audioPending.constData(), room);
    if (written > 0) {
        m_audioPending.remove(0, written);
        m_audioWritten += written;
    } else if (written<0) {
        m_audioFailed=true; m_audioPending.clear(); m_audioOutput->close();
        emit frameError(tr("The audio output stopped accepting sound."));
    }
}

void PlayerWorker::queueAudition(const QByteArray &audio) {
    m_audioPending=AudioEngine::mapChannels(audio,kChannels,m_monitorMono?QStringLiteral("mono"):m_monitorMapping);
    m_audioQueued=m_audioPending.size(); m_audioWritten=0;
    feedSink();
    if (!m_audioPending.isEmpty()) m_pump.start(2);
}

void PlayerWorker::presentFrame(QueuedFrame &frame, bool audible) {
    mediaGangReady();
    const MediaPlayer::Decoded &decoded = frame.decoded;
    m_playShown = frame.position;
    if (m_playSourceMode) {
        m_previewPos = frame.position;
        if (!decoded.error.isEmpty()) { emit frameError(decoded.error); setSpeed(0.0); return; }
        if (audible) {
            const QList<float> levels = levelsFor(decoded.audio, kChannels);
            emit audioLevels(levels.value(0, -100.0f), levels.value(1, -100.0f));
        }
        emit previewFrameReady(decoded.image, frame.position);
        refreshMediaGang();
        return;
    }
    m_position = frame.position;
    writeAutomation(frame.position);
    if (audible) {
        const QList<float> levels = levelsFor(decoded.audio, kChannels);
        emit audioLevels(levels.value(0, -100.0f), levels.value(1, -100.0f));
        emit trackLevels(frame.trackPeaks);
        emit audioRoutingLevels(frame.routing);
        feedMeters(decoded.audio);
    }
    if (!frame.scheduled) // the viewer already holds a scheduled picture
        emit frameReady(decoded.image, m_position);
    if (!decoded.error.isEmpty()) { emit frameError(decoded.error); setSpeed(0.0); }
    refreshMediaGang();
    if (m_compare && m_speed != 0.0)
        emit beforeFrameReady(frame.before);
}

// Per-channel peak in dBFS of one interleaved s16 buffer. -100 dB stands in
// for digital silence (and for a channel the buffer doesn't carry).
QList<float> PlayerWorker::levelsFor(const QByteArray &audio, int channels) {
    QList<float> levels;
    if (channels < 1)
        return levels;
    const auto *samples = reinterpret_cast<const qint16 *>(audio.constData());
    const int frames = audio.size() / (channels * int(sizeof(qint16)));
    for (int c = 0; c < channels; ++c) {
        double peak = 0.0;
        for (int i = 0; i < frames; ++i)
            peak = qMax(peak, qAbs(samples[i * channels + c]) / 32768.0);
        levels.append(peak > 0.0
                          ? float(qMax(-100.0, 20.0 * std::log10(peak)))
                          : -100.0f);
    }
    return levels;
}

bool PlayerWorker::ensureAudio() {
    if (m_audioFailed)
        return false;
    if (qEnvironmentVariableIsSet("OMAEDIT_SILENT")) { // headless renders: never open a device
        m_audioFailed = true;
        return false;
    }
    if (m_audioOutput->isOpen()) return true;
    m_audioFailed=!m_audioOutput->open(m_audioOutputId,kFrequency,kChannels,
        frameAudioBytes()*m_audioBufferFrames,m_monitorMuted?0.:m_monitorVolume*(m_monitorDim?.1:1.));
    return !m_audioFailed;
}

bool PlayerWorker::audioBackpressured() const {
    // Allow the configured device buffer and the normal picture look-ahead,
    // with a half-second minimum for short, high-rate frames. At most one
    // decoded block can cross this high-water mark.
    const qint64 limit = qMax<qint64>(qint64(frameAudioBytes()) * (m_audioBufferFrames + playDepth(true)),
                                    kFrequency * kChannels * kBytesPerSample / 2);
    // Sound made ahead is in hand by design: what counts is the sound of
    // pictures already made that the device has still not played.
    if (m_soundAhead) {
        const qint64 pictured = m_soundQueue.isEmpty() ? m_audioQueued : m_soundQueue.first().audioOffset;
        return pictured - (m_audioWritten - m_audioOutput->bufferedBytes()) >= limit;
    }
    return m_audioPending.size() + m_audioOutput->bufferedBytes() >= limit;
}

void PlayerWorker::flushAudio(bool keepPaused) {
    resetPlayback(keepPaused); // queued pictures belong to the sound being dropped
    m_audioOutput->reset();
}

int PlayerWorker::frameAudioBytes() const {
    const double fps = activeFps();
    if (fps <= 0.0)
        return kFrequency / 25 * kChannels * kBytesPerSample;
    return qCeil(kFrequency / fps) * kChannels * kBytesPerSample;
}

qint64 PlayerWorker::framePeriodNs() const {
    const double fps = activeFps();
    return fps > 0.0 ? qint64(1e9 / fps) : 40000000;
}
