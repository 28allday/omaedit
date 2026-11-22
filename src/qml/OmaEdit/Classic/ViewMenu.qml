import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The View menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("View")

    AppMenu {
        title: qsTr("Playback")

        KeyMenuItem {
            text: qsTr("Play / Pause")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.play"
        }

        KeyMenuItem {
            text: qsTr("Play Selection")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.play-selection"
        }

        KeyMenuItem {
            text: qsTr("Play Around Edit Point")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.play-around"
        }

        KeyMenuItem {
            text: qsTr("Play from Beginning")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.play-from-start"
        }

        KeyMenuItem {
            text: qsTr("Loop Playback")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "transport.loop"
        }

        KeyMenuItem {
            text: qsTr("Full Screen Viewer")
            kbd: "Ctrl+F11"
            enabled: !!win.activeLayout
            onTriggered: win.activeLayout.toggleViewerFullscreen()
            commandId: "view.viewer-fullscreen"
        }
    }

    AppMenu {
        title: qsTr("Viewer")

        KeyMenuItem {
            text: backend.compareMode ? qsTr("Hide Comparison") : qsTr("Comparison")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.compare"
        }

        KeyMenuItem {
            text: backend.viewerSettings.safeAreas ? qsTr("Hide Safe Areas") : qsTr("Safe Areas")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.safe-areas"
        }

        KeyMenuItem {
            text: win.editScopes ? qsTr("Hide Video Scopes") : qsTr("Video Scopes")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "view.video-scopes"
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Zoom In")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.zoom-in"
    }

    KeyMenuItem {
        text: qsTr("Zoom Out")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.pxPerFrame > 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.zoom-out"
    }

    KeyMenuItem {
        text: qsTr("Zoom to Fit")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.pxPerFrame > 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.zoom-fit"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Taller Tracks")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.hLaneRow < win.hLaneRowMax) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.tracks-taller"
    }

    KeyMenuItem {
        text: qsTr("Shorter Tracks")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.hLaneRow > win.hLaneRowMin) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.tracks-shorter"
    }

    KeyMenuItem {
        text: qsTr("Reset Track Height")
        enabled: (win.hLaneRow !== Ui.Theme.hLaneRow || win.trackSizedAlone) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "view.reset-track-height"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: win.skimEnabled ? qsTr("Disable Hover Preview") : qsTr("Enable Hover Preview")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.skimming"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: win.snapEnabled ? qsTr("Disable Snapping") : qsTr("Enable Snapping")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.snapping"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Waveforms From the Base")
        kbd: Ui.Commands.hint(commandId)
        checkable: true
        checked: win.rectifiedWaveforms
        // A click flips a checkable item by itself: bind it to the choice again.
        onTriggered: { Ui.Commands.trigger(commandId); checked = Qt.binding(function () { return win.rectifiedWaveforms }) }
        commandId: "view.rectified-waveforms"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Show Beat Map")
        kbd: Ui.Commands.hint(commandId)
        checkable: true
        checked: win.showBeatMap
        onTriggered: { Ui.Commands.trigger(commandId); checked = Qt.binding(function () { return win.showBeatMap }) }
        commandId: "view.beat-map"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {}

    KeyMenuItem {
        text: win.showTimelineIndex ? qsTr("Hide Timeline List") : qsTr("Show Timeline List")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.timeline-index"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Previous Timeline Opened")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.history-back"
    }

    KeyMenuItem {
        text: qsTr("Next Timeline Opened")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "view.history-forward"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Full Screen Application")
        kbd: "F11"
        checkable: true
        checked: win.applicationFullscreen
        onTriggered: win.toggleApplicationFullscreen()
        commandId: "view.fullscreen"
        enabled: !registeredCommand || registeredCommand.available
    }
}
