import QtQuick
import QtQuick.Controls
import OmaEdit.Ui as Ui

// Shared by the main Trim menu and the timeline's Trim & Arrange menu.
AppMenu {
    title: qsTr("Roll Edit")
    enabled: win.trimming.rollTarget() !== null
    KeyMenuItem {
        text: qsTr("Roll Edit Left 1 Frame")
        commandId: "edit.roll-left"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Roll Edit Right 1 Frame")
        commandId: "edit.roll-right"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Roll Edit Left 10 Frames")
        commandId: "edit.roll-left-ten"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Roll Edit Right 10 Frames")
        commandId: "edit.roll-right-ten"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Roll Edit to Playhead")
        commandId: "edit.roll-playhead"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
}
