import QtQuick
import OmaEdit.Ui

// A long-throw channel fader in dB with the mixer taper (unity sits two
// thirds up, the bottom is −∞), a numeric readout, drag, scroll and a
// double-click reset to 0 dB. `value` is dB; −100 and below reads "−∞".
Item {
    id: root

    property real value: 0
    property real from: -100
    property real to: 12
    property bool showScale: true
    property bool compact: false
    property color tint: Theme.textPrimary
    signal edited(real value)
    signal editingFinished(real value)
    signal editingStarted()

    activeFocusOnTab: true
    readonly property bool keyboardConsumesKeys: true
    readonly property string keyboardHelp: qsTr("Arrow keys adjust; Shift adjusts finely; Home/End set limits; Enter resets.")
    function keyboardSet(v) {
        const next = Math.max(from, Math.min(to, Math.round(v * 1000000) / 1000000))
        if (next !== value) { edited(next); editingFinished(next) }
    }
    Keys.onPressed: function(event) {
        const amount = 0.5 * ((event.modifiers & Qt.ShiftModifier) ? 0.1 : 1)
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) keyboardSet(value + amount)
        else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) keyboardSet(value - amount)
        else if (event.key === Qt.Key_Home) keyboardSet(from)
        else if (event.key === Qt.Key_End) keyboardSet(to)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) keyboardSet(0)
        else { event.accepted = false; return }
        event.accepted = true
    }
    Accessible.description: keyboardHelp
    HoverHandler { id: keyboardHover }
    Tooltip { text: root.keyboardHelp; visible: keyboardHover.hovered }


    implicitWidth: compact ? Theme.wFaderCap + Theme.s2 : Theme.wFaderCap + Theme.s5 + Theme.s2
    implicitHeight: Theme.hFader

    // Taper: piecewise-linear in dB with more travel near unity.
    function posFor(db) {
        db = Math.max(from, Math.min(to, db))
        if (db >= 0) return 0.68 + (db / to) * 0.32
        if (db >= -20) return 0.32 + ((db + 20) / 20) * 0.36
        return ((db - from) / (-20 - from)) * 0.32
    }
    function dbFor(pos) {
        pos = Math.max(0, Math.min(1, pos))
        if (pos >= 0.68) return (pos - 0.68) / 0.32 * to
        if (pos >= 0.32) return -20 + (pos - 0.32) / 0.36 * 20
        return from + pos / 0.32 * (-20 - from)
    }
    function readout() {
        if (value <= -100) return "−∞"
        return (value > 0 ? "+" : "") + value.toFixed(1)
    }

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Fader")

    Item {
        id: track
        objectName: "faderTrack"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.hFaderCap / 2
        anchors.bottomMargin: Theme.hFaderCap / 2 + readoutText.height + Theme.s1
        anchors.left: parent.left
        anchors.leftMargin: root.compact ? Theme.s1 : Theme.s5
        width: Theme.wFaderCap

        Rectangle { // the slot
            anchors.horizontalCenter: parent.horizontalCenter
            width: 4
            height: parent.height
            radius: 2
            color: Theme.meterWell
            border.color: Theme.border
            border.width: Theme.hairline
        }
        Rectangle { // unity tick
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * (1 - root.posFor(0))
            width: Theme.wFaderCap
            height: Theme.hairline
            color: Theme.borderStrong
        }
        Rectangle { // the cap
            id: cap
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * (1 - root.posFor(root.value)) - height / 2
            width: Theme.wFaderCap
            height: Theme.hFaderCap
            radius: Theme.rControl
            color: drag.active ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
            border.color: drag.active || hover.hovered ? Theme.accent : Theme.borderStrong
            border.width: Theme.hairline
            Rectangle {
                anchors.centerIn: parent
                width: parent.width - 6
                height: 2
                color: root.tint
            }
        }
        HoverHandler { id: hover }
        DragHandler {
            id: drag
            target: null
            property real startPos: 0
            onActiveChanged: {
                if (active) { startPos = root.posFor(root.value); root.editingStarted() }
                else root.editingFinished(root.value)
            }
            onTranslationChanged: {
                if (!active) return
                var v = Math.round(root.dbFor(startPos - translation.y / track.height) * 10) / 10
                if (v !== root.value) root.edited(v)
            }
        }
        TapHandler {
            onDoubleTapped: { root.edited(0); root.editingFinished(0) }
            onTapped: function (ev) {
                var v = Math.round(root.dbFor(1 - ev.position.y / track.height) * 10) / 10
                root.edited(v); root.editingFinished(v)
            }
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                var v = Math.max(root.from, Math.min(root.to, root.value + (event.angleDelta.y > 0 ? 0.5 : -0.5)))
                root.edited(v); root.editingFinished(v)
            }
        }
    }

    // dB scale to the left of the slot.
    Column {
        visible: root.showScale && !root.compact
        anchors.top: track.top
        anchors.bottom: track.bottom
        anchors.right: track.left
        anchors.rightMargin: Theme.s1
        Repeater {
            model: [12, 6, 0, -6, -12, -18, -24, -30, -40, -60, -100]
            delegate: Item {
                required property var modelData
                width: Theme.s5 - Theme.s1
                height: 0
                Text {
                    anchors.right: parent.right
                    y: track.height * (1 - root.posFor(parent.modelData)) - height / 2
                    text: parent.modelData === -100 ? "∞" : String(Math.abs(parent.modelData))
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel - 2
                    color: Theme.meterScale
                }
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: track.horizontalCenter
        width: Math.max(Theme.wFaderCap + Theme.s3, readoutText.implicitWidth + Theme.s2)
        height: readoutText.height + Theme.s1
        radius: Theme.rControl
        color: Theme.controlBg
        border.color: Theme.border
        border.width: Theme.hairline
        Text {
            id: readoutText
            anchors.centerIn: parent
            text: root.readout()
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textPrimary
        }
    }
}
