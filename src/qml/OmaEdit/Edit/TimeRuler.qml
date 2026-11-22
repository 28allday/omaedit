pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// The timecode ruler over the lanes. Labelled ticks land on "nice" intervals
// (1 s, 2 s, 5 s, …) chosen so labels never crowd at any zoom; markers are
// small flags whose colour tells the type; hovering near one surfaces its
// label. Hover skims, a press parks the playhead, right-click asks for the
// marker menu — all reported as frames, so the owner decides what a seek is.
//
// Owner-driven: `duration`/`fps`/`markers` describe the sequence, the
// signals carry every gesture out. The ruler knows nothing of the engine.
Rectangle {
    id: root

    property int rangeIn: -1
    property int rangeOut: -1
    property bool showFrames: false
    // The owner's full timecode for a frame (its start and drop-frame
    // rules), or null for short minute:second labels.
    property var timecodeOf: null
    readonly property bool fullTimecode: !fine && !showFrames && typeof timecodeOf === "function"
    // Audio owners can use the project sample grid without changing the
    // picture-frame coordinates of markers, ranges and render decorations.
    property string fineUnit: ""
    property int sampleRate: 48000
    readonly property bool fine: fineUnit === "samples" || fineUnit === "milliseconds"
    signal samplePressedAt(string ticks)
    signal sampleSkimmedTo(string ticks)
    signal rangeEdited(int first, int last)
    property real viewportX: 0
    property real viewportWidth: width
    property int duration: 0          // frames; 0 = nothing to rule
    property real fps: 0
    property bool ready: duration > 0 && fps > 0
    // [{ frame, type, text }]
    property var markers: []
    property var renderRanges: []
    property var renderedRanges: []
    // Skim (hover seeks) and snap state, so the ruler can say when they're off.
    property bool skimEnabled: true
    property bool snapEnabled: true
    // Hover seeks only while the owner says so (skim on, not playing).
    property bool skimming: skimEnabled
    property bool interactive: ready

    signal pressedAt(int frame)
    signal skimmedTo(int frame)
    // The pointer over the ruler, not pressed, while skimming: the frame it
    // is over, then that it left.
    signal hoverSkimmed(int frame)
    signal hoverEnded()
    signal rightPressedAt(int frame)

    implicitHeight: Theme.hTimeRuler
    radius: Theme.rControl
    color: Theme.panelBg

    Repeater {
        model: root.renderRanges.filter(function(s) { return s.last * root.fpx >= root.viewportX && s.first * root.fpx <= root.viewportX + root.viewportWidth })
        Rectangle {
            required property var modelData
            x: modelData.first * root.fpx
            width: Math.max(1, (modelData.last - modelData.first + 1) * root.fpx)
            height: 3; y: 0; z: 2
            color: Theme.warning
        }
    }
    Repeater {
        model: root.renderedRanges.filter(function(s) { return s.last * root.fpx >= root.viewportX && s.first * root.fpx <= root.viewportX + root.viewportWidth })
        Rectangle {
            required property var modelData
            x: modelData.first * root.fpx
            width: Math.max(1, (modelData.last - modelData.first + 1) * root.fpx)
            height: 3; y: 0; z: 3
            color: Theme.audio
        }
    }

    // Frame → pixel map shared with the clip blocks: frame/duration·width.
    readonly property real fpx: duration > 0 ? width / duration : 0
    readonly property real labelEvery: {
        if (fpx <= 0 || !ready)
            return 0
        if (fine) {
            const scale = fineUnit === "samples" ? sampleRate : 1000
            const minimum = Math.max(fineUnit === "samples" ? 1 : 0.001, 90 * scale / (fps * fpx))
            const magnitude = Math.pow(10, Math.floor(Math.log10(minimum)))
            const step = minimum / magnitude
            return (step <= 1 ? 1 : step <= 2 ? 2 : step <= 5 ? 5 : 10) * magnitude * fps / scale
        }
        // Full timecode labels are wider, and name frames, so they can
        // come every few frames when zoomed right in.
        const gap = fullTimecode ? 130 : 90
        const frames = fullTimecode ? [1, 2, 5, 10].filter(function (f) { return f < fps }) : []
        for (let i = 0; i < frames.length; i++)
            if (frames[i] * fpx >= gap)
                return frames[i]
        const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]
        for (let i = 0; i < steps.length; i++) {
            const f = Math.round(steps[i] * fps)
            if (f > 0 && f * fpx >= gap)
                return f
        }
        return Math.round(3600 * fps)
    }

    function frameFor(x) {
        // The frame whose span contains x, so a click on a block edge lands
        // on the same side of the cut the block shows.
        return Math.max(0, Math.min(duration - 1,
                                    Math.floor(x / width * duration)))
    }
    function xFor(frame) { return frame / Math.max(1, duration) * width }
    function sampleFor(x) {
        return MediaTime.samplePosition(x, fpx, fps, sampleRate, duration)
    }
    function timecode(frames) {
        if (fineUnit === "samples") return String(Math.round(frames / fps * sampleRate))
        if (fineUnit === "milliseconds") return String(Number((frames / fps * 1000).toFixed(6)))
        if(showFrames)return String(frames)
        if (fullTimecode)
            return timecodeOf(frames)
        if (fps <= 0)
            return ""
        const s = Math.round(frames / fps)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    function typeLabel(type) {
        if (type === "chapter") return qsTr("Chapter")
        if (type === "todo") return qsTr("To do")
        if (type === "done") return qsTr("Done")
        return qsTr("Marker")
    }

    Rectangle {
        id: rangeBar
        objectName:"editWorkRange"
        visible:root.rangeIn>=0 && root.rangeOut>=root.rangeIn
        x:root.xFor(root.rangeIn);width:Math.max(2,root.xFor(root.rangeOut+1)-x)
        anchors.bottom:parent.bottom;height:5;color:Theme.accent;z:9
        Repeater {
            model:2
            Rectangle {
                required property int index
                width:7;height:12;y:-7;x:index===0?0:rangeBar.width-width
                color:Theme.accent
                MouseArea {
                    anchors.fill:parent;cursorShape:Qt.SizeHorCursor
                    property int first:0
                    property int last:0
                    onPressed:{first=root.rangeIn;last=root.rangeOut}
                    onReleased:function(mouse){const frame=root.frameFor(mapToItem(root,mouse.x,0).x);root.rangeEdited(index===0?Math.min(frame,last):first,index===1?Math.max(first,frame):last)}
                }
            }
        }
    }

    readonly property real tickPixels: labelEvery * fpx
    // Ticks between the labels: a whole number of frames each, ten to a
    // label where that fits, and a row of finer marks along the top.
    readonly property int minorDivisions: {
        if (fineUnit === "samples")
            return Math.round(labelEvery / fps * sampleRate) >= 5 ? 5 : 1
        if (fine || tickPixels <= 0)
            return 4
        const choices = [10, 6, 5, 4, 3, 2]
        for (let i = 0; i < choices.length; i++)
            if (labelEvery % choices[i] === 0 && tickPixels / choices[i] >= 10)
                return choices[i]
        return 1
    }
    // Frames between the marks of the top row, 0 for none: the smallest
    // whole share of a minor step at least 6 pixels apart.
    readonly property int fineStep: {
        if (fine || tickPixels <= 0)
            return 0
        const minor = labelEvery / minorDivisions
        for (let d = 1; d < minor; d++)
            if (minor % d === 0 && d * fpx >= 6)
                return d
        return 0
    }
    readonly property real firstMajor: tickPixels > 0 ? Math.max(0, Math.floor(viewportX / tickPixels) - 1) : 0
    readonly property real lastMajor: tickPixels > 0 ? Math.min(Math.floor(duration / labelEvery), Math.ceil((viewportX + viewportWidth) / tickPixels) + 1) : -1
    Canvas { // minor ticks and the fine row, drawn only for the view
        id: ticks
        objectName: "rulerTicks"
        visible: root.visible && root.ready
        x: Math.max(0, Math.min(root.viewportX, root.width - 1))
        width: Math.max(1, Math.min(root.viewportWidth, root.width - x))
        height: root.height
        readonly property var paintKey: [x, width, height, root.fpx, root.labelEvery, root.minorDivisions,
                                         root.fineStep, root.duration, Theme.textMuted]
        onPaintKeyChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (root.tickPixels <= 0)
                return
            const end = Math.min(root.duration * root.fpx, x + width)
            const minor = root.tickPixels / root.minorDivisions
            const minorHeight = root.fine ? 4 : Math.round(height * 0.3)
            ctx.fillStyle = String(Theme.textMuted)
            ctx.globalAlpha = 0.6
            for (let i = Math.max(1, Math.floor(x / minor)); i * minor <= end; i++)
                if (i % root.minorDivisions !== 0)
                    ctx.fillRect(Math.round(i * minor - x), height - minorHeight - 1, 1, minorHeight)
            if (root.fineStep > 0) {
                const step = root.fineStep * root.fpx
                ctx.globalAlpha = 0.4
                for (let j = Math.floor(x / step); j * step <= end; j++)
                    ctx.fillRect(Math.round(j * step - x), 1, 1, 3)
            }
        }
    }
    Repeater { // labelled major ticks
        model: root.visible ? Math.max(0, root.lastMajor - root.firstMajor + 1) : 0
        Item {
            id: tick
            required property int index
            readonly property real frame: (root.firstMajor + index) * root.labelEvery
            x: frame * root.fpx
            visible: frame === 0 || x + (root.fullTimecode ? 84 : 44) < root.width
            Rectangle {
                width: Theme.hairline
                height: root.height
                color: root.fullTimecode ? Theme.textMuted : Theme.borderStrong
            }
            Rectangle { // keeps minor ticks off the label
                visible: root.fullTimecode
                x: label.x - 2; y: label.y
                width: label.implicitWidth + 4; height: label.implicitHeight
                color: root.color
            }
            Text {
                id: label
                objectName: "rulerLabel"
                x: Theme.s1
                y: root.fullTimecode ? root.height - implicitHeight - 1 : (root.height - implicitHeight) / 2
                text: root.timecode(tick.frame)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    readonly property var visibleMarkers: {
        if (!visible) return []
        return markers.filter(function(m) { const x = root.xFor(m.frame); return x >= root.viewportX - 12 && x <= root.viewportX + root.viewportWidth + 12 })
    }
    Repeater { // markers — a small flag; colour tells the type
        model: root.visibleMarkers
        Item {
            id: flag
            required property var modelData
            objectName: "rulerMarker:" + modelData.frame
            visible: root.duration > 1 && modelData.frame < root.duration
            x: root.xFor(modelData.frame) - 1
            anchors.bottom: parent.bottom
            width: 9
            height: 10
            readonly property color tint: Theme.markerTint(modelData)
            Rectangle { // the pole, on the frame's left edge
                x: 0
                width: 2
                height: parent.height
                color: flag.tint
            }
            Rectangle { // the pennant
                x: 2
                y: 0
                width: parent.width - 2
                height: 5
                color: flag.tint
            }
        }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        visible: !root.skimEnabled || !root.snapEnabled
        text: (root.skimEnabled ? "" : qsTr("HOVER PREVIEW OFF (S)"))
              + (!root.skimEnabled && !root.snapEnabled ? "   " : "")
              + (root.snapEnabled ? "" : qsTr("SNAP OFF (N)"))
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.letterSpacing: 1
    }

    // Hovering near a flag surfaces the marker's label.
    readonly property var hoverMarker: {
        if (!rulerArea.containsMouse || duration < 2)
            return null
        let best = null
        let bestDist = 9
        for (let i = 0; i < markers.length; i++) {
            const m = markers[i]
            const d = Math.abs(xFor(m.frame) - rulerArea.mouseX)
            if (d < bestDist) {
                bestDist = d
                best = m
            }
        }
        return best
    }

    Rectangle {
        id: hoverLabel
        readonly property var marker: root.hoverMarker
        visible: marker !== null
        z: 5
        x: marker === null ? 0
           : Math.min(root.xFor(marker.frame) + Theme.s2,
                      root.width - width - Theme.s1)
        y: (root.height - height) / 2
        width: hoverText.implicitWidth + Theme.s2 * 2
        height: hoverText.implicitHeight + Theme.s1 * 2 - 2
        radius: Theme.rControl
        color: Theme.scrimStrong
        border.width: Theme.hairline
        border.color: marker !== null ? Theme.markerTint(marker)
                                      : "transparent"
        Text {
            id: hoverText
            anchors.centerIn: parent
            text: hoverLabel.marker === null ? ""
                  : (hoverLabel.marker.text !== undefined
                     && hoverLabel.marker.text !== ""
                     ? hoverLabel.marker.text
                     : root.typeLabel(hoverLabel.marker.type))
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
        }
    }

    // A drag along the ruler: where the pointer sits in the view, so the
    // owner can scroll on while it rests at an edge.
    readonly property bool dragging: rulerArea.pressed && !fine
    property real dragViewX: 0
    MouseArea {
        id: rulerArea
        anchors.fill: parent
        enabled: root.interactive
        hoverEnabled: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPositionChanged: function (mouse) {
            if (pressed) root.dragViewX = mouse.x - root.viewportX
            if (root.fine) {
                if (pressed || root.skimming) root.sampleSkimmedTo(root.sampleFor(mouse.x))
            } else if (pressed) {
                root.skimmedTo(root.frameFor(mouse.x))
            } else if (root.skimming) {
                root.hoverSkimmed(root.frameFor(mouse.x))
            }
        }
        onExited: if (!root.fine) root.hoverEnded()
        onPressed: function (mouse) {
            root.dragViewX = mouse.x - root.viewportX
            if (root.fine) root.samplePressedAt(root.sampleFor(mouse.x))
            else root.pressedAt(root.frameFor(mouse.x))
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.interactive
        acceptedButtons: Qt.RightButton
        onPressed: function (mouse) { root.rightPressedAt(root.frameFor(mouse.x)) }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }
}
