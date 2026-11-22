import QtQuick
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Audio

// The seam: the Audio workspace bound to the engine. The workspace knows
// nothing of `backend` or `win`; everything it asks for is answered here.
AudioWorkspace {
    id: page

    tracks: backend.tracks
    embeddedAudioCount: win.embeddedAudioCount
    wheelAction: win.wheelAction
    wheelInvert: win.wheelInvert
    wheelSensitivity: win.wheelSensitivity
    gestureTarget: function(track, index) { return backend.audioEditTarget(track, index) }
    gesturePreview: function(target, action, changes, rate) { return backend.previewAudioGesture(target, action, changes, rate) }
    snapPoints: function(exclude, first, last) { return backend.audioSnapPoints(exclude, first, last, win.showBeatMap) }
    rectifiedWaveforms: win.rectifiedWaveforms
    beatLines: win.showBeatMap ? win.beatLines : []
    snapping: win.snapEnabled
    onSnappingToggled: Ui.Commands.trigger("view.snapping")
    property int gestureRequest: 0
    gesturePending: gestureRequest !== 0
    onGestureCommitted: function(target, action, changes, rate) {
        if (gesturePending) return
        gestureError = ""
        stopAuditionFirst()
        gestureRequest = backend.applyAudioGesture(target, action, changes, rate)
    }
    onTimeSeekRequested: function(position, audition) { backend.seekAudioPosition(position, audition) }
    backgroundRender: backend.backgroundRender
    clips: backend.connected
    // Only while this page is shown: it stays loaded once opened, and the
    // meters tick on every page during playback.
    trackLevels: !page.visible ? [] : backend.meters.trackLevels.map(function (m, i) { var c = Object.assign({}, m); var a = backend.meters.routingLevels[i]; if (a) { c.rms = a.rms; c.gainReduction = a.gainReduction; c.volume=a.volume; c.pan=a.pan; } return c })
    routingLevels: backend.meters.routingLevels
    master: backend.master
    monitorSettings: backend.monitorSettings
    onMonitorEdited: function (muted, dim, mono) { backend.setMonitorOptions(muted, dim, mono) }
    masterLeft: backend.meters.left
    masterRight: backend.meters.right
    loudness: backend.meters.loudness
    loudnessHistory: backend.meters.loudnessHistory
    spectrum: backend.meters.spectrum
    analysis: backend.loudnessPass.result
    sounds: backend.soundLibrary.items
    soundFolders: backend.soundLibrary.folders
    collections: backend.soundLibrary.collections
    scanning: backend.soundLibrary.scanning
    pending: backend.soundLibrary.pending
    scanStatus: backend.soundLibrary.scanStatus
    mixEffects: backend.mixEffects
    audioServices: backend.audioServices
    auditionVolume: backend.monitorVolume
    onAuditionVolumeEdited: function(v){backend.monitorVolume=v}
    adrCues: backend.adrCues
    inputs: backend.recorder.inputs
    outputs: backend.audioOutputs()
    recording: backend.recordingTake
    recordTrack: backend.recordTrack
    recordLevel: backend.recorder.level
    recorderAvailable: backend.recorder.available
    inputDevice: backend.recorder.inputDevice
    hasVideo: backend.hasVideo
    fps: backend.fps > 0 ? backend.fps : 25
    sampleRate: Number(backend.projectSettings.sampleRate || 48000)
    duration: backend.duration
    position: backend.viewingSource ? win.audioParkedPosition : backend.position
    cursorFrames: backend.audioPositionFrames
    cursorText: { const position = backend.audioPosition; return backend.audioPositionText(page.units) }
    onCursorTextEdited: function(text) { backend.seekAudioText(text, page.units) }
    onSampleStepRequested: function(samples) { backend.stepAudioSamples(samples) }
    onSampleSeekRequested: function(ticks, audition) { backend.seekAudioPosition({ticks: ticks, rate: page.sampleRate}, audition) }
    playing: backend.playing && !backend.viewingSource
    auditioning: backend.viewingSource && backend.auditionPath !== ""
    auditionPlaying: backend.playing && backend.viewingSource && backend.auditionPath !== ""
    auditionPath: backend.auditionPath
    rangeIn: win.timelineRangeIn
    rangeOut: win.timelineRangeOut
    markers: backend.markers
    loop: backend.loop
    projectName: backend.projectName
    sequenceName: backend.sequences.length > backend.activeSequence && backend.activeSequence >= 0
                  ? backend.sequences[backend.activeSequence].name : ""
    imageAspect: backend.videoWidth > 0 && backend.videoHeight > 0 ? backend.videoDisplayAspect : 16 / 9
    videoSurface: Component {
        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
            source: backend
            channel: VideoView.Program
        }
    }
    // Made again when the starting timecode or the rate changes, so every
    // timecode already drawn with it is drawn again.
    timecoder: backend.startFrame >= 0 && backend.fps >= 0 ? function (frame) { return backend.timecode(frame) } : null
    startFrame: backend.startFrame
    fxPresets: win.fxPresets

    // The Edit page and this one share the clip selection.
    selectedClip: win.selectedConnectedIndex
    onDecorationWindowChanged: function(first, last, pixels) { backend.setTimelineDecorationWindow(first, last, pixels) }
    onClipSelected: function (i) {
        win.selectedConnectedIndex = i
        // The standalone workspace owns local selection; the live page must
        // keep following the editor when a committed cut changes flat index.
        page.selectedClip = Qt.binding(function() { return win.selectedConnectedIndex })
    }

    function entryAt(i) { return i >= 0 && i < backend.connected.length ? backend.connected[i] : null }
    function stopAuditionFirst() { if (backend.viewingSource) backend.stopAudition() }

    // Faders, knobs, sliders and the EQ handles edit on every pointer move;
    // each edit rebuilds the track's filter chain in the engine, so they
    // coalesce: the newest value per (track, key) lands every 40 ms.
    property var pendingMix: ({})
    property int mixSequence: backend.activeSequence
    property string mixProject: backend.projectPath
    // Another sequence has other tracks: an index chosen in the last one names none of them.
    onMixSequenceChanged: { fxDraft = null; trackOp = null; if (selectedTrack >= 0) selectedTrack = -1 }
    onMixProjectChanged: { fxDraft = null; trackOp = null; if (selectedTrack >= 0) selectedTrack = -1 }
    Timer {
        id: mixFlush
        interval: 40
        repeat: false
        onTriggered: page.flushMix()
    }
    function flushMix() {
        mixFlush.stop()
        var batch = page.pendingMix
        page.pendingMix = {}
        for (var key in batch) {
            var e = batch[key]
            if(e.sequence!==backend.activeSequence || e.project!==backend.projectPath)continue
            if (e.track === -2) backend.setMasterMix(e.key, e.value)
            else backend.setTrackMix(e.track, e.key, e.value)
        }
    }
    onMixFinished: function (track, key) { flushMix(); backend.endAudioGesture(track, key) }
    onMixStarted: function (track, key) { flushMix(); backend.beginAudioGesture(track, key) }

    function queueMix(track, key, value) {
        var continuous = key === "volume" || key === "pan" || key === "fx" || key === "sends"
        if (!continuous) { if (track === -2) backend.setMasterMix(key, value); else backend.setTrackMix(track, key, value); return }
        var m = page.pendingMix
        m[track + ":" + key] = { track: track, key: key, value: value, sequence: backend.activeSequence, project: backend.projectPath }
        page.pendingMix = m
        if (!mixFlush.running) mixFlush.start()
    }
    onTrackMixEdited: function (t, k, v) { queueMix(t, k, v) }
    onTrackFlagEdited: function (t, f, on) { backend.setTrackFlag(t, f, on) }
    onTrackRenamed: function (t, n) { backend.renameTrack(t, n) }
    // How many tracks there were when a role was asked for: the role is for
    // the track that add makes, never for one that was already there.
    property int roleAskedAt: 0
    onAddTrackRequested: function (role) {
        if (backend.exporting) return // the add is refused: no role is left waiting for some later track
        win.pendingTrackRole = role; page.roleAskedAt = backend.tracks.length; backend.addTrack(true)
    }
    onRemoveTrackRequested: function (t) { page.trackOp = { remove: t }; backend.removeTrack(t) }
    onMoveTrackRequested: function (t, up) {
        var partner = backend.trackNeighbour(t, up)
        if (partner >= 0) page.trackOp = { swap: [t, partner] }
        backend.moveTrack(t, up)
    }
    onMasterEdited: function (k, v) { queueMix(-2, k, v) }
    onClipMoveRequested: function (i, t, s) {
        var e = entryAt(i)
        if (!e) return
        if (t === e.track) backend.moveConnectedClip(i, s)
        else backend.moveClipTo(e.track, e.index, t, s)
    }
    onDuplicateClipRequested: { var e=entryAt(selectedClip); if(e) backend.pasteClip(e.track,e.start+e.frames,e,true) }
    onClipTimingRequested: function(i) { var e = entryAt(i); if (e) audioTimingDialog.openFor(e.track, e.index) }
    onClipSpeedRequested: function (speed) { backend.setConnectedSpeed(selectedClip,speed) }
    onClipFadeCurveRequested: function (curve) { var e=entryAt(selectedClip); if(e) backend.setAudioFadeCurve(e.track,e.index,curve) }
    onClipTrimRequested: function (i, a, b, s) { backend.trimConnectedClip(i, a, b, s) }
    onClipRemoveRequested: function (i, ripple) {
        var e = entryAt(i)
        if (!e) return
        backend.removeClips([{ track: e.track, index: e.index }], ripple)
        win.selectedConnectedIndex = -1
    }
    onSplitRequested: function (t, f) { backend.bladeAt(t, f) }
    onClipFadesEdited: function (i, a, b) { backend.setConnectedFadeLengths(i, a, b) }
    onClipVolumeEdited: function (i, db) { backend.setConnectedVolume(i, db) }
    onClipVolumeKfEdited: function (i, kf) { backend.setConnectedVolumeKf(i, kf) }
    onSeekRequested: function (f) { stopAuditionFirst(); backend.seek(f) }
    onScrubRequested: function (f) { backend.scrubTo(f) }
    function layoutTransport(action,value) {
        if (action === "play" && backend.viewingSource && backend.auditionPath !== "") { backend.togglePlay(); return }
        if (action === "pause") { backend.pause(); return }
        stopAuditionFirst()
        if (action === "play") backend.togglePlay()
        else if (action === "shuttle") backend.shuttle(value)
        else if (action === "start") backend.seek(0)
        else if (action === "end") backend.seek(Math.max(0,backend.duration-1))
        else if (action === "step") backend.step(value)
    }
    onPlayToggled: Ui.Commands.trigger("transport.play")
    onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
    onStartRequested: Ui.Commands.trigger("transport.start")
    onEndRequested: Ui.Commands.trigger("transport.end")
    onStepRequested: function(n) { Ui.Commands.trigger(n < 0 ? "transport.previous-frame" : "transport.next-frame") }
    onLoopToggled: function (on) { backend.loop = on }
    onRangeEdited: function (a, b) { win.timelineRangeIn = a; win.timelineRangeOut = b }
    onRangeCleared: { win.timelineRangeIn = -1; win.timelineRangeOut = -1 }
    // A marker at a frame: the seek lands asynchronously, so the toggle
    // waits for the playhead to arrive (toggleMarker works at the playhead).
    property int pendingMarkerAt: -1
    onMarkerRequested: function (f) {
        stopAuditionFirst()
        if (backend.position === f) { backend.toggleMarker(); return }
        pendingMarkerAt = f
        backend.seek(f)
    }
    onAuditionRequested: function (p) { win.audioParkedPosition = backend.position; backend.auditionSound(p); backend.play() }
    onAuditionStopped: backend.stopAudition()
    onSoundFavorited: function (p, on) { backend.soundLibrary.setFavorite(p, on) }
    onSoundTagsEdited: function (path,tags) { backend.soundLibrary.setTags(path,tags) }
    onSoundCategorized: function (p, c) { backend.soundLibrary.setCategory(p, c) }
    onSoundFolderAddRequested: backend.chooseSoundFolder()
    onSoundFolderRemoved: function (p) { backend.soundLibrary.removeFolder(p) }
    onSoundRescanRequested: backend.soundLibrary.rescan()
    onSoundScanCancelRequested: backend.soundLibrary.cancelScan()
    onCollectionAdded: function (n, q, c) { backend.soundLibrary.addCollection(n, q, c) }
    onCollectionRemoved: function (n) { backend.soundLibrary.removeCollection(n) }
    onSoundDropped: function (p, t, f) { stopAuditionFirst(); backend.dropSoundAt(p, t, f) }
    onSoundPlacedAtPlayhead: function (p) {
        stopAuditionFirst()
        var t = page.selectedTrack >= 0 ? page.selectedTrack : page.recordTrackCandidate >= 0 ? page.recordTrackCandidate : backend.tracks.length
        if (t < 0 || t > backend.tracks.length) t = backend.tracks.length // the master or a bus was selected
        backend.dropSoundAt(p, t, backend.position)
    }
    onRecordToggled: {
        if (backend.recordingTake) backend.stopRecording()
        else if (page.recordTrackCandidate >= 0) backend.startRecording(page.recordTrackCandidate, qsTr("Recording"))
    }
    onRecordTakeRequested: function (cueIndex) { flushMix(); backend.startAdrRecording(cueIndex) }
    onAdrCuesEdited: function (c) { backend.setAdrCues(c) }
    onAdrReviewRequested: function (c, t) {
        var cue = backend.adrCues[c]
        if (cue && cue.takes[t]) { win.audioParkedPosition = backend.position; backend.auditionSound(cue.takes[t].path); backend.play() }
    }
    onAdrPromoteRequested: function (c, t) { backend.promoteAdrTake(c,t) }
    onAdrCompRequested: function (c,t) { backend.promoteAdrTake(c,t,page.rangeIn,page.rangeOut) }
    onInputDeviceEdited: function (id) { backend.recorder.inputDevice = id }
    onLoudnessReset: backend.resetLoudness()
    onAnalyzeRequested: function (selection) { backend.loudnessPass.analyze(selection ? page.rangeIn : -1, selection ? page.rangeOut : -1) }
    onCancelAnalysisRequested: backend.loudnessPass.cancel()
    onNormalizeRequested: function (t, p) { backend.loudnessPass.normalize(t, p) }
    onFxPresetSaved: function (n, fx) {
        var list = win.fxPresets.filter(function (p) { return p.name !== n })
        list.push({ name: n, fx: fx })
        win.fxPresets = list
    }
    onFxPresetDeleted: function (n) { win.fxPresets = win.fxPresets.filter(function (p) { return p.name !== n }) }
    onSnapshotRequested: win.grabStill()

    // A take that just landed: file it under its ADR cue.
    Connections {
        target: backend
        function onAudioGestureFinished(request, changed, error, selected) {
            if (request !== page.gestureRequest) return
            page.gestureRequest = 0
            page.gestureError = error
            if (!changed || !selected.clipId) return
            for (var i = 0; i < backend.connected.length; ++i) {
                if (backend.connected[i].clipId === selected.clipId) {
                    win.selectedConnectedIndex = i
                    page.selectedTrack = backend.connected[i].track
                    break
                }
            }
        }
        function onPositionChanged() {
            if (page.pendingMarkerAt >= 0 && backend.position === page.pendingMarkerAt && !backend.viewingSource) {
                page.pendingMarkerAt = -1
                backend.toggleMarker()
            }
            if (backend.recordingTake && win.adrStopAt >= 0 && backend.position >= win.adrStopAt) {
                win.adrStopAt = -1
                backend.stopRecording()
            }
        }
        function onTracksChanged() {
            // A track added from this page takes the role it was asked for.
            if (win.pendingTrackRole === "") return
            if (backend.tracks.length <= page.roleAskedAt) { win.pendingTrackRole = ""; return } // the add did not happen
            var last = null
            for (var i = 0; i < backend.tracks.length; ++i)
                if (backend.tracks[i].kind === "audio" && backend.tracks[i].clips === 0) last = backend.tracks[i]
            if (last !== null && (last.mix === undefined || last.mix.role === undefined)) {
                var role = win.pendingTrackRole
                win.pendingTrackRole = ""
                backend.setTrackMix(last.index, "role", role)
                page.selectedTrack = last.index
            }
        }
    }
    Component.onCompleted: {
        backend.recorder.refreshInputs()
    }

}
