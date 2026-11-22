import OmaEdit.Ui as Ui

// Legacy call sites can supply kbd; registered commands always use their
// current binding. Activation stays with the existing command handler.
Ui.MenuAction {
    property string kbd: ""
    readonly property var registeredCommand: Ui.Commands.command(commandId)
    objectName: commandId ? "menu:" + commandId : ""
    shortcut: kbd
}
