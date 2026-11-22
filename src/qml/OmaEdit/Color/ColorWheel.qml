import QtQuick
import OmaEdit.Ui

// A grading wheel: a hue ring with a puck for the colour offset (x, y in
// −1..1 mapped to R/G/B pushes), a master ring around it (Y), numeric
// fields to the right and RGB bars below, or (`stacked`) the ring, a master
// dial under it and the four fields, each under its letter, across the width. `value` is [y, r, g, b]; the Y
// field's origin is 0 for lift/gamma/offset and 1 for gain.
Item {
    id: root

    property string title: ""
    property var value: [0, 0, 0, 0]
    property real yOrigin: 0
    property real yMin: -1
    property real yMax: 1
    property real range: 0.5          // puck radius = this much RGB push
    property bool resettable: true
    property bool keyed: false
    property bool keyable: true
    // The ring takes the whole width and the Y/R/G/B fields sit under it:
    // the layout for a wheel that has to be big enough to steer.
    property bool stacked: false
    signal edited(var value)
    signal editingFinished()
    signal resetRequested()
    signal keyToggled()

    implicitWidth: 170
    implicitHeight: stacked ? wheelSize + chromeHeight : Theme.fsHeading + Theme.s2 + Math.max(wheelSize, fields.implicitHeight) + Theme.s3 + Theme.hControl + Theme.s2
    readonly property int dialHeight: Theme.hControl - 2
    // What a stacked wheel puts under its ring: the dial, then letters over fields.
    readonly property int stackedBelow: Theme.s2 + dialHeight + Theme.s2 + Theme.fsLabel + 4 + Theme.hControl
    // The ring takes the width left beside the Y/R/G/B fields, up to
    // maxWheel; a parent that has the room raises maxWheel to fill it.
    property int maxWheel: 96
    readonly property int chromeHeight: Theme.fsHeading + Theme.s2 + (stacked ? stackedBelow : Theme.s3 + Theme.hControl) + Theme.s2
    readonly property int wheelSize: Math.min(maxWheel, Math.max(64, stacked ? width : width - 74))
    readonly property int cellWidth: stacked ? Math.floor((width - 3 * Theme.s1) / 4) : 62 + Theme.s1
    readonly property int puckSize: wheelSize >= 140 ? 16 : 12
    readonly property var channelColors: [Theme.textMuted, Theme.danger, Theme.audio, Theme.accent]

    readonly property real py: value[0] !== undefined ? value[0] : yOrigin
    readonly property real pr: value[1] !== undefined ? value[1] : 0
    readonly property real pg: value[2] !== undefined ? value[2] : 0
    readonly property real pb: value[3] !== undefined ? value[3] : 0
    // Invert the three primary projections: their squared directions sum to 3/2.
    readonly property real puckX: (pr * Math.cos(0) + pg * Math.cos(2 * Math.PI / 3) + pb * Math.cos(4 * Math.PI / 3)) * (2 / 3) / range
    readonly property real puckY: (pr * Math.sin(0) + pg * Math.sin(2 * Math.PI / 3) + pb * Math.sin(4 * Math.PI / 3)) * (2 / 3) / range
    // `reach`: how far out the puck may be. The rim, unless a value typed
    // past the ring's range already has it further out: a drag then starts
    // from where the value is and never jumps it back to the rim.
    function fromPuck(x, y, reach) { // puck offset → RGB pushes
        var r = Math.sqrt(x * x + y * y), limit = reach > 1 ? reach : 1
        if (r > limit) { x *= limit / r; y *= limit / r }
        var rr = (x * Math.cos(0) + y * Math.sin(0)) * range
        var gg = (x * Math.cos(2 * Math.PI / 3) + y * Math.sin(2 * Math.PI / 3)) * range
        var bb = (x * Math.cos(4 * Math.PI / 3) + y * Math.sin(4 * Math.PI / 3)) * range
        // Remove the common component so the puck maps to a pure hue push.
        var m = (rr + gg + bb) / 3
        return [rr - m, gg - m, bb - m]
    }
    function emitValue(y, r, g, b) { root.edited([y, r, g, b]) }

    Row {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.fsHeading + Theme.s1
        Text {
            width: parent.width - reset.width - key.width
            text: root.title
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        IconButton { id: key; objectName: "wheelKeyframe"; visible: root.keyable; iconName: "diamond"; width: Theme.szIconHit; height: parent.height; iconColor: root.keyed ? Theme.accent : Theme.textMuted; Accessible.name: qsTr("Keyframe"); onClicked: root.keyToggled() }
        IconButton { id: reset; iconName: "rotate-ccw"; width: Theme.szIconHit; height: parent.height; visible: root.resettable; Accessible.name: qsTr("Reset"); onClicked: root.resetRequested() }
    }

    Item {
        id: wheel
        objectName: "wheelSurface"
        x: root.stacked ? Math.round((root.width - width) / 2) : 0
        anchors.top: parent.top
        anchors.topMargin: Theme.fsHeading + Theme.s2
        width: root.wheelSize
        height: root.wheelSize
        // Hue ring (a conical gradient) with a dark centre.
        Rectangle {
            anchors.fill: parent
            radius: width / 2
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
            opacity: 0.85
        }
        Canvas { // the conical hue ring drawn properly over the placeholder
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var cx = width / 2, cy = height / 2, r = width / 2
                for (var a = 0; a < 360; a += 3) {
                    ctx.beginPath()
                    ctx.moveTo(cx, cy)
                    ctx.arc(cx, cy, r, -(a + 3) * Math.PI / 180, -(a - 1) * Math.PI / 180)
                    ctx.closePath()
                    ctx.fillStyle = Qt.hsla(a / 360, 0.9, 0.55, 1)
                    ctx.fill()
                }
            }
        }
        Rectangle { // inner well
            anchors.centerIn: parent
            width: parent.width - 14
            height: width
            radius: width / 2
            color: Theme.windowBg
            border.color: Theme.border
            border.width: Theme.hairline
        }
        Rectangle { anchors.centerIn: parent; width: 1; height: parent.height - 24; color: Theme.border }
        Rectangle { anchors.centerIn: parent; height: 1; width: parent.width - 24; color: Theme.border }
        Rectangle { // puck
            // Drawn no further out than the rim, whatever was typed.
            readonly property real out: Math.max(1, Math.sqrt(root.puckX * root.puckX + root.puckY * root.puckY))
            x: wheel.width / 2 + root.puckX / out * (wheel.width / 2 - 10) - width / 2
            y: wheel.height / 2 - root.puckY / out * (wheel.height / 2 - 10) - height / 2
            width: root.puckSize; height: width; radius: width / 2
            color: Theme.textPrimary
            border.color: Theme.windowBg
            border.width: 2
        }
        DragHandler {
            target: null
            property real startX: 0
            property real startY: 0
            property real startMean: 0
            property real startReach: 1
            onActiveChanged: { if (active) { startX = root.puckX; startY = root.puckY; startReach = Math.sqrt(startX * startX + startY * startY); startMean = (root.pr + root.pg + root.pb) / 3 } else root.editingFinished() }
            onTranslationChanged: {
                if (!active) return
                var k = 1 / (wheel.width / 2 - 10)
                var rgb = root.fromPuck(startX + translation.x * k, startY - translation.y * k, startReach)
                root.emitValue(root.py, Math.max(-1, Math.min(1, rgb[0] + startMean)), Math.max(-1, Math.min(1, rgb[1] + startMean)), Math.max(-1, Math.min(1, rgb[2] + startMean)))
            }
        }
        TapHandler { onDoubleTapped: { root.emitValue(root.py, 0, 0, 0); root.editingFinished() } }
        WheelHandler { // the master ring: wheel over the puck adjusts Y
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (e) {
                var v = Math.max(root.yMin, Math.min(root.yMax, root.py + (e.angleDelta.y > 0 ? 0.01 : -0.01)))
                root.emitValue(v, root.pr, root.pg, root.pb); root.editingFinished()
            }
        }
    }

    Grid {
        id: fields
        x: root.stacked ? 0 : wheel.width + Theme.s2
        y: root.stacked ? dial.y + dial.height + Theme.s2 : wheel.y
        columns: root.stacked ? 4 : 1
        spacing: root.stacked ? Theme.s1 : 2
        Repeater {
            model: ["Y", "R", "G", "B"]
            delegate: Column {
                id: channel
                required property string modelData
                required property int index
                width: root.cellWidth
                spacing: 2
                // Stacked: its letter, in the channel's colour, over each field.
                Text { visible: root.stacked; width: parent.width; height: Theme.fsLabel + 2; horizontalAlignment: Text.AlignHCenter; text: channel.modelData; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading; color: root.channelColors[channel.index] }
                Row {
                    spacing: Theme.s1
                    Text { visible: !root.stacked; width: 10; height: Theme.hControl - 2; verticalAlignment: Text.AlignVCenter; text: channel.modelData; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    ValueField {
                        objectName: "wheelValue:" + channel.modelData
                        label: qsTr("%1 %2").arg(root.title).arg(channel.modelData)
                        tip: qsTr("Exact value for this wheel's master or colour channel")
                        width: root.stacked ? root.cellWidth : 52
                        height: root.stacked ? Theme.hControl : Theme.hControl - 2
                        decimals: 2
                        step: 0.01
                        from: channel.index === 0 ? root.yMin : -1
                        to: channel.index === 0 ? root.yMax : 1
                        readonly property real modelValue: channel.index === 0 ? root.py : channel.index === 1 ? root.pr : channel.index === 2 ? root.pg : root.pb
                        value: modelValue
                        onModelValueChanged: value = modelValue
                        onEditFinished: root.editingFinished()
                        onEdited: function (v) {
                            var y = root.py, r = root.pr, g = root.pg, b = root.pb
                            if (channel.index === 0) y = v; else if (channel.index === 1) r = v; else if (channel.index === 2) g = v; else b = v
                            root.emitValue(y, r, g, b); if (!scrubbing) root.editingFinished()
                        }
                    }
                }
            }
        }
    }

    // The master dial: a ribbed strip under the ring that rolls with the
    // master level. Drag it sideways; double-click puts it back.
    Rectangle {
        id: dial
        objectName: "wheelMaster"
        visible: root.stacked
        x: wheel.x
        y: wheel.y + wheel.height + Theme.s2
        width: wheel.width
        height: root.dialHeight
        radius: height / 2
        color: Theme.controlBg
        border.color: dialDrag.active || dialHover.hovered ? Theme.borderStrong : Theme.border
        border.width: Theme.hairline
        clip: true
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("%1 master").arg(root.title)
        readonly property real pitch: 9
        // The ribs travel a dial's width over a quarter of the range.
        readonly property real travel: (root.py - root.yOrigin) / (root.yMax - root.yMin) * 4 * width
        Repeater {
            model: dial.visible ? Math.ceil(dial.width / dial.pitch) + 2 : 0
            delegate: Rectangle {
                required property int index
                x: (index - 1) * dial.pitch + ((dial.travel % dial.pitch) + dial.pitch) % dial.pitch
                y: 4
                width: 1.5
                height: dial.height - 8
                color: Theme.textSecondary
                // Ribs fade toward the ends, as on a wheel seen edge on.
                opacity: 0.75 * Math.max(0.12, 1 - Math.pow(Math.abs(x - dial.width / 2) / (dial.width / 2), 2))
            }
        }
        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 1; width: 2; height: 4; color: Theme.accent }
        HoverHandler { id: dialHover; cursorShape: Qt.SizeHorCursor }
        DragHandler {
            id: dialDrag
            target: null
            property real start: 0
            onActiveChanged: { if (active) start = root.py; else root.editingFinished() }
            onTranslationChanged: {
                if (!active) return
                var v = Math.max(root.yMin, Math.min(root.yMax, start + translation.x / dial.width * (root.yMax - root.yMin) / 4))
                root.emitValue(v, root.pr, root.pg, root.pb)
            }
        }
        TapHandler { onDoubleTapped: { root.emitValue(root.yOrigin, root.pr, root.pg, root.pb); root.editingFinished() } }
    }

    // Master slider under the wheel (Y), plus the three channel bars.
    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: wheel.top
        anchors.topMargin: Math.max(root.wheelSize, fields.implicitHeight) + Theme.s3
        visible: !root.stacked
        spacing: Theme.s1
        Repeater {
            model: 4
            delegate: Item {
                required property int index
                width: (root.width - Theme.s1 * 3) / 4
                height: Theme.hControl
                readonly property real v: index === 0 ? root.py : index === 1 ? root.pr : index === 2 ? root.pg : root.pb
                readonly property real lo: index === 0 ? root.yMin : -1
                readonly property real hi: index === 0 ? root.yMax : 1
                readonly property real origin: index === 0 ? root.yOrigin : 0
                Rectangle { anchors.top: parent.top; width: parent.width; height: 3; radius: 1; color: index === 0 ? Theme.textMuted : index === 1 ? Theme.danger : index === 2 ? Theme.audio : Theme.accent; opacity: 0.6 }
                Rectangle { // the value mark on the bar
                    anchors.top: parent.top
                    anchors.topMargin: -2
                    x: parent.width * Math.max(0, Math.min(1, (v - lo) / (hi - lo))) - 2
                    width: 4; height: 7; radius: 1
                    color: Theme.textPrimary
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.hControl - 8
                    radius: Theme.rControl
                    color: Theme.controlBg
                    border.color: Theme.border
                    border.width: Theme.hairline
                    Text { anchors.centerIn: parent; text: v.toFixed(2); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textSecondary }
                }
                DragHandler {
                    target: null
                    property real start: 0
                    onActiveChanged: { if (active) start = v; else root.editingFinished() }
                    onTranslationChanged: {
                        if (!active) return
                        var nv = Math.max(lo, Math.min(hi, start + translation.x / parent.width * (hi - lo)))
                        var y = root.py, r = root.pr, g = root.pg, b = root.pb
                        if (index === 0) y = nv; else if (index === 1) r = nv; else if (index === 2) g = nv; else b = nv
                        root.emitValue(y, r, g, b)
                    }
                }
                TapHandler {
                    onDoubleTapped: {
                        var y = root.py, r = root.pr, g = root.pg, b = root.pb
                        if (index === 0) y = origin; else if (index === 1) r = 0; else if (index === 2) g = 0; else b = 0
                        root.emitValue(y, r, g, b); root.editingFinished()
                    }
                }
            }
        }
    }
}
