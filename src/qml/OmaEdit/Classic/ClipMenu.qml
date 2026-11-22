import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Clip menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Clip")

    KeyMenuItem {
        text: qsTr("Sync Clips…")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.sync"
    }

    KeyMenuItem {
        text: qsTr("Open Clip")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.open"
    }

    KeyMenuItem {
        text: win.videoAnimIndex >= 0 ? qsTr("Hide Video Animation") : qsTr("Show Video Animation")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.animation"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Ungroup Clip")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.break-apart"
    }

    KeyMenuItem {
        text: qsTr("Detach Audio")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.detach-audio"
    }

    KeyMenuItem {
        text: qsTr("Reattach Audio")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.reattach-audio"
    }

    KeyMenuItem {
        text: qsTr("Link Picture and Sound")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.link"
    }

    KeyMenuItem {
        text: qsTr("Unlink Picture and Sound")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.unlink"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: backend.multicamMode ? qsTr("Hide Multicam Viewer") : qsTr("Show Multicam Viewer")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "multicam.viewer"
    }

    KeyMenuItem {
        text: win.multicamAudioFollows ? qsTr("Multicam: Audio Follows Video ●") : qsTr("Multicam: Audio Follows Video")
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "edit.multicam-audio-follows-video"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {}

    KeyMenuItem {
        readonly property bool isDisabled: win.selectedTimelineIndex >= 0 && win.selectedTimelineIndex < backend.timeline.length && backend.timeline[win.selectedTimelineIndex].disabled

        text: isDisabled ? qsTr("Enable Clip") : qsTr("Disable Clip")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.toggle-enabled"
    }

    KeyMenuItem {
        text: win.clipCommands.selectedTrackSoloed() ? qsTr("Unsolo Track") : qsTr("Solo Track")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.solo-track"
    }

    LabelMenu {}

    KeyMenuItem {
        text: qsTr("Map Beats")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.map-beats"
    }

    KeyMenuItem {
        text: qsTr("Clear Beat Map")
        kbd: Ui.Commands.hint(commandId)
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "clip.clear-beat-map"
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Detect Edits")
        KeyMenuItem {
            text: qsTr("Blade at Each Edit")
            commandId: "clip.detect-edits-blade"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Add Markers at Each Edit")
            commandId: "clip.detect-edits-markers"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Stop Detecting")
            commandId: "clip.detect-edits-cancel"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
    }

    KeyMenuItem {
        text: qsTr("Match Frame")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.match-frame"
    }
}
