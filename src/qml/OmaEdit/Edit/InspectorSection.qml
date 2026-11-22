import QtQuick
import OmaEdit.Ui

// Collapsible Inspector group: disclosure caret, title, optional AI badge,
// and a reset that only lights once the section has been touched.
Item {
    id: root

    property string title: ""
    property bool expanded: false
    property bool ai: false
    property bool modified: false
    // Read-only groups (a metadata inspector) carry no reset affordance.
    property bool resettable: true

    default property alias content: body.data

    signal reset()

    implicitWidth: 260
    implicitHeight: head.height + (expanded ? body.implicitHeight + Theme.s2 : 0)

    Behavior on implicitHeight {
        NumberAnimation { duration: Theme.dNormal; easing.type: Theme.easing }
    }

    Rectangle {
        id: head
        width: parent.width
        height: Theme.hRow + Theme.s2
        color: hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"

        HoverHandler { id: hover }
        TapHandler { onTapped: root.expanded = !root.expanded }

        Icon {
            id: caret
            name: root.expanded ? "chevron-down" : "chevron-right"
            size: Theme.szIcon
            color: Theme.textSecondary
            anchors.left: parent.left
            anchors.leftMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.title
            anchors.left: caret.right
            anchors.leftMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            color: Theme.textPrimary
        }

        // AI-assisted sections carry the violet accent so they are never
        // mistaken for a plain parameter group.
        Rectangle {
            visible: root.ai
            anchors.right: resetButton.left
            anchors.rightMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            width: aiLabel.implicitWidth + Theme.s2
            height: 16
            radius: Theme.rControl
            color: "transparent"
            border.width: Theme.hairline
            border.color: Theme.aiAccent

            Text {
                id: aiLabel
                anchors.centerIn: parent
                text: "AI"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                font.weight: Theme.wHeading
                color: Theme.aiText
            }
        }

        IconButton {
            id: resetButton
            visible: root.resettable
            iconName: "rotate-ccw"
            text: qsTr("Reset %1").arg(root.title)
            iconColor: root.modified ? Theme.textSecondary : Theme.textMuted
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.reset()
        }
    }

    Column {
        id: body
        anchors.top: head.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s2
        visible: root.expanded
        clip: true
        spacing: Theme.s1
    }
}
