import QtQuick
import OmaEdit.Ui

// A vertical stereo peak meter in dBFS with a held peak line, a clip light
// that latches until clicked, and an optional scale. Levels fall at a
// fixed rate so a burst stays readable; the hold sits for ~1.5 s.
Item {
    id: root

    property real levelLeft: -100        // dBFS
    property real rmsLeft: -100
    property real rmsRight: -100
    property real levelRight: -100
    property real floor: -60
    property real ceiling: 0
    property bool showScale: false
    property bool compact: false
    property real peakHoldMs: 1500
    property real fallDbPerSec: 24
    readonly property bool clipped: clipLatch
    property bool clipLatch: false
    property real peakL: -100
    property real peakR: -100

    implicitWidth: (compact ? 10 : 14) + (showScale ? Theme.s4 + Theme.s1 : 0)
    implicitHeight: 160

    // Ballistics: displayed values chase the inputs downwards slowly.
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
        peakL = Math.max(peakL, levelLeft)
        peakR = Math.max(peakR, levelRight)
        if (levelLeft >= ceiling - 0.1 || levelRight >= ceiling - 0.1) clipLatch = true
    }
    function resetPeaks() { peakL = -100; peakR = -100; clipLatch = false; holdL = -100; holdR = -100 }
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

    Rectangle { // clip light
        id: clipLight
        anchors.top: parent.top
        anchors.left: parent.left
        width: bars.width
        height: 5
        radius: 1
        color: root.clipLatch ? Theme.meterHigh : Theme.meterWell
        border.color: Theme.border
        border.width: Theme.hairline
        TapHandler { onTapped: root.resetPeaks() }
    }

    Row {
        id: bars
        anchors.top: clipLight.bottom
        anchors.topMargin: 2
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        spacing: 2
        Repeater {
            model: [0, 1]
            delegate: Rectangle {
                id: well
                required property int modelData
                readonly property real shown: modelData === 0 ? root.shownL : root.shownR
                readonly property real hold: modelData === 0 ? root.holdL : root.holdR
                width: root.compact ? 4 : 6
                height: bars.height
                color: Theme.meterWell
                border.color: Theme.border
                border.width: Theme.hairline
                clip: true
                Rectangle { // green
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * Math.min(root.frac(well.shown), root.frac(-18))
                    color: Theme.meterLow
                }
                Rectangle { // amber
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: parent.height * root.frac(-18)
                    width: parent.width
                    height: Math.max(0, parent.height * (Math.min(root.frac(well.shown), root.frac(-6)) - root.frac(-18)))
                    color: Theme.meterMid
                    visible: well.shown > -18
                }
                Rectangle { // red
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: parent.height * root.frac(-6)
                    width: parent.width
                    height: Math.max(0, parent.height * (root.frac(well.shown) - root.frac(-6)))
                    color: Theme.meterHigh
                    visible: well.shown > -6
                }
                Rectangle { // RMS overlay inside the peak bar
                    width: Math.max(1, parent.width / 2)
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    height: parent.height * root.frac(well.modelData === 0 ? root.rmsLeft : root.rmsRight)
                    color: Theme.textPrimary
                    opacity: 0.55
                }
                Rectangle { // peak hold
                    width: parent.width
                    height: 1
                    y: parent.height * (1 - root.frac(well.hold))
                    visible: well.hold > root.floor
                    color: well.hold > -6 ? Theme.meterHigh : well.hold > -18 ? Theme.meterMid : Theme.textPrimary
                }
            }
        }
    }

    Item { // scale
        visible: root.showScale
        anchors.top: bars.top
        anchors.bottom: bars.bottom
        anchors.left: bars.right
        anchors.leftMargin: Theme.s1
        width: Theme.s4
        Repeater {
            model: [0, -6, -12, -18, -24, -30, -36, -42, -48, -54, -60]
            delegate: Text {
                required property var modelData
                y: parent ? parent.height * (1 - root.frac(modelData)) - height / 2 : 0
                text: String(Math.abs(modelData))
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 2
                color: Theme.meterScale
            }
        }
    }
}
