import QtQuick
import OmaEdit.Ui

// The playhead: a thin red line with a solid head that sits in the ruler.
// The owner positions it (x = frame's left edge in the same frame→pixel map
// as the clip blocks) and gives it the full lane-stack height; it draws over
// every lane, never under one.
Item {
    id: root

    // Height of the ruler band the head sits in.
    property int headHeight: Theme.hTimeRuler

    width: 1
    z: 20

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        width: 1
        height: parent.height
        color: Theme.playhead
    }

    // The head: a small flag with a pointed base, centred on the line.
    Canvas {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0
        width: 11
        height: Math.min(root.headHeight, 12)
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = String(Theme.playhead)
            ctx.beginPath()
            ctx.moveTo(0, 0)
            ctx.lineTo(width, 0)
            ctx.lineTo(width, height * 0.55)
            ctx.lineTo(width / 2, height)
            ctx.lineTo(0, height * 0.55)
            ctx.closePath()
            ctx.fill()
        }
    }
}
