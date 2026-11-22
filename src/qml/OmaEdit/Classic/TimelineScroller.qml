import QtQuick
import QtQuick.Controls
import QtQuick.Controls as C
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Edit as Edit
import "../Ui/MediaTime.js" as MediaTime

Flickable {
    id: timelineScroll
    objectName: "timelineScroll"
    // Siblings (track headers, precision editor) and the window
    // address these rows by id; ids stop at the file boundary.
    property alias ruler: ruler
    property int skimFrame: -1  // the frame under the pointer while skimming, or -1
    property alias connectedLane: connectedLane
    property alias storyline: storyline
    property alias audioLane: audioLane
    property alias attachedLane: attachedLane
    // The main-timeline clip being trimmed and its pending frame deltas, so
    // its linked sound below draws the same result while the drag lasts.
    property int trimIndex: -1
    property int trimIn: 0
    property int trimOut: 0
    property bool trimMovesStart: false
    // … and the one being slipped, by how many frames.
    property int slipIndex: -1
    property int slipBy: 0
    // Pending edge drags of pictures, by link id, so the sound linked to a
    // picture on any track draws the same trim while the drag lasts:
    // link → { dIn, dOut, movesStart }.
    property var linkTrims: ({})
    // (`rolled`: the picture's cut is being rolled, which a sound stops
    // following where its own media ends.)
    function setLinkTrim(link, dIn, dOut, movesStart, rolled) {
        if (link === undefined || link === "")
            return
        const next = Object.assign({}, linkTrims)
        if (dIn === 0 && dOut === 0)
            delete next[link]
        else
            next[link] = { dIn: dIn, dOut: dOut, movesStart: movesStart, rolled: rolled === true }
        linkTrims = next
    }
    // A cut being moved by the mouse (a roll): the track, the clip before
    // the cut, and how many frames the cut has gone. The two blocks that
    // meet at the cut both draw it, so they move as one while the drag
    // lasts; released, it is held until the engine's answer redraws them
    // (or it refuses), so nothing springs back in between.
    property int rollTrack: -1
    property int rollIndex: -1
    property int rollBy: 0
    property bool rollHeld: false
    // The two clips at the cut as they stood when the roll began.
    property string rollPlace: ""
    function cutPlace(track, index) {
        const a = win.selection.trackEntry(track, index), b = win.selection.trackEntry(track, index + 1)
        return a === null || b === null ? ""
             : [a.clipId, a.start, a.frames, a.in, a.out, b.clipId, b.start, b.frames, b.in, b.out].join(":")
    }
    function showRoll(track, index, by) {
        if (track !== rollTrack || index !== rollIndex) {
            rollBy = 0
            rollTrack = track
            rollIndex = index
            rollPlace = cutPlace(track, index)
        }
        rollHeld = false
        rollBy = by
    }
    function endRoll() {
        rollHold.stop()
        rollHeld = false
        rollBy = 0
        rollIndex = -1
        rollTrack = -1
        rollPlace = ""
    }
    // The release: the roll shown is made; where none could be shown (the
    // cut cannot move that way, or at all) the engine is asked all the same,
    // so that it says why.
    function commitRoll(track, index, wanted) {
        const by = track === rollTrack && index === rollIndex ? rollBy : 0
        if (by === 0) {
            endRoll()
            if (wanted !== 0)
                backend.rollEditOn(track, index, wanted)
            return
        }
        // Where the engine is not asked (a render runs, the viewer shows a
        // source) there is no answer to wait for.
        if (backend.exporting || backend.viewingSource) {
            endRoll()
            return
        }
        rollHeld = true
        rollHold.restart()
        backend.rollEditOn(track, index, by)
    }
    // How far the block of clip `index` on `track` has its end (outgoing) or
    // its start moved by the roll in hand.
    function rollOf(track, index, outgoing) {
        return rollIndex >= 0 && track === rollTrack && index === (outgoing ? rollIndex : rollIndex + 1) ? rollBy : 0
    }
    // The engine's answer is the edit made (the cut's clips change) or
    // refused; a status line from anything else meanwhile is neither, and
    // nor is the timeline told again with the cut where it was (a mixer
    // change, the last word of a load): the cut stays under the mouse, or
    // held, through that. The timer is for an answer that never comes.
    // (Where such a change makes the blocks again, the one the cut was
    // pressed on ends the roll as it goes: the press goes with it.)
    Timer { id: rollHold; interval: 5000; onTriggered: timelineScroll.endRoll() }
    Connections {
        target: backend
        function onTimelineEntriesChanged() {
            if (timelineScroll.rollIndex < 0)
                return
            const place = timelineScroll.cutPlace(timelineScroll.rollTrack, timelineScroll.rollIndex)
            if (place === "" || place !== timelineScroll.rollPlace)
                timelineScroll.endRoll()
        }
        function onEditRefused() { if (timelineScroll.rollHeld) timelineScroll.endRoll() }
    }
    // A main-timeline clip being dragged to a new place in the order
    // (Ctrl+Shift): its linked sound rides along until the drop.
    property string shuffleLink: ""
    property real shufflePx: 0

    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.right: meters.left
    anchors.left: trackHeaders.visible ? trackHeaders.right : indexPanel.right
    anchors.margins: 12
    anchors.leftMargin: trackHeaders.visible ? 4
                        : (win.showTimelineIndex ? 19 : 12)
    anchors.rightMargin: 12
    // Fit puts the edit across the view; the tracks past it scroll on at
    // the same scale.
    contentWidth: timelineScroll.contentFrames <= 0 ? width
                  : win.pxPerFrame > 0
                  ? Math.max(width, timelineScroll.shownFrames * win.pxPerFrame)
                  : width * timelineScroll.shownFrames / timelineScroll.contentFrames
    // Tracks stack to their own height; past the viewport they scroll
    // vertically (the headers ride contentY too). Below the lowest track,
    // room for the scroll bar along the bottom while it is shown, so it lies
    // over none of that track's clips.
    contentHeight: Math.max(height, laneColumn.implicitHeight
                                    + (timeBar.policy === C.ScrollBar.AlwaysOn ? timeBar.reach : 0))
    interactive: false // wheel scrolls; drags belong to the clips
    clip: true
    // Shown whenever tracks are out of view, so there is always something
    // to see and drag.
    C.ScrollBar.vertical: Ui.ScrollBar {
        objectName: "timelineTrackScroll"
        policy: timelineScroll.contentHeight > timelineScroll.height + 1 ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded
    }
    // Along the bottom, to move through the edit without touching the
    // playhead; shown whenever the edit is wider than the view.
    C.ScrollBar.horizontal: Ui.ScrollBar {
        id: timeBar
        objectName: "timelineTimeScroll"
        policy: timelineScroll.contentWidth > timelineScroll.width + 1 ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded
    }

    // Only clips near the viewport have visual delegates. Source addresses
    // stay explicit when the window moves; selected gesture targets stay live.
    readonly property real viewLeft: contentX - width / 2
    readonly property real viewRight: contentX + width * 1.5
    readonly property real framesPerPixel: timelineScroll.shownFrames / Math.max(1, contentWidth)
    // The frames the tracks show. A clip hovering over them makes room for
    // itself, as the timeline will once it lands, so all of it can be seen.
    // The tracks run on past the last clip (most of a screen at any zoom),
    // so the end of the edit scrolls into view with room to work beyond
    // it; fit still lays the edit itself edge to edge across the view.
    readonly property int contentFrames: backend.duration > 0 ? Math.max(backend.duration, dropEnd) : 0
    readonly property int tailFrames: contentFrames <= 0 ? 0
                                      : win.pxPerFrame > 0 ? Math.ceil(width * 0.8 / win.pxPerFrame)
                                      : Math.ceil(contentFrames * 0.8)
    readonly property int shownFrames: contentFrames + tailFrames
    function spans(x, w) { return x + w >= viewLeft && x <= viewRight }
    // The playhead may run on along the empty tracks after the edit.
    Binding {
        target: backend
        property: "pastEndFrames"
        value: Math.max(0, timelineScroll.shownFrames - backend.duration)
    }
    // Dragging the playhead against either edge of the view scrolls the
    // timeline on, faster the further past the edge, and the playhead
    // follows.
    Timer {
        id: playheadEdgeScroll
        objectName: "playheadEdgeScroll"
        interval: 16
        repeat: true
        readonly property real zone: 40
        readonly property real push: !ruler.dragging ? 0
            : ruler.dragViewX > timelineScroll.width - zone ? ruler.dragViewX - (timelineScroll.width - zone)
            : ruler.dragViewX < zone ? ruler.dragViewX - zone : 0
        running: push !== 0 && timelineScroll.contentWidth > timelineScroll.width
        onTriggered: {
            const step = Math.max(-60, Math.min(60, push / 2))
            const next = Math.max(0, Math.min(timelineScroll.contentX + step,
                                              timelineScroll.contentWidth - timelineScroll.width))
            if (next === timelineScroll.contentX)
                return
            timelineScroll.contentX = next
            const x = Math.max(0, Math.min(timelineScroll.width - 1, ruler.dragViewX))
            backend.seek(ruler.frameFor(timelineScroll.contentX + x))
        }
    }
    // Back to fit shows the edit itself, not the empty tracks after it.
    Connections {
        target: win
        function onPxPerFrameChanged() { if (win.pxPerFrame <= 0) timelineScroll.contentX = 0 }
    }
    // A transition with no handle frames in it: a plain overlap of the pair.
    function overlapsOnly(entry) {
        return !entry.mixAlign || !(win.snapping.mixTail(entry) > 0 || (entry.mixExtB || 0) > 0)
    }
    // Clip marker flags placed on a block that is not its entry's span (it
    // starts `lead` frames before it and is `shown` frames long).
    function blockFlags(flags, frames, lead, shown) {
        if (lead === 0 && frames === shown)
            return flags
        return flags.map(function (f) {
            return Object.assign({}, f, { frac: (f.frac * frames + lead) / Math.max(1, shown) })
        })
    }
    function dissolveLimit(entry) {
        if (!(entry.mixOut > 0)) return 0
        const next = win.selection.trackEntry(entry.track, entry.index + 1)
        return Math.max(0, Math.min(entry.frames,
                                   next ? next.frames + entry.mixOut : entry.frames) - 1)
    }
    readonly property var pinnedStory: {
        var out = []
        if (win.selectedTimelineIndex >= 0) out.push(win.selectedTimelineIndex)
        for (var i = 0; i < win.extraSelection.length; ++i)
            if (win.extraSelection[i].track === 0) out.push(win.extraSelection[i].index)
        return out
    }
    readonly property var pinnedConnected: {
        var out = []
        if (win.selectedConnectedIndex >= 0) out.push(win.selectedConnectedIndex)
        for (var i = 0; i < backend.connected.length; ++i)
            for (var j = 0; j < win.extraSelection.length; ++j)
                if (backend.connected[i].track === win.extraSelection[j].track && backend.connected[i].index === win.extraSelection[j].index) out.push(i)
        return out
    }
    component ClipWindow: TimelineWindowModel {
        fps: backend.fps > 0 ? backend.fps : 25
        firstFrame: timelineScroll.viewLeft * timelineScroll.framesPerPixel
        lastFrame: timelineScroll.viewRight * timelineScroll.framesPerPixel
        minimumFrames: 6 * timelineScroll.framesPerPixel
        active: timelineScroll.visible
    }

    Timer {
        id: decorWindowTimer
        interval: 60
        onTriggered: if (timelineScroll.visible) backend.setTimelineDecorationWindow(
            Math.max(0, timelineScroll.viewLeft) * timelineScroll.framesPerPixel,
            timelineScroll.viewRight * timelineScroll.framesPerPixel,
            // Screen pixels, not layout pixels: waveforms are drawn a column
            // per screen pixel, and their detail is measured to match.
            Math.ceil((timelineScroll.viewRight - Math.max(0, timelineScroll.viewLeft))
                      * Math.max(1, timelineScroll.Screen.devicePixelRatio)))
    }
    onViewLeftChanged: decorWindowTimer.restart()
    onViewRightChanged: decorWindowTimer.restart()
    onFramesPerPixelChanged: decorWindowTimer.restart()
    onVisibleChanged: if (visible) decorWindowTimer.restart()
    Component.onCompleted: decorWindowTimer.restart()

    function keepInView(x) {
        if (contentWidth <= width)
            return
        if (x < contentX + 40 || x > contentX + width - 40)
            contentX = Math.max(0, Math.min(x - width / 2,
                                            contentWidth - width))
    }

    WheelHandler {
        // Plain wheel scrolls time (or zooms, per Preferences ▸ Input; Ctrl
        // does the other); Shift+wheel scrolls the track stack when it overflows.
        onWheel: function (event) { win.timelineWheel(event, timelineScroll) }
    }
    // A purely sideways turn (a tilt wheel, a sideways swipe, or Shift+wheel
    // on systems that turn it sideways) never reaches a vertical handler.
    WheelHandler {
        orientation: Qt.Horizontal
        onWheel: function (event) { if (event.angleDelta.y === 0) win.timelineWheel(event, timelineScroll) }
    }

    // ── Drops from the media panel ────────────────────────────────────
    // A library tile dragged onto a lane lands as an OVERWRITE where it is let
    // go: the pointer picks the track and the time (the clip starts under the
    // pointer, and snaps as a moved clip does: to the playhead, cuts and
    // markers); onto an empty V1 it starts the edit. The strips below show
    // the clip at its landing spot while the drag hovers. An effect tile
    // lands on the clip under the pointer.
    property int dropTrack: -1
    property int dropFrame: -1
    property int dropFrames: 0
    property string dropName: ""
    property string dropThumb: ""
    property int dropEnd: 0
    // Stays a moment after the pointer leaves: crossing the gap between two
    // tracks, or the timeline growing after a drop, must not jump the scale.
    Timer {
        id: dropEndReset
        interval: 200
        onTriggered: timelineScroll.dropEnd = 0
    }
    // "clip" (a media tile: the ghost previews the span) or "effect" (a
    // catalogue tile: the clip under the pointer lights up instead).
    property string dropKind: "clip"
    property int dropConnectedIndex: -1
    // Where the dragged clip's pointer is: the drop area it is over, and how
    // far across the view (-1 = no clip is over the tracks).
    property Item dropArea: null
    property real dropViewX: -1
    function clearDrop() {
        dropArea = null
        dropViewX = -1
        dropTrack = -1
        dropFrame = -1
        dropFrames = 0
        dropName = ""
        dropThumb = ""
        dropKind = "clip"
        dropConnectedIndex = -1
        dropEndReset.restart()
        storyline.dropTargetIndex = -1
    }
    // The frame a clip of `frames` starts on with the pointer at `x` of a
    // drop area as wide as the tracks.
    function dropStart(area, x, frames) {
        if (shownFrames <= 0 || area.width <= 0)
            return 0
        var perFrame = area.width / shownFrames
        var under = Math.max(0, x / perFrame)
        // It meets the clips on its track and snaps as a moved clip does;
        // anything else is the frame the pointer is on.
        var landing = win.snapping.drag(under, frames, perFrame, dropTrack, null)
        return landing.snapped ? landing.frame : Math.floor(under)
    }
    function previewClip(source, area, x) {
        dropKind = "clip"
        dropFrames = source.frames
        dropName = source.name !== undefined ? source.name : ""
        dropThumb = source.thumb !== undefined ? source.thumb : ""
        dropEndReset.stop()
        // Room for the clip after the last one, the same all through the
        // drag: the scale holds still under the pointer.
        dropEnd = backend.duration + dropFrames
        dropArea = area
        dropViewX = area.mapToItem(timelineScroll, x, 0).x
        dropFrame = dropStart(area, x, dropFrames)
    }
    // Held against either edge of the view, the dragged clip scrolls the
    // timeline on, faster the further past the edge.
    Timer {
        id: dropEdgeScroll
        objectName: "dropEdgeScroll"
        interval: 16
        repeat: true
        readonly property real zone: 40
        readonly property real push: timelineScroll.dropArea === null || timelineScroll.dropKind !== "clip" ? 0
            : timelineScroll.dropViewX > timelineScroll.width - zone ? timelineScroll.dropViewX - (timelineScroll.width - zone)
            : timelineScroll.dropViewX < zone ? timelineScroll.dropViewX - zone : 0
        running: push !== 0 && timelineScroll.contentWidth > timelineScroll.width
        onTriggered: {
            const step = Math.max(-60, Math.min(60, push / 2))
            const next = Math.max(0, Math.min(timelineScroll.contentX + step,
                                              timelineScroll.contentWidth - timelineScroll.width))
            if (next === timelineScroll.contentX)
                return
            timelineScroll.contentX = next
            const area = timelineScroll.dropArea
            timelineScroll.dropFrame = timelineScroll.dropStart(
                area, area.mapFromItem(timelineScroll, timelineScroll.dropViewX, 0).x,
                timelineScroll.dropFrames)
        }
    }
    component LaneDrop: DropArea {
        id: laneDrop
        property Item strip
        // Which track a y inside the strip means; -1 = none.
        property var trackAt: function (y) { return -1 }
        anchors.fill: parent
        keys: ["omaedit/clip", "omaedit/effect", "omaedit/pancake"]
        function track(drag) {
            var t = laneDrop.trackAt(drag.y)
            return t
        }
        function isCatalogue(drag) {
            return drag.source !== null && drag.source.kind !== undefined
        }
        function preview(drag) {
            var t = track(drag)
            if (t < 0) {
                timelineScroll.clearDrop()
                return
            }
            timelineScroll.dropTrack = t
            if (isCatalogue(drag)) {
                // Light the clip under the pointer (titles need none).
                timelineScroll.dropKind = "effect"
                timelineScroll.dropFrames = 0
                var frame = Math.max(0, Math.floor(drag.x / laneDrop.strip.width
                                                   * timelineScroll.shownFrames))
                timelineScroll.dropFrame = frame
                var hit = drag.source.kind === "title" ? null : win.selection.clipAt(t, frame)
                storyline.dropTargetIndex = hit !== null ? hit.timelineIndex : -1
                timelineScroll.dropConnectedIndex = hit !== null ? hit.connectedIndex : -1
                return
            }
            timelineScroll.previewClip(drag.source, laneDrop, drag.x)
        }
        onEntered: function (drag) { preview(drag) }
        onPositionChanged: function (drag) { preview(drag) }
        onExited: timelineScroll.clearDrop()
        onDropped: function (drop) {
            var t = track(drop)
            var frame = timelineScroll.dropFrame
            var catalogue = isCatalogue(drop)
            timelineScroll.clearDrop()
            if (t < 0)
                return
            drop.accept(Qt.CopyAction)
            if (catalogue) {
                win.dropCatalogueAt(drop.source.kind, drop.source.key, t,
                                    Math.max(0, frame))
                return
            }
            backend.viewTimeline()
            if (drop.source.sequenceIndex >= 0) {
                backend.dropSequenceAt(drop.source.sequenceIndex, t, Math.max(0, frame))
                return
            }
            if (drop.source.pancake === true) {
                backend.placeEntryAt(drop.source.entry, t, Math.max(0, frame))
                return
            }
            backend.dropClipAt(drop.source.libraryIndex, t, Math.max(0, frame))
        }
    }
    // The empty row above the video rows / below the audio rows while a media
    // tile is dragged: a clip dropped there lands on a new track of that kind.
    // It looks like any empty track, and the clip shows on it as on a track.
    component NewTrackBand: DropArea {
        id: band
        property bool audio: false
        width: parent ? parent.width : 0
        height: win.mediaDragActive ? win.hLaneRow : 0
        visible: height > 0
        keys: ["omaedit/clip"]
        onEntered: function (drag) { timelineScroll.previewClip(drag.source, band, drag.x) }
        onPositionChanged: function (drag) { timelineScroll.previewClip(drag.source, band, drag.x) }
        onExited: timelineScroll.clearDrop()
        onDropped: function (drop) {
            var frame = Math.max(0, timelineScroll.dropFrame)
            timelineScroll.clearDrop()
            drop.accept(Qt.CopyAction)
            backend.viewTimeline()
            if (drop.source.sequenceIndex >= 0)
                backend.dropSequenceAt(drop.source.sequenceIndex, backend.tracks.length,
                                       frame, band.audio)
            else
                backend.dropClipAt(drop.source.libraryIndex, backend.tracks.length,
                                   frame, band.audio)
        }
        LaneBand { anchors.fill: parent }
        DropGhost {
            live: band.containsDrag
            pictures: !band.audio
            y: 2
            height: parent.height - 4
        }
    }
    // The clip itself where it will land: from its first frame, for its length.
    component DropGhost: Rectangle {
        id: dropGhost
        objectName: "dropGhost"
        property bool live: false
        // A picture for the video tracks; the audio tracks show the name alone.
        property bool pictures: true
        visible: live && timelineScroll.dropFrame >= 0
                 && timelineScroll.dropKind === "clip"
        x: timelineScroll.shownFrames > 0
           ? timelineScroll.dropFrame / timelineScroll.shownFrames * parent.width : 0
        width: timelineScroll.shownFrames > 0
               ? Math.max(6, timelineScroll.dropFrames / timelineScroll.shownFrames
                             * parent.width)
               : parent.width * 0.25
        radius: Ui.Theme.rControl
        color: Ui.Theme.controlBg
        border.width: Ui.Theme.selectionRing
        border.color: Ui.Theme.dropValid
        clip: true
        z: 15
        Row {
            anchors.fill: parent
            anchors.margins: dropGhost.border.width
            visible: dropGhost.pictures && timelineScroll.dropThumb !== ""
            readonly property real tile: Math.max(16, height * 16 / 9)
            Repeater {
                model: dropGhost.visible ? Math.min(60, Math.ceil(dropGhost.width / parent.tile)) : 0
                Image {
                    objectName: "dropGhostPicture"
                    source: timelineScroll.dropThumb
                    width: parent.tile
                    height: parent.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: Ui.Theme.transitionWash
        }
        Rectangle {
            x: Ui.Theme.s1
            y: Ui.Theme.s1
            width: Math.min(parent.width - 2 * x, ghostName.implicitWidth + 2 * Ui.Theme.s1)
            height: ghostName.implicitHeight
            visible: timelineScroll.dropName !== ""
            radius: Ui.Theme.rControl
            color: Ui.Theme.scrimLight
            Text {
                id: ghostName
                x: Ui.Theme.s1
                width: parent.width - 2 * x
                text: timelineScroll.dropName
                elide: Text.ElideMiddle
                font.family: Ui.Theme.fontFamily
                font.pixelSize: Ui.Theme.fsLabel
                color: Ui.Theme.textPrimary
            }
        }
    }

    Connections {
        target: backend
        function onPositionChanged() {
            if (backend.playing && timelineScroll.shownFrames > 1)
                timelineScroll.keepInView(backend.position
                                          / timelineScroll.shownFrames
                                          * timelineScroll.contentWidth)
        }
    }

    Column {
    id: laneColumn
    width: timelineScroll.contentWidth
    spacing: 4

    // Ruler: hover skims, click parks the playhead, right-click parks it
    // and opens the marker menu. The ruler reports frames; the seam here
    // decides what a seek means.
    Edit.TimeRuler {
        id: ruler
        viewportX: timelineScroll.contentX
        viewportWidth: timelineScroll.width
        rangeIn: win.timelineRangeIn
        rangeOut: win.timelineRangeOut
        showFrames: win.rulerFrames
        // Made again when the starting timecode or the rate changes, so every
        // timecode already drawn with it is drawn again.
        timecodeOf: backend.startFrame >= 0 && backend.fps >= 0 ? function (frame) { return backend.timecode(frame) } : null
        onRangeEdited:function(first,last){win.timelineRangeIn=first;win.timelineRangeOut=last}
        objectName: "timeRuler"
        width: parent.width
        // Pinned to the top while the tracks scroll up beneath it, so the
        // time and the playhead's head stay in reach at any depth.
        z: 30
        transform: Translate { y: timelineScroll.contentY }
        duration: backend.hasVideo ? timelineScroll.shownFrames : 0
        fps: backend.fps
        markers: backend.markers
        renderRanges: backend.backgroundRender.ranges || []
        renderedRanges: backend.backgroundRender.readyRanges || []
        skimEnabled: win.skimEnabled
        snapEnabled: win.snapEnabled
        skimming: win.skimEnabled && !backend.playing
        interactive: backend.hasVideo
        onSkimmedTo: function (frame) {
            backend.viewTimeline() // dragging in the ruler means the edit
            backend.seek(frame)
        }
        // Skimming shows the frame under the pointer; the playhead stays.
        onHoverSkimmed: function (frame) {
            backend.viewTimeline()
            timelineScroll.skimFrame = frame
            backend.skim(frame)
        }
        onHoverEnded: {
            timelineScroll.skimFrame = -1
            backend.endSkim()
        }
        onSkimmingChanged: if (!skimming) { timelineScroll.skimFrame = -1; backend.endSkim() }
        onPressedAt: function (frame) {
            backend.viewTimeline()
            backend.pause()
            backend.seek(frame)
        }
        onRightPressedAt: function (frame) {
            backend.viewTimeline()
            backend.pause()
            backend.seek(frame)
            rulerContextMenu.popup()
        }
    }


    // A track's background band: panel colour under a hairline, one per row.
    component LaneBand: Rectangle {
        width: parent ? parent.width : 0
        height: win.hLaneRow
        color: Ui.Theme.panelBg
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Ui.Theme.hairline
            color: Ui.Theme.border
        }
    }

    NewTrackBand { objectName: "newVideoTrackDrop"; audio: false }

    // Video connects ride ABOVE the storyline; blocks sit at
    // absolute positions, drag sideways to move one.
    Item {
        id: connectedLane
        objectName: "videoLaneStrip"
        width: parent.width
        height: visible ? win.lanes.laneHeight(false) : 0
        visible: win.videoLaneRows > 0

        Repeater { // one lane band per row
            model: win.videoLaneRows
            LaneBand { required property int index; y: win.lanes.laneY(win.laneKinds.video[index],false); height:win.lanes.trackHeightFor(win.laneKinds.video[index]) }
        }
        Repeater {
            model: ClipWindow { source: backend.connected; kind: "video"; pinned: timelineScroll.pinnedConnected }
            delegate: LaneClipBlock { scroller: timelineScroll; strip: connectedLane }
        }
        LaneDrop {
            strip: connectedLane
            // Row r from the top is laneRow (videoLaneRows - r).
            trackAt: function (y) {
                return win.lanes.laneTrackAt(y,false)
            }
        }
        DropGhost {
            readonly property int laneRow:
                win.laneKinds.video.indexOf(timelineScroll.dropTrack) + 1
            live: laneRow >= 1
            y: win.lanes.laneY(timelineScroll.dropTrack,false) + 2
            height: win.lanes.trackHeightFor(timelineScroll.dropTrack) - 4
        }
    }

    // Primary storyline.
    Item {
        id: storyline
        objectName: "storyline"
        width: parent.width
        height: win.lanes.trackHeightFor(0)

        // While a clip block is being dragged sideways, the index
        // it would land on.
        property int dropTargetIndex: -1

        LaneBand { height: storyline.height } // V1's band

        Ui.EmptyState { // an existing timeline with nothing on V1 yet
            visible: backend.hasTimeline && !backend.hasVideo
            anchors.fill: parent
            iconName: "film"
            title: backend.library.length === 0
                   ? qsTr("Your edit will appear here")
                   : qsTr("Select a clip in the Library and press E to start your edit")
        }

        // The frame under a storyline x, floor-mapped like the ruler.
        function frameAt(x) {
            return Math.max(0, Math.min(timelineScroll.shownFrames - 1,
                Math.floor(x / width * timelineScroll.shownFrames)))
        }
        function indexAt(x) {
            var f = Math.max(0, Math.min(1, x / width)) * timelineScroll.shownFrames
            for (var i = 0; i < backend.timeline.length; ++i) {
                var c = win.snapping.drawn(backend.timeline[i])
                if (f < c.start + c.frames)
                    return i
            }
            return backend.timeline.length - 1
        }

        Item { // clips sit at their absolute start — empty space is a gap
            anchors.fill: parent

            Repeater {
                model: ClipWindow { source: backend.timeline; pinned: timelineScroll.pinnedStory }

                delegate: StoryClipBlock { scroller: timelineScroll; story: storyline }
            }
        }

        LaneDrop {
            strip: storyline
            trackAt: function (y) { return 0 }
        }
        DropGhost {
            live: timelineScroll.dropTrack === 0
            y: 2
            height: parent.height - 4
        }

        // Timeline range selection — a yellow-bordered span;
        // drag the edges to adjust, Del ripples it out.
        Rectangle {
            id: rangeOverlay
            visible: win.hasTimelineRange && backend.hasVideo
            x: win.effRangeIn / Math.max(1, timelineScroll.shownFrames)
               * parent.width
            width: Math.max(2,
                (Math.min(win.effRangeOut, timelineScroll.shownFrames - 1)
                 - win.effRangeIn + 1)
                / Math.max(1, timelineScroll.shownFrames) * parent.width)
            height: parent.height
            color: Ui.Theme.rangeFill
            border.color: Ui.Theme.rangeStroke
            border.width: Ui.Theme.selectionRing
            radius: Ui.Theme.rControl

            MouseArea { // drag the range's start edge
                x: -5
                width: 10
                height: parent.height
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                onPositionChanged: function (mouse) {
                    if (!pressed)
                        return
                    // Frame under the cursor, same floor-based
                    // map as the ruler and clip blocks.
                    var f = Math.floor(
                        mapToItem(storyline, mouse.x, 0).x
                        / storyline.width * timelineScroll.shownFrames)
                    f = win.snapping.edge(f,
                        storyline.width
                        / Math.max(1, timelineScroll.shownFrames), -1, -1)
                    win.timelineRangeIn =
                        Math.max(0, Math.min(f, win.effRangeOut))
                }
            }
            MouseArea { // drag the range's end edge
                x: parent.width - 5
                width: 10
                height: parent.height
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                onPositionChanged: function (mouse) {
                    if (!pressed)
                        return
                    // The out edge is the boundary AFTER the
                    // frame under the cursor; snap in boundary
                    // space (clip start b → out = b−1) so a
                    // snapped range stops short of the clip.
                    var b = Math.floor(
                        mapToItem(storyline, mouse.x, 0).x
                        / storyline.width * timelineScroll.shownFrames) + 1
                    b = win.snapping.edge(b,
                        storyline.width
                        / Math.max(1, timelineScroll.shownFrames), -1, -1)
                    win.timelineRangeOut =
                        Math.max(win.effRangeIn,
                                 Math.min(b - 1,
                                          timelineScroll.shownFrames - 1))
                }
            }
        }

    }

    // Audio connects hang BELOW the storyline (guide p. 142):
    // Embedded audio: a V1 clip still carrying its sound inside the block
    // (a project from before linked audio, until its first edit; a nested
    // sequence) shows it here as a mirror row. Every ordinary clip has its
    // sound on a real audio track (A1…) instead, linked to the video.
    Item {
        id: attachedLane
        objectName: "attachedAudioStrip"
        width: parent.width
        height: visible ? win.hLaneRow : 0
        visible: backend.hasVideo && win.embeddedAudioCount > 0

        LaneBand { height: attachedLane.height }

        // Blocks narrower than a readable width carry no volume line, label or
        // badge, so at overview zoom they are drawn together here and hit-tested
        // by position instead of each getting a delegate. Selected blocks are
        // pinned and keep theirs. A fit view of a long edit is thousands wide.
        // Matches the block's own detail threshold (TimelineClip.detailed).
        readonly property real narrowFrames: 24 * Math.max(1, timelineScroll.shownFrames) / Math.max(1, storyline.width)
        function carries(e) {
            return e.gap !== true && e.videoOnly !== true && e.silent !== true
                   && (e.title === undefined || e.title === "")
        }
        // Lane x → source index of a narrow block, or -1. Slivers widen to
        // 6 px and overlap; the later block is on top, as with delegates.
        function narrowAt(x) {
            // The delegates' own arithmetic, so boundaries agree exactly.
            const duration = Math.max(1, timelineScroll.shownFrames), span = storyline.width
            const list = backend.timeline
            for (let i = list.length - 1; i >= 0; --i) {
                const e = list[i]
                const block = win.snapping.drawn(e) // as the model lays the delegates
                if (!(block.frames < narrowFrames) || !carries(e)) continue
                const left = block.start / duration * span
                if (x >= left && x < left + Math.max(6, block.frames / duration * span)) return i
            }
            return -1
        }
        Canvas {
            id: audioOverview
            objectName: "attachedAudioOverview"
            x: Math.max(0, timelineScroll.contentX)
            y: 2
            width: Math.max(1, Math.min(timelineScroll.width, attachedLane.width - x))
            height: win.hLaneClip
            readonly property var entries: backend.timeline
            readonly property var pins: timelineScroll.pinnedStory
            readonly property real span: storyline.width
            readonly property real duration: Math.max(1, timelineScroll.shownFrames)
            readonly property real narrow: attachedLane.narrowFrames
            onEntriesChanged: requestPaint()
            onPinsChanged: requestPaint()
            onSpanChanged: requestPaint()
            onDurationChanged: requestPaint()
            onNarrowChanged: requestPaint()
            onXChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Connections { target: Ui.Theme; function onAccentChanged() { audioOverview.requestPaint() } }
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                const pinned = {}
                for (let p = 0; p < pins.length; ++p) pinned[pins[p]] = true
                for (let i = 0; i < entries.length; ++i) {
                    const e = entries[i]
                    const block = win.snapping.drawn(e) // as the model lays the delegates
                    if (!(block.frames < narrow) || pinned[i] || !attachedLane.carries(e)) continue
                    const left = block.start / duration * span - x, w = Math.max(6, block.frames / duration * span)
                    if (left + w < 0 || left > width) continue
                    // As a narrow block paints itself: its tint (grey when
                    // disabled), a hairline, an accent ring over transitions.
                    const tint = e.disabled === true ? Ui.Theme.clipGap
                               : e.label !== undefined && e.label !== "" ? Ui.Theme.labelColor(e.label)
                               : Ui.Theme.clipAudio
                    const transition = e.fadeIn > 0 || e.fadeOut > 0 || e.mixOut > 0
                    ctx.fillStyle = String(tint)
                    ctx.fillRect(left, 0, w, height)
                    ctx.lineWidth = transition ? Ui.Theme.selectionRing : Ui.Theme.hairline
                    ctx.strokeStyle = String(transition ? Ui.Theme.accent : Qt.darker(tint, 1.5))
                    ctx.strokeRect(left + ctx.lineWidth / 2, ctx.lineWidth / 2,
                                   w - ctx.lineWidth, height - ctx.lineWidth)
                }
            }
        }
        MouseArea { // the drawn blocks select and open the menu like delegates
            objectName: "attachedAudioOverviewArea"
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            enabled: win.tool === "select" || win.tool === "trackselect"
            property int target: -1
            onPressed: function (mouse) {
                target = attachedLane.narrowAt(mouse.x)
                mouse.accepted = target >= 0 // empty lane space stays click-through
            }
            onClicked: function (mouse) {
                if (target < 0) return
                backend.viewTimeline()
                win.selectedEdge = -1
                if (mouse.button === Qt.RightButton) {
                    win.selection.promote(0, target)
                    clipContextMenu.popup()
                    return
                }
                win.selection.clickSelect(0, target, mouse.modifiers, win.selectedTimelineIndex === target)
            }
        }

        Repeater {
            model: ClipWindow { source: backend.timeline; kind: "attached"; pinned: timelineScroll.pinnedStory; active: timelineScroll.visible && attachedLane.visible; narrowFrames: attachedLane.narrowFrames }
            Edit.TimelineClip {
                fps: backend.fps > 0 ? backend.fps : 25
                id: audioBlock
                required property var modelData
                required property int sourceIndex
                // The block's own place, as its picture's above (see StoryClipBlock).
                required property real timelineStart
                required property real timelineFrames
                required property int blockLead
                required property int blockTail
                required property int blockBefore
                readonly property int index: sourceIndex
                objectName: "attachedAudio:" + index
                readonly property bool carries: modelData.gap !== true
                                                && modelData.videoOnly !== true && modelData.silent !== true
                                                && (modelData.title === undefined || modelData.title === "")
                // Follows a pending trim of its picture above, or the roll
                // of the cut at either of its ends.
                readonly property bool trimming: timelineScroll.trimIndex === index
                readonly property int pendingIn: trimming ? timelineScroll.trimIn : timelineScroll.rollOf(0, index, false)
                readonly property int pendingOut: trimming ? timelineScroll.trimOut : timelineScroll.rollOf(0, index, true)
                readonly property int pendingShift: trimming && !timelineScroll.trimMovesStart ? 0 : pendingIn
                x: timelineScroll.shownFrames > 0
                   ? (timelineStart + pendingShift) / timelineScroll.shownFrames * storyline.width : 0
                y: 2
                width: timelineScroll.shownFrames > 0
                       ? Math.max(6, (timelineFrames - pendingIn + pendingOut)
                                     / timelineScroll.shownFrames * storyline.width) : 0
                height: win.hLaneClip
                readonly property bool inView: timelineScroll.spans(x, width)
                visible: carries && (inView || selected)
                selected: win.selectedConnectedIndex < 0
                          && (index === win.selectedTimelineIndex || win.selection.isSelectedAt(0, index))
                edge: index === win.selectedTimelineIndex ? win.selectedEdge : -1
                name: modelData.name
                kind: "audio"
                label: modelData.label !== undefined ? modelData.label : ""
                categoryColor: win.categories.colorOf(modelData)
                disabled: modelData.disabled === true
                readonly property var decor: {
                    const revision = backend.decorationRevision
                    return inView && detailed ? backend.timelineDecoration(false, index) : ({})
                }
                waveEntry: inView && detailed ? modelData : null
                waveRectified: win.rectifiedWaveforms
                // The clip as its picture's drag would leave it: with ripple
                // its start stays and the sound comes up under it; a slip
                // moves the sound under a block that stays.
                wavePending: pendingIn !== 0 || pendingOut !== 0
                             ? ({ "start": modelData.start + pendingShift,
                                  "in": modelData.in + pendingIn,
                                  "frames": modelData.frames - pendingIn + pendingOut })
                             : timelineScroll.slipIndex === index ? ({ "in": modelData.in + timelineScroll.slipBy })
                             : ({})
                waveStart: timelineStart + pendingShift
                waveFrames: timelineFrames - pendingIn + pendingOut
                // Only the part on screen is painted (see TimelineClip.viewFrom).
                viewFrom: timelineScroll.contentX - x
                viewTo: timelineScroll.contentX + timelineScroll.width - x
                fadeInFrac: (modelData.fadeIn !== undefined ? modelData.fadeIn : 0) / Math.max(1, timelineFrames)
                fadeOutFrac: (modelData.fadeOut !== undefined ? modelData.fadeOut : 0) / Math.max(1, timelineFrames)
                mixOutFrac: Math.max(0, (modelData.mixOut !== undefined ? modelData.mixOut : 0) - blockTail) / Math.max(1, timelineFrames)
                mixOutPastFrac: blockTail / Math.max(1, timelineFrames)
                mixInFrac: blockLead / Math.max(1, timelineFrames)
                mixInBeforeFrac: blockBefore / Math.max(1, timelineFrames)
                mixInLength: blockLead + blockBefore
                badge: detailed ? win.sound.badge(modelData) : ""
                onTransitionRemoveRequested:function(which){
                    const e=modelData
                    if(which==="dissolve")backend.toggleDissolve(index)
                    else if(which==="dissolveIn")backend.toggleDissolve(index-1)
                    else backend.setFadeLengths(index,which==="in"?0:e.fadeIn,which==="out"?0:e.fadeOut)
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    enabled: win.tool === "select" || win.tool === "trackselect"
                    onClicked: function (mouse) {
                        backend.viewTimeline()
                        if (mouse.button === Qt.RightButton) {
                            if (audioBlock.openTransitionMenuAt(mapToItem(audioBlock, mouse.x, 0).x))
                                return
                            win.selectedEdge = -1
                            win.selection.promote(0, audioBlock.index)
                            clipContextMenu.popup()
                            return
                        }
                        win.selectedEdge = -1
                        win.selection.clickSelect(0, audioBlock.index, mouse.modifiers,
                                        win.selectedTimelineIndex === audioBlock.index)
                    }
                }
                // The volume line needs room to be read or dragged: a fit
                // view of a long edit makes it only on wide or selected
                // blocks instead of on thousands of slivers.
                Loader {
                    // Over the entry's own frames (see LaneClipBlock).
                    x: audioBlock.width * audioBlock.blockLead / Math.max(1, audioBlock.timelineFrames)
                    width: audioBlock.width * audioBlock.modelData.frames / Math.max(1, audioBlock.timelineFrames)
                    height: parent.height
                    active: audioBlock.detailed || audioBlock.selected
                    sourceComponent: VolumeLine {
                        entry: audioBlock.modelData
                        clipIndex: audioBlock.index
                        lane: false
                    }
                }
            }
        }
    }

    // audio-only clips and detached audio land here.
    Item {
        id: audioLane
        objectName: "audioLaneStrip"
        width: parent.width
        height: visible ? win.lanes.laneHeight(true) : 0
        visible: win.audioLaneRows > 0

        Repeater { // one lane band per row
            model: win.audioLaneRows
            LaneBand { required property int index; y: win.lanes.laneY(win.laneKinds.audio[index],true); height:win.lanes.trackHeightFor(win.laneKinds.audio[index]) }
        }
        LaneDrop {
            strip: audioLane
            trackAt: function (y) {
                return win.lanes.laneTrackAt(y,true)
            }
        }
        DropGhost {
            readonly property int laneRow:
                win.laneKinds.audio.indexOf(timelineScroll.dropTrack) + 1
            live: laneRow >= 1
            pictures: false
            y: win.lanes.laneY(timelineScroll.dropTrack,true) + 2
            height: win.lanes.trackHeightFor(timelineScroll.dropTrack) - 4
        }
        Repeater {
            model: ClipWindow { source: backend.connected; kind: "audio"; pinned: timelineScroll.pinnedConnected }
            delegate: LaneClipBlock {
                scroller: timelineScroll
                strip: audioLane
                below: true
            }
        }
    }

    NewTrackBand { objectName: "newAudioTrackDrop"; audio: true }
    } // Column

    // ── Marquee ─────────────────────────────────────────────────────────
    // With the Select tool, a drag on empty timeline space sweeps every
    // block it touches into the selection (Ctrl/Shift: added to it); a
    // plain click on empty space selects the gap there, or clears the
    // selection where there is none; a right-click on a gap selects it and
    // offers to delete it. Behind the strips, so any block's own handlers
    // win where a block is.
    MouseArea {
        id: marqueeArea
        objectName: "marqueeArea"
        anchors.fill: laneColumn
        z: -1
        enabled: win.tool === "select" && !win.mediaDragActive
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property real x0: 0
        property real y0: 0
        property bool sweeping: false
        onPressed: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                var gap = timelineScroll.gapUnder(mouse.x, mouse.y)
                if (gap === null)
                    return
                backend.viewTimeline()
                win.selection.selectGap(gap)
                gapContextMenu.popup()
                return
            }
            x0 = mouse.x
            y0 = mouse.y
            sweeping = false
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            if (!sweeping && (Math.abs(mouse.x - x0) > 6
                              || Math.abs(mouse.y - y0) > 6))
                sweeping = true
            if (!sweeping)
                return
            marqueeRect.x = Math.min(x0, mouse.x)
            marqueeRect.y = Math.min(y0, mouse.y)
            marqueeRect.width = Math.abs(mouse.x - x0)
            marqueeRect.height = Math.abs(mouse.y - y0)
        }
        onReleased: function (mouse) {
            if (mouse.button === Qt.RightButton)
                return
            var add = (mouse.modifiers
                       & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0
            if (!sweeping) {
                if (!add) {
                    var gap = timelineScroll.gapUnder(mouse.x, mouse.y)
                    if (gap !== null)
                        win.selection.selectGap(gap)
                    else
                        win.selection.clearSelection()
                }
                return
            }
            sweeping = false
            var hits = timelineScroll.blocksIn(marqueeRect.x, marqueeRect.y,
                                               marqueeRect.width, marqueeRect.height)
            win.selection.selectAddresses(add ? win.selection.selectedAddresses().concat(hits) : hits)
        }
    }
    Rectangle {
        id: marqueeRect
        objectName: "marqueeRect"
        visible: marqueeArea.sweeping
        z: 40
        color: Qt.rgba(Ui.Theme.accent.r, Ui.Theme.accent.g, Ui.Theme.accent.b, 0.15)
        border.color: Ui.Theme.accent
        border.width: 1
    }
    // The selected gap, outlined on its own track.
    Rectangle {
        objectName: "gapMark"
        readonly property var gap: win.selectedGap
        readonly property var row: gap !== null ? timelineScroll.rowOf(gap.track) : null
        visible: row !== null && timelineScroll.shownFrames > 0
        x: visible ? gap.start / timelineScroll.shownFrames * timelineScroll.contentWidth : 0
        y: visible ? row.y + 2 : 0
        width: visible ? Math.max(2, gap.frames / timelineScroll.shownFrames * timelineScroll.contentWidth) : 0
        height: visible ? Math.max(2, row.height - 4) : 0
        z: 39
        radius: Ui.Theme.rControl
        color: Qt.rgba(Ui.Theme.accent.r, Ui.Theme.accent.g, Ui.Theme.accent.b, 0.18)
        border.color: Ui.Theme.accent
        border.width: 1.5
    }
    // The track whose row covers a column-space y, or -1.
    function trackAtY(y) {
        if (storyline.visible && y >= storyline.y && y < storyline.y + storyline.height)
            return 0
        if (connectedLane.visible && y >= connectedLane.y && y < connectedLane.y + connectedLane.height)
            return win.lanes.laneTrackAt(y - connectedLane.y, false)
        if (audioLane.visible && y >= audioLane.y && y < audioLane.y + audioLane.height)
            return win.lanes.laneTrackAt(y - audioLane.y, true)
        return -1
    }
    // Under the Trim tool, what a press on a clip block does, by where it
    // lands: within 12 px of an edge, ripple that edge ("in"/"out") — or,
    // within 5 px of a cut its neighbour butts against, roll that cut
    // ("rollIn"/"rollOut"); on the name bar along the top, "slide"; on the
    // rest of the body, "slip".
    function trimZone(track, index, x, y, w, h) {
        var e = win.selection.trackEntry(track, index)
        if (e === null)
            return "slip"
        var edge = Math.min(12, w / 3)
        var before = win.selection.trackEntry(track, index - 1), after = win.selection.trackEntry(track, index + 1)
        if (x < edge)
            return before !== null && before.start + before.frames === e.start && x < 5 ? "rollIn" : "in"
        if (x > w - edge)
            return after !== null && after.start === e.start + e.frames && x > w - 5 ? "rollOut" : "out"
        return y < Math.min(18, h * 0.35) ? "slide" : "slip"
    }
    // With the Select tool the cut between two clips that butt together is
    // itself a thing to drag: within `cutReach` pixels of it, on either
    // block, a press moves the cut (a roll) as the Trim tool does there. The
    // reach is the Trim tool's five pixels, and never more than a sixth of
    // the block it is on, so a narrow block keeps its edges and its body.
    function cutReachFor(w) { return Math.min(5, w / 6) }
    // How many pixels at a block's start (incoming) or end are the cut's:
    // none where no clip butts against that edge.
    function cutReach(track, index, incoming, w) {
        var e = win.selection.trackEntry(track, index)
        var other = win.selection.trackEntry(track, incoming ? index - 1 : index + 1)
        if (e === null || other === null || e.gap === true || other.gap === true)
            return 0
        if (incoming ? other.start + other.frames !== e.start : other.start !== e.start + e.frames)
            return 0
        return cutReachFor(w)
    }
    // What a press on a clip block does under the Select tool, by where it
    // lands: "rollIn" / "rollOut" on a cut, "" anywhere else.
    function cutZone(track, index, x, w) {
        if (x < cutReach(track, index, true, w))
            return "rollIn"
        return x >= w - cutReach(track, index, false, w) && cutReach(track, index, false, w) > 0 ? "rollOut" : ""
    }
    // The cuts of every track, for the pointer: found once after each edit
    // and looked up by position, so moving the mouse over a long timeline
    // does not walk its clips. track → [{frame, before, after}] in order,
    // the cut's frame as drawn and the frames of the block either side.
    readonly property var cutStore: ({ cuts: null })
    function cutsOf(track) {
        if (cutStore.cuts === null) {
            const byTrack = {}
            const read = function (list) {
                const count = list.length
                for (let i = 0; i < count; ++i) {
                    const e = list[i]
                    if (byTrack[e.track] === undefined) byTrack[e.track] = []
                    byTrack[e.track].push(e)
                }
            }
            read(backend.timeline)
            read(backend.connected)
            const cuts = {}
            for (const key in byTrack) {
                const clips = byTrack[key].sort(function (p, q) { return p.index - q.index })
                const found = []
                let lead = 0 // the block's frames before its entry's start
                for (let i = 0; i < clips.length; ++i) {
                    const e = clips[i], tail = win.snapping.mixTail(e)
                    const frames = e.frames + lead - tail
                    if (found.length > 0 && found[found.length - 1].pending) {
                        found[found.length - 1].after = frames
                        found[found.length - 1].pending = false
                    }
                    const next = i + 1 < clips.length ? clips[i + 1] : null
                    lead = next !== null && next.mixIn > 0 ? tail : 0
                    if (next !== null && next.index === e.index + 1 && e.gap !== true && next.gap !== true
                            && next.start === e.start + e.frames)
                        found.push({ frame: next.start - lead, before: frames, after: 0, pending: true })
                }
                cuts[key] = found
            }
            cutStore.cuts = cuts
        }
        const list = cutStore.cuts[track]
        return list === undefined ? [] : list
    }
    Connections {
        target: backend
        function onTimelineEntriesChanged() { timelineScroll.cutStore.cuts = null }
    }
    // cutZone for a column-space point ("" off every cut).
    function cutZoneAt(x, y) {
        const track = trackAtY(y)
        if (track < 0 || timelineScroll.shownFrames <= 0)
            return ""
        const cuts = cutsOf(track), perFrame = timelineScroll.contentWidth / timelineScroll.shownFrames
        // The first cut at or after the point, and the one before it.
        let low = 0, high = cuts.length
        while (low < high) {
            const mid = (low + high) >> 1
            if (cuts[mid].frame * perFrame < x) low = mid + 1
            else high = mid
        }
        if (low < cuts.length && cuts[low].frame * perFrame - x <= cutReachFor(Math.max(6, cuts[low].before * perFrame)))
            return "rollOut"
        if (low > 0 && x - cuts[low - 1].frame * perFrame < cutReachFor(Math.max(6, cuts[low - 1].after * perFrame)))
            return "rollIn"
        return ""
    }
    // trimZone for the clip block under a column-space point ("" off
    // every block), from the same geometry blocksIn draws with.
    // The block under a column-space point, with its column-space box:
    // {track, index, x, y, width, height}, or null.
    function blockBoxAt(x, y) {
        var hits = blocksIn(x, y, 1, 1)
        if (hits.length === 0 || timelineScroll.shownFrames <= 0)
            return null
        var e = win.selection.trackEntry(hits[0].track, hits[0].index)
        if (e === null)
            return null
        var W = timelineScroll.contentWidth
        var block = win.snapping.drawn(e)
        var box = { track: e.track, index: e.index,
                    x: block.start / timelineScroll.shownFrames * W, width: block.frames / timelineScroll.shownFrames * W }
        if (e.track === 0) {
            box.y = storyline.y
            box.height = storyline.height
        } else {
            var audio = e.audioOnly === true
            box.y = (audio ? audioLane.y : connectedLane.y) + win.lanes.laneY(e.track, audio) + 2
            box.height = win.lanes.trackHeightFor(e.track) - 4
        }
        return box
    }
    function trimZoneAt(x, y) {
        var box = blockBoxAt(x, y)
        return box === null ? "" : trimZone(box.track, box.index, x - box.x, y - box.y, box.width, box.height)
    }
    // The video row (V1 = 0, lanes 1.., one past the top = a new track)
    // under a column-space y, held at V1 below it.
    function videoRowAt(y) {
        if (y >= storyline.y - laneColumn.spacing)
            return 0
        var track = win.lanes.laneTrackAt(y - connectedLane.y, false)
        return track >= 0 ? win.lanes.videoRowOf(track)
             : y < connectedLane.y ? win.videoLaneRows + 1 : 1
    }
    // The column-space top of a video row (one past the top: a row above).
    function videoRowY(row) {
        if (row <= 0)
            return storyline.y
        // With no track above V1 the lanes' row is hidden and holds no place
        // in the column: a new track's row is the one right above V1.
        if (win.videoLaneRows === 0)
            return storyline.y - win.hLaneRow - laneColumn.spacing
        var track = win.lanes.videoTrackAtRow(row)
        return track < backend.tracks.length && row <= win.videoLaneRows
            ? connectedLane.y + win.lanes.laneY(track, false)
            : connectedLane.y - win.hLaneRow
    }
    // A track's row in column space: {y, height}, or null when hidden.
    function rowOf(track) {
        if (track === 0)
            return storyline.visible ? { y: storyline.y, height: storyline.height } : null
        var audio = win.laneKinds.audio.indexOf(track) >= 0
        if (!audio && win.laneKinds.video.indexOf(track) < 0)
            return null
        return { y: (audio ? audioLane.y : connectedLane.y) + win.lanes.laneY(track, audio),
                 height: win.lanes.trackHeightFor(track) }
    }
    // The empty space between two clips under a column-space point, as
    // {track, start, frames}; null over a clip, past the last clip, or off
    // the tracks — the same span the engine closes.
    function gapUnder(x, y) {
        var track = trackAtY(y)
        if (track < 0 || timelineScroll.shownFrames <= 0)
            return null
        var frame = Math.floor(x / timelineScroll.contentWidth * timelineScroll.shownFrames)
        var list = track === 0 ? backend.timeline : backend.connected
        var start = 0, end = -1
        for (var i = 0; i < list.length; i++) {
            var c = list[i]
            if (c.gap === true || (track !== 0 && c.track !== track))
                continue
            if (frame >= c.start && frame < c.start + c.frames)
                return null
            if (c.start + c.frames <= frame)
                start = Math.max(start, c.start + c.frames)
            else if (end < 0 || c.start < end)
                end = c.start
        }
        return end > start ? { track: track, start: start, frames: end - start } : null
    }
    // The addresses of every block a column-space rectangle touches, in
    // the same frame→pixel and row→y maps the blocks draw with.
    function blocksIn(x, y, w, h) {
        var out = []
        if (timelineScroll.shownFrames <= 0)
            return out
        var W = timelineScroll.contentWidth
        function hit(bx, by, bw, bh) {
            return bx < x + w && bx + bw > x && by < y + h && by + bh > y
        }
        for (var i = 0; i < backend.timeline.length; i++) {
            var c = backend.timeline[i]
            if (c.gap === true)
                continue
            var cb = win.snapping.drawn(c)
            if (hit(cb.start / timelineScroll.shownFrames * W, storyline.y,
                    cb.frames / timelineScroll.shownFrames * W, storyline.height))
                out.push({ track: 0, index: i })
        }
        const lanes = backend.connected, count = lanes.length, places = win.snapping.lanesByPlace(lanes)
        for (var j = 0; j < count; j++) {
            var e = lanes[j]
            var audio = e.audioOnly === true
            var list = audio ? win.laneKinds.audio : win.laneKinds.video
            var row = list.indexOf(e.track) + 1
            if (row < 1)
                continue
            var by = (audio ? audioLane.y : connectedLane.y) + win.lanes.laneY(e.track,audio) + 2
            var eb = win.snapping.drawn(e, places)
            if (hit(eb.start / timelineScroll.shownFrames * W, by,
                    eb.frames / timelineScroll.shownFrames * W, win.lanes.trackHeightFor(e.track)-4))
                out.push({ track: e.track, index: e.index })
        }
        return out
    }

    // The skim line: the frame under the pointer while skimming.
    Rectangle {
        objectName: "skimLine"
        visible: timelineScroll.skimFrame >= 0 && timelineScroll.shownFrames > 0 && !backend.playing
        x: timelineScroll.shownFrames > 0 ? timelineScroll.skimFrame / timelineScroll.shownFrames * timelineScroll.contentWidth : 0
        y: timelineScroll.contentY
        width: Ui.Theme.hairline
        height: timelineScroll.height
        color: Ui.Theme.playhead
        opacity: 0.45
    }

    // The beats of mapped music (View ▸ Show Beat Map): lines down every
    // track in view, over the clips and under the playhead, so a cut on any
    // track can be laid against a beat. One item the size of the view; it
    // takes no mouse input, and a click on a line is a click on the clip
    // under it.
    BeatLinesItem {
        id: beatLines
        objectName: "beatLines"
        visible: win.showBeatMap && win.hasBeatLines && timelineScroll.shownFrames > 0
        x: timelineScroll.contentX
        y: timelineScroll.contentY + ruler.height
        width: timelineScroll.width
        height: Math.max(0, timelineScroll.height - ruler.height)
        z: 28
        lines: visible ? win.beatLines : []
        firstFrame: timelineScroll.contentX * timelineScroll.framesPerPixel
        pxPerFrame: 1 / Math.max(1e-9, timelineScroll.framesPerPixel)
        // From the top of the highest track to the foot of the lowest.
        spanTop: (connectedLane.visible ? connectedLane.y : storyline.y) - y
        spanBottom: Math.max(storyline.y + storyline.height,
                             attachedLane.visible ? attachedLane.y + attachedLane.height : 0,
                             audioLane.visible ? audioLane.y + audioLane.height : 0) - y
        // Each track's row, where its clips' blocks lie (two pixels in).
        rows: {
            const out = [], tracks = [0].concat(win.laneKinds.video, win.laneKinds.audio)
            for (let i = 0; i < tracks.length; ++i) {
                const row = timelineScroll.rowOf(tracks[i])
                if (row !== null)
                    out.push({ track: tracks[i], y: row.y + 2 - y, height: row.height - 4 })
            }
            return out
        }
        color: Ui.Theme.beatLine
        beatOpacity: Ui.Theme.beatLineAcross
        lineWidth: Ui.Theme.beatLineWidth
        barWidth: Ui.Theme.barLineWidth
        spacing: Ui.Theme.beatLineSpacing
    }

    // The playhead rides the whole stack, ruler to audio lanes, in the
    // same frame→pixel map as the blocks (its frame's left edge).
    Edit.Playhead {
        objectName: "playhead"
        visible: backend.hasVideo
        x: timelineScroll.shownFrames > 0
           ? backend.position / timelineScroll.shownFrames * timelineScroll.contentWidth
           : 0
        // Down the tracks in view, from the ruler pinned at the top.
        y: timelineScroll.contentY
        z: 31
        height: timelineScroll.height
        headHeight: ruler.height
    }

    // The blade's cut line: the frame edge a click would cut at, across
    // the clip under the pointer, so the cut is seen before it is made.
    Item {
        id: bladeLine
        objectName: "bladeCutLine"
        readonly property point pointerAt: toolPointer.mapToItem(laneColumn, toolPointer.hoverPosition.x, toolPointer.hoverPosition.y)
        readonly property var box: win.tool === "blade" && toolPointer.hovered && !backend.playing
                                   ? timelineScroll.blockBoxAt(pointerAt.x, pointerAt.y) : null
        readonly property int frame: box === null ? -1
            : Math.max(0, Math.min(timelineScroll.shownFrames - 1,
                                   Math.floor(pointerAt.x / timelineScroll.contentWidth * timelineScroll.shownFrames)))
        readonly property point lineTop: box === null ? Qt.point(0, 0)
            : laneColumn.mapToItem(bladeLine.parent, frame / timelineScroll.shownFrames * timelineScroll.contentWidth, box.y)
        visible: box !== null
        x: lineTop.x - 1
        y: lineTop.y
        width: 3
        height: box === null ? 0 : box.height
        z: 29
        Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.7 }
        Rectangle { x: 1; width: 1; height: parent.height; color: "#ffffff" }
    }

    // Hand and Zoom tools ride above every block: hand drags the view,
    // zoom clicks in (Alt or right button: out) around the frame under
    // the pointer, which stays put.
    MouseArea {
        id: toolOverlay
        objectName: "toolOverlay"
        anchors.fill: parent
        enabled: win.tool === "hand" || win.tool === "zoom"
        visible: enabled
        z: 30
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: win.tool === "hand"
                     ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                     : Qt.CrossCursor
        property real pressVx: 0
        property real pressVy: 0
        property real pressCx: 0
        property real pressCy: 0
        onPressed: function (mouse) {
            var v = toolOverlay.mapToItem(timelineScroll, mouse.x, mouse.y)
            pressVx = v.x
            pressVy = v.y
            pressCx = timelineScroll.contentX
            pressCy = timelineScroll.contentY
        }
        onPositionChanged: function (mouse) {
            if (!pressed || win.tool !== "hand")
                return
            var v = toolOverlay.mapToItem(timelineScroll, mouse.x, mouse.y)
            timelineScroll.contentX = Math.max(0, Math.min(
                pressCx - (v.x - pressVx),
                Math.max(0, timelineScroll.contentWidth - timelineScroll.width)))
            timelineScroll.contentY = Math.max(0, Math.min(
                pressCy - (v.y - pressVy),
                Math.max(0, timelineScroll.contentHeight - timelineScroll.height)))
        }
        onClicked: function (mouse) {
            if (win.tool !== "zoom" || timelineScroll.shownFrames < 1)
                return
            var frame = Math.floor(mouse.x / timelineScroll.contentWidth
                                   * timelineScroll.shownFrames)
            var out = mouse.button === Qt.RightButton
                      || (mouse.modifiers & Qt.AltModifier) !== 0
            win.navigation.zoomAt(frame, out ? 1 / 1.5 : 1.5)
        }
    }

    // The active tool's pointer over every lane (blade, trim, slip, slide,
    // track select), so the pointer always says which tool is armed. It
    // takes no clicks; the blocks below still do the work.
    Ui.ToolPointer {
        id: toolPointer
        objectName: "timelineToolPointer"
        anchors.fill: parent
        z: 31
        tool: win.tool
        zoneAt: function (x, y) {
            var p = toolPointer.mapToItem(laneColumn, x, y)
            return win.tool === "trim" ? timelineScroll.trimZoneAt(p.x, p.y) : timelineScroll.cutZoneAt(p.x, p.y)
        }
        // A cut being dragged keeps the roll pointer wherever the mouse goes.
        heldZone: timelineScroll.rollIndex >= 0 && !timelineScroll.rollHeld ? "rollIn" : ""
    }
}
