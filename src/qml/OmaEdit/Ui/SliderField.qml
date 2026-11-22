import QtQuick
import QtQuick.Controls as C

// Label + track + numeric readout, as used for Opacity and the grading
// controls. The readout is a real ValueField, so a value can always be typed
// exactly rather than hunted for with the handle.
Item {
    id: root

    property string label: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    property real value: 0
    property real from: 0
    property real to: 100
    property real stepSize: 0
    property int decimals: 1
    property string suffix: ""
    // A control whose value means nothing as a number (zoom) hides it.
    property bool showReadout: true
    property bool liveEdit: false
    property bool animatable: false
    property bool keyed: false
    property bool hasAnimation: false
    property string keyObjectName: ""
    property int labelWidth: 84
    // Where the fill starts. Bipolar controls (a −1..1 colour trim) fill out
    // from the centre instead of from the left.
    property real origin: from

    signal edited(real value)
    signal editingStarted()
    signal editingEnded()
    signal keyToggled()
    readonly property bool dragging: slider.pressed || readout.scrubbing
    // Emitted once at the end of a drag, for callers that must not commit an
    // undo step per pixel.
    signal editingFinished(real value)

    implicitHeight: Theme.hControl
    implicitWidth: labelWidth + 160 + Theme.s2 * 2
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Text {
        id: caption
        text: root.label
        visible: root.label !== ""
        width: root.labelWidth
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
        elide: Text.ElideRight
    }

    C.Slider {
        id: slider
        objectName: "sliderTrack"
        anchors.left: root.label !== "" ? caption.right : parent.left
        anchors.leftMargin: root.label !== "" ? Theme.s2 : 0
        anchors.right: readout.visible ? readout.left : parent.right
        anchors.rightMargin: readout.visible ? Theme.s2 : 0
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height

        from: root.from
        to: root.to
        value: root.value
        stepSize: root.stepSize

        property real pressedValue: value
        onMoved: {
            root.edited(value)
            if (!pressed) root.editingFinished(value)
        }
        onPressedChanged: {
            if (pressed) { pressedValue = value; root.editingStarted() }
            // Only a release that moved the value commits; a bare click must
            // not add a keyframe or an undo step.
            else {
                if (value !== pressedValue) root.editingFinished(value)
                root.editingEnded()
            }
        }

        Accessible.role: Accessible.Slider
        Accessible.name: root.label
        Accessible.description: root.tip

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: 3
            radius: 1
            color: Theme.controlBg

            // Fill runs from `origin` to the handle, so a bipolar control
            // reads its direction from the centre outwards.
            Rectangle {
                readonly property real originPos:
                    (root.origin - slider.from) / (slider.to - slider.from)
                x: Math.min(originPos, slider.visualPosition) * parent.width
                width: Math.abs(slider.visualPosition - originPos) * parent.width
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition
               * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: 10
            height: 10
            radius: 5
            color: slider.pressed ? Theme.accentPressed : Theme.textPrimary
            border.width: slider.visualFocus ? Theme.focusRing : 0
            border.color: Theme.accent

            Behavior on color {
                ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
            }
        }
    }

    ValueField {
        id: readout
        objectName: "sliderReadout"
        visible: root.showReadout
        anchors.right: root.animatable ? keyButton.left : parent.right
        anchors.rightMargin: root.animatable ? Theme.s1 : 0
        anchors.verticalCenter: parent.verticalCenter
        width: 64
        label: root.label
        tip: root.tip
        value: root.value
        from: root.from
        to: root.to
        decimals: root.decimals
        suffix: root.suffix
        step: root.stepSize > 0 ? root.stepSize : (root.to - root.from) / 200
        // A scrub finishes once, at its end, for every owner (one undo step,
        // one rebuild); a typed or nudged value finishes at once.
        property bool scrubbed: false
        onEdited: function (v) {
            root.edited(v)
            if (scrubbing) scrubbed = true
            else root.editingFinished(v)
        }
        onEditStarted: root.editingStarted()
        onEditFinished: {
            if (root.liveEdit || scrubbed) root.editingFinished(value)
            scrubbed = false
            root.editingEnded()
        }
        Connections {
            target: root
            function onValueChanged() { readout.value = root.value }
        }
    }

    IconButton {
        id: keyButton
        objectName: root.keyObjectName
        visible: root.animatable
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.szIcon + Theme.s1
        height: Theme.hControl
        iconName: "diamond"
        checked: root.keyed
        iconColor: root.keyed ? Theme.accent : root.hasAnimation ? Theme.textPrimary : Theme.textMuted
        text: root.keyed ? qsTr("Remove %1 keyframe at playhead").arg(root.label)
                         : qsTr("Add %1 keyframe at playhead").arg(root.label)
        onClicked: root.keyToggled()
    }

    HoverHandler { id: tipHover }
    Tooltip { text: root.tip; visible: tipHover.hovered && root.tip !== "" }
}
