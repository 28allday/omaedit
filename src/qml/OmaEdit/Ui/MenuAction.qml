import QtQuick
import QtQuick.Controls as C

// Shared menu row with separate columns for its marker, label and shortcut.
C.MenuItem {
    id: root

    property string iconName: ""
    property string shortcut: ""
    property string commandId: ""
    // Keep row geometry independent of the popup's inherited visibility, so
    // a closed menu measures correctly before it opens (or opens again).
    property bool showInMenu: true
    visible: showInMenu
    // Menus show the primary binding; the shortcut editor lists every alias.
    readonly property string shortcutHint: (commandId ? Commands.hint(commandId)
                                                      : Commands.hintForDefault(shortcut)).split(" / ")[0]

    implicitHeight: Theme.hRow + Theme.s1 * 2
    height: showInMenu ? implicitHeight : 0
    implicitWidth: leftPadding + label.implicitWidth + rightPadding
                   + (hint.text ? Theme.s5 + hint.implicitWidth : 0)
    leftPadding: Theme.s2 + Theme.szIcon + Theme.s2
    rightPadding: Theme.s3 + (subMenu ? Theme.szIcon + Theme.s2 : 0)
    topPadding: 0
    bottomPadding: 0
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.name: text

    indicator: Icon {
        x: Theme.s2
        y: (root.height - height) / 2
        name: root.checkable ? (root.checked ? "check" : "") : root.iconName
        size: Theme.szIcon
        color: root.checked ? Theme.accent : Theme.textSecondary
    }

    arrow: Icon {
        x: root.width - width - Theme.s2
        y: (root.height - height) / 2
        name: "chevron-right"
        visible: root.subMenu !== null
        size: Theme.szIcon
        color: Theme.textSecondary
    }

    contentItem: Item {
        Text {
            id: label
            objectName: "menuLabel"
            text: root.text
            anchors.left: parent.left
            anchors.right: hint.left
            anchors.rightMargin: hint.text ? Theme.s5 : 0
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: root.highlighted ? Theme.textPrimary : Theme.textSecondary
            elide: Text.ElideRight
        }

        Text {
            id: hint
            objectName: "menuShortcut"
            text: root.shortcutHint
            visible: text !== ""
            width: Math.min(implicitWidth, parent.width * 0.45)
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        radius: Theme.rControl
        color: root.highlighted ? Theme.controlBg : "transparent"
    }
}
