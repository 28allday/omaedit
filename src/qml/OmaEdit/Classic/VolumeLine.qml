import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// Volume line (guide p. 271): a horizontal dB line over every
// audio-carrying clip. Alt+click the line adds a white diamond
// keyframe; Alt+click a diamond removes it; drag a diamond to shape
// the ramp; with no keyframes, dragging the line sets whole-clip
// volume. Presses away from the line fall through to the block.
// Parameter line (guide p. 271): a horizontal control drawn over
// a clip. mode "volume" = the dB line on every audio-carrying clip;
// mode "opacity" = the Video Animation editor's 0..1 opacity line on a
// lane clip. Alt+click the line adds a white diamond keyframe (and
// drags it in the same gesture); Alt+click a diamond removes it; drag
// a diamond to shape the ramp; with no keyframes, dragging the line
// sets the whole-clip value. Presses away from the line fall through.
Item {
    id: vline
    objectName: (opacityMode ? "opacityLine:" : "volumeLine:") + clipIndex
    property var entry: null
    property int clipIndex: -1
    property bool lane: false
    property string mode: "volume"
    property bool active: true // instance-level gate (Video Animation swap)
    readonly property bool opacityMode: mode === "opacity"
    visible: active && entry !== undefined && entry !== null
             && (opacityMode
                 || (entry.gap !== true && entry.videoOnly !== true
                     && (entry.title === undefined
                         || entry.title === "")))

    readonly property string valueKey: opacityMode ? "alpha" : "db"
    readonly property real vMin: opacityMode ? 0 : -60
    readonly property real vMax: opacityMode ? 1 : 12
    readonly property color lineColor: opacityMode ? Ui.Theme.bandOpacity
                                                   : Ui.Theme.bandVolume
    readonly property var kf: {
        if (!entry)
            return []
        var list = opacityMode ? entry.opacityKf : entry.volumeKf
        return list !== undefined ? list : []
    }
    readonly property real val0: {
        if (!entry)
            return opacityMode ? 1 : 0
        var v = opacityMode ? entry.opacity : entry.volume
        return v !== undefined ? v : (opacityMode ? 1 : 0)
    }
    property int dragIndex: -2 // -2 idle, -1 whole line, >=0 a keyframe
    property var dragKf: []
    property real dragVal: 0
    property bool opacityGesture: false
    readonly property var shownKf: dragIndex > -2 ? dragKf : kf

    function valOf(point) { return point[valueKey] }
    function makeKey(frame, v, interpolation) {
        var key = { frame: frame }
        key[valueKey] = v
        if (opacityMode) key.interpolation = interpolation || "linear"
        return key
    }
    function yFor(v) { return height * (1 - (v - vMin) / (vMax - vMin)) }
    function vFor(y) {
        return Math.max(vMin, Math.min(vMax,
            (1 - y / height) * (vMax - vMin) + vMin))
    }
    function frames() { return entry ? Math.max(1, entry.frames) : 1 }
    function xFor(frame) { return frame / frames() * width }
    function frameFor(x) {
        return Math.max(0, Math.min(frames() - 1,
                                    Math.round(x / width * frames())))
    }
    function lineValAt(x) {
        var pts = shownKf
        if (pts.length === 0)
            return dragIndex === -1 ? dragVal : val0
        var f = x / width * frames()
        if (f <= pts[0].frame)
            return valOf(pts[0])
        for (var i = 1; i < pts.length; i++) {
            if (f < pts[i].frame) {
                var a = pts[i - 1], b = pts[i]
                if (opacityMode && a.interpolation === "hold") return valOf(a)
                var t = (f - a.frame) / Math.max(1, b.frame - a.frame)
                if (opacityMode && a.interpolation === "smooth") t = t * t * (3 - 2 * t)
                return valOf(a) + t * (valOf(b) - valOf(a))
            }
        }
        return valOf(pts[pts.length - 1])
    }
    function hitPoint(x, y) { // nearest diamond within 8px, else -1
        var pts = shownKf
        for (var i = 0; i < pts.length; i++)
            if (Math.abs(xFor(pts[i].frame) - x) <= 8
                    && Math.abs(yFor(valOf(pts[i])) - y) <= 8)
                return i
        return -1
    }
    function copyKf() {
        return JSON.parse(JSON.stringify(kf))
    }
    function commit(list) {
        if (opacityMode && !list.length && kf.length === 1)
            backend.setEditProperties(entry.track, entry.index, {opacity:valOf(kf[0]),opacityKf:[]})
        else if (opacityMode)
            backend.setConnectedOpacityKf(clipIndex, list)
        else if (lane)
            backend.setConnectedVolumeKf(clipIndex, list)
        else
            backend.setClipVolumeKf(clipIndex, list)
    }
    function beginOpacityGesture() {
        if (opacityMode) { backend.beginEditGesture(); opacityGesture = true }
    }
    function previewOpacity() {
        if (opacityGesture && entry)
            backend.setEditProperties(entry.track, entry.index, dragIndex === -1
                ? {opacity:dragVal,opacityKf:[]} : {opacityKf:dragKf})
    }
    function finishOpacityGesture() {
        if (opacityGesture) { backend.endEditGesture(); opacityGesture = false }
    }

    Canvas {
        id: volCanvas
        anchors.fill: parent
        clip: true
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (!vline.visible)
                return
            var pts = vline.shownKf
            ctx.globalAlpha = 0.9
            ctx.strokeStyle = String(vline.lineColor)
            ctx.lineWidth = 1.5
            ctx.beginPath()
            if (pts.length === 0) {
                var y = vline.yFor(vline.dragIndex === -1 ? vline.dragVal
                                                          : vline.val0)
                ctx.moveTo(0, y)
                ctx.lineTo(width, y)
            } else if (vline.opacityMode) {
                ctx.moveTo(Math.min(0, vline.xFor(pts[0].frame)), vline.yFor(vline.valOf(pts[0])))
                ctx.lineTo(vline.xFor(pts[0].frame), vline.yFor(vline.valOf(pts[0])))
                for (var segment = 1; segment < pts.length; ++segment) {
                    var a = pts[segment-1], b = pts[segment]
                    var ax = vline.xFor(a.frame), bx = vline.xFor(b.frame)
                    var ay = vline.yFor(vline.valOf(a)), by = vline.yFor(vline.valOf(b))
                    if (a.interpolation === "smooth")
                        ctx.bezierCurveTo(ax+(bx-ax)/3, ay, bx-(bx-ax)/3, by, bx, by)
                    else {
                        if (a.interpolation === "hold") ctx.lineTo(bx, ay)
                        ctx.lineTo(bx, by)
                    }
                }
                ctx.lineTo(Math.max(width, vline.xFor(pts[pts.length-1].frame)), vline.yFor(vline.valOf(pts[pts.length-1])))
            } else {
                ctx.moveTo(0, vline.yFor(vline.valOf(pts[0])))
                for (var i = 0; i < pts.length; i++)
                    ctx.lineTo(vline.xFor(pts[i].frame),
                               vline.yFor(vline.valOf(pts[i])))
                ctx.lineTo(width,
                           vline.yFor(vline.valOf(pts[pts.length - 1])))
            }
            ctx.stroke()
            ctx.globalAlpha = 1
            ctx.fillStyle = String(vline.lineColor)
            for (var j = 0; j < pts.length; j++) { // keyframe diamonds
                var px = vline.xFor(pts[j].frame)
                var py = vline.yFor(vline.valOf(pts[j]))
                ctx.beginPath()
                ctx.moveTo(px, py - 4)
                ctx.lineTo(px + 4, py)
                ctx.lineTo(px, py + 4)
                ctx.lineTo(px - 4, py)
                ctx.closePath()
                ctx.fill()
            }
        }
    }
    onShownKfChanged: volCanvas.requestPaint()
    onVal0Changed: volCanvas.requestPaint()
    onDragValChanged: volCanvas.requestPaint()
    onWidthChanged: volCanvas.requestPaint()
    onHeightChanged: volCanvas.requestPaint()
    onVisibleChanged: volCanvas.requestPaint()

    MouseArea {
        objectName: vline.opacityMode ? "opacityLineDrag" : "volumeLineDrag"
        anchors.fill: parent
        onPressed: function (mouse) {
            var point = vline.hitPoint(mouse.x, mouse.y)
            var nearLine =
                Math.abs(vline.yFor(vline.lineValAt(mouse.x)) - mouse.y)
                <= 7
            var alt = (mouse.modifiers & Qt.AltModifier) !== 0
            if (point >= 0 && alt) { // Alt on a diamond deletes it
                var trimmed = vline.copyKf()
                trimmed.splice(point, 1)
                vline.commit(trimmed)
                return
            }
            if (point >= 0) { // drag an existing keyframe
                vline.beginOpacityGesture()
                vline.dragKf = vline.copyKf()
                vline.dragIndex = point
                return
            }
            if (nearLine && alt) { // Alt on the line adds one, then drags
                vline.beginOpacityGesture()
                var added = vline.copyKf()
                var frame = vline.frameFor(mouse.x)
                var earlier = added.filter(function(k) { return k.frame <= frame })
                added = added.filter(function(k) { return k.frame !== frame })
                added.push(vline.makeKey(vline.frameFor(mouse.x),
                                         vline.lineValAt(mouse.x),
                                         earlier.length ? earlier[earlier.length-1].interpolation : "linear"))
                added.sort(function (a, b) { return a.frame - b.frame })
                vline.dragKf = added
                for (var i = 0; i < added.length; i++)
                    if (added[i].frame === vline.frameFor(mouse.x))
                        vline.dragIndex = i
                return
            }
            if (nearLine && vline.kf.length === 0) { // whole-clip drag
                vline.beginOpacityGesture()
                vline.dragVal = vline.val0
                vline.dragIndex = -1
                return
            }
            mouse.accepted = false // not ours — the block handles it
        }
        onPositionChanged: function (mouse) {
            if (!pressed || vline.dragIndex === -2)
                return
            if (vline.dragIndex === -1) {
                vline.dragVal = vline.vFor(mouse.y)
                vline.previewOpacity()
                return
            }
            var pts = vline.dragKf
            var i = vline.dragIndex
            var lo = Math.max(0, i > 0 ? pts[i - 1].frame + 1 : 0)
            var hi = Math.min(vline.frames() - 1, i < pts.length - 1 ? pts[i + 1].frame - 1 : vline.frames() - 1)
            pts[i] = vline.makeKey(
                Math.max(lo, Math.min(hi, vline.frameFor(mouse.x))),
                vline.vFor(mouse.y), pts[i].interpolation)
            vline.dragKf = pts // reassign so the binding notices
            volCanvas.requestPaint()
            vline.previewOpacity()
        }
        onReleased: {
            if (vline.opacityGesture) {
                vline.previewOpacity()
                vline.finishOpacityGesture()
            } else if (vline.dragIndex === -1) {
                var db = Math.round(vline.dragVal)
                if (db !== Math.round(vline.val0)) {
                    if (vline.lane)
                        backend.setConnectedVolume(vline.clipIndex, db)
                    else
                        backend.setClipVolume(vline.clipIndex, db)
                }
            } else if (vline.dragIndex >= 0) {
                vline.commit(vline.dragKf)
            }
            vline.dragIndex = -2
        }
        onCanceled: { vline.finishOpacityGesture(); vline.dragIndex = -2 }
    }
}
