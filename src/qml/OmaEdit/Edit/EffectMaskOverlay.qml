pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// An effect's mask drawn over the program picture, with handles: the middle
// one moves the shape; a rectangle's or ellipse's corners size it about its
// centre (along its own turned axes) and the round one above it turns it; a
// polygon's corners move one by one.
// The mask's places are fractions of `area`, where the clip's frame lands.
Item {
    id: root
    property var mask: null          // {shape, x, y, w, h, rotation, points, feather, invert}
    property rect area: Qt.rect(0, 0, 0, 0)
    property bool editable: true
    signal edited(var mask)
    signal started()
    signal finished()

    visible: mask !== null && area.width > 0 && area.height > 0

    // While a handle is held: the mask as dragged, and as it was.
    property var draft: null
    property var before: null
    readonly property var m: draft !== null ? draft : mask
    readonly property bool polygon: m !== null && m.shape === "polygon"
    readonly property var corners: {
        if (m === null) return []
        if (polygon) return (m.points || []).map(function(p) { return Qt.point(area.x + p[0] * area.width, area.y + p[1] * area.height) })
        const turn = (m.rotation || 0) * Math.PI / 180, c = Math.cos(turn), s = Math.sin(turn)
        const cx = area.x + m.x * area.width, cy = area.y + m.y * area.height
        const hw = m.w / 2 * area.width, hh = m.h / 2 * area.height
        return [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(function(k) {
            return Qt.point(cx + k[0] * hw * c - k[1] * hh * s, cy + k[0] * hw * s + k[1] * hh * c)
        })
    }
    readonly property point middle: {
        if (m === null || !corners.length) return Qt.point(0, 0)
        if (!polygon) return Qt.point(area.x + m.x * area.width, area.y + m.y * area.height)
        let x = 0, y = 0
        corners.forEach(function(p) { x += p.x; y += p.y })
        return Qt.point(x / corners.length, y / corners.length)
    }
    // The turning handle of a rectangle or an ellipse: out from the middle of its top side.
    readonly property int turnHandle: m === null || polygon || corners.length !== 4 ? -1 : 5
    readonly property point topMiddle: corners.length === 4 ? Qt.point((corners[0].x + corners[1].x) / 2, (corners[0].y + corners[1].y) / 2) : Qt.point(0, 0)
    readonly property point turnAt: {
        if (turnHandle < 0 || m === null) return Qt.point(0, 0)
        const turn = (m.rotation || 0) * Math.PI / 180
        return Qt.point(topMiddle.x + Math.sin(turn) * Theme.s3 * 2, topMiddle.y - Math.cos(turn) * Theme.s3 * 2)
    }
    function copy() { return JSON.parse(JSON.stringify(mask)) }
    function limit(v, low, high) { return Math.max(low, Math.min(high, v)) }
    function begin() { before = copy(); draft = copy(); started() }
    function end() { if (draft === null) return; edited(draft); finished(); draft = null; before = null }
    function cancel() { if (before === null) return; edited(before); finished(); draft = null; before = null }
    // Handle 0 is the middle; the rest are the corners in order.
    function drag(handle, px, py, pressX, pressY) {
        if (before === null) return
        const next = JSON.parse(JSON.stringify(before))
        const fx = (px - pressX) / area.width, fy = (py - pressY) / area.height
        if (handle === 0) {
            if (polygon) next.points = before.points.map(function(p) { return [limit(p[0] + fx, -1, 2), limit(p[1] + fy, -1, 2)] })
            else { next.x = limit(before.x + fx, -1, 2); next.y = limit(before.y + fy, -1, 2) }
        } else if (handle === turnHandle) {
            // Straight up from the centre is no turn; clockwise turns it clockwise.
            const dx = px - (area.x + before.x * area.width), dy = py - (area.y + before.y * area.height)
            if (dx !== 0 || dy !== 0) next.rotation = Math.round(Math.atan2(dx, -dy) * 180 / Math.PI * 10) / 10
        } else if (polygon) {
            next.points[handle - 1] = [limit((px - area.x) / area.width, -1, 2), limit((py - area.y) / area.height, -1, 2)]
        } else {
            const turn = (before.rotation || 0) * Math.PI / 180, c = Math.cos(turn), s = Math.sin(turn)
            const tx = px - (area.x + before.x * area.width), ty = py - (area.y + before.y * area.height)
            next.w = limit(2 * Math.abs(tx * c + ty * s) / area.width, .001, 4)
            next.h = limit(2 * Math.abs(ty * c - tx * s) / area.height, .001, 4)
        }
        draft = next
        edited(next)
    }
    onVisibleChanged: if (!visible) end()

    Canvas {
        id: outline
        anchors.fill: parent
        Connections { target: Theme; function onAccentChanged() { outline.requestPaint() } }
        Connections { target: root; function onCornersChanged() { outline.requestPaint() } }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const pts = root.corners
            if (root.m === null || pts.length < 3) return
            const trace = function() {
                ctx.beginPath()
                if (root.m.shape === "ellipse") {
                    // The ellipse inside the turned box: its centre, and half its sides as vectors.
                    const cx = root.middle.x, cy = root.middle.y
                    const ax = (pts[1].x - pts[0].x) / 2, ay = (pts[1].y - pts[0].y) / 2
                    const bx = (pts[3].x - pts[0].x) / 2, by = (pts[3].y - pts[0].y) / 2
                    for (let i = 0; i <= 72; ++i) {
                        const t = i / 72 * 2 * Math.PI, x = cx + ax * Math.cos(t) + bx * Math.sin(t), y = cy + ay * Math.cos(t) + by * Math.sin(t)
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                } else {
                    ctx.moveTo(pts[0].x, pts[0].y)
                    for (let i = 1; i < pts.length; ++i) ctx.lineTo(pts[i].x, pts[i].y)
                }
                ctx.closePath()
            }
            // Dark under bright, so the line reads on any picture.
            ctx.lineWidth = 3; ctx.strokeStyle = Qt.rgba(0, 0, 0, .55); trace(); ctx.stroke()
            ctx.lineWidth = 1.5; ctx.strokeStyle = Theme.accent; trace(); ctx.stroke()
            if (root.turnHandle >= 0 && root.editable) {
                const stem = function() { ctx.beginPath(); ctx.moveTo(root.topMiddle.x, root.topMiddle.y); ctx.lineTo(root.turnAt.x, root.turnAt.y) }
                ctx.lineWidth = 3; ctx.strokeStyle = Qt.rgba(0, 0, 0, .55); stem(); ctx.stroke()
                ctx.lineWidth = 1.5; ctx.strokeStyle = Theme.accent; stem(); ctx.stroke()
            }
        }
    }
    Repeater {
        // A count, not the corners themselves: the handles live through a drag.
        model: root.m === null ? 0 : root.corners.length + 1 + (root.turnHandle >= 0 ? 1 : 0)
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property bool turns: index === root.turnHandle
            readonly property point at: index === 0 ? root.middle : turns ? root.turnAt : (root.corners[index - 1] || Qt.point(0, 0))
            objectName: index === 0 ? "effectMaskMiddle" : turns ? "effectMaskTurn" : "effectMaskCorner:" + (index - 1)
            visible: root.editable
            width: index === 0 ? Theme.s3 : Theme.s2
            height: width
            radius: index === 0 || turns ? width / 2 : 0
            x: at.x - width / 2
            y: at.y - height / 2
            color: Theme.accent
            border.width: Theme.hairline
            border.color: Qt.rgba(0, 0, 0, .6)
            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.s1
                cursorShape: handle.index === 0 ? Qt.SizeAllCursor : handle.turns ? Qt.PointingHandCursor : Qt.CrossCursor
                property point press
                onPressed: function(mouse) { press = mapToItem(root, mouse.x, mouse.y); root.begin() }
                onPositionChanged: function(mouse) {
                    if (!pressed) return
                    const p = mapToItem(root, mouse.x, mouse.y)
                    root.drag(handle.index, p.x, p.y, press.x, press.y)
                }
                onReleased: root.end()
                onCanceled: root.cancel()
            }
        }
    }
}
