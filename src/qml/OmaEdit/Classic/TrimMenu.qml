import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Trim menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Trim")

    KeyMenuItem {
        text: qsTr("Blade at Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.blade"
    }

    KeyMenuItem {
        text: qsTr("Blade All at Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.blade-all"
    }

    KeyMenuItem {
        text: qsTr("Join Clips")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.join"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Trim Start to Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.trim-start"
    }

    KeyMenuItem {
        text: qsTr("Trim End to Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.trim-end"
    }

    KeyMenuItem {
        text: qsTr("Trim to Range")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.trim-to-range"
    }

    KeyMenuItem {
        text: qsTr("Extend Edit to Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.extend"
    }

    RollEditMenu {
        objectName: "mainRollEditMenu"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Nudge Left")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-left"
    }

    KeyMenuItem {
        text: qsTr("Nudge Right")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-right"
    }

    KeyMenuItem {
        text: qsTr("Nudge Left 10 Frames")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-left-ten"
    }

    KeyMenuItem {
        text: qsTr("Nudge Right 10 Frames")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-right-ten"
    }

    KeyMenuItem {
        text: qsTr("Nudge Edge Left")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-edge-left"
    }

    KeyMenuItem {
        text: qsTr("Nudge Edge Right")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.nudge-edge-right"
    }

    KeyMenuItem {
        text: qsTr("Swap Clip Left")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.swap-left"
    }

    KeyMenuItem {
        text: qsTr("Swap Clip Right")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.swap-right"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: win.dynamicTrim ? qsTr("Dynamic Trim ✓") : qsTr("Dynamic Trim")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.dynamic-trim"
    }

    KeyMenuItem {
        readonly property int target: win.trimming.dissolveTarget()

        text: win.precisionBoundary >= 0 ? qsTr("Close Trim Editor") : qsTr("Trim Editor")
        enabled: (win.precisionBoundary >= 0 || (target >= 0 && target + 1 < backend.timeline.length && backend.timeline[target].mixOut === 0)) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "edit.toggle-precision-editor"
    }
}
