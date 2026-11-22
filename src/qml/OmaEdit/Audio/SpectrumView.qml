import QtQuick
import OmaEdit.Ui

// 48 log-spaced bins of the master, dBFS, as a filled spectrum with a
// slow-falling outline; the noise floor of a noise-reduction insert draws
// as a line so the operator can see what is being taken out.
Rectangle {
    id: root

    property var bins: []           // dBFS per bin
    property real floorDb: -100     // the NR floor to draw (−100 = none)
    property real maxDb: 0
    property real minDb: -90

    implicitHeight: Theme.hSpectrum
    color: Theme.meterWell
    border.color: Theme.border
    border.width: Theme.hairline

    property var held: []
    onBinsChanged: {
        var h = held.slice()
        for (var i = 0; i < bins.length; ++i) h[i] = Math.max(bins[i], (h[i] !== undefined ? h[i] : -100) - 2)
        held = h
        plot.requestPaint()
    }
    function yFor(v) { return height * (1 - Math.max(0, Math.min(1, (v - minDb) / (maxDb - minDb)))) }

    Canvas {
        id: plot
        Connections { target: Theme; function onAccentChanged() { plot.requestPaint() } } // the desktop accent can change while this is on screen
        anchors.fill: parent
        anchors.margins: 1
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var n = root.bins.length
            if (n === 0) return
            var bw = width / n
            ctx.fillStyle = String(Theme.spectrumFill)
            for (var i = 0; i < n; ++i) {
                var y = root.yFor(root.bins[i])
                ctx.fillRect(i * bw, y, Math.max(1, bw - 1), height - y)
            }
            ctx.strokeStyle = String(Theme.accent)
            ctx.lineWidth = 1
            ctx.beginPath()
            for (var j = 0; j < n; ++j) { var hy = root.yFor(root.held[j] !== undefined ? root.held[j] : -100); if (j === 0) ctx.moveTo(j * bw + bw / 2, hy); else ctx.lineTo(j * bw + bw / 2, hy) }
            ctx.stroke()
            if (root.floorDb > -99) {
                ctx.strokeStyle = String(Theme.warning)
                ctx.setLineDash([3, 3])
                ctx.beginPath(); ctx.moveTo(0, root.yFor(root.floorDb)); ctx.lineTo(width, root.yFor(root.floorDb)); ctx.stroke()
                ctx.setLineDash([])
            }
        }
    }
    Connections { target: root; function onFloorDbChanged() { plot.requestPaint() } }
    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 2
        Repeater {
            model: ["100", "1k", "10k"]
            delegate: Text {
                required property string modelData
                required property int index
                width: (root.width - 4) / 3
                horizontalAlignment: index === 0 ? Text.AlignLeft : index === 2 ? Text.AlignRight : Text.AlignHCenter
                text: modelData
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 2
                color: Theme.meterScale
            }
        }
    }
}
