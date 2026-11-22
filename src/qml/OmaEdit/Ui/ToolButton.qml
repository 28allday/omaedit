import QtQuick
import QtQuick.Controls as C

// Toolbar button that may carry an icon, a label, or both. Checkable when it
// represents a mode (the timeline's select/blade/slip tools) rather than an
// action; a checked tool shows a filled well plus a cyan glyph so the active
// mode is readable at a glance across a 38 px strip.
C.AbstractButton {
    id: root

    property string iconName: ""
    property string shortcut: ""
    property string commandId: ""
    // What the tooltip says beyond the label. A labelled button with no tip
    // and no shortcut shows nothing rather than repeating its own caption.
    property string tip: ""
    readonly property string tipText: tip !== "" ? tip : (text !== "" ? text : Accessible.name)
    readonly property string shortcutHint: commandId ? Commands.hint(commandId) : Commands.hintForDefault(shortcut)
    property bool showLabel: text !== "" && iconName === ""

    implicitWidth: Math.max(Theme.szIconHit,
                            row.implicitWidth + Theme.s2 * 2)
    implicitHeight: Theme.hControl
    hoverEnabled: true
    // Tab reaches it; a click never takes focus — the keyboard belongs to the
    // editing shortcuts (Space, JKL) and must survive a toolbar click.
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
    Accessible.name: text
    Accessible.description: tip !== "" ? tip : shortcutHint ? qsTr("Shortcut: %1").arg(shortcutHint) : ""

    background: Rectangle {
        radius: Theme.rControl
        color: {
            if (root.checked)
                return root.down ? Theme.pressedOn(Theme.controlBg) : Theme.controlBg
            if (root.down)
                return Theme.pressedOn(Theme.panelRaised)
            if (root.hovered)
                return Theme.hovered(Theme.panelRaised)
            return "transparent"
        }
        border.width: root.visualFocus ? Theme.focusRing : 0
        border.color: Theme.accent

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    contentItem: Row {
        id: row
        spacing: Theme.s1
        anchors.centerIn: parent

        Icon {
            name: root.iconName
            visible: root.iconName !== ""
            size: Theme.szIcon
            color: root.checked ? Theme.accent : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.text
            visible: root.showLabel
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: root.checked ? Theme.accent : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Tooltip {
        text: root.tipText
        shortcut: root.shortcutHint
        visible: root.hovered && !root.down && root.tipText !== "" && (root.tip !== "" || !root.showLabel || root.shortcutHint !== "")
    }
}
