import QtQuick
import OmaEdit.Ui

// Loudness history: momentary LUFS per 100 ms hop as a filled trace, the
// target as a line with its tolerance band, a scale on the right.
Rectangle {
    id: root

    property var history: []        // LUFS, oldest first
    property real target: -23
    property real tolerance: 1
    property real maxLufs: -6
    property real minLufs: -40

    implicitHeight: Theme.hLoudnessGraph
    color: Theme.meterWell
    border.color: Theme.border
    border.width: Theme.hairline

    function yFor(v) { return height * (1 - Math.max(0, Math.min(1, (v - minLufs) / (maxLufs - minLufs)))) }

    Rectangle { // tolerance band
        x: 1
        width: parent.width - 2
        y: root.yFor(root.target + root.tolerance)
        height: Math.max(1, root.yFor(root.target - root.tolerance) - y)
        color: Theme.rangeFill
    }
    Rectangle { x: 1; width: parent.width - 2; y: root.yFor(root.target); height: 1; color: Theme.warning; opacity: 0.8 }

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: 1
        readonly property var series: root.history
        onSeriesChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var n = series.length
            if (n < 2) return
            var step = width / Math.max(1, n - 1)
            ctx.beginPath()
            ctx.moveTo(0, height)
            for (var i = 0; i < n; ++i) ctx.lineTo(i * step, root.yFor(series[i]) - 1)
            ctx.lineTo((n - 1) * step, height)
            ctx.closePath()
            ctx.fillStyle = String(Theme.eqFill)
            ctx.fill()
            ctx.beginPath()
            for (var j = 0; j < n; ++j) { var y = root.yFor(series[j]) - 1; if (j === 0) ctx.moveTo(0, y); else ctx.lineTo(j * step, y) }
            ctx.strokeStyle = String(Theme.eqCurve)
            ctx.lineWidth = 1.2
            ctx.stroke()
        }
    }
    Column {
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        Repeater {
            model: [-6, -12, -18, -24, -30, -36]
            delegate: Text {
                required property var modelData
                y: root.yFor(modelData) - height / 2
                text: String(modelData)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 2
                color: Theme.meterScale
            }
        }
    }
}
