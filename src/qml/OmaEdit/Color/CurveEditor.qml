import QtQuick
import OmaEdit.Ui

// A curve on a unit square: points as [[x, y], …], drawn with smooth
// (monotone cubic) or linear interpolation matching the renderer. Drag a point,
// double-click to add one, Alt-click to remove one (end points stay). `flat` curves (hue-vs-…) draw their rest
// line at y = 0.5; the background can be a hue strip for hue-vs curves.
Rectangle {
    id: root

    property var points: [[0, 0], [1, 1]]
    property color tint: Theme.eqCurve
    property bool hueBackground: false
    property real restY: -1              // 0.5 for the hue-vs curves
    property bool enabled2: true
    property string interpolation: "smooth"
    signal edited(var points)
    signal editingFinished(var points)

    implicitHeight: 160
    color: Theme.meterWell
    border.color: activeFocus ? Theme.accent : Theme.border
    border.width: activeFocus ? Theme.focusRing : Theme.hairline
    clip: true

    activeFocusOnTab: enabled2
    readonly property bool keyboardConsumesKeys: true
    property string accessibleLabel: qsTr("Colour curve")
    property int selectedPoint: 0
    Accessible.role: Accessible.Pane
    Accessible.name: accessibleLabel
    Accessible.description: qsTr("Page Up and Page Down select points; arrows adjust; Shift makes fine adjustments; Insert adds a point; Delete removes it.")
                            + (shown.length ? qsTr(" Point %1 of %2: input %3, output %4.")
                                .arg(selectedPoint + 1).arg(shown.length)
                                .arg(shown[Math.min(selectedPoint, shown.length - 1)][0].toFixed(3))
                                .arg(shown[Math.min(selectedPoint, shown.length - 1)][1].toFixed(3)) : "")
    onSelectedPointChanged: plot.requestPaint()
    onActiveFocusChanged: plot.requestPaint()
    onPointsChanged: {
        selectedPoint = Math.max(0, Math.min(selectedPoint, points.length - 1))
        if (!drag.active) working = null
    }
    Keys.onPressed: function(event) {
        if (!enabled2 || shown.length < 2) { event.accepted = false; return }
        let pts = sorted(shown)
        const i = Math.max(0, Math.min(selectedPoint, pts.length - 1))
        const step = event.modifiers & Qt.ShiftModifier ? 0.001 : 0.01
        if (event.key === Qt.Key_PageUp) selectedPoint = Math.max(0, i - 1)
        else if (event.key === Qt.Key_PageDown) selectedPoint = Math.min(pts.length - 1, i + 1)
        else if (event.key === Qt.Key_Home) selectedPoint = 0
        else if (event.key === Qt.Key_End) selectedPoint = pts.length - 1
        else {
            if (event.key === Qt.Key_Insert) {
                const left = Math.min(i, pts.length - 2)
                if (pts[left + 1][0] - pts[left][0] <= 0.02) return
                pts.splice(left + 1, 0, [(pts[left][0] + pts[left + 1][0]) / 2, (pts[left][1] + pts[left + 1][1]) / 2])
                selectedPoint = left + 1
            } else if (event.key === Qt.Key_Delete) {
                if (i === 0 || i === pts.length - 1) return
                pts.splice(i, 1); selectedPoint = Math.min(i, pts.length - 1)
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                pts[i][1] = Math.max(0, Math.min(1, pts[i][1] + (event.key === Qt.Key_Up ? step : -step)))
            } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                if (i === 0 || i === pts.length - 1) return
                pts[i][0] = Math.max(pts[i - 1][0] + 0.01, Math.min(pts[i + 1][0] - 0.01,
                    pts[i][0] + (event.key === Qt.Key_Right ? step : -step)))
            } else { event.accepted = false; return }
            working = pts
            root.editingFinished(pts)
        }
        event.accepted = true
    }

    property var working: null           // during a drag
    readonly property var shown: working !== null ? working : points
    function xFor(u) { return 1 + u * (width - 2) }
    function yFor(v) { return height - 1 - v * (height - 2) }
    function uFor(x) { return Math.max(0, Math.min(1, (x - 1) / (width - 2))) }
    function vFor(y) { return Math.max(0, Math.min(1, (height - 1 - y) / (height - 2))) }
    function sorted(pts) { var c = pts.map(function (p) { return [p[0], p[1]] }); c.sort(function (a, b) { return a[0] - b[0] }); return c }
    readonly property var samples: sorted(shown)
    // Fritsch–Carlson tangents, as the renderer takes them (DisplayGrade::
    // Curve): the secants' mean where they agree, flat at a turn, limited
    // so no segment overshoots its ends. Rising points never turn back.
    readonly property var slopes: {
        var p = samples, n = p.length, secant = [], m = []
        if (n < 2) return []
        for (var i = 0; i + 1 < n; ++i)
            secant[i] = (p[i+1][1] - p[i][1]) / Math.max(1e-9, p[i+1][0] - p[i][0])
        m[0] = secant[0]; m[n-1] = secant[n-2]
        for (var j = 1; j + 1 < n; ++j)
            m[j] = secant[j-1] * secant[j] <= 0 ? 0 : (secant[j-1] + secant[j]) / 2
        for (var k = 0; k + 1 < n; ++k) {
            if (secant[k] === 0) { m[k] = 0; m[k+1] = 0; continue }
            var a = m[k] / secant[k], b = m[k+1] / secant[k], r = a * a + b * b
            if (r > 9) { var t = 3 / Math.sqrt(r); m[k] = t * a * secant[k]; m[k+1] = t * b * secant[k] }
        }
        return m
    }
    function valueAt(x) {
        var p = samples
        if (p.length < 2) return p.length ? p[0][1] : x
        if (x <= p[0][0]) return p[0][1]
        for (var i = 1; i < p.length; ++i) {
            if (x > p[i][0]) continue
            var h = p[i][0] - p[i-1][0]
            if (h <= 0) return p[i][1]
            var t = (x - p[i-1][0]) / h, y
            if (interpolation === "smooth") {
                var t2 = t * t, t3 = t2 * t
                y = (2*t3 - 3*t2 + 1) * p[i-1][1] + (t3 - 2*t2 + t) * h * slopes[i-1]
                    + (-2*t3 + 3*t2) * p[i][1] + (t3 - t2) * h * slopes[i]
            } else y = p[i-1][1] + (p[i][1] - p[i-1][1]) * t
            return Math.max(0, Math.min(1, y))
        }
        return p[p.length-1][1]
    }
    function addPoint(u, v) {
        var pts = sorted(points)
        // Duplicate or near-duplicate inputs are rejected by the RGB filter.
        // Select the existing point instead of silently invalidating the curve.
        for (var i = 0; i < pts.length; ++i) {
            if (Math.abs(pts[i][0] - u) < 0.01) { selectedPoint = i; return }
        }
        pts.push([u, v]); pts = sorted(pts)
        selectedPoint = pts.findIndex(function(p) { return p[0] === u })
        root.editingFinished(pts)
    }
    function nearest(x, y) {
        var best = -1, bestD = 100
        for (var i = 0; i < shown.length; ++i) {
            var dx = xFor(shown[i][0]) - x, dy = yFor(shown[i][1]) - y
            var d = dx * dx + dy * dy
            if (d < bestD) { bestD = d; best = i }
        }
        return best
    }

    Rectangle { // hue strip background
        anchors.fill: parent
        anchors.margins: 1
        visible: root.hueBackground
        opacity: 0.35
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#ff4040" }
            GradientStop { position: 0.17; color: "#ffe040" }
            GradientStop { position: 0.33; color: "#40ff60" }
            GradientStop { position: 0.5; color: "#40e0ff" }
            GradientStop { position: 0.67; color: "#4060ff" }
            GradientStop { position: 0.83; color: "#e040ff" }
            GradientStop { position: 1.0; color: "#ff4040" }
        }
    }
    Canvas {
        id: plot
        anchors.fill: parent
        readonly property var series: root.shown
        onSeriesChanged: requestPaint()
        Connections { target: root; function onInterpolationChanged() { plot.requestPaint() } }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.strokeStyle = String(Theme.border)
            ctx.lineWidth = 1
            for (var g = 1; g < 4; ++g) {
                ctx.beginPath(); ctx.moveTo(root.xFor(g / 4), 0); ctx.lineTo(root.xFor(g / 4), height); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(0, root.yFor(g / 4)); ctx.lineTo(width, root.yFor(g / 4)); ctx.stroke()
            }
            ctx.strokeStyle = String(Theme.borderStrong)
            ctx.setLineDash([3, 3])
            ctx.beginPath()
            if (root.restY >= 0) { ctx.moveTo(0, root.yFor(root.restY)); ctx.lineTo(width, root.yFor(root.restY)) }
            else { ctx.moveTo(root.xFor(0), root.yFor(0)); ctx.lineTo(root.xFor(1), root.yFor(1)) }
            ctx.stroke()
            ctx.setLineDash([])
            var pts = root.sorted(series)
            if (pts.length < 2) return
            var n = pts.length
            ctx.strokeStyle = String(root.tint)
            ctx.lineWidth = 2
            ctx.beginPath()
            ctx.moveTo(root.xFor(0), root.yFor(root.valueAt(0)))
            for (var x = 1; x <= width; ++x)
                ctx.lineTo(root.xFor(x / width), root.yFor(root.valueAt(x / width)))
            ctx.stroke()
            ctx.fillStyle = String(Theme.textPrimary)
            for (var p = 0; p < n; ++p) {
                ctx.beginPath(); ctx.arc(root.xFor(pts[p][0]), root.yFor(pts[p][1]), 4, 0, Math.PI * 2); ctx.fill()
                if (root.activeFocus && p === root.selectedPoint) {
                    ctx.strokeStyle = String(Theme.textPrimary); ctx.lineWidth = 2
                    ctx.strokeRect(root.xFor(pts[p][0]) - 7, root.yFor(pts[p][1]) - 7, 14, 14)
                }
            }
        }
    }
    DragHandler {
        id: drag
        enabled: root.enabled2
        target: null
        property int index: -1
        onActiveChanged: {
            if (active) {
                index = root.nearest(centroid.pressPosition.x, centroid.pressPosition.y)
                if (index < 0) return
                root.selectedPoint = index
                root.working = root.points.map(function (p) { return [p[0], p[1]] })
            } else if (root.working !== null) {
                var out = root.sorted(root.working)
                root.working = null
                root.editingFinished(out)
            }
        }
        onCentroidChanged: {
            if (!active || index < 0 || root.working === null) return
            var w = root.working.map(function (p) { return [p[0], p[1]] })
            var u = root.uFor(centroid.position.x), v = root.vFor(centroid.position.y)
            if (index === 0) u = 0
            else if (index === w.length - 1) u = 1
            else u = Math.max(w[index - 1][0] + 0.01, Math.min(w[index + 1][0] - 0.01, u))
            w[index] = [u, v]
            root.working = w
            root.edited(w)
        }
    }
    TapHandler {
        enabled: root.enabled2
        acceptedModifiers: Qt.NoModifier
        onDoubleTapped: function (ev) {
            root.addPoint(root.uFor(ev.position.x), root.vFor(ev.position.y))
        }
    }
    TapHandler {
        enabled: root.enabled2
        acceptedModifiers: Qt.AltModifier
        onTapped: function (ev) {
            var i = root.nearest(ev.position.x, ev.position.y)
            var pts = root.sorted(root.points)
            if (i <= 0 || i >= pts.length - 1) return
            pts.splice(i, 1)
            root.editingFinished(pts)
        }
    }
}
