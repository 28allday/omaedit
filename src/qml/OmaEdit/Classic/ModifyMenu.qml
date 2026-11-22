import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Modify menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Modify")

    KeyMenuItem {
        text: win.clipCommands.selectedHasStabilization() ? qsTr("Remove Stabilisation") : qsTr("Stabilise")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.stabilize"
    }

    KeyMenuItem {
        text: qsTr("Auto Balance Colour")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.auto-balance"
    }

    KeyMenuItem {
        text: backend.reframing ? qsTr("Stop Reframe to Fit") : qsTr("Reframe to Fit")
        commandId: backend.reframing ? "modify.reframe-to-fit-stop" : "modify.reframe-to-fit"
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }

    AppMenu {
        title: qsTr("Match")
        KeyMenuItem {
            text: win.matchReferenceSet ? qsTr("Set Match Reference (now %1)").arg(backend.matchReference.name) : qsTr("Set Match Reference")
            commandId: "modify.match-reference"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Match Colour")
            commandId: "modify.match-color"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Match Audio")
            commandId: "modify.match-audio"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Adjust Volume")

        KeyMenuItem {
            text: qsTr("Up 1 dB")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.sound.targetExists) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "audio.volume-up"
        }

        KeyMenuItem {
            text: qsTr("Down 1 dB")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.sound.targetExists) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "audio.volume-down"
        }

        KeyMenuItem {
            text: qsTr("Absolute…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "modify.volume-absolute"
        }

        KeyMenuItem {
            text: qsTr("Relative…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "modify.volume-relative"
        }

        KeyMenuItem {
            text: qsTr("Silence")
            enabled: (win.sound.targetExists) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.silence-clip"
        }

        KeyMenuItem {
            text: qsTr("Reset")
            enabled: (win.sound.targetExists && win.sound.volume() !== 0) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.reset-volume"
        }
    }

    AppMenu {
        title: qsTr("Adjust Fades")

        KeyMenuItem {
            readonly property bool hasFade: win.selection.selectedEntry() !== null && win.selection.selectedEntry().fadeIn > 0

            text: hasFade ? qsTr("Remove Fade In") : qsTr("Fade In")
            kbd: Ui.Commands.hint(commandId)
            enabled: !!registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.fade-in"
        }

        KeyMenuItem {
            readonly property bool hasFade: win.selection.selectedEntry() !== null && win.selection.selectedEntry().fadeOut > 0

            text: hasFade ? qsTr("Remove Fade Out") : qsTr("Fade Out")
            kbd: Ui.Commands.hint(commandId)
            enabled: !!registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.fade-out"
        }

        KeyMenuItem {
            text: qsTr("Apply Fades")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "modify.apply-fades"
        }

        KeyMenuItem {
            text: qsTr("Remove Fades")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "modify.remove-fades"
        }
    }

    AppMenu {
        title: qsTr("Audio Pan")
        enabled: win.sound.targetExists

        KeyMenuItem {
            text: qsTr("Full Left")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.full-left"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Half Left")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.half-left"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Centre")
            enabled: (win.sound.targetExists && win.sound.pan() !== 0) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.centre"
        }

        KeyMenuItem {
            text: qsTr("Half Right")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.half-right"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Full Right")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.full-right"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    AppMenu {
        title: qsTr("Audio EQ")
        enabled: win.sound.targetExists

        KeyMenuItem {
            text: qsTr("Flat (off)")
            enabled: (win.sound.eq() !== "") && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.flat-off"
        }

        KeyMenuItem {
            text: qsTr("Voice Enhance")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.voice-enhance"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Bass Boost")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.bass-boost"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Bass Reduce")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.bass-reduce"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Treble Boost")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.treble-boost"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Treble Reduce")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.treble-reduce"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Hum Reduction")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.hum-reduction"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    MenuSeparator {}

    CategoryMenu {}

    KeyMenuItem {
        text: qsTr("Edit Categories…")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.edit-categories"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Rotate Left")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.rotate-left"
    }

    KeyMenuItem {
        text: qsTr("Rotate Right")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.rotate-right"
    }

    AppMenu {
        // Presets place the connected clip's picture; drag-in-viewer
        // transforms come later (Transform onscreen controls).
        title: qsTr("Picture in Picture")
        enabled: win.selectedConnectedIndex >= 0

        KeyMenuItem {
            text: qsTr("Full Frame")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.full-frame"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Top Left")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.top-left"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Top Right")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.top-right"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Bottom Left")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.bottom-left"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Bottom Right")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.bottom-right"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Centre Small")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.centre-small"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Change Duration…")
        commandId: "modify.change-duration"
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }

    AppMenu {
        title: qsTr("Retime")
        KeyMenuItem {
            text: qsTr("Speed / Duration…")
            commandId: "edit.speed-duration"
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Speed Segments…")
            commandId: "edit.speed-segments"
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Hold")
            commandId: "modify.retime-hold"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Blade Speed")
            commandId: "modify.retime-blade"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Instant Replay")
            commandId: "modify.retime-replay"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Rewind ×1")
            commandId: "modify.retime-rewind-1"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Rewind ×2")
            commandId: "modify.retime-rewind-2"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Rewind ×4")
            commandId: "modify.retime-rewind-4"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        MenuSeparator {}
        enabled: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0

        KeyMenuItem {
            text: qsTr("Slow 50%")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.slow-50"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Slow 25%")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.slow-25"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Fast 2×")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.fast-2"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Fast 4×")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.fast-4"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Normal 100%")
            enabled: (win.retiming.selectedSpeed() !== 1) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.normal-100"
        }

        MenuSeparator {}

        KeyMenuItem {
            text: qsTr("Reverse Clip")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "modify.reverse"
        }

        AppMenu {
            title: qsTr("Frame Sampling")

            KeyMenuItem {
                text: qsTr("Floor")
                kbd: Ui.Commands.hint(commandId)
                enabled: !registeredCommand || registeredCommand.available
                onTriggered: Ui.Commands.trigger(commandId)
                commandId: "modify.quality-3"
            }

            KeyMenuItem {
                text: qsTr("Nearest Frame")
                kbd: Ui.Commands.hint(commandId)
                enabled: !registeredCommand || registeredCommand.available
                onTriggered: Ui.Commands.trigger(commandId)
                commandId: "modify.quality-4"
            }

            KeyMenuItem {
                text: qsTr("Original Sampling")
                kbd: Ui.Commands.hint(commandId)
                enabled: !registeredCommand || registeredCommand.available
                onTriggered: Ui.Commands.trigger(commandId)
                commandId: "modify.quality-0"
            }

            KeyMenuItem {
                text: qsTr("Frame Blending")
                kbd: Ui.Commands.hint(commandId)
                enabled: !registeredCommand || registeredCommand.available
                onTriggered: Ui.Commands.trigger(commandId)
                commandId: "modify.quality-1"
            }

            KeyMenuItem {
                text: qsTr("Optical Flow")
                kbd: Ui.Commands.hint(commandId)
                enabled: !registeredCommand || registeredCommand.available
                onTriggered: Ui.Commands.trigger(commandId)
                commandId: "modify.quality-2"
            }
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: win.clipCommands.selectedIsRendered() ? qsTr("Restore Original") : qsTr("Render in Place")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.render-clip"
    }

    KeyMenuItem {
        text: backend.backgroundRenderEnabled ? qsTr("Pause Background Rendering") : qsTr("Render in Background")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "modify.background-render"
    }
}
