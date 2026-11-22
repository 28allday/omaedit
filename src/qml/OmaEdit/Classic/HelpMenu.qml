import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Help menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Help")

    KeyMenuItem {
        text: qsTr("Command Palette…")
        commandId: "app.palette"
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Keyboard Shortcuts")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "app.shortcuts"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {
    }

    KeyMenuItem {
        text: qsTr("About OmaEdit / Diagnostics…")
        commandId: "app.diagnostics"
        onTriggered: Ui.Commands.trigger(commandId)
        enabled: !registeredCommand || registeredCommand.available
    }
}
