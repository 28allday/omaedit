import QtCore
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.App as App
import OmaEdit.Classic as Classic
import OmaEdit.Media as Media
import OmaEdit.Deliver as Deliver
import OmaEdit.Audio as Audio
import OmaEdit.Color as Color

ApplicationWindow {
    id: win
    // Controls that name no face of their own take the app's one.
    font.family: Ui.Theme.fontFamily
    // The window itself, for the parts below: each is handed the window as
    // its own `win`, and inside `Part { win: … }` the name `win` would be
    // that property, not this id.
    readonly property var thisWindow: win
    property alias keyboardNavigation: keyboardNavigation
    Ui.KeyboardNavigation { id: keyboardNavigation; window: win }
    width: 1440
    height: 900
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    // Frameless: AppTitleBar is the drag handle, the double-click maximise
    // target and the min/max/close controls, as the brief specifies.
    flags: Qt.Window | Qt.FramelessWindowHint
    title: qsTr("OmaEdit — %1%2").arg(backend.projectName).arg(backend.dirty ? " •" : "")

    // The active workspace. The application opens on Media (main.cpp sets it
    // before the window is made); a window loaded bare, as the tests do, starts on Edit.
    readonly property var activeLayout: workspace === "Edit" ? classicLayout.layoutController
        : workspace === "Media" ? (mediaLoader.item ? mediaLoader.item.layoutController : null)
        : workspace === "Color" ? (colorLoader.item ? colorLoader.item.layoutController : null)
        : workspace === "Audio" ? (audioLoader.item ? audioLoader.item.layoutController : null)
        : (deliverLoader.item ? deliverLoader.item.layoutController : null)
    property bool applicationFullscreen: false
    onVisibilityChanged: applicationFullscreen = win.visibility === Window.FullScreen
    property int beforeFullscreen: Window.Windowed
    function toggleApplicationFullscreen() {
        if(applicationFullscreen) { applicationFullscreen=false; visibility=beforeFullscreen }
        else {beforeFullscreen=visibility;applicationFullscreen=true;showFullScreen()}
    }
    function layoutTransport(action,value) {
        if (workspace === "Media" && mediaLoader.item) { mediaLoader.item.layoutTransport(action,value); return }
        if (workspace === "Audio" && audioLoader.item) { audioLoader.item.layoutTransport(action,value); return }
        if ((workspace === "Color" || workspace === "Deliver") && backend.viewingSource) backend.viewTimeline()
        if (action === "play") backend.togglePlay()
        else if (action === "pause") backend.pause()
        else if (action === "shuttle") backend.shuttle(value)
        else if (action === "step") backend.step(value)
        else if (action === "start") backend.seek(0)
        else if (action === "end") backend.seek(Math.max(0,(backend.viewingSource ? backend.sourceDuration : backend.duration)-1))
    }
    Connections {target:win.activeLayout;function onTransportRequested(action,value){win.layoutTransport(action,value)}function onSaveRequested(){backend.saveProject()}function onApplicationFullscreenRequested(){win.toggleApplicationFullscreen()}function onLayoutRequested(){win.requestActivate();workspaceLayoutDialog.open()}}
    property string workspace: "Edit"
    onWorkspaceChanged: backend.setTimelineDecorationWindow(0, 0, 0)
    property bool rulerFrames: false
    // View ▸ Waveforms From the Base: a sound's clip on the Edit timeline and on
    // the Audio page draws its waveform standing from the foot (the height
    // of its loudest side) and not about a centre line.
    property bool rectifiedWaveforms: false
    // View ▸ Show Beat Map: the beats and bars of mapped music as lines down
    // the tracks of the Edit timeline and the Audio page. Drags snap to the
    // beats while the lines are shown, and not while they are hidden.
    property bool showBeatMap: true
    onShowBeatMapChanged: snapping.forget()
    // Every beat on the timeline (backend.beatLines); empty with no mapped music on it.
    readonly property var beatLines: backend.beatLines
    readonly property bool hasBeatLines: beatLines.length > 0
    Settings {
        category: "shell"
        property alias rulerFrames: win.rulerFrames
        property alias rectifiedWaveforms: win.rectifiedWaveforms
        property alias showBeatMap: win.showBeatMap
        property alias snapping: win.snapEnabled
        property alias windowWidth: win.width
        property alias windowHeight: win.height
        property alias trackHeight: win.hLaneRow
        property alias multicamAudioFollows: win.multicamAudioFollows
        property alias density: win.density
        property alias editScopes: win.editScopes
    }
    // Preferences ▸ General ▸ Density: the Theme singleton follows this.
    property string density: "compact"
    // Preferences ▸ Input: how the pointer works over the timeline.
    Settings {
        category: "input"
        property alias wheelAction: win.wheelAction
        property alias wheelInvert: win.wheelInvert
        property alias wheelSensitivity: win.wheelSensitivity
        property alias doubleClickClip: win.doubleClickClip
    }
    property string wheelAction: "scroll"      // plain wheel over the timeline: "scroll" | "zoom" (Ctrl does the other)
    property bool wheelInvert: false           // natural / reversed wheel
    property real wheelSensitivity: 1.0        // 0.25–4
    property string doubleClickClip: "open"    // "open" a nested sequence or title | "precision" editor at the nearer edit | "select" only
    function resetInputPreferences() { wheelAction = "scroll"; wheelInvert = false; wheelSensitivity = 1.0; doubleClickClip = "open" }
    // Shared by the timeline's wheel handlers: scroll or zoom, per the preference and Ctrl.
    function timelineWheel(event, flick) {
        if ((event.modifiers & Qt.ShiftModifier) && flick.contentHeight > flick.height) {
            scrollTracks(event, flick)
            return
        }
        var sign = wheelInvert ? -1 : 1
        var zoom = (wheelAction === "zoom") !== ((event.modifiers & Qt.ControlModifier) !== 0)
        if (zoom) {
            if (event.angleDelta.y * sign > 0) navigation.zoomIn(); else if (event.angleDelta.y * sign < 0) navigation.zoomOut()
            return
        }
        var delta = (event.angleDelta.y + event.angleDelta.x) * sign * wheelSensitivity
        flick.contentX = Math.max(0, Math.min(flick.contentX - delta, Math.max(0, flick.contentWidth - flick.width)))
    }
    // Up and down the track stack. Some systems turn Shift+wheel into a
    // sideways turn, so whichever axis moved is the one that counts.
    function wheelTurn(event) { return event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x }
    function scrollTracks(event, flick) {
        flick.contentY = Math.max(0, Math.min(flick.contentY - wheelTurn(event) * wheelSensitivity,
                                              Math.max(0, flick.contentHeight - flick.height)))
    }
    onDensityChanged: Ui.Theme.density = density
    Component.onCompleted: { Ui.Theme.density = density; backend.enableTimelineCreationPrompt(); placeToolTips() }
    // The tip a stock control shows (a tick box, a slider, a spin box: every
    // `ToolTip.text` in the app is this one bubble) stands under its control,
    // from its left edge, where it used to stand over the control above.
    Item { id: toolTipHost }
    function placeToolTips() {
        const tip = toolTipHost.ToolTip.toolTip
        tip.x = 0
        tip.y = Qt.binding(function () { return tip.parent ? tip.parent.height + Ui.Theme.s1 : 0 })
    }

    // Multicam: the audio-follow-video choice persists; the viewer mode is
    // per session. New Multicam Clip… lives on the library menus, the Edit
    // menu and the Media page.
    property bool multicamAudioFollows: true
    Binding { target: backend; property: "multicamAudioFollows"; value: win.multicamAudioFollows }
    function openMulticamDialog(indices) { multicamDialog.openFor(indices || []) }
    function openSyncDialog(indices) { multicamDialog.openForSync(indices || []) }
    // The fx badge on a block: the Inspector's Effects tab for that clip.
    function showEffectsTab() { classicLayout.inspectorTab = 2 }
    function showClipInspector(tab) {
        workspace = "Edit"
        classicLayout.showInspector(tab)
    }
    // Which multicam clip an entry is a part of ("" for any other clip).
    function multicamIdOf(entry) {
        if (entry === undefined || entry === null || entry.mcam === undefined || entry.mcam === "")
            return ""
        var id = JSON.parse(entry.mcam).id
        return id === undefined ? "" : String(id)
    }
    // A multicam clip's block reads "⧉ <angle>" so the cut is legible.
    function mcamAngleName(entry) {
        if (entry === undefined || entry.mcam === undefined || entry.mcam === "")
            return entry !== undefined ? entry.name : ""
        var mc = JSON.parse(entry.mcam)
        var a = entry.mcamAngle !== undefined ? entry.mcamAngle : 0
        var angle = a < mc.angles.length ? mc.angles[a].name : entry.name
        // (Timeline::noFootagePath: a stretch the angle has no footage for.)
        return "⧉ " + (entry.path === "color:#ff000000" ? qsTr("%1 — no footage").arg(angle) : angle)
    }

    // Track height (View ▸ Taller / Shorter Tracks, Shift+wheel over the
    // headers, header menu ▸ Track Height): every track shares it, V1 too,
    // unless one was sized by itself. Persisted with the shell.
    property int hLaneRow: Ui.Theme.hLaneRow
    readonly property int hLaneClip: hLaneRow - (Ui.Theme.hLaneRow - Ui.Theme.hLaneClip)
    readonly property int hLaneRowMin: 28
    readonly property int hLaneRowMax: 120
    readonly property Classic.TrackLanes lanes: Classic.TrackLanes { win: thisWindow }
    readonly property bool trackSizedAlone: backend.tracks.some(function (t) { return !!(t.mix && t.mix.editHeight) })

    // True while the Media page owns the keyboard (a search, a note, a
    // timecode field); false whenever it is not loaded.
    property bool mediaTextActive: false
    property bool deliverTextActive: false
    property bool audioTextActive: false
    property bool colorTextActive: false
    // Color page view state that outlives the page: the comparison mode
    // (wipe / split drive the engine's compare frame) and the matte highlight.
    property string colorComparison: "none"
    property bool colorHighlight: false
    // Audio page state shared with the seam: where the timeline was parked
    // when an audition began, the role a new track should take, the ADR
    // cue a take is being recorded for and where to auto-stop, saved insert
    // chains.
    property int audioParkedPosition: 0
    property string pendingTrackRole: ""
    property int adrRecordingCue: -1
    property int adrStopAt: -1
    property var fxPresets: audioSettings.fxPresets
    onFxPresetsChanged: audioSettings.fxPresets = fxPresets
    Settings {
        id: audioSettings
        category: "audio"
        property var fxPresets: []
    }
    function grabStill() { backend.saveStill() }

    property bool forceClose: false
    property bool quitAfterSave: false
    property bool newAfterSave: false
    property bool closeAfterSave: false
    property bool openAfterSave: false
    property string pendingOpenPath: "" // "" = the open-file dialog
    property bool backgroundRender: false // hidden while the queue finishes
    onClosing: function (close) {
        if (backend.projectBusy && !win.forceClose) {
            close.accepted = false
            backend.announce(qsTr("Wait for the project operation to finish before quitting."))
            return
        }
        if (backend.renderQueue.active && !win.forceClose) {
            close.accepted = false
            renderGuardDialog.open()
            return
        }
        if (backend.dirty && !win.forceClose) {
            close.accepted = false
            quitDialog.open()
        }
        if(close.accepted) {
            for(let page of [classicLayout,mediaLoader.item,audioLoader.item,colorLoader.item,deliverLoader.item])
                if(page){page.layoutController.saveNow();page.layoutController.suspended=true}
        }
    }
    Connections {
        target: backend.renderQueue
        function onQueueDrained() {
            if (!win.backgroundRender) return
            win.backgroundRender = false
            win.show()
            win.close() // the unsaved-changes guard runs as usual
        }
    }

    // Palette: the brief's fixed tokens (Ui.Theme). Only the ACCENT follows
    // the live Omarchy theme — everything nearer the picture stays pinned, so
    // a grade reads the same whatever the desktop wears. The classic layout
    // still reads these names; they map onto Theme so the old body and the
    // new chrome are one surface until Phase 1 retires the names altogether.
    readonly property color accent: Ui.Theme.accent
    readonly property color accentForeground: Ui.Theme.accentText
    readonly property color pageColor: Ui.Theme.panelRaised     // main chrome
    readonly property color panelColor: Ui.Theme.panelBg        // side panels
    readonly property color wellColor: Ui.Theme.windowBg        // viewer/timeline wells
    readonly property color controlColor: Ui.Theme.controlBg    // button/input fills
    readonly property color controlHiColor: Ui.Theme.borderStrong // hover/pressed
    readonly property color seamColor: Ui.Theme.windowBg        // region edges
    readonly property color hairlineColor: Ui.Theme.border      // 1px lines/borders
    readonly property color insetColor: Ui.Theme.windowBg       // recessed readouts
    readonly property color textColor: Ui.Theme.textPrimary
    readonly property color textBrightColor: Ui.Theme.textPrimary
    readonly property color textDimColor: Ui.Theme.textMuted
    // One corner language, kept TINY by design: just enough curve to
    // soften the box, never enough to read as a pill.
    readonly property int rCard: 6   // cards, wells, empty states, popups
    readonly property int rClip: 3   // timeline media surfaces
    function rPill(h) { return Math.min(4, Math.round(h / 2)) } // controls, badges

    property int selectedLibraryIndex: -1
    property int selectedTimelineIndex: -1
    property alias selectedConnectedIndex: connectedSelection.index
    Ui.ClipSelection {
        id: connectedSelection
        entries: backend.connected
        scope: backend.activeSequence >= 0 && backend.activeSequence < backend.sequences.length
               ? String(backend.sequences[backend.activeSequence].id || "") : ""
        // Another timeline: its own clips, range and edit points, none of
        // this one's indexes or marks.
        onScopeChanged: {
            // Another timeline is fitted when it is first shown, unless the
            // editor has set a zoom of their own.
            win.fittedLength = 0
            if (win.scaleHeld)
                win.pxPerFrame = 0
            win.selection.clearSelection()
            win.timelineRangeIn = -1
            win.timelineRangeOut = -1
            win.precisionBoundary = -1
            win.videoAnimIndex = -1
        }
    }
    property int selectedEdge: -1 // 0 = in point, 1 = out point, -1 = whole clip
    // The edit point the selected edge names, as a timeline frame (−1 when
    // no edge is selected): the two-up trim view and Play Around use it.
    readonly property int trimEdgeFrame: {
        // (The clip as it is drawn: its cut, where a transition lies across it.)
        if (precisionBoundary >= 0) {
            var outgoing = selection.trackEntry(precisionTrack, precisionBoundary)
            if (outgoing === null) return -1
            var block = snapping.drawn(outgoing)
            return block.start + block.frames
        }
        var c = selection.selectedEntry()
        if (selectedEdge < 0 || c === null) return -1
        var drawn = snapping.drawn(c)
        return selectedEdge === 0 ? drawn.start : drawn.start + drawn.frames
    }
    onTrimEdgeFrameChanged: backend.twoUpEdge = trimEdgeFrame
    // ── Dynamic trim (Shift+T) ─────────────────────────────────────────
    // With an edge selected, JKL / Space play as usual and the edge is
    // trimmed to wherever playback STOPS. Play Around ("/") previews the
    // edit point (2 s either side) without trimming.
    property bool dynamicTrim: false
    property bool playingAround: false
    property bool startingPlayAround: false
    property int playAroundEnd: -1
    readonly property Classic.RangePlaying playing: Classic.RangePlaying { win: thisWindow }
    Connections {
        target: backend
        function onPositionChanged() {
            if (playingAround && backend.playing && backend.position >= playAroundEnd)
                backend.pause()
        }
        function onPlayingChanged() {
            if (backend.playing || startingPlayAround)
                return
            if (playingAround) { // a preview stop trims nothing
                playingAround = false
                return
            }
            if (dynamicTrim && selectedEdge >= 0 && trimEdgeFrame >= 0 && !backend.viewingSource)
                trimming.extendEditToPlayhead()
        }
    }
    // ── Multi-selection ───────────────────────────────────────────────
    // The primary (the two indices above) keeps the Inspector, the monitor
    // overlay and every edge op exactly as before; `extraSelection` is the
    // rest of the set, as {track, index} engine addresses (V1 = track 0).
    // Ctrl-click toggles a clip, Shift-click ranges along a track from the
    // primary, a marquee on empty timeline space sweeps, Ctrl+A takes
    // everything, the Track Select tool takes a clip and everything after
    // it. Group ops (Delete/Backspace, nudge, drag, V, labels, copy/paste)
    // go through the backend's *Clips invokables — one undo step each.
    // Setting the primary directly (every single-click path) drops the
    // extras unless `holdExtras` is up.
    property var extraSelection: []
    // A clicked gap — empty space between clips on one track, as
    // {track, start, frames} — or null. Clips and gaps never share a
    // selection: choosing one clears the other.
    property var selectedGap: null
    readonly property Classic.ClipSelecting selection: Classic.ClipSelecting { win: thisWindow; connected: connectedSelection }
    property bool holdExtras: false
    readonly property bool hasPrimary: selectedTimelineIndex >= 0
                                       || selectedConnectedIndex >= 0
    readonly property int selectionCount: (hasPrimary ? 1 : 0) + extraSelection.length
    // Linked selection (Ctrl+Shift+L, the chain button): a clip and the
    // audio detached from it carry the same `link` id and select together.
    // Alt-click picks one side only (the temporary override).
    property bool linkedSelection: true
    onSelectedTimelineIndexChanged: {
        v1Identity = selectedTimelineIndex >= 0 && selectedTimelineIndex < backend.timeline.length
                     ? backend.timeline[selectedTimelineIndex] : null
        if (v1Identity !== null)
            v1Identity = { path: v1Identity.path, "in": v1Identity["in"], out: v1Identity.out,
                           title: v1Identity.title || "", gap: v1Identity.gap === true, start: v1Identity.start,
                           camera: multicamIdOf(v1Identity) }
        selection.primaryMoved()
    }
    // The V1 primary is an index; this is the clip it named, so an edit
    // that adds or removes V1 clips before it keeps the same clip selected.
    property var v1Identity: null
    property int v1Count: backend.timeline.length
    onSelectedConnectedIndexChanged: selection.primaryMoved()
    // A V1 drag-reorder sets the primary before the engine answers: the
    // linked mate is looked up once the new entries are in.
    property bool relinkPending: false
    property var reorderIdentity: null // a V1 drag-reorder's clip, until its entries arrive
    // What each extra IS, taken when the set changes, so an edit that
    // shifts clip indices (a blade, an insert, a single ripple delete)
    // re-finds the same clips rather than whatever now holds the address.
    property var extraIdentity: []
    property var trackClipCounts: ({})
    onExtraSelectionChanged: {
        extraIdentity = selection.identitiesOf(extraSelection)
    }
    // Any edit can retire an address: keep the set honest.
    Connections {
        target: backend
        function onMarkersChanged() { snapping.forget() }
        function onBeatMapsChanged() { snapping.forget() }
        function onTimelineEntriesChanged() {
            snapping.forget() // what a drag snaps to is read again from the new entries
            selectedGap = null // the space it marked has moved or filled
            // Resolve the primary before excluding it from the other selections,
            // regardless of which property binding received this notification first.
            connectedSelection.reconcile()
            selection.refindV1Primary()
            var counts = selection.clipCountsByTrack()
            var shifted = false
            for (var t in counts)
                if (counts[t] !== trackClipCounts[t])
                    shifted = true
            for (var u in trackClipCounts)
                if (counts[u] === undefined)
                    shifted = true
            trackClipCounts = counts
            if (relinkPending) {
                relinkPending = false
                if (reorderIdentity !== null) {
                    const id = reorderIdentity
                    reorderIdentity = null
                    let best = -1
                    for (let i = 0; i < backend.timeline.length; ++i) {
                        const e = backend.timeline[i]
                        if (e.path === id.path && e["in"] === id["in"] && e.out === id.out && (e.title || "") === id.title
                                && (best < 0 || Math.abs(i - id.target) < Math.abs(best - id.target)))
                            best = i
                    }
                    if (best >= 0) {
                        holdExtras = true
                        selectedTimelineIndex = best
                        holdExtras = false
                    }
                }
                extraSelection = linkedSelection ? selection.linkMates(selection.primaryAddress()) : []
                return
            }
            if (extraSelection.length === 0)
                return
            var lookup = selection.entryLookup() // both lists read once for the whole set
            var all = lookup.all
            var byId = Object.create(null)
            var lists = [backend.timeline, backend.connected]
            for (var group = 0; group < lists.length; ++group) {
                var rows = lists[group]
                for (var n = 0; n < rows.length; ++n) {
                    var entry = rows[n]
                    if (!entry.clipId) continue
                    var key = String(entry.clipId)
                    byId[key] = byId[key] === undefined ? { track: entry.track, index: entry.index } : null
                }
            }
            var keep = []
            for (var i = 0; i < extraSelection.length; i++) {
                var a = extraSelection[i]
                var id = i < extraIdentity.length ? extraIdentity[i] : null
                if (id !== null && id.clipId) {
                    var exact = byId[id.clipId]
                    if (exact && !selection.sameAddress(exact, selection.primaryAddress())) keep.push(exact)
                    continue
                }
                if (!shifted || id === null) {
                    if (lookup.entry(a.track, a.index) !== null)
                        keep.push(a)
                    continue
                }
                // Indices moved on some track: this extra is the clip that
                // still matches its identity, wherever that sits now.
                var found = null
                for (var k = 0; k < all.length && found === null; k++) {
                    if (all[k].track !== id.track)
                        continue
                    var e = lookup.entry(all[k].track, all[k].index)
                    if (e !== null && e.start === id.start && e.path === id.path
                            && e.in === id.in && e.title === id.title)
                        found = all[k]
                }
                if (found !== null && !selection.sameAddress(found, selection.primaryAddress()))
                    keep.push(found)
            }
            if (!hasPrimary && keep.length > 0) {
                var next = keep.shift()
                holdExtras = true
                selection.setPrimary(next.track, next.index)
                holdExtras = false
            }
            var same = keep.length === extraSelection.length
            for (var j = 0; j < keep.length && same; j++)
                same = selection.sameAddress(keep[j], extraSelection[j])
            if (!same)
                extraSelection = keep
            else
                extraIdentity = selection.identitiesOf(keep) // adopt IDs when a legacy track becomes precise
        }
    }
    // Live group drag (a lane clip of a multi-selection): every other
    // selected lane block rides the same pixel offset — and, for blocks of
    // the dragged clip's kind, the same row shift.
    property bool groupDragActive: false
    property real groupDragPx: 0
    property int groupDragRows: 0
    property bool groupDragAudio: false
    // The clip clipboard (Ctrl+C): a copy of one entry map, storyline or
    // track clip. Paste places it through backend.pasteClip. Never saved.
    property var clipboard: null
    property var clipboardClock: ({})
    property int clipboardPasteRequest: 0
    readonly property bool hasClipboard: clipboard !== null
    property bool skimEnabled: true
    // N, View ▸ Snapping, the magnets on the Edit and Audio toolbars: one
    // choice for both pages, kept between sessions.
    property bool snapEnabled: true // N — drags snap to edits/playhead/markers
    // Timeline range selection (I/O in program view, X = clip under
    // playhead); transient — never saved.
    property int timelineRangeIn: -1
    property int timelineRangeOut: -1
    onTimelineRangeInChanged: backend.setLoopRange(timelineRangeIn, timelineRangeOut)
    onTimelineRangeOutChanged: backend.setLoopRange(timelineRangeIn, timelineRangeOut)
    readonly property bool hasTimelineRange: timelineRangeIn >= 0
                                             || timelineRangeOut >= 0
    readonly property int effRangeIn: timelineRangeIn >= 0 ? timelineRangeIn : 0
    readonly property int effRangeOut: timelineRangeOut >= 0
                                       ? timelineRangeOut : backend.duration - 1
    property real pxPerFrame: 0 // timeline zoom; 0 = fit to width
    // The timeline's scale is the editor's. Fit lays the timeline across the
    // view; after that an edit that makes it longer or shorter leaves the
    // scale as it was, where it used to squeeze or stretch every clip to fill
    // the view again. Fit Timeline (Shift+Z) lays it out afresh; a timeline
    // just opened, and the first clip on an empty one, are fitted.
    property int fittedLength: 0 // the length the fit on screen was made for; 0: none yet
    property bool fitFollowsLength: false // the old way: fitted again after every edit
    // The zoom in force is a fit that an edit left standing, not one the
    // editor set: another timeline shown is fitted in its turn.
    property bool scaleHeld: false
    property bool holdingScale: false
    onPxPerFrameChanged: {
        if (!holdingScale)
            scaleHeld = false
        if (pxPerFrame <= 0)
            fittedLength = backend.duration
    }
    function holdTimelineScale() {
        if (pxPerFrame > 0)
            return
        var length = backend.duration
        if (!fitFollowsLength && fittedLength > 0 && length > 0 && length !== fittedLength
                && timelineScroll && timelineScroll.width > 0) {
            holdingScale = true
            pxPerFrame = timelineScroll.width / fittedLength // as drawn, even past the zoom's own ceiling
            holdingScale = false
            scaleHeld = true
        } else {
            fittedLength = length
        }
    }
    Connections {
        target: backend
        function onVideoChanged() { win.holdTimelineScale() }
    }
    // The active timeline tool. Trim is the engine's ripple-trim flag (T);
    // the rest are pure UI modes over the same blocks: select, blade (click
    // a clip to cut it there), slip (drag a clip's content), hand (drag the
    // view), zoom (click in / Alt-click out). Slide is not built.
    property string toolMode: "select"
    readonly property string tool: backend.rippleTrim ? "trim" : toolMode
    function setTool(name) {
        if (name === "trim") {
            backend.rippleTrim = true
            return
        }
        backend.rippleTrim = false
        toolMode = name
    }
    // Two-up monitors: source viewer beside the program monitor. Off =
    // the single mode-switching viewer.
    property bool dualViewer: false
    // View ▸ Viewer ▸ Video Scopes: scopes beside the Edit page's viewer.
    property bool editScopes: false
    // Each page that shows scopes asks for them; the engine draws them while
    // either does, so leaving one page cannot put the other's to sleep.
    property bool colorScopesWanted: false
    property bool editScopesWanted: false
    Binding { target: backend; property: "scopesActive"; value: win.colorScopesWanted || win.editScopesWanted }
    // Connected lanes, split by kind: video lanes stack ABOVE the storyline,
    // audio lanes (audio-only connects, detached audio) hang BELOW.
    // Engine lanes stay homogeneous (pickLane), so each engine
    // lane maps to one display row of its kind; legacy mixed lanes still
    // render sanely because classification is per clip.
    // Rows come from the track list itself (Phase C), so a user-added
    // empty track has a row; a track's kind is the header's kind. Entry
    // lane numbers == track indices.
    readonly property var laneKinds: {
        var video = [], audio = []
        for (var i = 1; i < backend.tracks.length; i++) {
            var track = backend.tracks[i]
            if (track.kind === "audio")
                audio.push(track.index)
            else
                video.push(track.index)
        }
        return { video: video, audio: audio }
    }
    readonly property int videoLaneRows: laneKinds.video.length
    readonly property int audioLaneRows: laneKinds.audio.length
    readonly property int laneRows: videoLaneRows + audioLaneRows
    // Timeline index (Ctrl+Shift+2): the searchable clip/tag list to
    // the left of the timeline.
    property bool showTimelineIndex: false
    // Effects live in the media panel's Effects / Transitions / Titles tabs
    // (top left). Ctrl+5 flips that panel to Effects and back to Project.
    function showEffects(on) { classicLayout.browserTab = on ? 1 : 0 }
    // A media-panel tile is being dragged (clip, effect, transition or
    // title): the timeline opens its empty new-track rows meanwhile.
    readonly property bool mediaDragActive: classicLayout.mediaDragActive
    // App-context shortcuts fire even while a TextField has focus, so every
    // one of them stands down while a text-entry dialog is open — or while
    // a persistent search field is being typed into.
    readonly property bool keysFree: !keyboardNavigation.textFocused && !Ui.Commands.popupOpen && !classicLayout.titleTextActive && !titleDialog.visible
                                     && !classicLayout.sequenceRenameOpen
                                     && !multicamDialog.visible
                                     && !markerDialog.visible
                                     && !projectSettingsDialog.visible
                                     && !preferencesDialog.visible
                                     && !quitDialog.visible && !openDialog.visible && !newDialog.visible
                                     && !closeProjectDialog.visible && !missingProjectDialog.visible && !viewerPlaybackDialog.visible
                                     && !renderGuardDialog.visible
                                     && !pasteAttributesDialog.visible && !savePresetDialog.visible
                                     && !(win.activeLayout && win.activeLayout.detachedWindowActive)
                                     && !workspaceLayoutDialog.visible
                                     && !shortcutEditor.visible && !commandPalette.visible && !backgroundTasksDialog.activeFocus && !statusBar.infoControlsFocused
                                     && !projectSafetyDialog.visible
                                     && !indexSearch.activeFocus
                                     && !classicLayout.mediaSearchActive
                                     && !classicLayout.transcriptSearchActive
                                     && !mediaTextActive
                                     && !deliverTextActive
                                     && !audioTextActive
                                     && !colorTextActive

    // ── Clip markers ──────────────────────────────────────────────────
    readonly property Classic.ClipMarkers clipMarkers: Classic.ClipMarkers { win: thisWindow; dialog: markerDialog }

    readonly property Classic.ProjectGuards project: Classic.ProjectGuards { win: thisWindow; openPrompt: openDialog; closePrompt: closeProjectDialog; creationDialog: projectCreationDialog; newPrompt: newDialog }



    readonly property Classic.TimelineRanges ranges: Classic.TimelineRanges { win: thisWindow }

    // Precision editor (guide p. 548): expanded view of ONE edit point —
    // the boundary after storyline clip `precisionBoundary`.
    property int precisionBoundary: -1
    // Which track the open edit point is on (0 = V1). Entries on other
    // tracks are addressed by their index ON that track.
    property int precisionTrack: 0
    // Video Animation (Ctrl+V, guide p. 271's video twin): the lane
    // clip currently showing its OPACITY line instead of its volume line.
    property int videoAnimIndex: -1
    // An edit point on a track is usable when the pair touches and no
    // dissolve owns it (drag the wash instead).
    function precisionUsable(track, boundary) {
        var a = selection.trackEntry(track, boundary), b = selection.trackEntry(track, boundary + 1)
        // (A transition across the cut rolls and trims with it; one that
        // shares the pair's frames, an overlap, has no cut to show.)
        if (a === null || b === null || (a.mixOut > 0 && !snapping.acrossCut(a)))
            return false
        return track === 0 || b.start === a.start + a.frames
    }
    function openPrecision(boundary, track) {
        if (track === undefined)
            track = 0
        if (!precisionUsable(track, boundary))
            return
        backend.viewTimeline()
        backend.pause()
        precisionTrack = track
        precisionBoundary = boundary
    }
    function stepPrecision(direction) { // ↑/↓ walk edit points while open
        var next = precisionBoundary + direction
        var count = selection.trackClipCount(precisionTrack)
        while (next >= 0 && next + 1 < count
               && !precisionUsable(precisionTrack, next))
            next += direction
        if (next >= 0 && next + 1 < count)
            precisionBoundary = next
    }

    readonly property Classic.SelectedClipCommands clipCommands: Classic.SelectedClipCommands { win: thisWindow; layout: classicLayout; attributesDialog: pasteAttributesDialog }

    // ── The effect catalogue ────────────────────────────────────────────
    readonly property Classic.EffectCatalogue catalogue: Classic.EffectCatalogue { win: thisWindow }
    // A catalogue tile dropped on the timeline. Titles land at the frame
    // (the engine picks the lane); effects apply to the clip under the
    // pointer; transitions go on the clip's nearer edge.
    function dropCatalogueAt(kind, key, track, frame) {
        backend.viewTimeline()
        if (kind === "title" && key.indexOf("gen:") === 0) {
            backend.addGeneratorAt(key.substring(4), frame)
            return
        }
        if (kind === "title") {
            var p = catalogue.titlePreset(key)
            backend.addTitleAt(p.name, frame,
                               { "place": p.place, "size": p.size, "font": "", "color": "" })
            return
        }
        var hit = selection.clipAt(track, frame)
        if (hit === null)
            return
        selectedEdge = -1
        selectedTimelineIndex = hit.timelineIndex
        selectedConnectedIndex = hit.connectedIndex
        if (kind !== "transition" || key === "fade-in" || key === "fade-out") {
            catalogue.apply(key)
            return
        }
        var style = key === "dissolve" ? "" : key.substring(6)
        // The nearer cut, or the clip's other one when that end has no neighbour.
        var nearerIn = frame < hit.entry.start + hit.entry.frames / 2
        var cuts = selection.trackClipCount(hit.entry.track) - 1
        var target = nearerIn ? hit.entry.index - 1 : hit.entry.index
        if (target < 0 || target >= cuts)
            target = nearerIn ? hit.entry.index : hit.entry.index - 1
        if (target < 0 || target >= cuts) {
            backend.announce(qsTr("A transition goes between two clips on the same track"))
            return
        }
        // Select the clip that holds the dissolve (the one before the cut),
        // so Ctrl+T or the inspector act on this transition.
        if (hit.entry.track === 0) {
            selectedTimelineIndex = target
        } else {
            for (var j = 0; j < backend.connected.length; j++)
                if (backend.connected[j].track === hit.entry.track && backend.connected[j].index === target)
                    selectedConnectedIndex = j
        }
        backend.applyTransitionAt(hit.entry.track, target, style)
    }
    readonly property Classic.EdgeTrimming trimming: Classic.EdgeTrimming { win: thisWindow }

    readonly property Classic.ClipCopying copying: Classic.ClipCopying { win: thisWindow }
    // ── Paste Attributes / presets ─────────────────────────────────────
    readonly property Classic.ClipAttributes attributes: Classic.ClipAttributes { win: thisWindow; pasteDialog: pasteAttributesDialog; presetDialog: savePresetDialog }
    // (An entry map has a `length` of its own — the clip's source length —
    // so only a real array counts as a set.)
    readonly property bool clipboardIsSet: clipboard !== null
                                           && Array.isArray(clipboard)
    Connections {
        target: backend
        function onClipboardPasteFinished(request, changed, error, added) {
            if (request !== win.clipboardPasteRequest) return
            win.clipboardPasteRequest = 0
            if (!changed) return
            var addresses = []
            for (var i = 0; i < added.length; ++i) {
                if (added[i].track !== undefined && added[i].index !== undefined)
                    addresses.push({track: added[i].track, index: added[i].index})
            }
            win.selection.selectAddresses(addresses)
        }
    }
    readonly property Classic.SourceMatching matching: Classic.SourceMatching { win: thisWindow }
    // ── Linked audio (linked tracks) ────────────────────────────
    // V1 clips still carrying their sound inside the block (a project
    // saved before linked audio, until its first edit; a nested sequence).
    readonly property int embeddedAudioCount: {
        var n = 0
        for (var i = 0; i < backend.timeline.length; i++) {
            var c = backend.timeline[i]
            if (c.gap !== true && c.videoOnly !== true && c.silent !== true && (c.title === undefined || c.title === ""))
                n++
        }
        return n
    }
    // ── Nested sequences / takes ───────────────────────────────────────
    // The chroma key's eyedropper, armed: { track, index, uid, clipFrame } of
    // the keyer whose backdrop the next click on the program picture picks.
    property var keyPick: null
    // The effect whose mask is shown on the program picture for placing ("" = none).
    property string maskEditUid: ""
    function finishKeyPick(x, y) {
        const pick = keyPick
        keyPick = null
        if (pick) backend.editVideoEffect(pick.track, pick.index, "pickBackdrop", pick.uid, {clipFrame: pick.clipFrame, x: x, y: y})
    }
    // ── Categories ─────────────────────────────────────────────────────
    readonly property Classic.ClipCategories categories: Classic.ClipCategories { win: thisWindow }

    // ── Menu bar commands ──────────────────────────────────────────────
    function copyPlayheadTimecode() {
        var text = backend.timecode(backend.position)
        backend.copyText(text)
        backend.announce(qsTr("Copied %1").arg(text))
    }
    function findInTimeline() {
        showTimelineIndex = true
        Qt.callLater(function () { indexSearch.forceActiveFocus(); indexSearch.selectAll() })
    }
    property bool modifyAddedMarker: false
    Connections {
        target: backend
        function onMarkersChanged() { win.clipMarkers.openAdded() }
        function onTimelineEntriesChanged() { win.clipMarkers.openAdded() }
    }
    // Deliver's answer when the edit is not the delivery's shape: a copy of
    // the timeline at width × height, the active one from then on, its clips
    // reframed once the copy has taken its new size.
    property var pendingReframe: null
    function reshapedCopy(width, height) {
        if (!backend.hasVideo || backend.reframing || width <= 0 || height <= 0) return false
        const d = {}
        const s = backend.projectSettings
        for (const k in s) d[k] = s[k]
        d.custom = true
        d.width = width
        d.height = height
        selection.clearSelection()
        backend.duplicateSequence(backend.activeSequence)
        backend.setProjectSettings(d)
        pendingReframe = { width: width, height: height, from: backend.activeSequence, tries: 0 }
        reframeWhenReshaped.start()
        return true
    }
    Timer {
        id: reframeWhenReshaped
        interval: 100
        repeat: true
        onTriggered: {
            const p = win.pendingReframe
            if (!p) { stop(); return }
            if (backend.activeSequence !== p.from && backend.videoWidth === p.width && backend.videoHeight === p.height) {
                stop()
                win.pendingReframe = null
                backend.reframeToFit(win.reframeToFitTargets())
            } else if (++p.tries > 100) {
                stop()
                win.pendingReframe = null
                console.warn("The " + p.width + "x" + p.height + " copy did not take its new size; Reframe to Fit was not started")
            }
        }
    }
    // Reframe to Fit: the selected clips, or with none selected every clip with
    // pictures (those already the timeline's shape are passed over).
    function reframeToFitTargets() {
        const chosen = selection.selectedAddresses()
        if (chosen.length) return chosen.map(function (a) { return { track: a.track, index: a.index } })
        const all = []
        const add = function (e) { if (!e.gap && !e.title && e.audioOnly !== true) all.push({ track: e.track, index: e.index }) }
        for (let i = 0; i < backend.timeline.length; ++i) add(backend.timeline[i])
        for (let i = 0; i < backend.connected.length; ++i) add(backend.connected[i])
        return all
    }
    readonly property bool matchReferenceSet: backend.matchReference.track !== undefined
    readonly property Classic.SelectionSpeed retiming: Classic.SelectionSpeed { win: thisWindow }
    // Back and forward through the timelines opened this session.
    property var timelineHistory: []
    property int timelineHistoryAt: -1
    function noteTimelineOpened() {
        var active = backend.sequences[backend.activeSequence]
        if (active === undefined || timelineHistory[timelineHistoryAt] === active.id)
            return
        timelineHistory = timelineHistory.slice(0, timelineHistoryAt + 1).concat([active.id])
        timelineHistoryAt = timelineHistory.length - 1
    }
    function stepTimelineHistory(step) {
        for (var at = timelineHistoryAt + step; at >= 0 && at < timelineHistory.length; at += step) {
            var index = backend.sequenceIndexForId(timelineHistory[at])
            if (index >= 0) {
                timelineHistoryAt = at
                selection.clearSelection()
                backend.switchSequence(index)
                return
            }
        }
    }
    Connections {
        target: backend
        function onSequencesChanged() { win.noteTimelineOpened() }
    }
    function recordVoiceover() {
        if (backend.recordingTake) {
            backend.stopRecording()
            return
        }
        for (var t = 0; t < backend.tracks.length; t++) {
            if (backend.tracks[t].kind === "audio" && !backend.tracks[t].locked) {
                backend.startRecording(t, "Voiceover")
                return
            }
        }
        backend.announce(qsTr("Add an audio track to record a voiceover"))
    }
    function onMediaPage(action) {
        workspace = "Media"
        Qt.callLater(function () {
            if (mediaLoader.item !== null)
                action(mediaLoader.item)
        })
    }

    // ── Match Frame / Reveal Source / Fit to Fill ──────────────────────
    signal showLibraryTab() // the browser flips to Project




    readonly property Classic.SelectionSound sound: Classic.SelectionSound { win: thisWindow }



    readonly property Classic.TimelineSnapping snapping: Classic.TimelineSnapping { win: thisWindow }



    readonly property Classic.TimelineNavigation navigation: Classic.TimelineNavigation { win: thisWindow }

    Material.theme: Material.Dark // chrome is FIXED dark, omaphoto-style
    Material.accent: win.accent
    Material.background: win.pageColor
    Material.foreground: win.textColor
    color: pageColor

    function secondsLabel(frames, fps) {
        if (fps <= 0)
            return ""
        var s = Math.round(frames / fps)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }



    // Cross-file handles into the classic layout. The old code reaches
    // these by id, and ids stop at a file boundary — an unqualified name
    // that misses in a file still resolves against this window, so the
    // aliases keep every existing reference working unchanged.
    property alias storyline: classicLayout.storyline
    property alias ruler: classicLayout.ruler
    property alias timelineScroll: classicLayout.timelineScroll
    property alias indexSearch: classicLayout.indexSearch

    // ── Shell chrome ────────────────────────────────────────────────────────
    header: App.AppTitleBar {
        target: win
        projectName: backend.projectName
        dirty: backend.dirty
        canUndo: backend.canUndo
        canRedo: backend.canRedo
        workspace: win.workspace
        settingsAvailable: true
        onSettingsRequested: Ui.Commands.trigger("app.preferences")
        onWorkspaceActivated: function (name) { Ui.Commands.trigger("workspace." + name.toLowerCase()) }
        onSaveRequested: Ui.Commands.trigger("project.save")
        onUndoRequested: Ui.Commands.trigger("edit.undo")
        onRedoRequested: Ui.Commands.trigger("edit.redo")
        menuContent: Classic.MainMenuBar {}
        onMenuRequested: function (anchor) {
            narrowMenu.active = true
            narrowMenu.item.popup(anchor, 0, anchor.height)
        }
    }

    footer: App.StatusBar {
        id: statusBar
        tasks: backend.backgroundTasks
        onMessagesRequested: backgroundTasksDialog.openFor(true)
        proxyState: backend.mediaHealth.label || qsTr("No media")
        proxyReady: backend.mediaHealth.ready === true
        proxyDetails: backend.mediaHealth.message || ""
        cacheText: backend.backgroundRender.state === "completed" ? qsTr("Effects ready")
                   : backend.backgroundRender.state === "running" ? qsTr("Rendering effects")
                   : backend.backgroundRender.state === "paused" ? qsTr("Effects render paused")
                   : backend.backgroundRender.state === "failed" ? qsTr("Effects render failed")
                   : backend.backgroundRender.state === "partial" ? qsTr("Effects partly ready")
                   : backend.backgroundTasks.cache.text
        renderCache: backend.backgroundRender.state === "running" ? backend.backgroundRender.percent / 100
                     : backend.backgroundRender.state === "completed" ? -1
                     : backend.backgroundTasks.cache.percent < 0 ? -1 : backend.backgroundTasks.cache.percent / 100
        activeTasks: backend.backgroundTasks.activeCount
        rendering: backend.exporting
        diskLow: backend.volumes.diskLow
        onTasksRequested: Ui.Commands.trigger("view.tasks")
        onPlaybackRequested: Ui.Commands.trigger("view.playback")
        playbackText: backend.playing ? Math.round(Number(backend.playbackStats.displayedFps)) + " fps · " + backend.playbackStats.dropped + " dropped" : qsTr("Viewer")
        playbackWarning: backend.playing && backend.playbackStats.dropped > 0
        resolution: backend.hasVideo ? backend.videoWidth + "\u00d7" + backend.videoHeight : ""
        frameRate: backend.hasVideo ? win.fpsLabel(backend.fps) : ""
        aspect: backend.hasVideo ? win.aspectLabel(backend.videoWidth, backend.videoHeight) : ""
        colorSpace: backend.hasVideo ? backend.colorSpace : ""
        diskFree: backend.volumes.diskFree
        message: backend.status
    }

    function fpsLabel(fps) {
        var whole = Math.abs(fps - Math.round(fps)) < 0.001
        return (whole ? Math.round(fps) : fps.toFixed(3)) + " fps"
    }
    function aspectLabel(w, h) {
        if (w <= 0 || h <= 0) return ""
        var a = w, b = h
        while (b) { var t = b; b = a % b; a = t }
        return (w / a <= 32 && h / a <= 32) ? (w / a) + ":" + (h / a)
                                            : (w / h).toFixed(2) + ":1"
    }

    Loader {
        id: narrowMenu
        active: false
        sourceComponent: Classic.MainMenu { objectName: "mainMenu" }
    }

    Binding { target: Ui.Commands; property: "registry"; value: backend.commands }
    Classic.Shortcuts {}
    Classic.ShortcutEditor { id: shortcutEditor }
    Classic.CommandPalette { id: commandPalette }
    App.BackgroundTasks { id: backgroundTasksDialog }
    App.CloseProjectDialog { id: closeProjectDialog }
    App.MissingProjectDialog { id: missingProjectDialog }
    App.ViewerPlaybackDialog { id: viewerPlaybackDialog }

    // Context menus (right-click). Right-clicking selects first, so
    // every item just operates on the current selection through the
    // same invokables the menus use.
    Classic.ClipContextMenu { id: clipContextMenu; objectName: "clipContextMenu" }
    Classic.LaneContextMenu { id: laneContextMenu; objectName: "laneContextMenu" }
    Classic.GapContextMenu { id: gapContextMenu; objectName: "gapContextMenu" }
    Classic.LibraryContextMenu { id: libraryContextMenu; objectName: "libraryContextMenu" }
    Classic.BinContextMenu { id: binContextMenu; objectName: "binContextMenu" }
    Classic.RulerContextMenu { id: rulerContextMenu }

    Classic.TitleDialog { id: titleDialog }
    Classic.CaptionStyleDialog { id: captionStyleDialog }
    Classic.MulticamDialog { id: multicamDialog; objectName: "multicamDialog" }
    Classic.MarkerDialog { id: markerDialog; objectName: "markerDialog" }
    Classic.PasteAttributesDialog { id: pasteAttributesDialog; objectName: "pasteAttributesDialog" }
    Classic.VolumeDialog { id: volumeDialog; objectName: "volumeDialog" }
    Classic.ChangeDurationDialog { id: changeDurationDialog }
    Classic.CategoriesDialog { id: categoriesDialog; objectName: "categoriesDialog" }
    Classic.SavePresetDialog { id: savePresetDialog; objectName: "savePresetDialog" }
    Classic.NewProjectDialog { id: newDialog }
    Classic.ProjectCreationDialog { id: projectCreationDialog }
    Classic.SequenceCreationDialog { id: sequenceCreationDialog }
    Classic.ProjectSettingsDialog { id: projectSettingsDialog }
    Classic.PreferencesDialog { id: preferencesDialog }
    Classic.DiagnosticsDialog { id: diagnosticsDialog }
    Classic.SpeedDurationDialog { id: speedDurationDialog }
    Audio.AudioTimingDialog { id: audioTimingDialog; editor: backend }
    Classic.SpeedRampDialog { id: speedRampDialog }
    Classic.ProjectSafetyDialog { id: projectSafetyDialog }
    Classic.WorkspaceLayoutDialog { id: workspaceLayoutDialog }
    property bool recoveryStartupChecked: false
    Connections {
        target: backend
        function onSafetyChanged() {
            if (win.recoveryStartupChecked) return
            win.recoveryStartupChecked = true
            if (backend.recoveryCopies.length > 0) projectSafetyDialog.openFor(0)
        }
    }
    App.FileBrowser { owner: backend }
    Classic.OpenProjectDialog { id: openDialog }
    Classic.QuitDialog { id: quitDialog }
    Classic.RenderGuardDialog {
        id: renderGuardDialog
        onKeepRendering: { win.backgroundRender = true; win.hide() }
        onCancelAndQuit: {
            // The queue stays paused (its other jobs wait for next time); the
            // window closes once the cancelled render has really stopped.
            win.closeAfterRenderCancel = true
            backend.renderQueue.pauseAll()
            backend.renderQueue.cancelJob(backend.renderQueue.activeJob)
        }
    }
    property bool closeAfterRenderCancel: false
    Connections {
        target: backend.renderQueue
        function onJobFinished() {
            if (!win.closeAfterRenderCancel || backend.renderQueue.active) return
            win.closeAfterRenderCancel = false
            win.forceClose = !backend.dirty
            win.close() // the unsaved-changes guard runs as usual
        }
    }

    Connections {
        target: backend
        function onSequenceCreateFinished(request,created,error) { if (created) win.selection.clearSelection() }
        function onCompoundCreated(index) {
            win.timelineRangeIn = -1
            win.timelineRangeOut = -1
            win.selection.clearSelection()
            win.selection.setPrimary(0, index)
        }
        // The clip selection is not cleared here: it goes when the timeline
        // changes (connectedSelection's scope), which happens before the new
        // clips are shown. This arrives after them, and clearing here undid a
        // selection made in the first moment after a project opened.
        function onProjectPlaybackReset() { win.timelineRangeIn = -1; win.timelineRangeOut = -1; win.selectedLibraryIndex = -1 }
        function onRecentProjectMissing(path) { missingProjectDialog.openFor(path) }
        function onProjectSaveFinished(ok) {
            if (win.closeAfterSave && ok && !backend.dirty) backend.closeProject()
            if (win.quitAfterSave && ok && !backend.dirty) {
                win.forceClose = true
                win.close()
            }
            if (win.newAfterSave) {
                if (ok && !backend.dirty) win.project.commitNew()
                else projectCreationDialog.awaiting=false
            }
            if (win.openAfterSave && ok && !backend.dirty)
                win.project.proceedOpen(win.pendingOpenPath)
            win.quitAfterSave = false
            win.newAfterSave = false
            win.closeAfterSave = false
            win.openAfterSave = false
        }
        function onTimelineCreationRequested() { sequenceCreationDialog.openFor() }
        function onChooserCanceled() {
            if (win.newAfterSave) projectCreationDialog.awaiting=false
            win.quitAfterSave = false
            win.newAfterSave = false
            win.closeAfterSave = false
            win.openAfterSave = false
        }
        function onVideoChanged() {
            if (!backend.hasVideo) {
                win.timelineRangeIn = -1
                win.timelineRangeOut = -1
            }
        }
        function onTimelineEntriesChanged() {
            // The lists can shrink under a selection (project Discard, undo
            // of an append…) — a stale index throws TypeErrors from every
            // binding that dereferences it. Follow the clip first, whichever
            // handler hears the change first.
            win.selection.refindV1Primary()
            if (win.selectedTimelineIndex >= backend.timeline.length) {
                win.selectedTimelineIndex = -1
                win.selectedEdge = -1
            }
            // Connected selection follows its persistent cut ID; ClipSelection
            // also clears removed clips and keeps legacy index selection bounded.
            if (win.precisionBoundary >= 0
                    && win.selection.trackEntry(win.precisionTrack, win.precisionBoundary + 1) === null) // lanes too, not only V1
                win.precisionBoundary = -1
            if (win.videoAnimIndex >= backend.connected.length)
                win.videoAnimIndex = -1
        }
    }

    // ── Workspace host ──────────────────────────────────────────────────────
    // The classic layout IS the Edit page until Phase 1 replaces it. The other
    // four pages load on first visit and then stay resident, so switching back
    // keeps their layout, scroll and selection.
    Item {
        anchors.fill: parent

        Classic.ClassicLayout {
            id: classicLayout
            objectName: "classicLayout"
            layoutStore: backend.workspaceLayouts
            visible: win.workspace === "Edit"
        }

        Loader {
            id: mediaLoader
            objectName: "loader:Media"
            anchors.fill: parent
            visible: win.workspace === "Media"
            active: false
            onVisibleChanged: if (visible) active = true
            // Opened straight onto Media: the page is visible from the start.
            Component.onCompleted: if (win.workspace === "Media") active = true
            sourceComponent: Media.MediaPage {
                objectName: "workspace:Media"
                layoutStore: backend.workspaceLayouts
            }
            onLoaded: win.mediaTextActive = Qt.binding(function () { return mediaLoader.item.visible && mediaLoader.item.textActive })
        }

        Loader {
            id: deliverLoader
            objectName: "loader:Deliver"
            anchors.fill: parent
            visible: win.workspace === "Deliver"
            active: false
            onVisibleChanged: if (visible) active = true
            sourceComponent: Deliver.DeliverPage {
                objectName: "workspace:Deliver"
                layoutStore: backend.workspaceLayouts
                onReshapeRequested: function (width, height) { win.reshapedCopy(width, height) }
            }
            onLoaded: win.deliverTextActive = Qt.binding(function () { return deliverLoader.item.visible && deliverLoader.item.textActive })
        }

        Loader {
            id: audioLoader
            objectName: "loader:Audio"
            anchors.fill: parent
            visible: win.workspace === "Audio"
            active: false
            onVisibleChanged: if (visible) active = true
            sourceComponent: Audio.AudioPage {
                objectName: "workspace:Audio"
                layoutStore: backend.workspaceLayouts
            }
            onLoaded: win.audioTextActive = Qt.binding(function () { return audioLoader.item.visible && audioLoader.item.textActive })
        }

        Loader {
            id: colorLoader
            objectName: "loader:Color"
            anchors.fill: parent
            visible: win.workspace === "Color"
            active: false
            onVisibleChanged: if (visible) active = true
            sourceComponent: Color.ColorPage {
                objectName: "workspace:Color"
                layoutStore: backend.workspaceLayouts
            }
            onLoaded: win.colorTextActive = Qt.binding(function () { return colorLoader.item.visible && colorLoader.item.textActive })
        }
    }
}
