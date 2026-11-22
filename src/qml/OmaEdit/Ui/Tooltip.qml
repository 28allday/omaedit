import QtQuick
import QtQuick.Controls as C

// Delayed tooltip carrying an optional shortcut, per the brief: appears after
// 450 ms and reads "Blade Tool  B". The shortcut sits in a dimmer weight so
// the label still reads first.
C.ToolTip {
    id: root

    property string shortcut: ""

    delay: Theme.tooltipDelay
    padding: 0

    contentItem: Row {
        spacing: Theme.s2
        leftPadding: Theme.s2
        rightPadding: Theme.s2
        topPadding: Theme.s1
        bottomPadding: Theme.s1

        Text {
            text: root.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.shortcut
            visible: root.shortcut !== ""
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        color: Theme.panelRaised
        radius: Theme.rMenu
        border.width: Theme.hairline
        border.color: Theme.border
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: Theme.dFast; easing.type: Theme.easing
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: Theme.dFast; easing.type: Theme.easing
        }
    }
}
