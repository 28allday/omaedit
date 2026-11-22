import QtQuick
import omaedit 1.0
import OmaEdit.Ui

// One clip on the audio timeline: role-tinted body, its waveform,
// fade ramps, the gain/automation band (a flat line at the clip volume, or
// the keyframe polyline), a dB readout and an fx badge. The owner handles
// move/trim; this item raises band and fade edits itself.
Item {
    id: root

    property var entry: ({})
    // When the gain band last took a double-tap: the clip's own double-tap
    // (open its effects) stands down for that one.
    property real bandDoubleTapTime: 0
    property real fps: 25
    // The part of the clip on screen, in its own pixels.
    property real viewportX: 0
    property real viewportWidth: width
    // The timeline frame at the clip's left edge (a precise cut's is not a
    // whole frame): the waveform is of the sound from there on.
    property real startFrame: entry.start !== undefined ? entry.start : 0
    // The clip as a trim or a slip in hand would leave it (WaveformItem.pending).
    property var pending: ({})
    // The waveform standing from the foot of its lane, not about a centre line.
    property bool rectified: false
    // The waveform covers the part on screen and a margin, in steps: a
    // scroll within a step paints nothing again.
    readonly property real paintFrom: Math.max(0, Math.min(width, Math.floor(viewportX / 512) * 512 - 512))
    readonly property real paintTo: Math.max(paintFrom, Math.min(width, Math.ceil((viewportX + viewportWidth) / 512) * 512 + 512))
    property real fpx: 1                  // px per frame
    property color tint: Theme.clipAudio
    property bool selected: false
    property bool dropTarget: false
    property bool showAutomation: true
    property bool dragging: false
    property bool locked: false

    signal volumeEdited(real db)
    signal volumeKfEdited(var kf)
    signal fadesEdited(int fadeIn, int fadeOut)
    signal fxRequested()

    readonly property int frames: entry.frames !== undefined ? entry.frames : 0
    // While the flat gain line is dragged the band previews `previewDb`;
    // the engine hears one edit, on release (an undo step per pointer move
    // would flood the history).
    property real previewDb: NaN
    readonly property real volume: !isNaN(previewDb) ? previewDb : (entry.volume !== undefined ? entry.volume : 0)
    readonly property var kf: entry.volumeKf !== undefined ? entry.volumeKf : []
    readonly property int fadeIn: entry.fadeIn !== undefined ? entry.fadeIn : 0
    readonly property int fadeOut: entry.fadeOut !== undefined ? entry.fadeOut : 0
    readonly property bool hasFx: (entry.eq !== undefined && entry.eq !== "") || (entry.pan !== undefined && entry.pan !== 0)
    readonly property bool disabled: entry.disabled === true

    // dB → y inside the body: +12 at the top, 0 at 30 %, −60 at the bottom
    // (the fader taper, so the band reads like the strips).
    function yFor(db) {
        db = Math.max(-60, Math.min(12, db))
        var p = db >= 0 ? 0.68 + db / 12 * 0.32 : db >= -20 ? 0.32 + (db + 20) / 20 * 0.36 : (db + 60) / 40 * 0.32
        return body.height * (1 - p)
    }
    function dbFor(y) {
        var p = Math.max(0, Math.min(1, 1 - y / body.height))
        return p >= 0.68 ? (p - 0.68) / 0.32 * 12 : p >= 0.32 ? -20 + (p - 0.32) / 0.36 * 20 : -60 + p / 0.32 * 40
    }
    function bandPoints() { // [{x, y, frame, db}] in body coordinates
        var pts = []
        if (kf.length === 0) {
            pts.push({ x: 0, y: yFor(volume), frame: 0, db: volume })
            pts.push({ x: body.width, y: yFor(volume), frame: frames - 1, db: volume })
            return pts
        }
        for (var i = 0; i < kf.length; ++i)
            pts.push({ x: kf[i].frame * root.fpx, y: yFor(kf[i].db), frame: kf[i].frame, db: kf[i].db })
        return pts
    }

    Rectangle {
        id: body
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        radius: Theme.rControl
        color: root.disabled ? Theme.clipGap : Qt.darker(root.tint, 1.9)
        border.color: root.dropTarget ? Theme.dropValid : root.selected ? Theme.accent : Qt.darker(root.tint, 1.3)
        border.width: root.selected || root.dropTarget ? Theme.selectionRing : Theme.hairline
        opacity: root.dragging ? 0.7 : 1
        clip: true

        WaveformItem { // waveform: left over right when the row is tall enough
            id: clipWave
            objectName: "audioClipWave"
            x: root.paintFrom
            y: Theme.hairline
            width: root.paintTo - root.paintFrom
            height: parent.height - 2 * Theme.hairline
            visible: ready && width > 0
            entry: root.entry
            pending: root.pending
            rectified: root.rectified
            fps: root.fps
            firstFrame: root.startFrame + root.paintFrom / Math.max(.000001, root.fpx)
            lastFrame: root.startFrame + root.paintTo / Math.max(.000001, root.fpx)
            // While the gain line is dragged the wave follows the drag.
            previewDb: root.previewDb
            // The peaks in the track's colour, the body of the sound (its
            // RMS) lighter inside them.
            peakColor: root.disabled ? Theme.textMuted : Qt.lighter(root.tint, 1.2)
            bodyColor: root.disabled ? Theme.textMuted : Qt.lighter(root.tint, 1.9)
            clipColor: Theme.danger
        }
        Rectangle { // no-wave placeholder line
            visible: !clipWave.visible
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 1
            color: Qt.lighter(root.tint, 1.3)
            opacity: 0.5
        }

        // Fade ramps as darkened triangles.
        Canvas {
            id: fades
            objectName: "audioFadeRamps"
            Connections { target: Theme; function onAccentChanged() { fades.requestPaint() } }
            x: root.viewportX; width: Math.min(root.viewportWidth,8192); height: parent.height
            onXChanged: requestPaint()
            readonly property real inW: fadeInHandle.displayValue * root.fpx
            readonly property real outW: fadeOutHandle.displayValue * root.fpx
            onInWChanged: requestPaint()
            onOutWChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset(); ctx.clearRect(0, 0, width, height); ctx.translate(-root.viewportX,0)
                ctx.fillStyle = String(Theme.scrim)
                if (inW > 0) { ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(inW, 0); ctx.lineTo(0, height); ctx.closePath(); ctx.fill() }
                if (outW > 0) { ctx.beginPath(); ctx.moveTo(root.width, 0); ctx.lineTo(root.width - outW, 0); ctx.lineTo(root.width, height); ctx.closePath(); ctx.fill() }
                ctx.strokeStyle = String(Theme.accent)
                ctx.globalAlpha = 1
                ctx.lineWidth = 2
                if (inW > 0) { ctx.beginPath(); ctx.moveTo(0, height); ctx.lineTo(inW, 0); ctx.stroke() }
                if (outW > 0) { ctx.beginPath(); ctx.moveTo(root.width, height); ctx.lineTo(root.width - outW, 0); ctx.stroke() }
            }
        }

        // Name + dB, each on a backing of its own over the waveform. The
        // name steps past a fade-in knob at the clip's start, so selecting a
        // clip never hides the first letters.
        Rectangle {
            objectName: "audioClipNameBacking"
            x: nameText.x - 2
            y: nameText.y
            width: Math.min(nameText.implicitWidth, nameText.width) + 4
            height: nameText.height
            radius: Theme.rControl
            color: Theme.scrim
            visible: nameText.visible && nameText.text !== "" && nameText.width > 0
        }
        Rectangle {
            x: dbText.x - 2
            y: dbText.y
            width: dbText.width + 4
            height: dbText.height
            radius: Theme.rControl
            color: Theme.scrim
            visible: dbText.visible
        }
        Text {
            id: nameText
            objectName: "audioClipName"
            readonly property real knobClear: fadeInHandle.visible && fadeInHandle.x < Theme.s5 ? fadeInHandle.x + fadeInHandle.width * 0.75 + 2 : 0
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: Math.max(Theme.s1, knobClear)
            anchors.topMargin: 2
            width: Math.max(0, parent.width - anchors.leftMargin - dbText.width - Theme.s2)
            text: root.entry.name !== undefined ? root.entry.name : ""
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.clipLabelOnMedia
            style: Text.Outline
            styleColor: Theme.scrimStrong
            visible: parent.width > 40
        }
        Text {
            id: dbText
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: Theme.s1
            anchors.bottomMargin: 2
            text: qsTr("%1 dB").arg((root.volume > 0 ? "+" : "") + root.volume.toFixed(1))
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textPrimary
            style: Text.Outline
            styleColor: Theme.scrimStrong
            visible: parent.width > 70
        }
        Rectangle { // fx badge
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 2
            width: 18; height: 12; radius: Theme.rControl
            color: Theme.scrimStrong
            visible: root.hasFx && parent.width > 50
            Text { anchors.centerIn: parent; text: "fx"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 2; color: Theme.accent }
            TapHandler { onTapped: root.fxRequested() }
        }

        // Automation band.
        Canvas {
            id: band
            Connections { target: Theme; function onAccentChanged() { band.requestPaint() } } // the desktop accent can change while this is on screen
            x: root.viewportX; width: Math.min(root.viewportWidth,8192); height: parent.height
            onXChanged: requestPaint()
            visible: root.showAutomation
            readonly property var points: root.bandPoints()
            onPointsChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset();ctx.clearRect(0, 0, width, height);ctx.translate(-root.viewportX,0)
                var pts = points
                if (pts.length === 0) return
                ctx.strokeStyle = String(Theme.automationLine)
                ctx.lineWidth = root.selected ? 1.5 : 1
                ctx.globalAlpha = root.selected ? 0.95 : 0.7
                ctx.beginPath()
                ctx.moveTo(0, pts[0].y)
                for (var i = 0; i < pts.length; ++i) ctx.lineTo(pts[i].x, pts[i].y)
                ctx.lineTo(width, pts[pts.length - 1].y)
                ctx.stroke()
                if (root.kf.length > 0) {
                    ctx.fillStyle = String(Theme.automationPoint)
                    ctx.globalAlpha = 1
                    for (var k = 0; k < pts.length; ++k) { ctx.beginPath(); ctx.arc(pts[k].x, pts[k].y, root.selected ? 3.5 : 2.5, 0, Math.PI * 2); ctx.fill() }
                }
            }
        }

        // Band interaction: on the selected clip, drag the flat line for
        // gain, drag a point to move it, double-click to add one, Alt-click
        // to remove one.
        Item {
            id: bandHit
            anchors.fill: parent
            visible: root.showAutomation && root.selected && !root.locked
            property int hitIndex: -1
            property var working: []
            function nearest(x, y) {
                var pts = root.bandPoints()
                var best = -1, bestD = 64
                for (var i = 0; i < pts.length; ++i) {
                    var d = (pts[i].x - x) * (pts[i].x - x) + (pts[i].y - y) * (pts[i].y - y)
                    if (d < bestD) { bestD = d; best = i }
                }
                return best
            }
            function nearBand(x, y) {
                var pts = root.bandPoints()
                for (var i = 0; i + 1 < pts.length; ++i) {
                    if (x < pts[i].x - 2 || x > pts[i + 1].x + 2) continue
                    var t = pts[i + 1].x === pts[i].x ? 0 : (x - pts[i].x) / (pts[i + 1].x - pts[i].x)
                    var yy = pts[i].y + (pts[i + 1].y - pts[i].y) * t
                    if (Math.abs(yy - y) <= 6) return true
                }
                return false
            }
            DragHandler {
                id: bandDrag
                target: null
                enabled: bandHit.visible
                property real startDb: 0
                property bool onPoint: false
                property bool accepted: false
                // As the clip drag: a cancel (reported after deactivation)
                // voids the pending commit, and the preview is dropped.
                property int ticket: 0
                onCanceled: { ++ticket; root.previewDb = NaN; accepted = false }
                onActiveChanged: {
                    if (active) {
                        var p = centroid.pressPosition
                        accepted = false
                        if (root.kf.length > 0) {
                            bandHit.hitIndex = bandHit.nearest(p.x, p.y)
                            onPoint = bandHit.hitIndex >= 0
                            if (!onPoint) return
                            bandHit.working = root.kf.map(function (k) { return { frame: k.frame, db: k.db } })
                            accepted = true
                        } else {
                            if (!bandHit.nearBand(p.x, p.y)) return
                            startDb = root.volume
                            accepted = true
                        }
                    } else if (accepted) {
                        const mine = ++ticket, keyed = root.kf.length > 0, working = bandHit.working
                        Qt.callLater(function () {
                            if (mine !== ticket) return
                            if (keyed) root.volumeKfEdited(working)
                            else { var db = root.previewDb; root.previewDb = NaN; if (!isNaN(db)) root.volumeEdited(db) }
                        })
                    }
                }
                onTranslationChanged: {
                    if (!active || !accepted) return
                    if (root.kf.length > 0) {
                        var p = centroid.position
                        var w = bandHit.working.slice()
                        var i = bandHit.hitIndex
                        var lo = i > 0 ? w[i - 1].frame + 1 : 0
                        var hi = i + 1 < w.length ? w[i + 1].frame - 1 : root.frames - 1
                        w[i] = { frame: Math.max(lo, Math.min(hi, Math.round(p.x / root.fpx))), db: Math.round(root.dbFor(p.y) * 10) / 10 }
                        bandHit.working = w
                        band.requestPaint()
                        previewLine.points = w
                    } else {
                        root.previewDb = Math.round(root.dbFor(root.yFor(startDb) + translation.y) * 10) / 10
                    }
                }
            }
            TapHandler {
                enabled: bandHit.visible
                acceptedModifiers: Qt.NoModifier
                onDoubleTapped: function (ev) {
                    root.bandDoubleTapTime = Date.now()
                    var frame = Math.max(0, Math.min(root.frames - 1, Math.round(ev.position.x / root.fpx)))
                    var db = Math.round(root.dbFor(ev.position.y) * 10) / 10
                    var w = root.kf.map(function (k) { return { frame: k.frame, db: k.db } })
                    if (w.length === 0) w = [{ frame: 0, db: root.volume }, { frame: root.frames - 1, db: root.volume }]
                    w.push({ frame: frame, db: db })
                    w.sort(function (a, b) { return a.frame - b.frame })
                    root.volumeKfEdited(w)
                }
            }
            TapHandler {
                enabled: bandHit.visible && root.kf.length > 0
                acceptedModifiers: Qt.AltModifier
                onTapped: function (ev) {
                    var i = bandHit.nearest(ev.position.x, ev.position.y)
                    if (i < 0) return
                    var w = root.kf.map(function (k) { return { frame: k.frame, db: k.db } })
                    w.splice(i, 1)
                    root.volumeKfEdited(w.length >= 2 ? w : [])
                }
            }
            Canvas { // live preview of a point drag
                id: previewLine
                Connections { target: Theme; function onAccentChanged() { previewLine.requestPaint() } } // the desktop accent can change while this is on screen
                x: root.viewportX; width: Math.min(root.viewportWidth,8192); height: parent.height
                onXChanged: requestPaint()
                property var points: []
                visible: bandDrag.active && root.kf.length > 0
                onPointsChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset();ctx.clearRect(0, 0, width, height);ctx.translate(-root.viewportX,0)
                    if (points.length === 0) return
                    ctx.strokeStyle = String(Theme.accent)
                    ctx.lineWidth = 1.5
                    ctx.beginPath()
                    ctx.moveTo(0, root.yFor(points[0].db))
                    for (var i = 0; i < points.length; ++i) ctx.lineTo(points[i].frame * root.fpx, root.yFor(points[i].db))
                    ctx.lineTo(width, root.yFor(points[points.length - 1].db))
                    ctx.stroke()
                }
            }
        }
    }

    // Applied fades remain visible and editable without selecting the clip.
    ClipLengthHandle {
        id: fadeInHandle
        objectName: "fadeInHandle"
        visible: root.width >= 24 && (root.selected || value > 0)
        enabled: !root.locked
        value: root.fadeIn
        maximumValue: Math.max(0, root.frames - 1)
        pixelsPerFrame: root.fpx
        fps: root.fps
        coordinateItem: root.parent
        onValueEdited: function(frames) { root.fadesEdited(frames, root.fadeOut) }
    }
    ClipLengthHandle {
        id: fadeOutHandle
        objectName: "fadeOutHandle"
        visible: root.width >= 24 && (root.selected || value > 0)
        enabled: !root.locked
        value: root.fadeOut
        maximumValue: Math.max(0, root.frames - 1)
        pixelsPerFrame: root.fpx
        fps: root.fps
        fromEnd: true
        otherHandleX: fadeInHandle.visible ? fadeInHandle.x : NaN
        coordinateItem: root.parent
        onValueEdited: function(frames) { root.fadesEdited(root.fadeIn, frames) }
    }
}
