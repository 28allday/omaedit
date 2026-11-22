import QtQuick
import OmaEdit.Ui

// One line of the Render Settings column: a muted label on the left, the
// control(s) filling the right. The control column starts at one x for
// every row so the values line up as a form.
Item {
    id: root

    property string label: ""
    property int labelWidth: 136
    property bool dimmed: false
    default property alias content: slot.data

    width: parent ? parent.width : 300
    implicitHeight: Math.max(Theme.hControl, slot.childrenRect.height) + Theme.s2
    opacity: dimmed ? Theme.disabledOpacity : 1

    Text {
        width: root.labelWidth
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }

    Item {
        id: slot
        anchors.left: parent.left
        anchors.leftMargin: root.labelWidth
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: childrenRect.height
    }
}
