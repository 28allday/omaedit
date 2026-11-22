import QtQuick
import OmaEdit.Ui

// The viewer's horizontal stereo meter: two bars with L/R labels, a dB
// scale underneath, peak hold and the same fall as VMeter.
Item {
    id: root

    property real levelLeft: -100
    property real levelRight: -100
    property real floor: -60
    property real ceiling: 0
    property real fallDbPerSec: 24
    property real peakHoldMs: 1500

    implicitHeight: Theme.hHMeter * 2 + 4 + scaleRow.height + Theme.s1

    property real shownL: -100
    property real shownR: -100
    property real holdL: -100
    property real holdR: -100
    property real holdLAt: 0
    property real holdRAt: 0
    function feed() {
        var now = Date.now()
        shownL = Math.max(levelLeft, shownL)
        shownR = Math.max(levelRight, shownR)
        if (levelLeft >= holdL || now - holdLAt > peakHoldMs) { holdL = levelLeft; holdLAt = now }
        if (levelRight >= holdR || now - holdRAt > peakHoldMs) { holdR = levelRight; holdRAt = now }
    }
    onLevelLeftChanged: feed()
    onLevelRightChanged: feed()
    Timer {
        interval: 33
        running: root.visible && (root.shownL > root.floor || root.shownR > root.floor || root.holdL > root.floor || root.holdR > root.floor)
        repeat: true
        onTriggered: {
            var fall = root.fallDbPerSec * interval / 1000
            root.shownL = Math.max(root.levelLeft, root.shownL - fall)
            root.shownR = Math.max(root.levelRight, root.shownR - fall)
            var now = Date.now()
            if (now - root.holdLAt > root.peakHoldMs) root.holdL = Math.max(root.levelLeft, root.holdL - fall)
            if (now - root.holdRAt > root.peakHoldMs) root.holdR = Math.max(root.levelRight, root.holdR - fall)
        }
    }
    function frac(db) { return Math.max(0, Math.min(1, (db - floor) / (ceiling - floor))) }

    Column {
        id: labels
        anchors.left: parent.left
        anchors.top: parent.top
        spacing: 4
        Repeater {
            model: ["L", "R"]
            delegate: Text {
                required property string modelData
                width: Theme.s3
                height: Theme.hHMeter
                verticalAlignment: Text.AlignVCenter
                text: modelData
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    Column {
        id: bars
        anchors.left: labels.right
        anchors.leftMargin: Theme.s1
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 4
        Repeater {
            model: [0, 1]
            delegate: Rectangle {
                id: well
                required property int modelData
                readonly property real shown: modelData === 0 ? root.shownL : root.shownR
                readonly property real hold: modelData === 0 ? root.holdL : root.holdR
                width: bars.width
                height: Theme.hHMeter
                color: Theme.meterWell
                border.color: Theme.border
                border.width: Theme.hairline
                clip: true
                Rectangle { width: parent.width * Math.min(root.frac(well.shown), root.frac(-18)); height: parent.height; color: Theme.meterLow }
                Rectangle {
                    x: parent.width * root.frac(-18)
                    width: Math.max(0, parent.width * (Math.min(root.frac(well.shown), root.frac(-6)) - root.frac(-18)))
                    height: parent.height; color: Theme.meterMid; visible: well.shown > -18
                }
                Rectangle {
                    x: parent.width * root.frac(-6)
                    width: Math.max(0, parent.width * (root.frac(well.shown) - root.frac(-6)))
                    height: parent.height; color: Theme.meterHigh; visible: well.shown > -6
                }
                Rectangle {
                    x: parent.width * root.frac(well.hold) - 1
                    width: 2; height: parent.height
                    visible: well.hold > root.floor
                    color: well.hold > -6 ? Theme.meterHigh : well.hold > -18 ? Theme.meterMid : Theme.textPrimary
                }
                // Segment ticks every 3 dB for the broadcast look.
                Repeater {
                    model: 20
                    delegate: Rectangle {
                        required property int index
                        x: well.width * root.frac(root.floor + (index + 1) * 3) - 1
                        width: 1; height: well.height
                        color: Theme.meterWell
                        opacity: 0.8
                    }
                }
            }
        }
    }

    Item {
        id: scaleRow
        anchors.left: bars.left
        anchors.right: bars.right
        anchors.top: bars.bottom
        anchors.topMargin: Theme.s1
        height: Theme.fsLabel + 2
        Repeater {
            model: [-60, -48, -36, -30, -24, -18, -12, -6, 0]
            delegate: Text {
                required property var modelData
                x: Math.min(scaleRow.width - width, Math.max(0, scaleRow.width * root.frac(modelData) - width / 2))
                text: String(modelData)
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 2
                color: Theme.meterScale
            }
        }
    }
}
