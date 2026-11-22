import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Window menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Window")

    AppMenu {
        title: qsTr("Go To")

        KeyMenuItem {
            text: qsTr("Next Panel")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.next-panel"
        }

        KeyMenuItem {
            text: qsTr("Previous Panel")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.previous-panel"
        }
    }

    AppMenu {
        title: qsTr("Panels")

        KeyMenuItem {
            text: classicLayout.browserTab === 1 ? qsTr("Show Project Media") : qsTr("Show Effects")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.effects"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: win.dualViewer ? qsTr("Hide Source Viewer") : qsTr("Show Source Viewer")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.dual-viewer"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: classicLayout.inspectorShown ? qsTr("Hide Inspector") : qsTr("Show Inspector")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.inspector"
        }
    }

    AppMenu {
        title: qsTr("Workspaces")

        KeyMenuItem {
            text: qsTr("Media")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "workspace.media"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Edit")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "workspace.edit"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Colour")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "workspace.color"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Audio")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "workspace.audio"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Export")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "workspace.deliver"
            enabled: !registeredCommand || registeredCommand.available
        }

        MenuSeparator {}

        KeyMenuItem {
            text: qsTr("Workspace Layout…")
            kbd: "Ctrl+Alt+W"
            onTriggered: workspaceLayoutDialog.open()
            commandId: "view.layout"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Restore All Panels")
            onTriggered: {
                if (win.activeLayout) {
                    win.activeLayout.restorePanels();
                }
            }
            kbd: Ui.Commands.hint(commandId)
            commandId: "view.restore-all-panels"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Reset Workspace")
            onTriggered: {
                if (win.activeLayout) {
                    win.activeLayout.reset();
                }
            }
            kbd: Ui.Commands.hint(commandId)
            commandId: "view.reset-workspace"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Background Tasks…")
        commandId: "view.tasks"
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
    }

    KeyMenuItem {
        text: qsTr("Timeline Settings…")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "project.settings"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Viewer and Playback…")
        commandId: "view.playback"
        onTriggered: Ui.Commands.trigger(commandId)
    }

    KeyMenuItem {
        text: backend.recordingTake ? qsTr("Stop Recording") : qsTr("Record Voiceover")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "window.record-voiceover"
    }
}
