import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import QtCore
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs as D
import omaedit
import OmaEdit.Edit as Edit
import OmaEdit.Color as Color
import OmaEdit.Ui as Ui
import "../Ui/Keyframes.js" as Keyframes
import "../Ui/Timecode.js" as Timecode

// Viewer(s): the new Edit-page monitor driven by the engine — a single
// mode-switching monitor, or source + program side by side when dualViewer
// is on. This file is the seam between OmaEdit.Edit (which knows nothing of
// the backend) and the classic behaviour it inherits: the skimmable source
// strip and the empty prompts.
Rectangle {
    id: viewerPane
    readonly property var sourceEntry: backend.previewLibraryIndex >= 0
                                       && backend.previewLibraryIndex < backend.library.length
                                       ? backend.library[backend.previewLibraryIndex] : null

    // Monitor overlays persist across launches.
    property bool monitorSafeAreas: backend.viewerSettings.safeAreas
    onMonitorSafeAreasChanged: backend.setViewerOption("safeAreas", monitorSafeAreas)
    property string monitorGuides: backend.viewerSettings.guides
    onMonitorGuidesChanged: backend.setViewerOption("guides", monitorGuides)
    Connections { target: backend; function onViewerSettingsChanged() { viewerPane.monitorSafeAreas = backend.viewerSettings.safeAreas; viewerPane.monitorGuides = backend.viewerSettings.guides } }
    // The colour around the picture, shared by every viewer.
    readonly property color canvasColor: backend.viewerSettings.canvas
    // The timeline's timecode for a frame, read again when the starting
    // timecode or the rate changes and not only when the playhead moves.
    function timelineTimecode(frame) { return backend.startFrame >= 0 && backend.fps >= 0 ? backend.timecode(frame) : "" }
    // A timecode typed in a monitor's readout. A timecode goes there,
    // counted from the timeline's start (a source's from its first frame);
    // + or - and an amount moves the playhead by it, or the selected clips
    // when there are any, as a nudge of that many frames would. Answers ""
    // when done, or what to say where the entry stands.
    function takeTimecode(text, source) {
        const fps = source ? backend.sourceFps : backend.fps
        const origin = source ? 0 : backend.startFrame
        const position = source ? backend.sourcePosition : backend.position
        const last = Math.max(0, (source ? backend.sourceDuration : backend.duration) - 1)
        const entry = Timecode.readEntry(text, fps, position + origin)
        if (entry.kind === "")
            return qsTr("“%1” is not a timecode at %2 fps. Type hours:minutes:seconds:frames, such as 10:00:00:00, or + or - and an amount.")
                       .arg(text.trim()).arg(Number((fps > 0 ? fps : 25).toFixed(3)))
        if (!source && backend.viewingSource) backend.viewTimeline()
        var target = 0, outside = 0
        if (entry.kind === "by") {
            if (!source && win.selectionCount > 0 && win.selection.selectedClip() !== null) {
                const chosen = win.selection.selectedAddresses()
                for (var i = 0; i < chosen.length; ++i)
                    if (backend.tracks[chosen[i].track] && backend.tracks[chosen[i].track].locked)
                        return qsTr("The selected clips are on a locked track. Unlock it to move them, or select nothing to move the playhead.")
                win.trimming.nudgeSelected(entry.frames)
                return ""
            }
            target = Math.max(0, Math.min(last, position + entry.frames))
            outside = position + entry.frames < 0 ? -1 : position + entry.frames > last ? 1 : 0
        } else {
            const at = Timecode.place(entry.frame, origin, fps, last)
            target = at.frame; outside = at.outside
        }
        backend.seek(target)
        const shown = Timecode.format(target + origin, fps)
        if (outside !== 0)
            backend.announce((outside < 0 ? qsTr("“%1” is before the start: the playhead is on the first frame, %2")
                                          : qsTr("“%1” is past the end: the playhead is on the last frame, %2")).arg(text.trim()).arg(shown))
        return ""
    }
    // Opens the entry in the monitor the transport drives, `first` typed.
    function enterTimecode(first) {
        const monitor = win.dualViewer && backend.viewingSource && !backend.multicamMode ? sourceMonitor : programMonitor
        monitor.beginTimecode(first)
    }
    readonly property bool timecodeEntering: sourceMonitor.timecodeEntering || programMonitor.timecodeEntering
    function chooseCanvas(colour) { backend.setViewerOption("canvas", String(colour)) }
    D.ColorDialog {
        id: canvasPicker
        objectName: "canvasPicker"
        title: qsTr("Canvas Colour")
        options: D.ColorDialog.DontUseNativeDialog
        onAccepted: viewerPane.chooseCanvas(selectedColor)
    }
    function pickCustomCanvas() { canvasPicker.selectedColor = viewerPane.canvasColor; canvasPicker.open() }
    property string monitorOverlay: "transform" // transform | crop
    // View ▸ Viewer ▸ Video Scopes: their own layout and kinds, kept here;
    // scale, channels and the rest are the engine's, shared with Color.
    property string scopeLayout: "single"
    property var scopeKinds: ["waveform", "vectorscope", "parade", "histogram"]
    property bool scopesPaused: false
    Settings {
        category: "monitor"
        property alias overlay: viewerPane.monitorOverlay
        property alias scopeLayout: viewerPane.scopeLayout
        property alias scopeKinds: viewerPane.scopeKinds
    }
    readonly property bool scopesShown: win.editScopes && win.workspace === "Edit" && visible
    Binding { target: win; property: "editScopesWanted"; value: viewerPane.scopesShown && !viewerPane.scopesPaused }
    // The selected video-bearing clip (V1 or a track) for the crop box.
    readonly property var selectedVideoEntry:
        selectedLaneEntry !== null
        ? (selectedLaneEntry.audioOnly === true ? null : selectedLaneEntry)
        : win.selectedTimelineIndex >= 0
          && win.selectedTimelineIndex < backend.timeline.length
          && backend.timeline[win.selectedTimelineIndex].gap !== true
          ? backend.timeline[win.selectedTimelineIndex] : null
    // The mask chosen for placing on the picture: the selected clip's effect of that id, while it has one.
    readonly property var effectMask: {
        if (selectedVideoEntry === null || win.maskEditUid === "") return null
        const fx = maskEffect
        return fx && fx.mask && fx.bypass !== true ? Keyframes.maskAt(fx, maskFrame) : null
    }
    readonly property var maskEffect: selectedVideoEntry === null || win.maskEditUid === "" ? null
        : (selectedVideoEntry.videoEffects || []).find(function(e) { return e.uid === win.maskEditUid }) || null
    // The clip's frame under the playhead: a mask with keys shows, and is placed, as it stands there.
    readonly property int maskFrame: selectedVideoEntry === null ? 0
        : Math.max(0, Math.min(selectedVideoEntry.frames - 1, backend.position - selectedVideoEntry.start))
    readonly property var cropRect: selectedVideoEntry !== null ? {
        l: classicLayout.pictureValue("cropL", selectedVideoEntry.cropL || 0),
        r: classicLayout.pictureValue("cropR", selectedVideoEntry.cropR || 0),
        t: classicLayout.pictureValue("cropT", selectedVideoEntry.cropT || 0),
        b: classicLayout.pictureValue("cropB", selectedVideoEntry.cropB || 0) } : null
    // The selected lane clip's picture-in-picture rect, for the transform
    // overlay; titles and audio-only clips have no rect to drag.
    readonly property var selectedLaneEntry:
        win.selectedConnectedIndex >= 0
        && win.selectedConnectedIndex < backend.connected.length
        ? backend.connected[win.selectedConnectedIndex] : null
    // Titles too: their picture rides an affine rect like any lane clip.
    readonly property bool transformable:
        selectedVideoEntry !== null && selectedVideoEntry.audioOnly !== true && selectedVideoEntry.adjust !== true
    readonly property var transformRect: transformable ? {
        x:classicLayout.pictureValue("rectX",selectedVideoEntry.rectX || 0),
        y:classicLayout.pictureValue("rectY",selectedVideoEntry.rectY || 0),
        w:classicLayout.pictureValue("rectW",selectedVideoEntry.rectW || 1),
        h:classicLayout.pictureValue("rectH",selectedVideoEntry.rectH || 1)} : null
    readonly property bool titleTextActive: inlineTitleEditor.visible
    // The Source viewer's picture is a handle on what it shows: pressed and
    // moved past the drag threshold, it carries the clip to the timeline as
    // its library tile would, with the range marked here (no In: from the
    // start; no Out: to the end). A click does nothing, as before; the handle
    // stops short of the strip along the picture's foot (`foot` is the strip's
    // height and margin), which keeps its scrubbing and its edges.
    component SourceDrag: Item {
        id: sourceDrag
        property rect picture
        property real foot: 0
        property bool live: true
        readonly property var entry: backend.previewLibraryIndex >= 0 && backend.previewLibraryIndex < backend.library.length
                                     ? backend.library[backend.previewLibraryIndex] : null
        x: picture.width > 0 ? picture.x : 0
        y: picture.height > 0 ? picture.y : 0
        width: picture.width > 0 ? picture.width : parent.width
        height: Math.max(0, Math.min(picture.height > 0 ? picture.y + picture.height : parent.height, parent.height - foot) - y)
        DragHandler {
            id: sourceDragHandler
            objectName: "sourcePictureDrag"
            target: null
            acceptedButtons: Qt.LeftButton
            // (A panel in a window of its own has no timeline to drop on.)
            enabled: sourceDrag.live && sourceDrag.entry !== null && sourceDrag.entry.offline !== true
                     && viewerPane.Window.window === win && !backend.projectBusy && !backend.exporting
            onActiveChanged: {
                if (active)
                    browserPane.beginClipDrag(backend.previewLibraryIndex, sourceDragHandler, sourceDrag,
                                              MediaTime.durationText(MediaTime.rangeFrames(sourceDrag.entry), MediaTime.effectiveFps(sourceDrag.entry)))
                else
                    browserPane.endClipDrag(sourceDragHandler)
            }
            onCanceled: browserPane.breakClipDrag()
        }
    }
    // The narrowest the pane can be with no monitor's transport row cut or
    // its buttons over one another.
    readonly property real leastWidth: programMonitor.leastWidth
                                       + (sourceMonitor.visible ? sourceMonitor.leastWidth + 1 : 0)


    Layout.fillWidth: true
    Layout.fillHeight: true
    color: win.wellColor

    RowLayout {
        anchors.fill: parent
        spacing: 1

        // Source monitor (only in the two-up layout). Its transport goes
        // inert while the program side is active: the engine's play/seek
        // act on whichever view is current, and a stray click here would
        // drive the timeline with source-relative frames.
        // Multicam viewer (Ctrl+Shift+M): takes the source monitor's slot.
        MulticamGrid {
            objectName: "multicamGrid"
            visible: backend.multicamMode
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Edit.ProgramMonitor {
            id: sourceMonitor
            objectName: "sourceMonitor"
            onFrameExportRequested: backend.exportFrameDialog()
            timecodeTaker: function (text) { return viewerPane.takeTimecode(text, true) }
            fullscreenAvailable: true
            onFullscreenRequested: classicLayout.layoutController.toggleViewerFullscreen()
            visible: win.dualViewer && !backend.multicamMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: qsTr("Source")
            canvasColor: viewerPane.canvasColor
            onCanvasSelected: function (colour) { viewerPane.chooseCanvas(colour) }
            resolutionDivisor: backend.viewerSettings.previewDivisor
            onResolutionSelected: function (divisor) { backend.setViewerOption("previewDivisor", divisor) }
            onCanvasCustomRequested: viewerPane.pickCustomCanvas()
            projectName: sourceEntry ? sourceEntry.name : ""
            timecode: backend.previewLibraryIndex >= 0
                      ? backend.sourceTimecode(backend.sourcePosition) : "--:--:--:--"
            duration: backend.previewLibraryIndex >= 0
                      ? backend.sourceTimecode(backend.sourceDuration - 1) : "--:--:--:--"
            playing: backend.playing && backend.viewingSource
            speed: backend.speed
            hasMedia: backend.previewLibraryIndex >= 0 && backend.viewingSource
            highlighted: backend.viewingSource
            sourceMode: true
            editTakes: backend.editTakes
            // (editTakes read first: editTakesFor is a call, which no change of the choice would make the binding read again.)
            editTakesEffective: { const takes = backend.editTakes; return backend.previewLibraryIndex >= 0 && backend.library.length > 0 ? backend.editTakesFor(backend.previewLibraryIndex) : takes }
            sourceMulticam: backend.previewLibraryIndex >= 0 && backend.previewLibraryIndex < backend.library.length && backend.library[backend.previewLibraryIndex].multicam === true
            onEditTakesSelected: function (takes) { backend.editTakes = takes }
            emptyText: backend.previewLibraryIndex >= 0
                       ? qsTr("Source view is idle — click the library clip again")
                       : qsTr("Click a library clip to view its source")
            onPlayToggled: Ui.Commands.trigger("transport.play")
            onPaused: Ui.Commands.trigger("transport.stop")
            onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
            onStartRequested: Ui.Commands.trigger("transport.start")
            onEndRequested: Ui.Commands.trigger("transport.end")

            surfaceRect: Qt.rect(sourceSurface.x + sourceSurface.contentRect.x, sourceSurface.y + sourceSurface.contentRect.y, sourceSurface.contentRect.width, sourceSurface.contentRect.height)
            VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
                id: sourceSurface
                objectName: "sourceSurface"
                anchors.fill: parent
                anchors.margins: 2
                source: backend
                channel: VideoView.Source
                opacity: backend.viewingSource ? 1 : 0.45
            }

            SourceDrag {
                picture: sourceMonitor.surfaceRect
                foot: dualSourceStrip.visible ? dualSourceStrip.height + dualSourceStrip.anchors.margins : 0
                live: backend.viewingSource
            }

            SourceStrip {
                id: dualSourceStrip
                visible: backend.previewLibraryIndex >= 0 && frames > 0
                enabled: backend.viewingSource
                opacity: backend.viewingSource ? 1 : 0.45
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
            }
        }

        // Program monitor (the edit) — the only monitor when dualViewer is
        // off, in which case it switches modes with the engine.
        Edit.ProgramMonitor {
            fullscreenAvailable: true
            onFullscreenRequested: classicLayout.layoutController.toggleViewerFullscreen()
            id: programMonitor
            timecodeTaker: function (text) { return viewerPane.takeTimecode(text, showingSource) }
            Layout.fillWidth: true
            Layout.fillHeight: true
            readonly property bool showingSource: !win.dualViewer && backend.viewingSource
            title: showingSource ? qsTr("Source") : qsTr("Program")
            canvasColor: viewerPane.canvasColor
            onCanvasSelected: function (colour) { viewerPane.chooseCanvas(colour) }
            resolutionDivisor: backend.viewerSettings.previewDivisor
            onResolutionSelected: function (divisor) { backend.setViewerOption("previewDivisor", divisor) }
            mediaAvailable: true
            proxyPlayback: backend.mediaPlayback !== "original"
            mediaLocked: backend.mediaHealth.originalsOnly === true
            // Back to proxies restores the proxy choice that was in use.
            property string proxyMode: "preferred"
            readonly property string playbackMode: backend.mediaPlayback
            onPlaybackModeChanged: if (playbackMode !== "original") proxyMode = playbackMode
            Component.onCompleted: if (playbackMode !== "original") proxyMode = playbackMode
            onMediaSelected: function (proxies) { backend.mediaPlayback = proxies ? proxyMode : "original" }
            onCanvasCustomRequested: viewerPane.pickCustomCanvas()
            projectName: showingSource ? (sourceEntry ? sourceEntry.name : "")
                                       : backend.projectName
            timecode: showingSource
                      ? backend.sourceTimecode(backend.sourcePosition)
                      : backend.hasVideo ? viewerPane.timelineTimecode(backend.position) : "--:--:--:--"
            duration: showingSource
                      ? backend.sourceTimecode(backend.sourceDuration - 1)
                      : backend.hasVideo ? viewerPane.timelineTimecode(backend.duration - 1) : "--:--:--:--"
            playing: backend.playing && (showingSource || !backend.viewingSource)
            speed: backend.speed
            hasMedia: showingSource || backend.hasVideo
            highlighted: win.dualViewer && !backend.viewingSource
            sourceMode: showingSource
            editTakes: backend.editTakes
            editTakesEffective: { const takes = backend.editTakes; return showingSource && backend.previewLibraryIndex >= 0 && backend.library.length > 0 ? backend.editTakesFor(backend.previewLibraryIndex) : takes }
            sourceMulticam: showingSource && backend.previewLibraryIndex >= 0 && backend.previewLibraryIndex < backend.library.length && backend.library[backend.previewLibraryIndex].multicam === true
            onEditTakesSelected: function (takes) { backend.editTakes = takes }
            emptyText: backend.library.length === 0
                       ? qsTr("Import media to begin")
                       : qsTr("Select a clip and press E to add it to the timeline")
            onPlayToggled: Ui.Commands.trigger("transport.play")
            onPaused: Ui.Commands.trigger("transport.stop")
            onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
            onStartRequested: Ui.Commands.trigger("transport.start")
            onEndRequested: Ui.Commands.trigger("transport.end")
            objectName: "programMonitor"
            picking: !showingSource && win.keyPick !== null
            onPointPicked: function (x, y) { win.finishKeyPick(x, y) }
            imageAspect: backend.videoWidth > 0 && backend.videoHeight > 0
                         ? backend.videoDisplayAspect : 16 / 9
            safeAreas: viewerPane.monitorSafeAreas
            guides: viewerPane.monitorGuides
            onSafeAreasToggled: function (on) { viewerPane.monitorSafeAreas = on }
            onGuidesSelected: function (mode) { viewerPane.monitorGuides = mode }
            effectMask: showingSource ? null : viewerPane.effectMask
            effectMaskEditable: viewerPane.selectedVideoEntry !== null && !backend.exporting
            onEffectMaskEdited: function (mask) {
                const e = viewerPane.selectedVideoEntry
                if (e && viewerPane.maskEffect && pictureTargetValid())
                    backend.editVideoEffect(e.track, e.index, "update", win.maskEditUid, Keyframes.maskUpdate(viewerPane.maskEffect, mask, viewerPane.maskFrame))
            }
            onEffectMaskStarted: beginPictureGesture()
            onEffectMaskFinished: endPictureGesture()
            transformRect: showingSource ? null : viewerPane.transformRect
            transformEditable: !showingSource && viewerPane.transformable
            overlayMode: viewerPane.monitorOverlay
            onOverlayModeSelected: function (mode) { viewerPane.monitorOverlay = mode }
            cropRect: showingSource ? null : viewerPane.cropRect
            cropEditable: !showingSource && viewerPane.cropRect !== null
            onCropEdited:function(l,r,t,b){ if (pictureTargetValid()) classicLayout.editPicture({cropL:l,cropR:r,cropT:t,cropB:b}) }
            onCropStarted: beginPictureGesture()
            onCropFinished: endPictureGesture()
            multicamAvailable: !showingSource && backend.hasVideo
            multicam: backend.multicamMode
            onMulticamToggled: function (on) { backend.multicamMode = on }
            // (Beside a Source viewer this one is the timeline's, whichever was last played.)
            onFrameExportRequested: { if (win.dualViewer && backend.viewingSource) backend.viewTimeline(); backend.exportFrameDialog() }
            compareAvailable: !showingSource && backend.hasVideo
            compare: backend.compareMode
            onCompareToggled: function (on) { backend.compareMode = on }
            onTransformEdited: function (x, y, w, h) {
                if (pictureTargetValid()) classicLayout.editPicture({rectX:x,rectY:y,rectW:w,rectH:h})
            }
            function pictureTargetValid() {
                return !transformTarget || !(!viewerPane.selectedVideoEntry
                    || backend.activeSequence !== transformTarget.sequence
                    || viewerPane.selectedVideoEntry.track !== transformTarget.entry.track
                    || viewerPane.selectedVideoEntry.index !== transformTarget.entry.index
                    || viewerPane.selectedVideoEntry.path !== transformTarget.entry.path)
            }
            property var transformTarget: null
            function beginPictureGesture() {
                transformTarget = {sequence:backend.activeSequence, entry:viewerPane.selectedVideoEntry}
                backend.beginEditGesture()
            }
            function endPictureGesture() { backend.endEditGesture(); transformTarget = null }
            onTransformStarted: beginPictureGesture()
            onTransformFinished: endPictureGesture()

            surfaceRect: Qt.rect(programSurface.x + programSurface.contentRect.x, programSurface.y + programSurface.contentRect.y, programSurface.contentRect.width, programSurface.contentRect.height)
            VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
                id: programSurface
                objectName: "programSurface"
                overlaysEnabled: false
                anchors.fill: parent
                anchors.margins: 2
                source: backend
                channel: win.dualViewer ? VideoView.Program : VideoView.Auto
            }
            Button {
                objectName:"editTitleInViewer"
                ToolTip.text: qsTr("Edit this title's words on the picture itself"); ToolTip.visible: hovered; ToolTip.delay: 450
                anchors.top:parent.top;anchors.horizontalCenter:parent.horizontalCenter
                z:40
                visible:!programMonitor.showingSource && viewerPane.selectedLaneEntry !== null && !!viewerPane.selectedLaneEntry.title && !inlineTitleEditor.visible
                text:qsTr("Edit title here")
                onClicked:{backend.pause();inlineTitleEditor.entry=viewerPane.selectedLaneEntry;inlineTitleEditor.clipIndex=win.selectedConnectedIndex;inlineTitleText.text=inlineTitleEditor.entry.title;inlineTitleEditor.visible=true;inlineTitleText.forceActiveFocus()}
            }
            Rectangle {
                id:inlineTitleEditor
                objectName:"inlineTitleEditor"
                anchors.centerIn:parent
                width:Math.max(180,parent.width*.85);height:Math.min(180,parent.height*.8)
                color:Ui.Theme.panelBg;border.color:Ui.Theme.accent;border.width:1;z:50;visible:false
                property var entry:null
                property int clipIndex:-1
                Connections { // undo or a delete can move another clip under the saved index
                    target: backend
                    function onTimelineEntriesChanged() {
                        if (!inlineTitleEditor.visible || inlineTitleEditor.entry === null) return
                        const e = backend.connected[inlineTitleEditor.clipIndex]
                        if (!e || e.start !== inlineTitleEditor.entry.start || e.frames !== inlineTitleEditor.entry.frames || !e.title)
                            inlineTitleEditor.visible = false
                    }
                }
                TextArea {
                    id:inlineTitleText
                    objectName:"inlineTitleText"
                    anchors.fill:parent;anchors.margins:8;anchors.bottomMargin:45
                    wrapMode:TextEdit.Wrap
                    font.pixelSize:22
                    Keys.onEscapePressed:inlineTitleEditor.visible=false
                }
                Row {
                    anchors.bottom:parent.bottom;anchors.right:parent.right;spacing:8
                    Button {text:qsTr("Cancel");ToolTip.text: qsTr("Close the title editor and keep the old words");ToolTip.visible: hovered;ToolTip.delay: 450;onClicked:inlineTitleEditor.visible=false}
                    Button {
                        objectName:"applyInlineTitle"
                        ToolTip.text: qsTr("Replace the title's words with what you typed"); ToolTip.visible: hovered; ToolTip.delay: 450
                        text:qsTr("Apply");enabled:inlineTitleText.text.trim().length>0
                        onClicked:{backend.updateTitle(inlineTitleEditor.clipIndex,inlineTitleText.text,win.catalogue.titleStyle(inlineTitleEditor.entry));inlineTitleEditor.visible=false}
                    }
                }
                Connections {target:win;function onSelectedConnectedIndexChanged(){inlineTitleEditor.visible=false}function onSelectedTimelineIndexChanged(){inlineTitleEditor.visible=false}}
            }
            // Before / after: the "before" surface (every attribute off)
            // clipped to the left of a draggable divider, over the edit.
            Item {
                id: compareWipe
                objectName: "compareWipe"
                anchors.fill: parent
                anchors.margins: 2
                visible: backend.compareMode && !programMonitor.showingSource
                Item {
                    clip: true
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    width: parent.width * programMonitor.compareSplit
                    VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
                        width: compareWipe.width
                        height: compareWipe.height
                        source: backend
                        channel: VideoView.Before
                    }
                }
                Rectangle {
                    id: compareDivider
                    x: parent.width * programMonitor.compareSplit - 1
                    width: 2
                    height: parent.height
                    color: Ui.Theme.accent
                }
                Ui.Chip {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    tip: qsTr("Left of the divider is the clip with no attributes applied")
                    label: qsTr("BEFORE")
                }
                Ui.Chip {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    tip: qsTr("Right of the divider is the clip as you have edited it")
                    label: qsTr("AFTER")
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SplitHCursor
                    onPressed: function (mouse) {
                        programMonitor.compareSplit = Math.max(0.02, Math.min(0.98, mouse.x / width))
                    }
                    onPositionChanged: function (mouse) {
                        if (pressed)
                            programMonitor.compareSplit = Math.max(0.02, Math.min(0.98, mouse.x / width))
                    }
                }
            }

            // Two-up trim view: with an edge selected, the last frame of
            // the outgoing clip beside the first of the incoming, each
            // with its timecode; stands down while playing (the program
            // shows the motion) and under the before/after wipe.
            Item {
                id: twoUp
                objectName: "twoUpView"
                anchors.fill: parent
                anchors.margins: 2
                visible: backend.twoUpEdge >= 0 && !programMonitor.showingSource
                         && !backend.compareMode && !backend.playing && !backend.multicamMode
                Rectangle { anchors.fill: parent; color: viewerPane.canvasColor }
                Row {
                    anchors.fill: parent
                    spacing: 2
                    Item {
                        width: (parent.width - 2) / 2
                        height: parent.height
                        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
                            anchors.fill: parent
                            source: backend
                            channel: VideoView.TwoUpOutgoing
                        }
                        Ui.Chip {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 8
                            tip: qsTr("Last frame of the clip before the selected edit point")
                            label: qsTr("OUTGOING · ") + (backend.twoUpEdge > 0 ? viewerPane.timelineTimecode(backend.twoUpEdge - 1) : "—")
                        }
                    }
                    Item {
                        width: (parent.width - 2) / 2
                        height: parent.height
                        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
                            anchors.fill: parent
                            source: backend
                            channel: VideoView.TwoUpIncoming
                        }
                        Ui.Chip {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            tip: qsTr("First frame of the clip after the selected edit point")
                            label: qsTr("INCOMING · ") + (backend.twoUpEdge < backend.duration ? viewerPane.timelineTimecode(backend.twoUpEdge) : "—")
                        }
                    }
                }
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 2
                    height: parent.height
                    color: Ui.Theme.accent
                }
                Ui.Chip {
                    visible: win.dynamicTrim
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    tip: qsTr("Playback keys trim the edit instead of moving the playhead")
                    label: qsTr("DYNAMIC TRIM")
                }
            }

            SourceDrag {
                visible: programMonitor.showingSource
                picture: programMonitor.surfaceRect
                foot: soloSourceStrip.visible ? soloSourceStrip.height + soloSourceStrip.anchors.margins : 0
                live: programMonitor.showingSource && !programMonitor.picking
            }

            SourceStrip {
                id: soloSourceStrip
                visible: !win.dualViewer && backend.viewingSource && frames > 0
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
            }
        }

        // Video scopes beside the viewer, reading the same frame.
        Color.ScopeStack {
            objectName: "editScopes"
            visible: viewerPane.scopesShown
            Layout.fillHeight: true
            Layout.preferredWidth: Math.max(240, viewerPane.width * 0.3)
            detachable: false
            tick: backend.scopeTick
            layout: viewerPane.scopeLayout
            kinds: viewerPane.scopeKinds
            scopeScale: backend.scopeOptions.scale || "ire"
            channels: backend.scopeOptions.channels || "rgb"
            intensity: backend.scopeOptions.intensity || 1
            graticule: backend.scopeOptions.graticule !== false
            clamp: backend.scopeOptions.clamp === true
            bitDepth: backend.scopeOptions.bitDepth || "Float"
            paused: viewerPane.scopesPaused
            function option(key, value) { const o = Object.assign({}, backend.scopeOptions); o[key] = value; backend.scopeOptions = o }
            onLayoutEdited: function (l) { viewerPane.scopeLayout = l }
            onKindEdited: function (slot, k) { const ks = viewerPane.scopeKinds.slice(); ks[slot] = k; viewerPane.scopeKinds = ks }
            onScaleEdited: function (v) { option("scale", v) }
            onChannelsEdited: function (v) { option("channels", v) }
            onIntensityEdited: function (v) { option("intensity", v) }
            onGraticuleToggled: function (on) { option("graticule", on) }
            onClampToggled: function (on) { option("clamp", on) }
            onBitDepthEdited: function (v) { option("bitDepth", v) }
            onPausedToggled: function (on) { viewerPane.scopesPaused = on }
        }
    }
}
