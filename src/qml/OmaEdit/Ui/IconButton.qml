import QtQuick
import QtQuick.Controls as C

// Icon-only button. Because it carries no visible label it MUST be given a
// `text` — that is what the tooltip shows and what a screen reader announces.
C.AbstractButton {
    id: root

    // `text` is the accessible name and tooltip title; `shortcut` is appended
    // to the tooltip as the brief requires ("Blade Tool  B").
    property string iconName: ""
    property string shortcut: ""
    property string commandId: ""
    // What the tooltip says. Falls back to the label / accessible name, so a
    // button only needs `tip` when the name alone does not explain it.
    property string tip: ""
    readonly property string tipText: tip !== "" ? tip : (text !== "" ? text : Accessible.name)
    readonly property string shortcutHint: commandId ? Commands.hint(commandId) : Commands.hintForDefault(shortcut)
    property color iconColor: checked ? Theme.accent : Theme.textSecondary
    // Flat buttons carry no chrome until hovered — used in dense toolbars
    // where a grid of filled boxes would read as noise.
    property bool flat: true

    implicitWidth: Theme.szIconHit
    implicitHeight: Theme.szIconHit
    hoverEnabled: true
    // Tab reaches it; a click never takes focus — the keyboard belongs to the
    // editing shortcuts (Space, JKL) and must survive a toolbar click.
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: tip !== "" ? tip : shortcutHint ? qsTr("Shortcut: %1").arg(shortcutHint) : ""

    background: Rectangle {
        radius: Theme.rControl
        color: {
            if (root.checked)
                return root.down ? Theme.pressedOn(Theme.controlBg) : Theme.controlBg
            if (root.down)
                return Theme.pressedOn(Theme.controlBg)
            if (root.hovered)
                return Theme.hovered(Theme.panelRaised)
            return root.flat ? "transparent" : Theme.controlBg
        }
        border.width: root.visualFocus ? Theme.focusRing : 0
        border.color: Theme.accent

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    // AbstractButton stretches contentItem to fill, so the glyph is centred
    // inside a filling wrapper rather than being the contentItem itself.
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: root.iconName
            size: Theme.szIcon
            color: root.iconColor
        }
    }

    Tooltip {
        text: root.tipText
        shortcut: root.shortcutHint
        visible: root.hovered && !root.down && root.tipText !== ""
    }
}
