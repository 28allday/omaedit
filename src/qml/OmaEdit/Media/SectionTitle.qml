import QtQuick
import OmaEdit.Ui

// The cyan uppercase caption that heads every group on the Media page —
// STORAGE, SOURCE PREVIEW, CLIP LOG. Optional trailing slot for a "+".
Item {
    id: root

    property string text: ""
    property bool collapsible: false
    property bool expanded: true
    default property alias trailing: trailingRow.data

    signal toggled()

    implicitHeight: Theme.hRow + Theme.s1
    implicitWidth: caption.implicitWidth + (collapsible ? Theme.szIcon + Theme.s1 : 0)

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

        Icon {
            visible: root.collapsible
            name: root.expanded ? "chevron-down" : "chevron-right"
            size: Theme.szIcon - 2
            color: Theme.accent
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            id: caption
            text: root.text.toUpperCase()
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            font.weight: Theme.wHeading
            font.letterSpacing: 0.6
            color: Theme.accent
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    TapHandler {
        enabled: root.collapsible
        onTapped: root.toggled()
    }

    Row {
        id: trailingRow
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
    }
}
