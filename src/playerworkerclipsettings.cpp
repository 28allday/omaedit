#include "playerworker_p.h"

void PlayerWorker::addDissolve(int index, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.mixAt(index, length))
        return;
    recordEdit(before, true, tr("Add dissolve"));
    // A dissolve consumes the NEXT clip's head in place — only content
    // downstream of the pair shifts (the round-23 boundary).
    rippleMarkers(entryEnd(before, index + 1),
                  m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

// Lane dissolves/fades: flat connected index → (track, index-on-track),
// then the any-track engine op. One undo step each; markers hold (V1 only
// moves if it closes empty space, which is not a marker ripple).
void PlayerWorker::addConnectedDissolve(int index, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip)
        || !m_timeline.mixOn(track, clip, length))
        return;
    recordEdit(before, true, tr("Add connected dissolve"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::removeConnectedDissolve(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip)
        || !m_timeline.unmixOn(track, clip))
        return;
    recordEdit(before, true, tr("Remove connected dissolve"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::resizeConnectedDissolve(int index, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip)
        || !m_timeline.setMixLengthOn(track, clip, length))
        return;
    recordEdit(before, true, tr("Resize connected dissolve"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::restyleConnectedDissolve(int index, const QString &style) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip)
        || !m_timeline.setMixStyleOn(track, clip, style))
        return;
    recordEdit(before, true, tr("Restyle connected dissolve"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedFades(int index, int fadeIn, int fadeOut) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip))
        return;
    if (!m_timeline.setFadesOn(track, clip, fadeIn, fadeOut)) {
        reportFadeHeldByTransition(m_timeline.entryMapOn(track, clip), fadeIn, fadeOut);
        return;
    }
    recordEdit(before, true, tr("Set connected fades"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

// An edge carries a transition or a fade, not both: a fade asked for on an
// edge a transition holds changes nothing, and the editor is told why.
void PlayerWorker::reportFadeHeldByTransition(const QVariantMap &entry, int fadeIn, int fadeOut) {
    if ((fadeIn > 0 && entry.value(ClipKey::mixIn).toInt() > 0) || (fadeOut > 0 && entry.value(ClipKey::mixOut).toInt() > 0))
        emit editRefused(tr("A transition holds that edge of the clip; nothing was faded."));
}

void PlayerWorker::toggleFadeEdge(bool connected, int index, bool outEdge, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    QVariantMap entry;
    if (connected) {
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(index, &track, &clip))
            return;
        entry = m_timeline.entryMapOn(track, clip);
    } else {
        entry = m_timeline.entryMapOn(0, index);
    }
    if (entry.isEmpty())
        return;
    int fadeIn = entry.value(ClipKey::fadeIn).toInt();
    int fadeOut = entry.value(ClipKey::fadeOut).toInt();
    if (outEdge)
        fadeOut = fadeOut > 0 ? 0 : length;
    else
        fadeIn = fadeIn > 0 ? 0 : length;
    if (connected)
        setConnectedFades(index, fadeIn, fadeOut);
    else
        setClipFades(index, fadeIn, fadeOut);
}

void PlayerWorker::setClipFades(int index, int fadeIn, int fadeOut) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setFades(index, fadeIn, fadeOut)) {
        reportFadeHeldByTransition(m_timeline.entryMapOn(0, index), fadeIn, fadeOut);
        return;
    }
    recordEdit(before, true, tr("Set clip fades"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setClipVolume(int index, double db) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipVolume(index, db))
        return;
    recordEdit(before, true, tr("Set clip volume"));
    announceTimeline();
}

void PlayerWorker::setConnectedVolume(int index, double db) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedVolume(index, db))
        return;
    recordEdit(before, true, tr("Set connected volume"));
    announceTimeline();
}

void PlayerWorker::setClipVolumeKf(int index, const QVariantList &kf) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipVolumeKf(index, kf))
        return;
    recordEdit(before, true, tr("Edit volume keyframes"));
    announceTimeline();
}

void PlayerWorker::setConnectedVolumeKf(int index, const QVariantList &kf) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedVolumeKf(index, kf))
        return;
    recordEdit(before, true, tr("Edit connected volume keyframes"));
    announceTimeline();
}

void PlayerWorker::setConnectedOpacity(int index, double alpha) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedOpacity(index, alpha))
        return;
    recordEdit(before, true, tr("Set connected opacity"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // the composite under the playhead changed
}

void PlayerWorker::setConnectedOpacityKf(int index, const QVariantList &kf) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedOpacityKf(index, kf))
        return;
    recordEdit(before, true, tr("Edit opacity keyframes"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setClipPan(int index, double pan) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipPan(index, pan))
        return;
    recordEdit(before, true, tr("Set clip pan"));
    announceTimeline();
}

void PlayerWorker::setConnectedPan(int index, double pan) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedPan(index, pan))
        return;
    recordEdit(before, true, tr("Set connected pan"));
    announceTimeline();
}

void PlayerWorker::setClipEq(int index, const QString &preset) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipEq(index, preset))
        return;
    recordEdit(before, true, tr("Adjust equaliser"));
    announceTimeline();
}

void PlayerWorker::setConnectedEq(int index, const QString &preset) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedEq(index, preset))
        return;
    recordEdit(before, true, tr("Adjust connected equaliser"));
    announceTimeline();
}

void PlayerWorker::detachAudio(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.detachAudio(index))
        return;
    recordEdit(before, true, tr("Detach audio")); // rebuild + connect land as ONE undo step
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::detachAudioOn(int track, int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.detachAudioOn(track, index))
        return;
    recordEdit(before, true, tr("Detach audio"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::reattachAudio(int track, int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.reattachAudioOn(track, index)) {
        const QVariantMap clip = m_timeline.entryMapOn(track, index);
        emit editRefused(m_timeline.trackLocked(track)
                             ? tr("%1 is locked; the sound was not reattached.").arg(m_timeline.trackName(track))
                         : !clip.value(ClipKey::videoOnly).toBool() || clip.contains(ClipKey::freeze)
                             ? tr("Reattach works on a picture whose sound was split off; nothing was reattached.")
                             : tr("The sound no longer sits exactly under its picture; it was not reattached."));
        return;
    }
    recordEdit(before, true, tr("Reattach audio"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setFlip(int track, int index, bool h, bool v) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setFlipAt(track, index, h, v))
        return;
    recordEdit(before, true, tr("Set flip"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setAudioMap(int track, int index, const QString &mode) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setAudioMapAt(track, index, mode))
        return;
    recordEdit(before, true, tr("Map audio channels"));
    announceTimeline();
}

void PlayerWorker::setNormalize(int track, int index, bool on) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setNormalizeAt(track, index, on))
        return;
    recordEdit(before, true, tr("Normalise audio"));
    announceTimeline();
}

void PlayerWorker::freezeFrame(int track, int index, int frame, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.freezeFrameOn(track, index, frame, length))
        return;
    recordEdit(before, true, tr("Freeze frame"));
    rippleMarkers(frame, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedColor(int index, double brightness,
                                     double contrast, double saturation) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectedAddress(index, &track, &clip)
        || !m_timeline.setColorAt(track, clip, brightness, contrast, saturation))
        return;
    recordEdit(before, true, tr("Set connected colour"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setClipColor(int index, double brightness, double contrast,
                                double saturation) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipColor(index, brightness, contrast, saturation))
        return;
    recordEdit(before, true, tr("Adjust Colour"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // the frame under the playhead may look different now
}

void PlayerWorker::setClipCrop(int index, double l, double r, double t, double b,
                               double feather, bool invert) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipCrop(index, l, r, t, b, feather, invert))
        return;
    recordEdit(before, true, tr("Set clip crop"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedCrop(int index, double l, double r, double t,
                                    double b, double feather, bool invert) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedCrop(index, l, r, t, b, feather, invert))
        return;
    recordEdit(before, true, tr("Set connected crop"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedAnchor(int index, double anchorX, double anchorY) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedAnchor(index, anchorX, anchorY))
        return;
    recordEdit(before, true, tr("Set connected anchor"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setEditProperties(int track, int index, const QVariantMap &patch) {
    if (staleEdit())
        return;
    applyEditProperties(track, index, patch, 0);
}

void PlayerWorker::applyMotionPreset(int track, int index, const QString &preset, const QVariantMap &options) {
    if (staleEdit())
        return;
    if (exportActive()) { emit editRefused(tr("Motion presets cannot be changed during export.")); return; }
    const auto before = m_timeline.entries();
    QString error;
    if (!m_timeline.applyMotionPresetAt(track, index, preset, options, &error)) {
        if (!error.isEmpty()) emit editRefused(error);
        return;
    }
    recordEdit(before, true, tr("Apply motion preset"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
    emit editOperationFinished(tr("Motion preset applied"));
}

void PlayerWorker::applyEditProperties(int track, int index, const QVariantMap &patch, quint64 gesture) {
    if (exportActive()) return;
    setSpeed(0.0);
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setEditProperties(track, index, patch)) {
        if (!gesture)
            emit editOperationFinished(tr("No edit applied. Check the track lock, clip type and parameter values."));
        return;
    }
    // The undo entry names what changed, as the Inspector calls it.
    QStringList keys = patch.keys();
    keys.removeAll(QStringLiteral("editKeys"));
    keys.removeAll(QStringLiteral("videoEffectKeys"));
    keys.removeAll(QStringLiteral("videoEffectMaskKeys"));
    const auto touches = [&](std::initializer_list<const char *> prefixes) {
        for (const QString &key : std::as_const(keys))
            for (const char *prefix : prefixes)
                if (key.startsWith(QLatin1String(prefix))) return true;
        return false;
    };
    const QString label = patch.contains("videoEffectUpdate") ? tr("Adjust video effect")
        : patch.contains("resetEffect") ? tr("Reset effect")
        : patch.contains("effectOrder") ? tr("Reorder effects")
        : keys.isEmpty() ? tr("Change keyframes")
        : touches({"rect", "rotation", "anchor"}) ? tr("Transform")
        : touches({"crop"}) ? tr("Crop")
        : touches({"opacity"}) ? tr("Opacity")
        : touches({"color"}) ? tr("Clip colour")
        : tr("Change clip settings");
    recordGestureEdit(before, gesture, label);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::recordGestureEdit(const QVariantList &before, quint64 gesture, const QString &action) {
    if (gesture && gesture == m_lastEditGesture && m_editRevision == m_lastGestureRevision
        && !m_undoStack.isEmpty())
    {
        ++m_editRevision; // one undo snapshot for the entire pointer drag
        if (synchronizeColorLinks(before)) announceSequences();
    }
    else
        recordEdit(before, true, action);
    m_lastEditGesture = gesture;
    m_lastGestureRevision = m_editRevision;
}

void PlayerWorker::setFrameBlend(int track, int index, int mode) {
    if (staleEdit())
        return;
    if (exportActive()) { emit editRefused(tr("Cannot change frame sampling while exporting."));return; }
    if (!FrameSampling::valid(mode)) { emit editRefused(tr("Choose a valid frame sampling mode."));return; }
    const auto entry=m_timeline.entryMapOn(track,index);
    if (entry.isEmpty() || entry.value(ClipKey::gap).toBool()) { emit editRefused(tr("This clip is no longer available."));return; }
    if (m_timeline.trackLocked(track)) { emit editRefused(tr("%1 is locked; frame sampling did not change.").arg(m_timeline.trackName(track)));return; }
    if (entry.value(ClipKey::frameBlend).toInt()==mode) return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setFrameBlendAt(track, index, mode)) {
        emit editRefused(tr("Frame sampling could not be changed for this clip."));return;
    }
    recordEdit(before, true, tr("Set frame sampling"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::connectGenerator(const QString &resource, int position, int length, bool adjust) {
    if (exportActive() || resource.isEmpty() || length < 1)
        return;
    const QVariantList before = m_timeline.entries();
    // An empty timeline has no V1 to connect above: the generator starts
    // the edit on V1 instead. An adjustment clip grades what lies under it,
    // and there is nothing yet.
    const bool wasEmpty = m_timeline.isEmpty();
    if (wasEmpty && adjust) {
        emit editRefused(tr("An effect layer grades the picture under it; add a clip first."));
        return;
    }
    // The count generator counts down to its own out point, a minute or more
    // away: take the leader from the end of the source so it counts the
    // clip's seconds down to one, and pulling its head out counts longer.
    int in = 0;
    if (Timeline::generatorKindOf(resource) == QStringLiteral("count"))
        in = qMax(0, m_timeline.sourceFrameCount(resource) - length);
    if (wasEmpty ? !m_timeline.append(resource, in, in + length - 1)
                 : !m_timeline.connectAt(resource, position, in, in + length - 1)) {
        reportRefusedPlacement(wasEmpty ? 0 : -1);
        return;
    }
    if (wasEmpty)
        m_position = 0;
    if (adjust) {
        int t = -1, c = -1;
        if (m_timeline.connectedAddress(m_timeline.connectedCount() - 1, &t, &c))
            m_timeline.setAdjustAt(t, c, true);
    }
    recordEdit(before, true, tr("Connect generator"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setGeneratorColor(int track, int index, const QString &color) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QString resource = Timeline::generatorResource(QStringLiteral("solid"), color);
    const QVariantList before = m_timeline.entries();
    QVariantList clips = before;
    bool found = false;
    for (int i = 0; i < clips.size(); ++i) {
        QVariantMap e = clips.at(i).toMap();
        if (e.value(ClipKey::track).toInt() != track
            || e.value(ClipKey::index).toInt() != index
            || e.value(ClipKey::generator).toString() != QStringLiteral("solid"))
            continue;
        if (e.value(ClipKey::path).toString() == resource)
            return;
        e.insert(ClipKey::path, resource);
        clips[i] = e;
        found = true;
        break;
    }
    if (!found)
        return;
    // A rebuild that stops part way leaves the timeline short of clips:
    // rebuild() puts the arrangement from before back.
    if (!m_timeline.rebuild(clips, before)) {
        emit editRefused(tr("The colour could not be changed, so nothing was changed."));
        return;
    }
    recordEdit(before, true, tr("Set generator colour"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedRotation(int index, double degrees) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedRotation(index, degrees))
        return;
    recordEdit(before, true, tr("Set connected rotation"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::applyRetime(int request, const QVariantMap &target, double speed) {
    const auto fail=[this,request](const QString &message){emit retimeFinished(request,false,message);};
    if (exportActive()) { fail(tr("Wait for the current export before changing speed.")); return; }
    if (m_activeSequence<0 || m_activeSequence>=m_sequences.size() ||
        target.value("sequenceId").toString()!=m_sequences[m_activeSequence].id) {
        fail(tr("The sequence changed. Reopen the clip to continue.")); return;
    }
    const int track=target.value("track",-1).toInt(),index=target.value("index",-1).toInt();
    const auto entry=m_timeline.entryMapOn(track,index);
    if (entry.isEmpty() || m_timeline.trackLocked(track) || MarkerEdit::clipState(entry)!=target.value("entryState").toString()) {
        fail(tr("The clip changed or its track is locked. Reopen it to continue.")); return;
    }
    const auto result=RetimeEdit::preview(entry,speed);
    if (!result.value("valid").toBool()) { fail(result.value("error").toString()); return; }
    if (qFuzzyCompare(entry.value(ClipKey::speed,1.0).toDouble(),speed)) { emit retimeFinished(request,false,{}); return; }
    const auto before=m_timeline.entries();
    const int primaryBefore=m_timeline.primaryFrameCount();
    const int oldEnd=entry.value(ClipKey::start).toInt()+entry.value(ClipKey::frames).toInt();
    if (!m_timeline.setClipSpeedOn(track,index,speed)) { fail(tr("The clip could not be retimed. Its previous state was preserved.")); return; }
    recordEdit(before,true,tr("Speed / Duration"));
    const int delta=m_timeline.primaryFrameCount()-primaryBefore;
    rippleMarkers(oldEnd+qMin(0,delta),delta);
    setSpeed(0.0); announceTimeline();
    m_position=qBound(0,m_position,qMax(0,m_timeline.frameCount()-1));
    refreshFrame();
    emit retimeFinished(request,true,{});
}

void PlayerWorker::prepareSpeedRamp(int request,const QVariantMap &target) {
    const int track=target.value("track",-1).toInt(),index=target.value("index",-1).toInt();
    QString error;
    if(exportActive())error=tr("Wait for the current export before changing speed.");
    else if(m_activeSequence<0 || m_activeSequence>=m_sequences.size() || target.value("sequenceId").toString()!=m_sequences[m_activeSequence].id)
        error=tr("The sequence changed. Reopen the clip to continue.");
    else if(MarkerEdit::clipState(m_timeline.entryMapOn(track,index))!=target.value("entryState").toString())
        error=tr("The clip changed. Reopen it to continue.");
    emit speedRampPrepared(request,error.isEmpty()?m_timeline.speedRampState(track,index):QVariantMap{{"valid",false},{"error",error}});
}

void PlayerWorker::applySpeedRamp(int request,const QVariantMap &target,const QVariantMap &definition,bool normal) {
    auto fail=[&](const QString &error){emit retimeFinished(request,false,error);};
    if(exportActive()){fail(tr("Wait for the current export before changing speed."));return;}
    if(m_activeSequence<0 || m_activeSequence>=m_sequences.size() || target.value("sequenceId").toString()!=m_sequences[m_activeSequence].id) {
        fail(tr("The sequence changed. Reopen the clip to continue."));return;
    }
    const int track=target.value("track",-1).toInt(),index=target.value("index",-1).toInt();
    const auto entry=m_timeline.entryMapOn(track,index);
    if(entry.isEmpty() || MarkerEdit::clipState(entry)!=target.value("entryState").toString()) {fail(tr("The clip changed. Reopen it to continue."));return;}
    const auto before=m_timeline.entries();const int primaryBefore=m_timeline.primaryFrameCount();
    const int oldEnd=entry.value(ClipKey::start).toInt()+entry.value(ClipKey::frames).toInt();QString error;
    if(!m_timeline.setSpeedRampOn(track,index,definition,&error,normal)){emit retimeFinished(request,false,error);return;}
    recordEdit(before,true,tr("Edit speed segments"));
    const int delta=m_timeline.primaryFrameCount()-primaryBefore;rippleMarkers(oldEnd+qMin(0,delta),delta);
    setSpeed(0.);announceTimeline();m_position=qBound(0,m_position,qMax(0,m_timeline.frameCount()-1));refreshFrame();
    emit retimeFinished(request,true,{});
}

void PlayerWorker::setConnectedSpeed(int index, double speed) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    int track = -1, clip = -1;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.connectedAddress(index, &track, &clip)) return;
    const auto entry=m_timeline.entryMapOn(track,clip);
    const int oldEnd=entry.value(ClipKey::start).toInt()+entry.value(ClipKey::frames).toInt();
    if (!m_timeline.setClipSpeedOn(track, clip, speed)) return;
    recordEdit(before, true, tr("Set connected speed"));
    // V1 only moves if it rode the ripple (closing/opening space).
    const int delta=m_timeline.primaryFrameCount()-primaryBefore;
    rippleMarkers(oldEnd+qMin(0,delta),delta);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::setClipSpeed(int index, double speed) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.setClipSpeed(index, speed))
        return;
    recordEdit(before, true, tr("Set clip speed"));
    const int delta=m_timeline.primaryFrameCount()-primaryBefore;
    rippleMarkers(entryEnd(before,index)+qMin(0,delta),delta);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::removeDissolve(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.unmixAt(index))
        return;
    recordEdit(before, true, tr("Remove dissolve"));
    rippleMarkers(entryEnd(before, index + 1),
                  m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::restyleDissolve(int index, const QString &style) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setMixStyle(index, style))
        return;
    recordEdit(before, true, tr("Restyle dissolve"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::resizeDissolve(int index, int length) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.setMixLength(index, length))
        return;
    recordEdit(before, true, tr("Resize dissolve"));
    rippleMarkers(entryEnd(before, index + 1),
                  m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::setMixAlign(int track, int index, const QString &align) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.setMixAlignOn(track, index, align))
        return;
    recordEdit(before, true, tr("Align transition"));
    // Overlap ↔ handles changes the storyline's length: markers past the
    // pair ride, like every other length change.
    if (track == 0)
        rippleMarkers(entryEnd(before, index + 1),
                      m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::addTransition(int track, int index, int length, const QString &style) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.addMixAcrossOn(track, index, length, style)) {
        if (m_timeline.trackLocked(track))
            emit editRefused(tr("%1 is locked; nothing was added.").arg(m_timeline.trackName(track)));
        else if (m_timeline.entryMapOn(track, index).value(ClipKey::mixOut).toInt() < 1)
            emit editRefused(tr("A transition goes between two clips that meet on one track; nothing was added."));
        return;
    }
    recordEdit(before, true, tr("Add transition"));
    // Only frames neither handle could give shorten the storyline.
    if (track == 0)
        rippleMarkers(entryEnd(before, index + 1),
                      m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::setTransitionSides(int track, int index, int before, int after) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList was = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.setMixSidesOn(track, index, before, after))
        return;
    recordEdit(was, true, before < 1 && after < 1 ? tr("Remove transition") : tr("Resize transition"));
    if (track == 0)
        rippleMarkers(entryEnd(was, index + 1),
                      m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::setScaling(int track, int index, const QString &scaling) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setScalingAt(track, index, scaling) || m_timeline.entries() == before)
        return;
    recordEdit(before, true, tr("Set scaling"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setBlend(int track, int index, int mode) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setBlendAt(track, index, mode))
        return;
    recordEdit(before, true, tr("Set blend"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::editVideoEffect(int track, int index, const QString &action,
                                  const QString &uid, const QVariantMap &values) {
    if (staleEdit())
        return;
    if (exportActive()) { emit editRefused(tr("Video effects cannot be changed during export.")); return; }
    const QVariantList before = m_timeline.entries();
    QString error;
    if (!m_timeline.editVideoEffectAt(track, index, action, uid, values, &error)) {
        if (!error.isEmpty()) emit editRefused(error);
        return;
    }
    // A chroma key starts from the backdrop actually filmed, under the playhead,
    // and looks again for another screen colour: all one undo step.
    const QVariantMap given = values.value(QStringLiteral("params")).toMap();
    const bool added = action == "add" && values.value(QStringLiteral("id")).toString() == QLatin1String("chroma-key");
    const QString screen = given.value(QStringLiteral("screen")).toString();
    if ((added || (action == "update" && !screen.isEmpty() && screen != QLatin1String("other"))) && !given.contains(QStringLiteral("backdrop"))) {
        const QVariantMap clip = m_timeline.entryMapOn(track, index);
        const QVariantList stack = clip.value(ClipKey::videoEffects).toList();
        const QVariantMap state = added ? stack.value(stack.size() - 1).toMap() : [&] {
            for (const QVariant &v : stack) if (v.toMap().value(QStringLiteral("uid")).toString() == uid) return v.toMap();
            return QVariantMap{};
        }();
        if (state.value(QStringLiteral("id")).toString() == QLatin1String("chroma-key")) {
            const int clipFrame = std::clamp(m_position - clip.value(ClipKey::start).toInt(), 0, std::max(0, clip.value(ClipKey::frames).toInt() - 1));
            const QString key = state.value(QStringLiteral("uid")).toString();
            QString ignored;
            const QImage picture = m_timeline.effectInputAt(track, index, key, clipFrame, &ignored);
            const QVariantMap params = state.value(QStringLiteral("params")).toMap();
            const Keyer::Backdrop found = Keyer::find(picture, params.value(QStringLiteral("screen"), QStringLiteral("green")).toString(), params.value(QStringLiteral("backdrop")).toString());
            if (!found.colour.isEmpty())
                m_timeline.editVideoEffectAt(track, index, QStringLiteral("update"), key,
                                             {{QStringLiteral("params"), QVariantMap{{QStringLiteral("backdrop"), found.colour}}}}, &ignored);
        }
    }
    const QString label = action == "add" ? tr("Add video effect")
        : action == "remove" ? tr("Remove video effect") : action == "move" ? tr("Reorder video effects")
        : action == "reset" ? tr("Reset video effect") : tr("Adjust video effect");
    recordEdit(before, true, label);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
    emit editOperationFinished(label);
}

// The chroma key's backdrop from the clip's frame under the playhead: found
// (x < 0) or picked at x, y (0..1 across and down the picture).
void PlayerWorker::findKeyBackdrop(int track, int index, const QString &uid, int clipFrame, double x, double y) {
    if (exportActive()) { emit editRefused(tr("Video effects cannot be changed during export.")); return; }
    QString error;
    const QImage picture = m_timeline.effectInputAt(track, index, uid, clipFrame, &error);
    if (picture.isNull()) { emit editRefused(error); return; }
    QVariantMap params;
    for (const QVariant &v : m_timeline.entryMapOn(track, index).value(ClipKey::videoEffects).toList())
        if (v.toMap().value(QStringLiteral("uid")).toString() == uid) params = v.toMap().value(QStringLiteral("params")).toMap();
    const Keyer::Backdrop found = x < 0 ? Keyer::find(picture, params.value(QStringLiteral("screen"), QStringLiteral("green")).toString(),
                                                      params.value(QStringLiteral("backdrop")).toString())
                                        : Keyer::pick(picture, x, y);
    if (found.colour.isEmpty()) { emit editRefused(found.error); return; }
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.editVideoEffectAt(track, index, QStringLiteral("update"), uid,
                                      {{QStringLiteral("params"), QVariantMap{{QStringLiteral("backdrop"), found.colour}}}}, &error)) {
        if (!error.isEmpty()) emit editRefused(error);
        return;
    }
    const QString label = x < 0 ? tr("Find backdrop") : tr("Pick backdrop");
    recordEdit(before, true, label);
    announceTimeline();
    refreshFrame();
    emit editOperationFinished(label);
}

void PlayerWorker::setLens(int track, int index, double k1, double k2, double cx, double cy) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setLensAt(track, index, k1, k2, cx, cy))
        return;
    recordEdit(before, true, tr("Adjust lens correction"));
    announceTimeline();
    refreshFrame();
}
