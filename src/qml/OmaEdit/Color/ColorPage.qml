import QtQuick
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Color

// The seam: the Color workspace bound to the engine. The workspace knows
// nothing of `backend` or `win`; everything it asks for is answered here.
ColorWorkspace {
    id: page

    onDecorationWindowChanged: function(first,last,pixels) { backend.setTimelineDecorationWindow(first,last,pixels) }
    gradeClip: backend.gradeClip
    colourConfig: backend.colourPipelineInfo()
    versionsEditable: !backend.exporting && hasClip && gradeClip.locked !== true
    hasCopiedNode: backend.hasCopiedNode
    hasCopiedGrade: backend.hasCopiedGrade
    clips: backend.colorClips
    stills: backend.stills.items
    albums: backend.stills.albums
    maskProviderOffered:backend.maskProviderOffered
    maskProvider:backend.maskProvider
    maskJob:backend.maskJob
    onMaskConfigured:function(settings){backend.maskProvider=settings}
    onMaskRequested:function(kind,prompt){page.flushGrade();backend.generateColorMask(page.gradeClip.track,page.gradeClip.index,page.nodePath,page.clipFrame,kind,prompt)}
    onMaskCancelled:backend.cancelColorMask()
    onColourSourceApplied:function(scope,fields){page.flushGrade();backend.applyColourSource(page.gradeClip.track,page.gradeClip.index,scope,fields)}
    colorGroups: backend.colorGroups
    onShareRequested:function(action,name,addresses){page.flushGrade();backend.shareColor(page.gradeClip.track,page.gradeClip.index,action,name,addresses)}
    nodePresets: backend.nodePresets
    onSaveNodePresetRequested: function(name) { backend.saveNodePreset(name,page.node) }
    onRemoveNodePresetRequested: function(id) { backend.removeNodePreset(id) }
    onApplyNodePresetRequested: function(id) { page.flushGrade();backend.applyNodePreset(id,page.gradeClip.track,page.gradeClip.index,page.nodeIndex,page.editPath.join("/")) }
    luts: backend.lutLibrary.luts
    lutPreview: backend.lutPreview
    scopeTick: backend.scopeTick
    trackJob: backend.trackJob
    hasVideo: backend.hasVideo
    fps: backend.fps > 0 ? backend.fps : 25
    duration: backend.duration
    position: backend.position
    playing: backend.playing && !backend.viewingSource
    sequenceName: backend.sequences.length > backend.activeSequence && backend.activeSequence >= 0
                  ? backend.sequences[backend.activeSequence].name : ""
    sequenceIndex: backend.activeSequence
    colorSpace: backend.colorSpace
    imageAspect: backend.videoWidth > 0 && backend.videoHeight > 0 ? backend.videoDisplayAspect : 16 / 9
    videoSurface: Component {
        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
            source: backend
            channel: page.highlight ? VideoView.ColorMatte : VideoView.Program
            colorOptions: page.videoColorOptions
        }
    }
    function updateMatte() {
        if (page.visible && page.highlight && page.hasClip)
            backend.requestColorMatte(page.gradeClip.track, page.gradeClip.index, page.nodePath, page.clipFrame)
        else backend.clearColorMatte()
    }
    Timer { id: matteDelay; interval: 100; onTriggered: page.updateMatte() }
    Connections {
        target: page
        function onPositionChanged() { if (page.highlight && !matteDelay.running) matteDelay.start() }
        function onNodeChanged() { if (page.highlight) matteDelay.restart() }
        function onHighlightChanged() { page.updateMatte() }
        function onVisibleChanged() { page.updateMatte() }
    }
    // The "before" half of a wipe or split: the engine's comparison frame,
    // every attribute (the grade included) switched off.
    referenceSurface: Component { VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;source:backend;channel:VideoView.Still;stillId:page.referenceStillId;colorOptions:page.referenceColorOptions} }
    beforeSurface: Component {
        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
            source: backend
            channel: VideoView.Before
            colorOptions: page.beforeColorOptions
        }
    }
    // Made again when the starting timecode or the rate changes, so every
    // timecode already drawn with it is drawn again.
    timecoder: backend.startFrame >= 0 && backend.fps >= 0 ? function (frame) { return backend.timecode(frame) } : null
    comparison: win.colorComparison
    highlight: win.colorHighlight

    // Send pointer updates directly into the ordered worker queue. It combines
    // pending positions without letting a GUI timer run after Save or a switch.
    property var gradeGesture: null
    function gradeTargetMatches() {
        return page.hasClip && gradeGesture && gradeGesture.sequence === backend.activeSequence
            && gradeGesture.track === page.gradeClip.track && gradeGesture.index === page.gradeClip.index
            && gradeGesture.path === page.gradeClip.path
    }
    function flushGrade() {
        if (gradeGesture) backend.endEditGesture()
        gradeGesture = null
    }
    onGradeEdited: function (g) {
        if (!page.hasClip) return
        // A gesture left from another clip is over: this edit starts its own,
        // on the clip it is made on, and is not dropped.
        if (gradeGesture && !gradeTargetMatches()) flushGrade()
        if (!gradeGesture) {
            gradeGesture = {sequence:backend.activeSequence, track:page.gradeClip.track,
                            index:page.gradeClip.index, path:page.gradeClip.path}
            backend.beginEditGesture()
        }
        if (gradeTargetMatches()) backend.setGrade(page.gradeClip.track, page.gradeClip.index, g)
    }
    onGradeCommitted: function (g) {
        if (page.hasClip && (!gradeGesture || gradeTargetMatches()))
            backend.setGrade(page.gradeClip.track, page.gradeClip.index, g)
        flushGrade()
    }
    onGradeGestureEnded: flushGrade()
    onVersionsEdited: function (v, active) { if (page.hasClip) backend.setGradeVersions(page.gradeClip.track, page.gradeClip.index, v, active) }
    onVersionActionRequested: function (action, version, name) {
        if (!page.versionsEditable) return
        page.flushGrade()
        backend.editGradeVersion(page.gradeClip.track, page.gradeClip.index, action, version, name)
    }
    onCopyColorRequested: function (kind) { Ui.Commands.trigger(kind === "node" ? "color.copy-node" : "color.copy-grade") }
    function copyColor(kind) {
        if (page.hasClip) backend.copyColor(kind, kind === "node" ? page.graph : page.grade, page.nodeIndex)
    }
    onPasteColorRequested: function (kind) { Ui.Commands.trigger(kind === "node" ? "color.paste-node" : "color.paste-grade") }
    function pasteColor(kind) {
        if (!page.versionsEditable) return
        page.flushGrade()
        if(kind==="grade")page.editPath=[]
        backend.pasteColor(page.gradeClip.track, page.gradeClip.index, kind, page.nodeIndex, kind === "node" ? page.editPath.join("/") : "")
    }
    onAutoBalanceRequested: {
        if (!page.versionsEditable) return
        page.flushGrade()
        backend.autoBalance(page.gradeClip.track, page.gradeClip.index, page.clipFrame)
    }
    Connections {
        target: backend
        function onColorPasted(track, index, node) {
            if (page.gradeClip.track === track && page.gradeClip.index === index) page.currentNode = node
        }
        function onAutoBalanceFinished(track, index, node) {
            if (node >= 0 && page.gradeClip.track === track && page.gradeClip.index === index) {page.editPath=[];page.currentNode = node}
        }
        function onColorAssetChosen(action,path) { page.chooseColorAsset(action,path) }
        function onLutFileChosen(path) { page.chooseLut(path) }
        function onLutApplyFinished(changed, message) { page.finishLutApply(changed, message) }
    }
    
    
    
    
    
    
    
    onBypassToggled: function (on) { if (page.hasClip) backend.setBypass(page.gradeClip.track, page.gradeClip.index, "grade", on) }
    onComparisonEdited: function (m) {
        win.colorComparison = m
        backend.compareMode = m === "wipe" || m === "split"
    }
    onHighlightToggled: function (on) { win.colorHighlight = on }
    onSeekRequested: function (f) { if (backend.viewingSource) backend.viewTimeline(); backend.seek(f) }
    onPlayToggled: Ui.Commands.trigger("transport.play")
    onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
    onStepRequested: function(n) { Ui.Commands.trigger(n < 0 ? "transport.previous-frame" : "transport.next-frame") }
    onGrabRequested: function (name, album) { backend.grabGradeStill(name, album) }
    onStillApplied: function (id, addrs) { backend.applyStill(id, addrs) }
    onStillRenamed: function (id, n) { backend.stills.rename(id, n) }
    onStillMoved: function (id, a) { backend.stills.setAlbum(id, a) }
    onStillFavorited: function (id, on) { backend.stills.setFavorite(id, on) }
    onStillRemoved: function (id) { backend.stills.remove(id) }
    onTrackRequested: function(node,window,frame,direction,singleFrame) { if(page.hasClip) { page.flushGrade();backend.trackWindowRange(page.gradeClip.track,page.gradeClip.index,page.topNodeIndex,window,frame,direction,singleFrame,page.nodePath) } }
    onTrackStopped: function(keep) { backend.stopWindowTracking(keep) }
    onTrackMotionCopied: function(points) { if (page.hasClip) backend.copyTrackAsMotion(page.gradeClip.track, page.gradeClip.index, points) }
    onColorAssetRequested: function(action,id) { backend.chooseColorAsset(action,id || "") }
    onLutFolderRequested: backend.chooseLutFolder()
    onLutFileRequested: backend.chooseLutFile()
    onLutRefreshRequested: backend.lutLibrary.rescan()
    onLutFavoriteToggled: function (path, on) { backend.lutLibrary.setFavorite(path, on) }
    onLutPreviewRequested: function (path, intensity) {
        if (!page.hasClip) return
        page.flushGrade()
        backend.requestLutPreview(page.gradeClip.track, page.gradeClip.index, page.topNodeIndex, page.nodePath, page.clipFrame, path, intensity)
    }
    onLutApplyRequested: function (path, intensity) {
        if (!page.versionsEditable) return
        page.flushGrade()
        backend.setNodeLut(page.gradeClip.track, page.gradeClip.index, page.topNodeIndex, page.nodePath, path, intensity)
    }
    onLutPreviewClosed: backend.clearLutPreview()
    onScopesActiveEdited: function (on) { win.colorScopesWanted = on }
    onScopeOptionsEdited: function (o) { backend.scopeOptions = o }
    // The eyedropper: the pixel under the click, off the programme frame.
    onColorPickRequested: function (fx, fy, mods) {
        var hex = backend.colorAt(fx, fy)
        if (hex === "") return
        var mode = (mods & Qt.ControlModifier) !== 0 ? "add" : (mods & Qt.ShiftModifier) !== 0 ? "subtract" : page.pickMode
        page.pickedColor(hex, mode)
        if (mode === "set") page.picking = false
    }
    // Leaving the page: no wipe left on the Edit monitor, scopes asleep.
    // Arriving: its scope settings are the engine's again (the Edit page's
    // scopes may have changed them). Leaving: no wipe on the Edit monitor.
    onVisibleChanged: {
        if (visible) { page.pushScopeOptions(); return }
        page.flushGrade(); backend.compareMode = false; win.colorComparison = "none"
    }
}
