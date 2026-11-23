#include "playerworker_p.h"

namespace {
QVariantList labelHistory(QVariantList state, const QString &label) {
    if (!state.isEmpty()) { auto last = state.last().toMap(); last["omavid:undoLabel"] = label; state.last() = last; }
    return state;
}
}

QVariantMap PlayerWorker::markerSentinel() const {
    QVariantMap sentinel;
    sentinel.insert(QStringLiteral("omavid:markers"), m_markers);
    if(m_activeSequence>=0 && m_activeSequence<m_sequences.size())
        sentinel["omavid:sequenceId"]=m_sequences[m_activeSequence].id;
    auto format=m_timeline.projectSettings();
    if(const auto *profile=m_timeline.profile()) {
        format["custom"]=true;format["width"]=profile->width();format["height"]=profile->height();
        format["fpsNum"]=profile->frame_rate_num();format["fpsDen"]=profile->frame_rate_den();
    }
    sentinel["omavid:sequenceFormat"]=format;
    return sentinel;
}

int PlayerWorker::markerIndexAt(int frame) const {
    for (int i = 0; i < m_markers.size(); ++i)
        if (m_markers.at(i).toMap().value(QStringLiteral("frame")).toInt()
            == frame)
            return i;
    return -1;
}

void PlayerWorker::sortMarkers() {
    std::sort(m_markers.begin(), m_markers.end(),
              [](const QVariant &a, const QVariant &b) {
                  return a.toMap().value(QStringLiteral("frame")).toInt()
                         < b.toMap().value(QStringLiteral("frame")).toInt();
              });
}

// The undo-time snapshot carries the track order too: redoing a track
// move must put the moved headers back where they went.
QVariantList PlayerWorker::snapshotNow() {
    QVariantList snap = m_timeline.entries();
    QVariantMap sentinel = markerSentinel();
    sentinel.insert(QStringLiteral("omavid:tracks"), m_timeline.trackFlagsVariant());
    sentinel.insert(QStringLiteral("omavid:master"), m_timeline.masterMix());
    sentinel.insert(QStringLiteral("omavid:adr"), m_timeline.adrCues());
    sentinel.insert(QStringLiteral("omavid:captions"), m_timeline.captionSettings());
    snap.append(sentinel);
    return snap;
}

void PlayerWorker::recordEdit(const QVariantList &before, bool settleAudio, const QString &action) {
    ++m_editRevision;
    if (m_timeline.takeRippleFailure()) {
        // The storyline moved but its connected clips could not follow. Half
        // an edit puts every later clip out of step: put the whole one back.
        QVariantList snap = before;
        if (snap.isEmpty() || !snap.last().toMap().contains(QStringLiteral("omavid:markers")))
            snap.append(markerSentinel()); // still the pre-edit markers
        m_timeline.takeBrokenDissolves();
        restoreSnapshot(snap);
        // Only for the rest of this edit: its caller's marker ripple follows.
        m_editRolledBack = true;
        QTimer::singleShot(0, this, [this] { m_editRolledBack = false; });
        emit editRefused(tr("The connected clips could not follow this edit, so nothing was changed."));
        return;
    }
    // Linked tracks: embedded sound goes to an audio track, linked
    // audio follows its video. Before the snapshot lands, so one undo
    // takes the whole edit back.
    if (settleAudio)
        m_timeline.settleLinkedAudio(before, m_pictureOnlyEdit);
    if (const int broken = m_timeline.takeBrokenDissolves(); broken > 0)
        emit editOperationFinished(tr("Removed %n dissolve(s) the edit cut through. Undo brings them back.", nullptr, broken));
    const QVariantMap linkedBefore=inactiveColorSnapshots();
    const bool shared=synchronizeColorLinks(before);
    QVariantList snap = before;
    // rippleMarkers runs AFTER recordEdit, so the current list is still
    // the pre-edit list; toggleMarker appends its own pre-toggle sentinel.
    if (snap.isEmpty()
        || !snap.last().toMap().contains(QStringLiteral("omavid:markers")))
        snap.append(markerSentinel());
    if(shared) {
        auto sentinel=snap.last().toMap();
        if(!sentinel.contains("omavid:colorSequences"))sentinel["omavid:colorSequences"]=linkedBefore;
        snap.last()=sentinel;
        announceSequences();
    }
    auto history = snap.last().toMap();
    history["omavid:undoLabel"] = action.isEmpty() ? tr("Edit") : action;
    history["omavid:sequenceId"] = m_sequences[m_activeSequence].id;
    history["omavid:sequenceFormat"] = markerSentinel().value("omavid:sequenceFormat");
    snap.last() = history;
    m_undoStack.append(snap);
    m_redoStack.clear();
    constexpr int kUndoDepth = 100;
    while (m_undoStack.size() > kUndoDepth)
        m_undoStack.removeFirst();
}

void PlayerWorker::toggleMarker(int frame) {
    if (exportActive() || frame < 0)
        return;
    QVariantList before = m_timeline.entries();
    before.append(markerSentinel()); // the pre-toggle marker list
    const int existing = markerIndexAt(frame);
    if (existing >= 0) {
        m_markers.removeAt(existing);
    } else {
        QVariantMap marker; // new markers are blue standard, the default
        marker.insert(QStringLiteral("frame"), frame);
        marker.insert(QStringLiteral("text"), QString());
        marker.insert(QStringLiteral("type"), QStringLiteral("standard"));
        marker.insert(QStringLiteral("notes"), QString());
        m_markers.append(marker);
        sortMarkers();
    }
    recordEdit(before, true, tr("Add / Remove Marker"));
    emit markersChanged(m_markers);
    announceTimeline(); // canUndo/canRedo ride timelineChanged (and it dirties)
}

void PlayerWorker::removeMarkersIn(int first, int last) {
    if (exportActive())
        return;
    QVariantList kept;
    for (const QVariant &marker : m_markers) {
        const int frame = marker.toMap().value(QStringLiteral("frame")).toInt();
        if (frame < first || frame > last)
            kept.append(marker);
    }
    if (kept.size() == m_markers.size())
        return;
    QVariantList before = m_timeline.entries();
    before.append(markerSentinel());
    m_markers = kept;
    recordEdit(before, true, tr("Delete Markers in Range"));
    emit markersChanged(m_markers);
    announceTimeline();
}

void PlayerWorker::setMarkerInfo(int frame, const QString &text,
                                 const QString &type, const QString &notes) {
    if (exportActive())
        return;
    const int index = markerIndexAt(frame);
    if (index < 0)
        return;
    if (!MarkerEdit::types().contains(type)) {
        emit editRefused(tr("Choose a valid marker type."));
        return;
    }
    QVariantMap marker = m_markers.at(index).toMap();
    if (marker.value(QStringLiteral("text")).toString() == text
        && marker.value(QStringLiteral("type")).toString() == type
        && marker.value(QStringLiteral("notes")).toString() == notes)
        return;
    QVariantList before = m_timeline.entries();
    before.append(markerSentinel());
    marker.insert(QStringLiteral("text"), text);
    marker.insert(QStringLiteral("type"), type);
    marker.insert(QStringLiteral("notes"), notes);
    m_markers[index] = marker;
    recordEdit(before, true, tr("Edit Marker"));
    emit markersChanged(m_markers);
    announceTimeline();
}

void PlayerWorker::applyMarkerEdit(int request, const QVariantMap &target, int frame,
                                  const QString &type, const QString &text, const QString &notes, bool remove,
                                  const QString &colour) {
    const auto fail = [this,request](const QString &error) { emit markerEditFinished(request,false,error); };
    if (exportActive()) { fail(tr("Wait for the current export before editing markers.")); return; }
    if (m_activeSequence < 0 || m_activeSequence >= m_sequences.size() ||
        target.value("sequenceId").toString() != m_sequences[m_activeSequence].id) {
        fail(tr("The sequence changed. Reopen the marker to continue.")); return;
    }
    const int track = target.value("track",-1).toInt(), index = target.value("index").toInt();
    const auto entry = track >= 0 ? m_timeline.entryMapOn(track,index) : QVariantMap();
    if (track >= 0 && (entry.isEmpty() || MarkerEdit::clipState(entry) != target.value("entryState").toString() || m_timeline.trackLocked(track))) {
        fail(tr("The clip changed or its track is locked. Reopen the marker to continue.")); return;
    }
    auto markers = track >= 0 ? entry.value(ClipKey::markers).toList() : m_markers;
    const auto original = target.value("marker").toMap();
    const int at = markers.indexOf(original);
    if (at < 0) { fail(tr("This marker changed or was removed. Reopen it to continue.")); return; }
    if (!remove) {
        const int maximum = qMax(original.value("frame").toInt(), track >= 0 ? entry.value(ClipKey::length).toInt()-1 : m_timeline.frameCount()-1);
        if (frame < 0 || frame > maximum) { fail(tr("Marker position must be between 0 and %1 frames.").arg(maximum)); return; }
        if (!MarkerEdit::types().contains(type)) { fail(tr("Choose a valid marker type.")); return; }
        if (!colour.isEmpty() && !MarkerEdit::colours().contains(colour)) { fail(tr("Choose one of the marker colours.")); return; }
        for (int i=0;i<markers.size();++i) if (i != at && markers[i].toMap().value("frame").toInt() == frame) {
            fail(tr("Another marker already occupies this position. Choose a different frame.")); return;
        }
        auto edited = original;
        edited["frame"]=frame; edited["type"]=type; edited["text"]=text; edited["notes"]=notes;
        if (colour.isEmpty()) edited.remove("colour"); else edited["colour"]=colour;
        if (edited == original) { emit markerEditFinished(request,false,{}); return; }
        markers[at]=edited;
        std::sort(markers.begin(),markers.end(),[](const QVariant &a,const QVariant &b) {
            return a.toMap().value("frame").toInt() < b.toMap().value("frame").toInt();
        });
    } else markers.removeAt(at);
    auto before = m_timeline.entries();
    before.append(markerSentinel());
    if (track >= 0) {
        if (!m_timeline.setClipMarkersAt(track,index,markers)) { fail(tr("The clip marker could not be updated.")); return; }
    } else m_markers=markers;
    recordEdit(before,false,remove ? tr("Delete Marker") : tr("Edit Marker"));
    if (track < 0) emit markersChanged(m_markers);
    announceTimeline();
    emit markerEditFinished(request,true,{});
}

void PlayerWorker::rippleMarkers(int pivot, int delta) {
    if (m_editRolledBack) // the edit that asked for this was put back
        return;
    if (delta == 0 || m_markers.isEmpty())
        return;
    QVariantList kept;
    bool changed = false;
    for (const QVariant &v : std::as_const(m_markers)) {
        QVariantMap marker = v.toMap();
        const int frame =
            marker.value(QStringLiteral("frame")).toInt();
        if (frame >= pivot) {
            const int shifted = frame + delta;
            if (shifted < pivot) { // sat inside the removed span
                changed = true;
                continue;
            }
            marker.insert(QStringLiteral("frame"), shifted);
            changed = true;
        }
        kept.append(marker);
    }
    if (!changed)
        return;
    m_markers = kept;
    emit markersChanged(m_markers);
}

void PlayerWorker::setMarkers(const QVariantList &markers) {
    m_markers = markers;
    sortMarkers();
    emit markersChanged(m_markers);
}
// A history snapshot carries the track headers as they were when it was
// taken, and with them each track's own height on the Audio page. That height
// is how the editor looks at the track now, not part of any edit: a replay
// keeps today's heights. `then` is the snapshot's headers and `now` the
// present ones (Timeline::trackFlagsVariant); the answer is `then` with the
// present heights on the tracks that are in both. Which track is which is
// told as the Audio page tells it for its selection: the same list, two
// tracks that changed places, or one track that came or went. A track the
// replay brings back keeps the height it had, and where the tracks cannot be
// told apart the snapshot's heights stand.
static QVariantList withPresentAudioHeights(QVariantList then, const QVariantList &now) {
    static const QString height = QStringLiteral("audioHeight"), mixKey = QStringLiteral("mix");
    const auto apart = [](const QVariant &track) { // what a track is, apart from its height
        QVariantMap map = track.toMap();
        QVariantMap mix = map.value(mixKey).toMap();
        mix.remove(height);
        map.insert(mixKey, mix);
        return map;
    };
    const auto carry = [&](int to, int from) {
        QVariantMap map = then[to].toMap();
        QVariantMap mix = map.value(mixKey).toMap();
        const QVariant present = now[from].toMap().value(mixKey).toMap().value(height);
        if (present.isValid())
            mix.insert(height, present);
        else
            mix.remove(height);
        map.insert(mixKey, mix);
        then[to] = map;
    };
    QVariantList was, is;
    for (const QVariant &track : std::as_const(then)) was.append(apart(track));
    for (const QVariant &track : now) is.append(apart(track));
    const int count = int(then.size());
    if (now.size() == count) {
        QList<int> changed;
        for (int i = 0; i < count; ++i)
            if (was[i] != is[i]) changed.append(i);
        const bool swapped = changed.size() == 2 && was[changed[0]] == is[changed[1]] && was[changed[1]] == is[changed[0]];
        for (int i = 0; i < count; ++i)
            carry(i, swapped && i == changed[0] ? changed[1] : swapped && i == changed[1] ? changed[0] : i);
        return then;
    }
    const bool returns = count == now.size() + 1, leaves = count == now.size() - 1;
    if (!returns && !leaves)
        return then;
    // One track comes back or goes: every place it can be at.
    const QVariantList &longer = returns ? was : is, &shorter = returns ? is : was;
    int first = -1, last = -1;
    for (int place = 0; place < longer.size(); ++place) {
        bool fits = true;
        for (int i = 0; i < shorter.size() && fits; ++i) fits = shorter[i] == longer[i < place ? i : i + 1];
        if (fits) { if (first < 0) first = place; last = place; }
    }
    if (first < 0)
        return then;
    for (int i = 0; i < count; ++i) {
        if (returns) { // `then` is the longer list
            if (i < first) carry(i, i);
            else if (i > last) carry(i, i - 1);
        } else {
            if (i < first) carry(i, i);
            else if (i >= last) carry(i, i + 1);
        }
    }
    return then;
}

void PlayerWorker::undo() {
    restoreHistory(false);
}
void PlayerWorker::redo() {
    restoreHistory(true);
}
void PlayerWorker::restoreHistory(bool redo) {
    m_lastEditGesture = 0;
    auto &from=redo?m_redoStack:m_undoStack;
    auto &to=redo?m_undoStack:m_redoStack;
    if(exportActive() || from.isEmpty())return;
    m_replaying = true; // the clip list is about to change under the window (see staleEdit)
    ++m_editRevision; // mixer and pointer gestures cannot span a history replay
    setSpeed(0.);
    const auto target=from.last();const auto sentinel=target.isEmpty()?QVariantMap{}:target.last().toMap();
    const int previous=m_activeSequence;
    // Structural history can restore a closed timeline. Ordinary clip history
    // must first select its existing owner, before capturing the redo state.
    if(!sentinel.contains("omavid:sequenceState")) {
        const auto owner=sentinel.value("omavid:sequenceId").toString();
        int index=-1;
        for(int i=0;i<m_sequences.size();++i)if(m_sequences[i].id==owner)index=i;
        if(index<0) {emit editRefused(tr("The timeline for this history entry is no longer available."));return;}
        switchSequence(index);
        if(m_activeSequence!=index)return;
    }
    const auto label=historyLabel(from);
    const auto counterpart=labelHistory(sourceAttributeCounterpart(target),label);
    from.removeLast();to.append(counterpart);
    QVariantList replayed=target;
    if(sentinel.contains(QStringLiteral("omavid:tracks")) && !sentinel.contains("omavid:sequenceState") && !sentinel.contains("omavid:sourceState")) {
        QVariantMap headers=sentinel;
        headers.insert(QStringLiteral("omavid:tracks"),withPresentAudioHeights(sentinel.value(QStringLiteral("omavid:tracks")).toList(),m_timeline.trackFlagsVariant()));
        replayed.last()=headers;
    }
    if (!restoreSnapshot(replayed)) {
        to.removeLast();from.append(target);
        if(previous!=m_activeSequence)switchSequence(previous);
        announceTimeline(false);
    }
    announceSequences();
}

bool PlayerWorker::putBack(const QVariantList &clips, const QVariantList &tracks) {
    if (m_timeline.entries() == clips && m_timeline.trackFlagsVariant() == tracks)
        return true;
    if (m_timeline.restore(clips, Timeline::trackFlagsFromVariant(tracks)))
        return true;
    // Fewer clips than the window shows, and the next save would write them:
    // show what is there, with the history that can still reach the rest.
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
    emit editRefused(tr("The timeline could not be put back as it was after this edit. Undo, or reopen the project, before saving."));
    return false;
}

bool PlayerWorker::restoreSnapshot(const QVariantList &snapshot, bool contentEdit) {
    discardPausedAudio();
    const auto sequenceState=snapshot.isEmpty()?QVariantMap{}:snapshot.last().toMap().value("omavid:sequenceState").toMap();
    if(!sequenceState.isEmpty()) {
        const auto problem=restoreSourceAttributeState(sequenceState,contentEdit,true,true);
        if(!problem.isEmpty())emit editRefused(problem);
        return problem.isEmpty();
    }
    const auto sourceState=snapshot.isEmpty()?QVariantMap{}:snapshot.last().toMap().value("omavid:sourceState").toMap();
    if(!sourceState.isEmpty()) {
        const auto problem=restoreSourceAttributeState(sourceState,contentEdit);
        if(!problem.isEmpty())emit editRefused(problem);
        return problem.isEmpty();
    }
    setSpeed(0.0);
    const QVariantList fallback = m_timeline.entries();
    const auto previousMarkers=m_markers;
    QVariantList clips = snapshot;
    bool hasTracks = false;
    QVariantMap colorSnapshots;
    QVariantMap audioMaster; QVariantList audioCues; bool hasAudioMix = false;
    QVariantMap captionSettings; bool hasCaptionSettings = false;
    bool hasColorSnapshots=false;
    std::vector<Timeline::TrackFlags> tracks;
    if (!clips.isEmpty()) {
        const QVariantMap last = clips.last().toMap();
        if (last.contains(QStringLiteral("omavid:markers"))) {
            clips.removeLast();
            hasAudioMix = last.contains("omavid:master");
            audioMaster = last.value("omavid:master").toMap();
            audioCues = last.value("omavid:adr").toList();
            hasCaptionSettings = last.contains("omavid:captions");
            captionSettings = last.value("omavid:captions").toMap();
            hasColorSnapshots=last.contains("omavid:colorSequences");
            colorSnapshots=last.value("omavid:colorSequences").toMap();
            const QVariantList markers =
                last.value(QStringLiteral("omavid:markers")).toList();
            if (markers != m_markers) {
                m_markers = markers;
                emit markersChanged(m_markers);
            }
            // Only a track-order edit (and the undo-time snapshot) carries
            // the headers; every other snapshot leaves the flags alone.
            if (last.contains(QStringLiteral("omavid:tracks"))) {
                hasTracks = true;
                tracks = Timeline::trackFlagsFromVariant(
                    last.value(QStringLiteral("omavid:tracks")).toList());
            }
        }
    }
    // A snapshot that no longer rebuilds (media gone missing) must not
    // wipe the timeline — put the current arrangement back and say so.
    const bool ok = hasTracks ? m_timeline.rebuild(clips, fallback, tracks)
                              : m_timeline.rebuild(clips, fallback);
    if(!ok && m_markers!=previousMarkers) {m_markers=previousMarkers;emit markersChanged(m_markers);}
    if (ok && hasAudioMix) { m_timeline.setMasterMixMap(audioMaster); m_timeline.setAdrCues(audioCues); }
    if (ok && hasCaptionSettings) m_timeline.setCaptionSettings(captionSettings);
    if(ok && hasColorSnapshots)restoreColorSnapshots(colorSnapshots);
    announceTimeline(contentEdit);
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
    return ok;
}
