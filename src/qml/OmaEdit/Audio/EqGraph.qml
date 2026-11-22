import QtQuick
import OmaEdit.Ui

// The five-band EQ curve on a log frequency axis: the response is the sum
// of each band's bell/shelf in dB (a display model of the avfilter
// biquads), the bands' handles drag frequency (x) and gain (y).
Rectangle {
    id: root

    property var bands: []          // [{f, g, q}] ×5
    property int current: -1
    property real gainRange: 18
    signal bandEdited(int index, real f, real g)
    signal bandSelected(int index)
    signal editingStarted()
    signal editingFinished()
    clip: true

    implicitHeight: Theme.hEqGraph
    color: Theme.meterWell
    border.color: Theme.border
    border.width: Theme.hairline

    readonly property real fLo: 20
    readonly property real fHi: 20000
    function xFor(f) { return width * Math.log(f / fLo) / Math.log(fHi / fLo) }
    function fFor(x) { return fLo * Math.pow(fHi / fLo, Math.max(0, Math.min(1, x / width))) }
    function yFor(g) { return height / 2 - g / gainRange * (height / 2 - 6) }
    function gFor(y) { return -(y - height / 2) / (height / 2 - 6) * gainRange }
    // Match the 48 kHz bass/equalizer/treble biquads used by the audio engine.
    // Prepare coefficients once per edit, then evaluate their transfer functions.
    property real sampleRate: 48000
    readonly property var coefficients: bands.map(function(b, i) {
        var gain = Math.max(-24, Math.min(24, b.g))
        if (Math.abs(gain) < 0.05) return null
        var A = Math.pow(10, gain / 40), w = 2 * Math.PI * Math.max(20, b.f) / sampleRate
        var c = Math.cos(w), alpha = Math.sin(w) / (2 * Math.max(0.1, b.q))
        var p = A + 1, m = A - 1, t = Math.sqrt(2 * A) * alpha
        if (i === 0) return [[A*(p-m*c+t), 2*A*(m-p*c), A*(p-m*c-t)], [p+m*c+t, -2*(m+p*c), p+m*c-t]]
        if (i === 4) return [[A*(p+m*c+t), -2*A*(m+p*c), A*(p+m*c-t)], [p-m*c+t, 2*(m-p*c), p-m*c-t]]
        return [[1+alpha*A, -2*c, 1-alpha*A], [1+alpha/A, -2*c, 1-alpha/A]]
    })
    function powerAt(v, w) {
        var re = v[0] + v[1]*Math.cos(w) + v[2]*Math.cos(2*w)
        var im = v[1]*Math.sin(w) + v[2]*Math.sin(2*w)
        return re*re + im*im
    }
    function response(f) {
        var total = 0, w = 2 * Math.PI * f / sampleRate
        for (var i = 0; i < coefficients.length; ++i) {
            var c = coefficients[i]
            if (c) total += 10 * Math.log(powerAt(c[0], w) / powerAt(c[1], w)) / Math.LN10
        }
        return total
    }

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: 1
        readonly property var series: root.bands
        onSeriesChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            // grid
            ctx.strokeStyle = String(Theme.border)
            ctx.lineWidth = 1
            var marks = [50, 100, 200, 500, 1000, 2000, 5000, 10000]
            for (var m = 0; m < marks.length; ++m) { var x = root.xFor(marks[m]); ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke() }
            for (var g = -12; g <= 12; g += 6) { var y = root.yFor(g); ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke() }
            ctx.strokeStyle = String(Theme.borderStrong)
            ctx.beginPath(); ctx.moveTo(0, root.yFor(0)); ctx.lineTo(width, root.yFor(0)); ctx.stroke()
            // curve
            ctx.beginPath()
            ctx.moveTo(0, root.yFor(0))
            for (var px = 0; px <= width; px += 2) ctx.lineTo(px, root.yFor(root.response(root.fFor(px))))
            ctx.lineTo(width, root.yFor(0))
            ctx.closePath()
            ctx.fillStyle = String(Theme.eqFill)
            ctx.fill()
            ctx.beginPath()
            for (var px2 = 0; px2 <= width; px2 += 2) { var yy = root.yFor(root.response(root.fFor(px2))); if (px2 === 0) ctx.moveTo(px2, yy); else ctx.lineTo(px2, yy) }
            ctx.strokeStyle = String(Theme.eqCurve)
            ctx.lineWidth = 1.5
            ctx.stroke()
        }
    }
    Repeater {
        model: root.bands.length
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property var band: root.bands[index]
            width: 10; height: 10; radius: 5
            x: root.xFor(band.f) - 5
            y: root.yFor(band.g) - 5
            color: root.current === index ? Theme.accent : Theme.textPrimary
            border.color: Theme.scrimStrong
            border.width: Theme.hairline
            DragHandler {
                target: null
                onActiveChanged: { if (active) { root.bandSelected(handle.index); root.editingStarted() } else root.editingFinished() }
                onCentroidChanged: if (active) root.bandEdited(handle.index, Math.round(root.fFor(centroid.position.x + handle.x)), Math.round(Math.max(-root.gainRange, Math.min(root.gainRange, root.gFor(centroid.position.y + handle.y))) * 10) / 10)
            }
            TapHandler { onTapped: root.bandSelected(handle.index); onDoubleTapped: root.bandEdited(handle.index, handle.band.f, 0) }
        }
    }
    Repeater {
        model: root.width >= 320 ? [20, 100, 1000, 10000, 20000] : [20, 100, 1000, 20000]
        delegate: Text {
            required property var modelData
            x: Math.max(2, Math.min(root.width - width - 2, root.xFor(modelData) - width / 2))
            y: root.height - height - 2
            text: modelData >= 1000 ? (modelData / 1000) + "k" : String(modelData)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel - 2
            color: Theme.meterScale
        }
    }
    Text { anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 2; text: "+" + root.gainRange; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 2; color: Theme.meterScale }
    Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 2; text: "0"; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 2; color: Theme.meterScale }
}
