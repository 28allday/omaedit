pragma ComponentBehavior: Bound
import QtQuick
import omaedit
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Edit as Edit
import "../Ui/MediaTime.js" as MediaTime

// The audio timeline: audio tracks grouped by role, each with its control
// block on the left and its clips (waveform + automation) on the right
// under a shared ruler; the Master row under them all. Pure: tracks and
// clip entries come in, every edit goes out as a signal the seam maps to
// the engine's address-based operations.
Item {
    id: root

    // ── Inputs ──────────────────────────────────────────────────────────────
    property var tracks: []              // backend.tracks
    property var gestureTarget: null
    property var gesturePreview: null
    property var snapPoints: null
    // Every beat of mapped music on the timeline, as the owner's
    // backend.beatLines gives them; empty when none are to be shown. Shown
    // or hidden, the points a drag snaps to change with them.
    property var beatLines: []
    onBeatLinesChanged: rulerSnapCache = null
    property bool gesturePending: false
    property string gestureError: ""
    onGestureErrorChanged: if (gestureError !== "") gestureProblem = gestureError
    signal gestureCommitted(var target, string action, var changes, int sampleRate)
    signal timeSeekRequested(var position, bool audition)
    property var activeGesture: null
    property string gestureProblem: ""
    property var rulerSnapCache: null
    // The active pointer edit owns Escape before the global dismiss shortcut.
    Keys.onShortcutOverride: function(event) {
        if (event.key === Qt.Key_Escape && activeGesture && !activeGesture.waiting)
            event.accepted = true
    }
    Keys.onEscapePressed: function(event) {
        if (activeGesture && !activeGesture.waiting) {
            activeGesture.cancel(); gestureProblem = ""; event.accepted = true
        }
    }
    property var clips: []               // backend.connected entries (track ≥ 1)
    property real fps: 25
    property string units: "frames"
    property var timecoder: null          // the sequence's timecode for a frame
    property int duration: 0
    property int position: 0
    property real cursorFrames: position
    property int sampleRate: 48000
    property var markers: []
    property var renderRanges: []
    property var renderedRanges: []
    property int rangeIn: -1
    property int rangeOut: -1
    property var trackLevels: []
    property var master: ({})
    property real masterLeft: -100
    property real masterRight: -100
    property bool masterMuted: false
    property int selectedTrack: -1       // engine track index; -2 = master
    property int selectedClip: -1        // connected index
    property int embeddedAudioCount: 0   // video clips carrying their own sound on V1
    property string tool: "select"
    property bool snapping: true
    property real zoom: 0                // px per frame; 0 = fit
    // Preferences ▸ Input, as the Edit timeline reads them.
    property string wheelAction: "scroll"
    property bool wheelInvert: false
    property real wheelSensitivity: 1.0
    property int rowHeight: Theme.hAudioTrack
    // Tracks dragged to their own height by the lower edge of their header.
    // The height is kept with the track itself (`mix.audioHeight`), not
    // against the row it happens to be on: it stays with that track when
    // tracks are added, removed, moved or brought back by undo, and in another
    // sequence it is that sequence's own. It is how the track is looked at,
    // as its header flags are: remembered with the project, but no undo step
    // and no change to the project (PlayerWorker::setTrackMix). The common
    // height (`rowHeight`) is used for the rest, and choosing another common
    // height puts every track back on it.
    readonly property int leastTrackHeight: Theme.hAudioGroupHeader + 8
    readonly property int mostTrackHeight: Theme.hAudioTrackTall * 4
    function heightOfTrack(track) {
        var own = track && track.mix ? Number(track.mix.audioHeight) : 0
        return own > 0 ? Math.round(Math.max(leastTrackHeight, Math.min(mostTrackHeight, own))) : rowHeight
    }
    function setTrackHeight(index, h) {
        root.trackMixEdited(index, "audioHeight",
                            h === undefined ? undefined : Math.round(Math.max(leastTrackHeight, Math.min(mostTrackHeight, h))))
    }
    property bool showAutomation: true
    property bool rectifiedWaveforms: false // the clips' waveforms stand from the foot
    property int recordTrack: -1
    property bool recording: false
    property real recordLevel: -100
    property bool playing: false

    Timer {
        id: decorationTimer
        interval: 60
        onTriggered: if (root.visible) root.decorationWindowChanged(
            (lanes.contentX - lanes.width / 2) / Math.max(.000001, root.fpx),
            (lanes.contentX + lanes.width * 1.5) / Math.max(.000001, root.fpx), Math.ceil(lanes.width * 2))
    }
    signal decorationWindowChanged(real first, real last, int pixels)
    onVisibleChanged: if (visible) decorationTimer.restart()
    onFpxChanged: { decorationTimer.restart(); rulerSnapCache = null }
    onClipsChanged: rulerSnapCache = null
    onMarkersChanged: rulerSnapCache = null
    onRangeInChanged: rulerSnapCache = null
    onRangeOutChanged: rulerSnapCache = null
    onDurationChanged: rulerSnapCache = null
    onFpsChanged: rulerSnapCache = null
    Connections { target: lanes; function onContentXChanged() { decorationTimer.restart(); root.rulerSnapCache = null } function onWidthChanged() { decorationTimer.restart(); root.rulerSnapCache = null } }
    Component.onCompleted: decorationTimer.restart()

    // ── Outputs ─────────────────────────────────────────────────────────────
    signal trackSelected(int track)
    signal clipSelected(int index)
    signal trackFlagEdited(int track, string flag, bool on)
    signal trackMixEdited(int track, string key, var value)
    signal mixStarted(int track, string key)
    signal mixFinished(int track, string key)
    signal trackRenamed(int track, string name)
    signal addTrackRequested(string role)
    signal removeTrackRequested(int track)
    signal moveTrackRequested(int track, bool up)
    signal masterVolumeEdited(real db)
    signal masterMuteToggled(bool on)
    signal clipMoveRequested(int index, int newTrack, int newStart)
    signal clipTrimRequested(int index, int inFrame, int outFrame, int newStart)
    signal clipRemoveRequested(int index, bool ripple)
    signal splitRequested(int track, int frame)
    signal clipFadesEdited(int index, int fadeIn, int fadeOut)
    signal clipVolumeEdited(int index, real db)
    signal clipVolumeKfEdited(int index, var kf)
    signal clipFxRequested(int index)
    signal seekRequested(int frame)
    signal scrubRequested(int frame)
    signal sampleSeekRequested(string ticks, bool audition)
    signal rangeEdited(int rangeIn, int rangeOut)
    signal rangeCleared()
    signal zoomEdited(real level)
    signal soundDropped(string path, int track, int frame)
    signal markerRequested(int frame)
    signal contextRequested(int track, int frame)

    property var collapsed: ({})         // role → true

    // ── Model ───────────────────────────────────────────────────────────────
    readonly property var audioTracks: {
        var out = []
        for (var i = 0; i < tracks.length; ++i) if (tracks[i].kind === "audio") out.push(tracks[i])
        return out
    }
    function roleOf(t) { return t.mix !== undefined && t.mix.role !== undefined && t.mix.role !== "" ? t.mix.role : "dialogue" }
    readonly property var groups: {
        var order = Theme.roleNames.slice()
        var by = {}
        for (var i = 0; i < audioTracks.length; ++i) {
            var r = roleOf(audioTracks[i])
            if (by[r] === undefined) by[r] = []
            by[r].push(audioTracks[i])
        }
        var out = []
        for (var k = 0; k < order.length; ++k) if (by[order[k]] !== undefined) out.push({ role: order[k], tracks: by[order[k]] })
        for (var key in by) if (order.indexOf(key) < 0) out.push({ role: key, tracks: by[key] })
        return out
    }
    // Rows with their y: [{kind: "group"|"track", role, count, track, y, h}]
    readonly property var rows: {
        var out = []
        var y = 0
        for (var g = 0; g < groups.length; ++g) {
            var grp = groups[g]
            out.push({ kind: "group", role: grp.role, count: grp.tracks.length, y: y, h: Theme.hAudioGroupHeader })
            y += Theme.hAudioGroupHeader
            if (collapsed[grp.role] === true) continue
            for (var t = 0; t < grp.tracks.length; ++t) {
                var th = heightOfTrack(grp.tracks[t])
                out.push({ kind: "track", role: grp.role, track: grp.tracks[t], y: y, h: th })
                y += th
            }
        }
        return out
    }
    readonly property int rowsHeight: rows.length > 0 ? rows[rows.length - 1].y + rows[rows.length - 1].h : 0
    function rowForTrack(index) {
        for (var i = 0; i < rows.length; ++i) if (rows[i].kind === "track" && rows[i].track.index === index) return rows[i]
        return null
    }
    function trackAtY(y) { // engine index, or tracks.length for "below the last"
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].kind === "track" && y >= rows[i].y && y < rows[i].y + rows[i].h) return rows[i].track.index
        return y >= rowsHeight ? tracks.length : -1
    }
    readonly property real fpx: zoom > 0 ? zoom : Math.max(0.005, lanes.width / Math.max(1, duration))
    readonly property real contentW: Math.max(lanes.width, duration * fpx)
    function frameAt(x) { return Math.max(0, Math.min(Math.max(0, duration - 1), Math.round(x / fpx))) }
    function levelsFor(index) { return trackLevels[index] !== undefined ? trackLevels[index] : ({}) }
    function gesturePoints(target) {
        if (!snapPoints) return []
        const first = (lanes.contentX - 8) / fpx, last = (lanes.contentX + lanes.width + 8) / fpx
        const result = snapPoints(target, first, last)
        const points = Array.from(result.points || [])
        const clock = result.context
        if (clock) for (const frame of [rangeIn, rangeOut >= 0 ? rangeOut + 1 : -1]) {
            if (frame >= 0 && frame >= first && frame <= last)
                points.push({frame: frame, time: {ticks: String(frame * clock.fpsDen), rate: clock.fpsNum}})
        }
        return points
    }
    function seekSample(ticks, audition) {
        if (!snapping || !snapPoints) { sampleSeekRequested(ticks, audition); return }
        if (rulerSnapCache === null) rulerSnapCache = gesturePoints({}).filter(function(p) { return !p.cursor && p.frame < root.duration })
        const frame = Number(ticks) / sampleRate * fps
        let best = null, distance = 8.000001
        for (const point of rulerSnapCache) {
            const d = Math.abs(point.frame - frame) * fpx
            if (d < distance) { best = point; distance = d }
        }
        if (best) timeSeekRequested(best.time, audition)
        else sampleSeekRequested(ticks, audition)
    }
    function rippleShift(entry) {
        const g = activeGesture
        if (!g || !g.capture || !g.capture.ripple || !g.displayEntry || !g.lastValid || entry.track !== g.capture.track) return 0
        const target = g.capture.target
        if (target.clipId ? target.clipId === entry.clipId : target.track === entry.track && target.index === entry.index) return 0
        const start = entry.audioTiming ? MediaTime.seconds(entry.audioTiming.start) : entry.start / fps
        return start + 1e-12 >= MediaTime.seconds(g.lastValid.ripplePivot)
            ? MediaTime.seconds(g.lastValid.rippleDelta) * fps : 0
    }

    // Snap: playhead, range, markers, every audio clip's edges, the ends, and
    // the beats of mapped music while they are shown (as on the Edit page),
    // less the beats of the clip `excludeIndex` itself, which go with it.
    function snapCandidates(excludeIndex) {
        var cands = [0, duration, position]
        if (rangeIn >= 0) cands.push(rangeIn)
        if (rangeOut >= 0) cands.push(rangeOut + 1)
        for (var m = 0; m < markers.length; ++m) cands.push(markers[m].frame)
        for (var c = 0; c < clips.length; ++c) {
            if (c === excludeIndex) continue
            cands.push(clips[c].start)
            cands.push(clips[c].start + clips[c].frames)
        }
        var own = excludeIndex >= 0 && excludeIndex < clips.length ? clips[excludeIndex] : null
        for (var b = 0; b < beatLines.length; ++b) {
            var line = beatLines[b]
            if (!own || line.track !== own.track || line.index !== own.index) cands.push(line.frame)
        }
        return cands
    }
    // The candidate nearest `frame` within the reach, as {frame, distance}; null for none.
    function nearestOf(frame, cands) {
        var tol = Math.max(1, Math.round(8 / fpx))
        var best = null, bestD = tol + 1
        for (var i = 0; i < cands.length; ++i) {
            var d = Math.abs(cands[i] - frame)
            if (d < bestD) { bestD = d; best = { frame: cands[i], distance: d } }
        }
        return best
    }
    function snap(frame, excludeIndex) {
        if (!snapping) return frame
        var near = nearestOf(frame, snapCandidates(excludeIndex))
        return near ? near.frame : frame
    }
    // A clip `frames` long moved to start at `start`: either end may snap
    // (the nearer wins), as a clip moved in samples does. The start it gets.
    function snapMove(start, frames, excludeIndex) {
        if (!snapping) return start
        var cands = snapCandidates(excludeIndex)
        var head = nearestOf(start, cands), tail = nearestOf(start + frames, cands)
        if (tail && (!head || tail.distance < head.distance)) return tail.frame - frames
        return head ? head.frame : start
    }
    function scrollTo(frame) {
        var x = frame * fpx
        if (x < lanes.contentX || x > lanes.contentX + lanes.width) lanes.contentX = Math.max(0, Math.min(contentW - lanes.width, x - lanes.width / 3))
    }
    onPositionChanged: if (playing) {
        var x = position * fpx
        if (x > lanes.contentX + lanes.width * 0.9) lanes.contentX = Math.max(0, Math.min(contentW - lanes.width, x - lanes.width * 0.1))
    }

    // ── Layout ──────────────────────────────────────────────────────────────
    Rectangle { anchors.fill: parent; color: Theme.windowBg }

    // Ruler (over the lane area only).
    Item {
        id: rulerHost
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.leftMargin: Theme.wAudioTrackControl
        anchors.right: parent.right
        height: Theme.hTimeRuler
        clip: true
        Edit.TimeRuler {
            objectName: "audioRuler"
            // Whole-frame positioning reads as the sequence's timecode, as on the Edit page.
            timecodeOf: root.units === "frames" ? root.timecoder : null
            fineUnit: root.units
            sampleRate: root.sampleRate
            skimming: !root.playing
            renderRanges: root.renderRanges
            renderedRanges: root.renderedRanges
            id: ruler
            viewportX: lanes.contentX
            viewportWidth: lanes.width
            x: -lanes.contentX
            // The ruler scales by its own width: the sequence's length at the
            // lanes' scale, so zoomed out past Fit it still lines up with them.
            width: Math.max(1, root.duration * root.fpx)
            height: parent.height
            duration: root.duration
            fps: root.fps
            markers: root.markers
            onPressedAt: function (f) { root.seekRequested(f) }
            onSkimmedTo: function (f) { root.scrubRequested(f) }
            onSamplePressedAt: function(ticks) { root.seekSample(ticks, false) }
            onSampleSkimmedTo: function(ticks) { root.seekSample(ticks, true) }
            onRightPressedAt: function (f) { root.markerRequested(f) }
        }
        // Range shade on the ruler.
        Rectangle {
            visible: root.rangeIn >= 0 || root.rangeOut >= 0
            x: (root.rangeIn >= 0 ? root.rangeIn : 0) * root.fpx - lanes.contentX
            width: ((root.rangeOut >= 0 ? root.rangeOut + 1 : root.duration) - (root.rangeIn >= 0 ? root.rangeIn : 0)) * root.fpx
            height: parent.height
            color: Theme.rangeFill
            border.color: Theme.rangeStroke
            border.width: Theme.hairline
        }
    }
    Rectangle { // corner
        anchors.top: parent.top
        anchors.left: parent.left
        width: Theme.wAudioTrackControl
        height: Theme.hTimeRuler
        color: Theme.panelBg
        Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: Theme.border }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        Text {
            anchors.centerIn: parent
            text: root.audioTracks.length === 0 ? qsTr("No audio tracks") : root.audioTracks.length === 1 ? qsTr("1 track") : qsTr("%1 tracks").arg(root.audioTracks.length)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    // Track controls column, scrolled with the lanes.
    Item {
        id: controlsHost
        anchors.top: rulerHost.bottom
        anchors.left: parent.left
        anchors.bottom: masterLeft.top
        width: Theme.wAudioTrackControl
        clip: true
        Item {
            id: controls
            y: -lanes.contentY
            width: parent.width
            height: root.rowsHeight
            Repeater {
                // A COUNT model: rows re-bind in place when tracks change, so a
                // fader drag on a control survives its own edits.
                model: root.rows.length
                delegate: Loader {
                    id: rowLoader
                    required property int index
                    readonly property var row: root.rows[index] !== undefined ? root.rows[index] : ({ kind: "group", role: "", count: 0, y: 0, h: 0 })
                    y: row.y
                    width: controls.width
                    height: row.h
                    sourceComponent: row.kind === "group" ? groupControl : trackControl
                    // The lower edge of a track's header drags its height;
                    // a double click puts it back on the common height.
                    MouseArea {
                        objectName: rowLoader.row.kind === "track" ? "audioTrackResize:" + rowLoader.row.track.index : ""
                        visible: rowLoader.row.kind === "track"
                        z: 5
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                        height: 6
                        cursorShape: Qt.SizeVerCursor
                        hoverEnabled: true
                        preventStealing: true
                        property real pressY: 0
                        property real pressHeight: 0
                        // The track the drag began on, whatever row comes under the pointer.
                        property int held: -1
                        onPressed: function (ev) {
                            pressY = mapToItem(root, ev.x, ev.y).y; pressHeight = rowLoader.row.h
                            held = rowLoader.row.track.index
                        }
                        onPositionChanged: function (ev) { if (pressed && held >= 0) root.setTrackHeight(held, pressHeight + mapToItem(root, ev.x, ev.y).y - pressY) }
                        onReleased: held = -1
                        onCanceled: held = -1
                        onDoubleClicked: root.setTrackHeight(rowLoader.row.track.index, undefined)
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                            height: 2
                            color: Theme.accent
                            visible: parent.containsMouse || parent.pressed
                        }
                    }
                    Component {
                        id: groupControl
                        TrackGroupHeader {
                            role: rowLoader.row.role
                            trackCount: rowLoader.row.count
                            expanded: root.collapsed[role] !== true
                            onToggled: { var c = {}; for (var k in root.collapsed) c[k] = root.collapsed[k]; c[role] = !(c[role] === true); root.collapsed = c }
                        }
                    }
                    Component {
                        id: trackControl
                        AudioTrackControl {
                            objectName: "audioTrackControl:" + rowLoader.row.track.index
                            track: rowLoader.row.track !== undefined ? rowLoader.row.track : ({})
                            selected: root.selectedTrack === track.index
                            current: root.recordTrack === track.index
                            levels: {
                                var l = root.levelsFor(track.index)
                                if (root.recording && root.recordTrack === track.index) { var m = {}; for (var k in l) m[k] = l[k]; m.input = root.recordLevel; return m }
                                return l
                            }
                            onSelectedRequested: root.trackSelected(track.index)
                            onFlagToggled: function (flag, on) { root.trackFlagEdited(track.index, flag, on) }
                            onMixStarted: function(key) { root.mixStarted(track.index, key) }
                            onMixFinished: function(key) { root.mixFinished(track.index, key) }
                            onMixEdited: function (key, value) { root.trackMixEdited(track.index, key, value) }
                            onRenamed: function (name) { root.trackRenamed(track.index, name) }
                            onRemoveRequested: root.removeTrackRequested(track.index)
                            onMoveRequested: function (up) { root.moveTrackRequested(track.index, up) }
                            onAddBelowRequested: root.addTrackRequested(rowLoader.row.role)
                        }
                    }
                }
            }
        }
        EmptyState {
            anchors.centerIn: parent
            visible: root.audioTracks.length === 0
            iconName: "audio-waveform"
            // The video clips may already carry sound (mixed on V1); say so,
            // or this page looks like it has lost the audio the Edit page shows.
            title: root.embeddedAudioCount > 0 ? qsTr("No audio-only tracks") : qsTr("No audio tracks")
            description: root.embeddedAudioCount > 0
                ? qsTr("The video clips carry their own sound on V1.")
                : qsTr("Add one, or drop a sound below the video.")
            actionText: qsTr("Add Track")
            onActionTriggered: root.addTrackRequested("dialogue")
        }
    }
    MasterRow {
        id: masterLeft
        objectName: "masterRow"
        anchors.left: parent.left
        anchors.bottom: hbar.top
        width: Theme.wAudioTrackControl
        master: root.master
        muted: root.masterMuted
        selected: root.selectedTrack === -2
        onSelectedRequested: root.trackSelected(-2)
        onMuteToggled: function (on) { root.masterMuteToggled(on) }
        onVolumeEdited: function (db) { root.masterVolumeEdited(db) }
    }
    MasterRow {
        anchors.left: masterLeft.right
        anchors.right: parent.right
        anchors.bottom: hbar.top
        lane: true
        master: root.master
        levelLeft: root.masterLeft
        levelRight: root.masterRight
    }

    // The active tool's pointer over the lanes (blade, trim, slip); it
    // takes no clicks.
    ToolPointer {
        objectName: "audioToolPointer"
        x: lanes.x; y: lanes.y; width: lanes.width; height: lanes.height
        z: 20
        tool: root.tool
    }

    // Lanes.
    Flickable {
        id: lanes
        objectName: "audioLanes"
        anchors.top: rulerHost.bottom
        anchors.left: controlsHost.right
        anchors.right: parent.right
        anchors.bottom: masterLeft.top
        contentWidth: root.contentW
        contentHeight: Math.max(height, root.rowsHeight + Theme.s4)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: root.tool === "hand"
        C.ScrollBar.vertical: ScrollBar {}
        // Along the bottom, to move through the edit without the playhead.
        C.ScrollBar.horizontal: ScrollBar {
            objectName: "audioTimeScroll"
            policy: lanes.contentWidth > lanes.width + 1 ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded
        }

        // Group tint bands + track row lines.
        Repeater {
            model: root.rows.length
            delegate: Item {
                id: laneRow
                required property int index
                readonly property var row: root.rows[index] !== undefined ? root.rows[index] : ({ kind: "group", role: "", count: 0, y: 0, h: 0 })
                y: row.y
                width: lanes.contentWidth
                height: row.h
                TrackGroupHeader {
                    visible: laneRow.row.kind === "group"
                    width: parent.width
                    role: laneRow.row.role
                    lane: true
                }
                Rectangle {
                    visible: laneRow.row.kind === "track"
                    anchors.fill: parent
                    color: laneRow.row.kind === "track" && root.selectedTrack === laneRow.row.track.index ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border; opacity: 0.6 }
                    Rectangle { // locked hatch stand-in: a dim wash
                        anchors.fill: parent
                        visible: laneRow.row.kind === "track" && laneRow.row.track.locked === true
                        color: Theme.scrimLight
                    }
                }
            }
        }
        // Range shade.
        Rectangle {
            visible: root.rangeIn >= 0 || root.rangeOut >= 0
            x: (root.rangeIn >= 0 ? root.rangeIn : 0) * root.fpx
            width: ((root.rangeOut >= 0 ? root.rangeOut + 1 : root.duration) - (root.rangeIn >= 0 ? root.rangeIn : 0)) * root.fpx
            height: lanes.contentHeight
            color: Theme.rangeFill
        }
        // Markers as thin lines.
        Repeater {
            model: root.markers
            delegate: Rectangle {
                required property var modelData
                x: modelData.frame * root.fpx
                width: 1
                height: lanes.contentHeight
                color: Theme.markerTint(modelData)
                opacity: 0.5
            }
        }

        // The beats of mapped music: lines down every track in view, over
        // the clips, solid on the music's own (as on the Edit timeline).
        // They take no mouse input.
        BeatLinesItem {
            objectName: "audioBeatLines"
            visible: root.beatLines.length > 0
            x: lanes.contentX
            y: lanes.contentY
            width: lanes.width
            height: lanes.height
            z: 4
            lines: root.beatLines
            firstFrame: lanes.contentX / Math.max(.000001, root.fpx)
            pxPerFrame: root.fpx
            // From the first track's row (under its group's heading) to the foot of the last.
            spanTop: {
                for (let i = 0; i < root.rows.length; ++i)
                    if (root.rows[i].kind === "track") return root.rows[i].y - lanes.contentY
                return -lanes.contentY
            }
            spanBottom: root.rowsHeight - lanes.contentY
            rows: {
                const out = []
                for (let i = 0; i < root.rows.length; ++i)
                    if (root.rows[i].kind === "track")
                        out.push({ track: root.rows[i].track.index, y: root.rows[i].y + 1 - lanes.contentY, height: root.rows[i].h - 2 })
                return out
            }
            color: Theme.beatLine
            beatOpacity: Theme.beatLineAcross
            lineWidth: Theme.beatLineWidth
            barWidth: Theme.barLineWidth
            spacing: Theme.beatLineSpacing
        }

        // Empty-space interactions: click clears, range tool drags a range,
        // blade on empty does nothing, right-click = context.
        TapHandler {
            id: laneTap
            acceptedButtons: Qt.LeftButton
            onTapped: function (ev) {
                if (root.tool === "select" || root.tool === "trim" || root.tool === "slip") { root.clipSelected(-1); root.trackSelected(root.trackAtY(ev.position.y)) }
                // The event point carries no modifiers; the handler's point does.
                else if (root.tool === "zoom") root.zoomEdited(root.fpx * (laneTap.point.modifiers & Qt.AltModifier ? 1 / 1.5 : 1.5))
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function (ev) { root.contextRequested(root.trackAtY(ev.position.y), root.frameAt(ev.position.x)) }
        }
        DragHandler {
            id: rangeDrag
            enabled: root.tool === "range"
            target: null
            property int anchorFrame: 0
            onActiveChanged: {
                if (active) anchorFrame = root.frameAt(centroid.pressPosition.x)
                else {
                    var a = anchorFrame, b = root.frameAt(centroid.position.x)
                    if (Math.abs(a - b) < 2) root.rangeCleared()
                    else root.rangeEdited(Math.min(a, b), Math.max(a, b))
                }
            }
            onTranslationChanged: if (active) { var a = anchorFrame, b = root.frameAt(centroid.position.x); root.rangeEdited(Math.min(a, b), Math.max(a, b)) }
        }

        // Clips.
        Repeater {
            model: TimelineWindowModel {
                source: root.clips
                fps: root.fps
                kind: "audio"
                firstFrame: (lanes.contentX - lanes.width / 2) / Math.max(.000001, root.fpx)
                lastFrame: (lanes.contentX + lanes.width * 1.5) / Math.max(.000001, root.fpx)
                minimumFrames: 2 / Math.max(.000001, root.fpx)
                pinned: root.selectedClip >= 0 ? [root.selectedClip] : []
                active: root.visible
            }
            delegate: Item {
                id: clipHost
                required property int sourceIndex
                required property var modelData
                required property real timelineStart
                required property real timelineFrames
                // A block on a transition's cut starts blockLead frames before
                // its entry and ends blockTail before the entry's end (the two
                // blocks meet at the cut, as on the Edit page).
                required property int blockLead
                required property int blockTail
                readonly property int index: sourceIndex
                readonly property var entry: modelData
                readonly property var row: root.rowForTrack(entry.track)
                readonly property bool onAudio: row !== null
                readonly property int connectedIndex: index // flat position in backend.connected
                visible: onAudio && x + width >= lanes.contentX && x <= lanes.contentX + lanes.width
                readonly property var previewEntry: fineGesture.displayEntry
                readonly property var previewRow: previewEntry ? root.rowForTrack(fineGesture.displayTrack) : null
                x: previewEntry ? MediaTime.seconds(previewEntry.audioTiming.start) * root.fps * root.fpx
                                : (timelineStart + root.rippleShift(entry)) * root.fpx + dragDx
                y: (previewRow !== null ? previewRow.y : row !== null ? row.y : 0) + 1 + dragDy
                width: previewEntry ? Math.max(1, MediaTime.seconds(previewEntry.audioTiming.duration) * root.fps * root.fpx)
                                    : Math.max(entry.audioTiming ? 1 : 2, timelineFrames * root.fpx + trimDw)
                height: (previewRow !== null ? previewRow.h : row !== null ? row.h : root.rowHeight) - 2
                z: root.selectedClip === connectedIndex ? 3 : 1
                property real dragDx: 0
                property real dragDy: 0
                property real trimDw: 0
                // The clip as the trim or the slip in hand would leave it, for its waveform.
                property var pending: ({})
                function actionAt(px) {
                    if (root.tool === "blade") return "split"
                    if (root.tool === "slip") return "slip"
                    if (width < 3 * Theme.wTrimHandle && root.tool !== "trim") return "move"
                    const handle = Math.min(Theme.wTrimHandle, width / 3)
                    return px < handle ? "trimIn" : px > width - handle ? "trimOut" : "move"
                }
                SoundGesture {
                    id: fineGesture
                    objectName: "soundGesture:" + clipHost.connectedIndex
                    entry: clipHost.entry
                    units: root.units; tool: root.tool; fps: root.fps; fpx: root.fpx
                    sampleRate: root.sampleRate; duration: root.duration
                    snapping: root.snapping; pending: root.gesturePending
                    available: root.visible && root.enabled && !clipHost.locked
                               && (!root.Window.window || root.Window.window.visible)
                    targetFor: root.gestureTarget; previewFor: root.gesturePreview
                    pointsFor: function(target) { return root.gesturePoints(target) }
                    onCommitted: function(target, action, changes, rate) { root.gestureCommitted(target, action, changes, rate) }
                    onProblemChanged: root.gestureProblem = problem
                    onActiveChanged: if (active) root.activeGesture = fineGesture
                    onCaptureChanged: if (!capture && root.activeGesture === fineGesture) root.activeGesture = null
                    Component.onDestruction: if (root.activeGesture === fineGesture) root.activeGesture = null
                }
                readonly property bool locked: row !== null && row.track.locked === true
                readonly property color tint: row !== null && row.track.mix !== undefined && row.track.mix.color !== undefined && row.track.mix.color !== ""
                                              ? Theme.labelColor(row.track.mix.color) : Theme.roleColor(row !== null ? root.roleOf(row.track) : "")
                AudioClipItem {
                    id: clipItem
                    objectName: "audioClip:" + clipHost.connectedIndex
                    anchors.fill: parent
                    fps: root.fps
                    // (A precise cut being moved is drawn where it would land, its sound with it.)
                    // (An in edge being trimmed takes the block's start with it and leaves the sound where it is.)
                    startFrame: clipHost.previewEntry ? MediaTime.seconds(clipHost.previewEntry.audioTiming.start) * root.fps
                              : clipHost.timelineStart + (clipHost.pending.start !== undefined ? clipHost.pending.start - clipHost.entry.start : 0)
                    pending: clipHost.pending
                    viewportX: Math.max(0, lanes.contentX - clipHost.x)
                    viewportWidth: Math.max(0, Math.min(clipHost.width - viewportX, lanes.width))
                    entry: clipHost.previewEntry || clipHost.entry
                    fpx: root.fpx
                    tint: clipHost.tint
                    selected: root.selectedClip === clipHost.connectedIndex
                    showAutomation: root.showAutomation
                    rectified: root.rectifiedWaveforms
                    dragging: moveDrag.active
                    locked: clipHost.locked || root.gesturePending
                    onVolumeEdited: function (db) { root.clipVolumeEdited(clipHost.connectedIndex, db) }
                    onVolumeKfEdited: function (kf) { root.clipVolumeKfEdited(clipHost.connectedIndex, kf) }
                    onFadesEdited: function (a, b) { root.clipFadesEdited(clipHost.connectedIndex, a, b) }
                    onFxRequested: root.clipFxRequested(clipHost.connectedIndex)
                }
                // Selection + blade.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    enabled: !root.gesturePending
                    onPressedChanged: if (pressed) {
                        root.gestureProblem = ""
                        moveDrag.pressLaneY = clipHost.y + point.position.y
                        moveDrag.pressContentX = lanes.contentX
                        fineGesture.arm(clipHost.actionAt(point.position.x), clipHost.x + point.position.x)
                    }
                    onTapped: function (ev) {
                        if (root.tool === "blade" && !clipHost.locked) {
                            if (fineGesture.usesFine) fineGesture.split()
                            else root.splitRequested(clipHost.entry.track, clipHost.entry.start - clipHost.blockLead + root.frameAt(ev.position.x))
                            return
                        }
                        root.clipSelected(clipHost.connectedIndex)
                        root.trackSelected(clipHost.entry.track)
                    }
                    // Decide after both handlers have seen this double-tap.
                    onDoubleTapped: {
                        const index = clipHost.connectedIndex
                        Qt.callLater(function () { if (Date.now() - clipItem.bandDoubleTapTime > 250) root.clipFxRequested(index) })
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: function (ev) { root.clipSelected(clipHost.connectedIndex); root.contextRequested(clipHost.entry.track, clipHost.entry.start - clipHost.blockLead + root.frameAt(ev.position.x)) }
                }
                // Move (select tool, body) / trim (edges) / slip.
                DragHandler {
                    id: moveDrag
                    target: null
                    enabled: !root.gesturePending && !clipHost.locked && (root.tool === "select" || root.tool === "trim" || root.tool === "slip")
                    property string mode: "move"
                    property int newStart: 0
                    property int newTrack: 0
                    property int slipIn: 0
                    property real pressLaneY: 0
                    property real pressContentX: 0
                    // Qt deactivates before it reports a cancel, in the same
                    // event: commit later, and let a cancel void the ticket.
                    property int ticket: 0
                    onCanceled: { ++ticket; fineGesture.cancel(); clipHost.dragDx = 0; clipHost.dragDy = 0; clipHost.trimDw = 0; clipHost.pending = ({}) }
                    onActiveChanged: {
                        if (active) {
                            root.clipSelected(clipHost.connectedIndex)
                            root.forceActiveFocus()
                            if (fineGesture.usesFine) { fineGesture.activate(); return }
                            var px = centroid.pressPosition.x
                            var edge = px < Theme.wTrimHandle ? "in" : px > clipHost.width - Theme.wTrimHandle ? "out" : ""
                            if (root.tool === "slip") mode = "slip"
                            else if (edge !== "" || root.tool === "trim") mode = edge === "in" ? "trimIn" : edge === "out" ? "trimOut" : "move"
                            else mode = "move"
                            newStart = clipHost.entry.start
                            newTrack = clipHost.entry.track
                        } else {
                            if (fineGesture.usesFine) { fineGesture.finish(); return }
                            const e = clipHost.entry, index = clipHost.connectedIndex, how = mode
                            const start = newStart, track = newTrack, slip = slipIn
                            // The entry's frames as trimmed, not the block's (see blockLead).
                            const width = Math.round(clipHost.width / root.fpx) - clipHost.blockLead + clipHost.blockTail, mine = ++ticket
                            Qt.callLater(function () {
                                clipHost.dragDx = 0; clipHost.dragDy = 0; clipHost.trimDw = 0; clipHost.pending = ({})
                                if (mine !== ticket) return
                                if (how === "move") {
                                    if (start !== e.start || track !== e.track) root.clipMoveRequested(index, track, start)
                                } else if (how === "trimIn") {
                                    var d = start - e.start
                                    if (d !== 0) root.clipTrimRequested(index, e.in + d, e.out, start)
                                } else if (how === "trimOut") {
                                    if (width !== e.frames) root.clipTrimRequested(index, e.in, e.in + width - 1, e.start)
                                } else if (how === "slip") {
                                    if (slip !== e.in) root.clipTrimRequested(index, slip, slip + e.frames - 1, e.start)
                                }
                            })
                        }
                    }
                    onTranslationChanged: {
                        if (!active) return
                        if (fineGesture.usesFine) {
                            const track = root.trackAtY(pressLaneY + translation.y)
                            fineGesture.update(translation.x + lanes.contentX - pressContentX,
                                               track < 0 && fineGesture.capture ? fineGesture.capture.track : track)
                            return
                        }
                        var e = clipHost.entry
                        var df = Math.round(translation.x / root.fpx)
                        if (mode === "move") {
                            newStart = Math.max(0, root.snapMove(e.start + df, e.frames, clipHost.connectedIndex))
                            // The pointer in lane coordinates: the host's undragged y plus
                            // the pointer's offset inside it.
                            var t = root.trackAtY(clipHost.y - clipHost.dragDy + centroid.position.y)
                            var row = root.rowForTrack(t)
                            if (t >= 0 && row !== null) newTrack = t
                            var targetRow = root.rowForTrack(newTrack)
                            clipHost.dragDx = (newStart - e.start) * root.fpx
                            clipHost.dragDy = targetRow !== null && clipHost.row !== null ? targetRow.y - clipHost.row.y : 0
                        } else if (mode === "trimIn") {
                            var maxIn = e.start + e.frames - 1
                            var minIn = e.start - e.in // no media before the file's start
                            newStart = Math.max(minIn, Math.min(maxIn, root.snap(e.start + df, clipHost.connectedIndex)))
                            clipHost.dragDx = (newStart - e.start) * root.fpx
                            clipHost.trimDw = -(newStart - e.start) * root.fpx
                            clipHost.pending = ({ "start": newStart, "in": e.in + newStart - e.start, "frames": e.frames - (newStart - e.start) })
                        } else if (mode === "trimOut") {
                            var maxOut = e.length > 0 ? e.length - e.in : e.frames + 100000
                            var frames = Math.max(1, Math.min(maxOut, root.snap(e.start + e.frames + df, clipHost.connectedIndex) - e.start))
                            clipHost.trimDw = (frames - e.frames) * root.fpx
                            clipHost.pending = ({ "frames": frames })
                        } else if (mode === "slip") {
                            var maxSlip = e.length > 0 ? e.length - e.frames : e.in + 100000
                            slipIn = Math.max(0, Math.min(maxSlip, e.in - df))
                            clipHost.pending = ({ "in": slipIn })
                        }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"; radius: Theme.rControl
                    border.width: Theme.selectionRing; border.color: Theme.dropInvalid
                    visible: fineGesture.active && fineGesture.problem !== ""
                }
                HoverHandler { id: clipHover; cursorShape: root.tool === "blade" ? Qt.SplitHCursor : (clipHover.point.position.x < Theme.wTrimHandle || clipHover.point.position.x > clipHost.width - Theme.wTrimHandle) ? Qt.SizeHorCursor : Qt.ArrowCursor }
            }
        }

        // Drop target for library sounds.
        DropArea {
            id: drop
            anchors.fill: parent
            // Library rows drag internally (their ghost carries `soundPath`);
            // files from a file manager arrive as URLs.
            keys: ["omaedit-sound", "text/uri-list"]
            property int hoverTrack: -1
            property int hoverFrame: 0
            onPositionChanged: function (d) { hoverTrack = root.trackAtY(d.y); hoverFrame = root.snap(root.frameAt(d.x), -1) }
            onExited: hoverTrack = -1
            onDropped: function (d) {
                var path = d.source && d.source.soundPath !== undefined ? d.source.soundPath
                         : d.hasUrls && d.urls.length > 0 ? decodeURIComponent(String(d.urls[0]).replace(/^file:\/\//, "")) : ""
                var t = root.trackAtY(d.y)
                if (path !== "" && t >= 0) { d.accept(); root.soundDropped(path, t, root.snap(root.frameAt(d.x), -1)) }
                hoverTrack = -1
            }
            Rectangle { // ghost
                visible: drop.containsDrag && drop.hoverTrack >= 0
                x: drop.hoverFrame * root.fpx
                y: { var r = root.rowForTrack(drop.hoverTrack); return r !== null ? r.y + 1 : root.rowsHeight }
                width: Math.max(40, 25 * root.fpx * 3)
                height: { var r = root.rowForTrack(drop.hoverTrack); return (r !== null ? r.h : root.rowHeight) - 2 }
                radius: Theme.rControl
                color: Theme.transitionWash
                border.color: Theme.dropValid
                border.width: Theme.selectionRing
            }
        }
    }

    Rectangle {
        objectName: "audioGestureProblem"
        visible: root.gestureProblem !== ""
        x: Theme.wAudioTrackControl + Theme.s2; y: Theme.hTimeRuler + Theme.s2
        width: Math.min(420, root.width - x - Theme.s2); height: problemText.implicitHeight + Theme.s2 * 2
        z: 20; radius: Theme.rControl; color: Theme.panelBg
        border.color: Theme.dropInvalid
        Text {
            id: problemText
            anchors.fill: parent; anchors.margins: Theme.s2
            text: root.gestureProblem; color: Theme.textPrimary
            font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            wrapMode: Text.Wrap
        }
    }
    Timer { id: problemTimer; interval: 4000; onTriggered: if (!root.activeGesture) root.gestureProblem = ""; else restart() }
    onGestureProblemChanged: if (gestureProblem !== "") problemTimer.restart()

    // Playhead over the lane viewport.
    Edit.Playhead {
        objectName: "audioPlayhead"
        x: Theme.wAudioTrackControl + root.cursorFrames * root.fpx - lanes.contentX
        y: 0
        height: parent.height - masterLeft.height - hbar.height
        visible: x >= Theme.wAudioTrackControl - 1 && x <= parent.width
    }

    // Horizontal scrollbar strip.
    Item {
        id: hbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.s3
        Rectangle { anchors.fill: parent; color: Theme.panelBg }
        ScrollBar {
            anchors.left: parent.left
            anchors.leftMargin: Theme.wAudioTrackControl
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            orientation: Qt.Horizontal
            size: lanes.width / Math.max(1, lanes.contentWidth)
            position: lanes.contentX / Math.max(1, lanes.contentWidth)
            visible: size < 1
            onPositionChanged: if (pressed) lanes.contentX = position * lanes.contentWidth
        }
    }

    // The wheel does here what it does over the Edit timeline: a plain turn
    // scrolls time (or zooms, per Preferences ▸ Input; Ctrl does the other),
    // and Shift+wheel scrolls the track stack when it overflows.
    function wheelTurned(e) {
        const sideways = e.angleDelta.y === 0
        if ((e.modifiers & Qt.ShiftModifier) && lanes.contentHeight > lanes.height) {
            const turn = sideways ? e.angleDelta.x : e.angleDelta.y
            lanes.contentY = Math.max(0, Math.min(lanes.contentY - turn * root.wheelSensitivity,
                                                  Math.max(0, lanes.contentHeight - lanes.height)))
            return
        }
        const sign = root.wheelInvert ? -1 : 1
        if ((root.wheelAction === "zoom") !== ((e.modifiers & Qt.ControlModifier) !== 0)) {
            if (e.angleDelta.y * sign > 0) root.zoomEdited(root.fpx * 1.2)
            else if (e.angleDelta.y * sign < 0) root.zoomEdited(root.fpx / 1.2)
            return
        }
        const delta = (e.angleDelta.y + e.angleDelta.x) * sign * root.wheelSensitivity
        lanes.contentX = Math.max(0, Math.min(lanes.contentX - delta, Math.max(0, lanes.contentWidth - lanes.width)))
    }
    WheelHandler {
        target: lanes
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (e) { root.wheelTurned(e) }
    }
    // A purely sideways turn (a tilt wheel or a sideways swipe) never
    // reaches a vertical handler.
    WheelHandler {
        target: lanes
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        orientation: Qt.Horizontal
        onWheel: function (e) { if (e.angleDelta.y === 0) root.wheelTurned(e) }
    }
}
