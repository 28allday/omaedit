import QtQuick
import QtQuick.Controls as C

// A tag or keyword pill — tiny radius, tinted from the label palette when it
// carries a colour, otherwise the raised chrome. Optional remove affordance.
C.AbstractButton {
    id: root

    property string label: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    // A label colour name (red … purple) tints the chip; "" leaves it neutral.
    property string tint: ""
    property bool removable: false
    property bool selected: false

    signal removeRequested()

    readonly property color base: tint !== "" ? Theme.labelColor(tint) : Theme.controlBg

    implicitHeight: 18
    implicitWidth: caption.implicitWidth + Theme.s2 * 2 + (removable ? 14 : 0)
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.description: tip + (removable ? qsTr(". Press Delete to remove.") : "")
    Keys.onDeletePressed: function(event) {
        event.accepted = removable
        if (removable) removeRequested()
    }

    background: Rectangle {
        radius: Theme.rControl
        color: root.down ? Theme.pressedOn(root.base)
             : root.hovered ? Theme.hovered(root.base) : root.base
        border.width: root.selected || root.visualFocus ? Theme.hairline : 0
        border.color: Theme.accent
    }

    contentItem: Item {
        Text {
            id: caption
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            color: root.tint !== "" ? Theme.clipLabelOnMedia : Theme.textSecondary
        }

        Icon {
            visible: root.removable
            anchors.right: parent.right
            anchors.rightMargin: 3
            anchors.verticalCenter: parent.verticalCenter
            name: "x"
            size: 10
            color: root.tint !== "" ? Theme.clipLabelOnMedia : Theme.textMuted

            TapHandler { onTapped: root.removeRequested() }
        }
    }

    Tooltip { text: root.tip; visible: root.hovered && !root.down && root.tip !== "" }
}
