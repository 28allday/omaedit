import QtQuick
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Deliver

// The seam: the Deliver workspace bound to the engine. The workspace knows
// nothing of `backend` or `win`; everything it asks for is answered here.
DeliverWorkspace {
    id: page

    presets: backend.renderPresets.presets
    jobs: backend.renderQueue.jobs
    queueActive: backend.renderQueue.active
    queuePaused: backend.renderQueue.paused
    activeJob: backend.renderQueue.activeJob
    afterCompletion: backend.renderQueue.afterCompletion
    encoders: backend.hardware.encoders
    hasVideo: backend.hasVideo
    timelineFormats: backend.timelineFormats().map(function (f) { return { id: f, name: backend.timelineFormatName(f), available: backend.timelineFormatAvailable(f) } })
    canExportTimeline: backend.hasVideo && !backend.exporting
    onTimelineExportRequested: function (format) { backend.exportTimelineDialog(format) }
    fps: backend.fps
    audioTracks: backend.audioRenderTracks
    timelineWidth: backend.videoWidth
    timelineHeight: backend.videoHeight
    duration: backend.duration
    position: backend.viewingSource ? 0 : backend.position
    playing: backend.playing && !backend.viewingSource
    imageAspect: backend.videoWidth > 0 && backend.videoHeight > 0 ? backend.videoDisplayAspect : 16 / 9
    rangeIn: win.timelineRangeIn
    rangeOut: win.timelineRangeOut
    selectedSpans: {
        var out = []
        var list = win.selection.selectedEntries()
        for (var i = 0; i < list.length; ++i) {
            var e = list[i]
            if (e.start === undefined || e.frames === undefined) continue
            out.push({ start: e.start, end: e.start + e.frames - 1, name: e.name !== undefined ? e.name : "" })
        }
        return out
    }
    projectName: backend.projectName
    sequenceName: backend.sequences.length > backend.activeSequence && backend.activeSequence >= 0
                  ? backend.sequences[backend.activeSequence].name : ""
    defaultDir: backend.defaultRenderDir
    hasCaptions: backend.captionCount > 0
    captionCount: backend.captionCount
    videoSurface: Component {
        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
            source: backend
            channel: VideoView.Program
        }
    }

    validator: function (spec, path) { return backend.validateRender(spec, path) }
    estimator: function (spec, path) { return backend.renderEstimate(spec, path) }
    checker: function (spec, path) { return backend.renderChecks(spec, path) }
    nameTidier: function (name) { return backend.tidyRenderName(name) }
    fileNamer: function (pattern, presetName, dir, spec) { return backend.renderFileName(pattern, presetName, dir, spec) }
    stemNamer: function (spec, path) { return backend.renderStems(spec, path) }
    extensionOf: function (spec) { return backend.renderExtension(spec) }
    summarizer: function (spec) { return backend.renderSummary(spec) }
    // Made again when the starting timecode or the rate changes, so every
    // timecode already drawn with it is drawn again.
    timecoder: backend.startFrame >= 0 && backend.fps >= 0 ? function (frame) { return backend.timecode(frame) } : null

    onEnqueueRequested: function (name, path, spec, presetName, warnings) { backend.enqueueRender(name, path, spec, presetName, warnings) }
    onRenderAllRequested: backend.renderQueue.renderAll()
    onPauseAllRequested: backend.renderQueue.pauseAll()
    onResumeAllRequested: backend.renderQueue.resumeAll()
    onClearFinishedRequested: backend.renderQueue.clearFinished()
    onAfterCompletionEdited: function (a) { backend.renderQueue.afterCompletion = a }
    onJobAction: function (action, id) {
        switch (action) {
        case "pause": backend.renderQueue.pauseJob(id); break
        case "resume": backend.renderQueue.resumeJob(id); break
        case "cancel": backend.renderQueue.cancelJob(id); break
        case "retry": backend.renderQueue.retryJob(id); break
        case "remove": backend.renderQueue.removeJob(id); break
        case "duplicate": backend.renderQueue.duplicateJob(id); break
        case "up": backend.renderQueue.moveJob(id, -1); break
        case "down": backend.renderQueue.moveJob(id, 1); break
        case "open": backend.openRenderOutput(id); break
        case "reveal": backend.revealRenderOutput(id); break
        }
    }
    onPresetSaveRequested: function (name, spec) { backend.renderPresets.savePreset(name, spec) }
    onPresetUpdateRequested: function (id, spec) { backend.renderPresets.updatePreset(id, spec) }
    onPresetDuplicateRequested: function (id) { backend.renderPresets.duplicatePreset(id) }
    onPresetRenameRequested: function (id, name) { backend.renderPresets.renamePreset(id, name) }
    onPresetDeleteRequested: function (id) { backend.renderPresets.deletePreset(id) }
    onPresetFavoriteRequested: function (id, on) { backend.renderPresets.setFavorite(id, on) }
    onPresetExportRequested: function (id) { backend.renderPresets.exportPresetDialog(id) }
    onPresetImportRequested: backend.renderPresets.importPresetsDialog()
    onFolderRequested: backend.chooseRenderFolder()
    onRangeEdited: function (a, b) { win.timelineRangeIn = a; win.timelineRangeOut = b }
    onRangeCleared: { win.timelineRangeIn = -1; win.timelineRangeOut = -1 }
    onSeekRequested: function (f) { if (backend.viewingSource) backend.viewTimeline(); backend.seek(f) }
    onPlayToggled: Ui.Commands.trigger("transport.play")
    onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
    onStartRequested: Ui.Commands.trigger("transport.start")
    onEndRequested: Ui.Commands.trigger("transport.end")

    Connections {
        target: backend
        function onRenderFolderChosen(path) { page.setFolder(path) }
        // The checks follow the timeline's size and timeline (a reframed copy
        // made from the shape line) and Reframe to Fit working on it.
        function onVideoChanged() { page.refresh() }
        function onSequencesChanged() { page.refresh() }
        function onReframingChanged() { page.refresh() }
    }
    Component.onCompleted: backend.hardware.probeEncoders()
}
