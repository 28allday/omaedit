import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Mark menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Mark")

    KeyMenuItem {
        text: qsTr("Set Range Start")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.previewLibraryIndex >= 0 || backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "range.in"
    }

    KeyMenuItem {
        text: qsTr("Set Range End")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.previewLibraryIndex >= 0 || backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "range.out"
    }

    KeyMenuItem {
        text: qsTr("Select Clip Range")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo && !backend.viewingSource) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "range.clip"
    }

    KeyMenuItem {
        text: qsTr("Clear Range")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.previewLibraryIndex >= 0 || backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "range.clear"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Favourite")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "mark.favorite"
    }

    KeyMenuItem {
        text: qsTr("Reject")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "mark.reject"
    }

    KeyMenuItem {
        text: qsTr("Unrate")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "mark.unrate"
    }

    KeyMenuItem {
        text: qsTr("Remove All Keywords")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "mark.remove-keywords"
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Markers")

        KeyMenuItem {
            text: win.clipMarkers.targetClip() !== null ? qsTr("Add/Remove Clip Marker") : qsTr("Add/Remove Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.toggle"
        }

        KeyMenuItem {
            text: qsTr("Add/Remove Timeline Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.toggle-timeline"
        }

        KeyMenuItem {
            text: qsTr("Add Marker and Edit…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.add-modify"
        }

        KeyMenuItem {
            text: qsTr("Add To-Do Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.add-todo"
        }

        KeyMenuItem {
            text: qsTr("Add Chapter Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.add-chapter"
        }

        KeyMenuItem {
            text: qsTr("Modify Marker…")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.clipMarkers.timelineMarkerHere() !== null || win.clipMarkers.clipMarkerHere() !== null) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.edit"
        }

        KeyMenuItem {
            text: qsTr("Nudge Marker Left")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.nudge-left"
        }

        KeyMenuItem {
            text: qsTr("Nudge Marker Right")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.nudge-right"
        }

        KeyMenuItem {
            text: qsTr("Delete Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.delete"
        }

        KeyMenuItem {
            text: qsTr("Delete Markers in Range")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.delete-range"
        }
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Go To")

        KeyMenuItem {
            text: qsTr("Beginning")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo || backend.viewingSource) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.start"
        }

        KeyMenuItem {
            text: qsTr("End")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo || backend.viewingSource) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.end"
        }

        KeyMenuItem {
            text: qsTr("Timecode…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.go-to-timecode"
        }

        KeyMenuItem {
            text: qsTr("Range Start")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.range-start"
        }

        KeyMenuItem {
            text: qsTr("Range End")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.range-end"
        }
    }

    AppMenu {
        title: qsTr("Previous")

        KeyMenuItem {
            text: qsTr("Frame")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.previous-frame"
        }

        KeyMenuItem {
            text: qsTr("Edit")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.previous-edit"
        }

        KeyMenuItem {
            text: qsTr("Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: !!registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.previous"
        }
    }

    AppMenu {
        title: qsTr("Next")

        KeyMenuItem {
            text: qsTr("Frame")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.next-frame"
        }

        KeyMenuItem {
            text: qsTr("Edit")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.next-edit"
        }

        KeyMenuItem {
            text: qsTr("Marker")
            kbd: Ui.Commands.hint(commandId)
            enabled: !!registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "marker.next"
        }
    }
}
