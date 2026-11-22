import QtQuick
import OmaEdit.Ui

// A rotary control: 270° sweep, drag up/down (or scroll) to turn, double-
// click to reset, readout under it. Pan knobs put their origin at the
// centre; level knobs at the bottom of the sweep.
Item {
    id: root

    property string label: ""
    property real value: 0
    property real from: -1
    property real to: 1
    property real origin: 0            // where the arc starts (centre for pan)
    property real step: 0.01
    property int decimals: 1
    property string suffix: ""
    property bool bipolar: true        // readout "L 20 / C / R 20" style when pan
    property bool pan: false
    property int size: Theme.szKnob
    property color tint: Theme.accent
    property bool enabled2: enabled
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
        const amount = (root.step > 0 ? root.step : (root.to - root.from) / 100) * ((event.modifiers & Qt.ShiftModifier) ? 0.1 : 1)
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) keyboardSet(value + amount)
        else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) keyboardSet(value - amount)
        else if (event.key === Qt.Key_Home) keyboardSet(from)
        else if (event.key === Qt.Key_End) keyboardSet(to)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) keyboardSet(root.origin)
        else { event.accepted = false; return }
        event.accepted = true
    }
    Accessible.description: keyboardHelp
    HoverHandler { id: keyboardHover }
    Tooltip { text: root.keyboardHelp; visible: keyboardHover.hovered }


    implicitWidth: Math.max(size, readout.implicitWidth)
    implicitHeight: (label !== "" ? caption.height + Theme.s1 : 0) + size + Theme.s1 + readout.height

    readonly property real frac: (value - from) / Math.max(1e-9, to - from)
    readonly property real originFrac: (origin - from) / Math.max(1e-9, to - from)
    readonly property real sweep: Math.PI * 1.5
    readonly property real startAngle: Math.PI * 0.75

    function readoutText() {
        if (pan) {
            if (Math.abs(value) < 0.005) return "C"
            var pct = Math.round(Math.abs(value) * 100)
            return (value < 0 ? "L " : "R ") + pct
        }
        var t = value.toFixed(decimals)
        if (value > 0 && bipolar) t = "+" + t
        return t + suffix
    }
    function clamp(v) { return Math.max(from, Math.min(to, v)) }

    Accessible.role: Accessible.Dial
    Accessible.name: label !== "" ? label : qsTr("Knob")

    Text {
        id: caption
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.label !== ""
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
        height: visible ? implicitHeight : 0
    }

    Canvas {
        id: dial
        objectName: "knobDial"
        anchors.top: root.label !== "" ? caption.bottom : parent.top
        anchors.topMargin: root.label !== "" ? Theme.s1 : 0
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.size
        height: root.size
        opacity: root.enabled ? 1 : Theme.disabledOpacity
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var cx = width / 2, cy = height / 2, r = width / 2 - 3
            ctx.lineWidth = 3
            ctx.lineCap = "round"
            // The track.
            ctx.strokeStyle = Theme.controlBg
            ctx.beginPath()
            ctx.arc(cx, cy, r, root.startAngle, root.startAngle + root.sweep)
            ctx.stroke()
            // The value arc from the origin.
            var a0 = root.startAngle + root.sweep * root.originFrac
            var a1 = root.startAngle + root.sweep * root.frac
            ctx.strokeStyle = root.tint
            ctx.beginPath()
            if (a1 >= a0) ctx.arc(cx, cy, r, a0, a1)
            else ctx.arc(cx, cy, r, a1, a0)
            ctx.stroke()
            // The cap and pointer.
            ctx.fillStyle = drag.active || hover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
            ctx.beginPath()
            ctx.arc(cx, cy, r - 5, 0, Math.PI * 2)
            ctx.fill()
            ctx.strokeStyle = Theme.textPrimary
            ctx.lineWidth = 2
            ctx.beginPath()
            ctx.moveTo(cx + Math.cos(a1) * (r - 12), cy + Math.sin(a1) * (r - 12))
            ctx.lineTo(cx + Math.cos(a1) * (r - 6), cy + Math.sin(a1) * (r - 6))
            ctx.stroke()
        }
        Connections {
            target: root
            function onValueChanged() { dial.requestPaint() }
            function onFromChanged() { dial.requestPaint() }
            function onToChanged() { dial.requestPaint() }
            function onTintChanged() { dial.requestPaint() }
        }
        HoverHandler { id: hover; onHoveredChanged: dial.requestPaint() }
        DragHandler {
            id: drag
            enabled: root.enabled
            target: null
            property real startValue: 0
            onActiveChanged: {
                dial.requestPaint()
                if (active) { startValue = root.value; root.editingStarted() }
                else root.editingFinished(root.value)
            }
            onTranslationChanged: {
                if (!active) return
                var range = root.to - root.from
                var v = root.clamp(startValue - translation.y / 150 * range)
                if (root.step > 0) v = Math.round(v / root.step) * root.step
                if (v !== root.value) root.edited(v)
            }
        }
        TapHandler {
            enabled: root.enabled
            onDoubleTapped: { root.edited(root.origin); root.editingFinished(root.origin) }
        }
        WheelHandler {
            enabled: root.enabled
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                var range = root.to - root.from
                var v = root.clamp(root.value + (event.angleDelta.y / 120) * range / 50)
                root.edited(v)
                root.editingFinished(v)
            }
        }
    }

    Text {
        id: readout
        anchors.top: dial.bottom
        anchors.topMargin: Theme.s1
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.readoutText()
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel
        color: root.enabled ? Theme.textSecondary : Theme.textMuted
    }
}
