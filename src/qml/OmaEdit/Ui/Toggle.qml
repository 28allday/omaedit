import QtQuick
import QtQuick.Controls as C

// Small switch for the Inspector's per-module enables (De-Esser, Noise
// Reduction). Deliberately restrained: a short track and a square-ish knob,
// never a large rounded consumer switch.
C.AbstractButton {
    id: root

    property string label: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    readonly property int trackWidth: 26
    readonly property int trackHeight: 14

    checkable: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus // keyboard users reach it with Tab; a click must not take the keys from the editor
    implicitHeight: Math.max(trackHeight, Theme.hRow)
    implicitWidth: trackWidth + (label ? caption.implicitWidth + Theme.s2 : 0)
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.CheckBox
    Accessible.name: label !== "" ? label : text
    Accessible.checked: checked
    Accessible.description: tip

    background: null

    contentItem: Item {
        anchors.fill: parent

        Text {
            id: caption
            text: root.label
            visible: root.label !== ""
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }

        Rectangle {
            id: track
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: root.trackWidth
            height: root.trackHeight
            radius: Theme.rControl
            color: root.checked ? Theme.accent : Theme.controlBg
            border.width: root.visualFocus ? Theme.focusRing : Theme.hairline
            border.color: root.visualFocus ? Theme.accent
                        : root.checked ? Theme.accent : Theme.border

            Behavior on color {
                ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
            }

            Rectangle {
                id: knob
                width: root.trackHeight - 4
                height: width
                radius: Theme.rControl - 1
                y: 2
                x: root.checked ? track.width - width - 2 : 2
                color: root.checked ? Theme.accentText
                     : root.hovered ? Theme.textPrimary : Theme.textSecondary

                Behavior on x {
                    NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
                }
                Behavior on color {
                    ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
                }
            }
        }
    }

    Tooltip { text: root.tip; visible: root.hovered && !root.down && root.tip !== "" }
}
