import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The Edit menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("Edit")

    KeyMenuItem {
        text: Ui.Commands.title(commandId, "Undo")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.canUndo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.undo"
    }

    KeyMenuItem {
        text: Ui.Commands.title(commandId, "Redo")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.canRedo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.redo"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Cut")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.cut"
    }

    KeyMenuItem {
        text: qsTr("Copy")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.copy"
    }

    KeyMenuItem {
        text: qsTr("Copy Timecode")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.copy-timecode"
    }

    KeyMenuItem {
        text: qsTr("Paste at Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.hasClipboard) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.paste"
    }

    KeyMenuItem {
        text: qsTr("Paste Insert")
        kbd: Ui.Commands.hint(commandId)
        enabled: win.hasClipboard && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.paste-insert"
    }

    AppMenu {
        title: qsTr("Captions")
        KeyMenuItem {
            text: qsTr("Add Caption")
            commandId: "edit.caption-add"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Split Caption")
            commandId: "edit.caption-split"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Join Captions")
            commandId: "edit.caption-join"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Generate Captions")
            commandId: "edit.caption-generate"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Caption Style…")
            commandId: "edit.caption-style"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
    }

    AppMenu {
        title: qsTr("Keyframes")
        KeyMenuItem {
            text: qsTr("Cut Keyframes")
            commandId: "edit.keyframes-cut"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Copy Keyframes")
            commandId: "edit.keyframes-copy"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Paste Keyframes")
            commandId: "edit.keyframes-paste"
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Delete")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.ripple-delete"
    }

    KeyMenuItem {
        text: qsTr("Replace with Gap")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.lift"
    }

    KeyMenuItem {
        text: qsTr("Delete Gaps")
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.delete-gaps"
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Select")

        KeyMenuItem {
            text: qsTr("Select All")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-all"
        }

        KeyMenuItem {
            text: qsTr("Clip at Playhead")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-at-playhead"
        }

        KeyMenuItem {
            text: qsTr("Next Clip")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-next"
        }

        KeyMenuItem {
            text: qsTr("Previous Clip")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-previous"
        }

        KeyMenuItem {
            text: qsTr("Clip Above")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-above"
        }

        KeyMenuItem {
            text: qsTr("Clip Below")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.select-below"
        }

        KeyMenuItem {
            text: qsTr("Clips Forward on Track")
            enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.select-clips-forward-on-track"
        }

        KeyMenuItem {
            text: qsTr("Deselect All")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectionCount > 0) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.deselect-all"
        }

        KeyMenuItem {
            text: qsTr("Linked Selection")
            kbd: Ui.Commands.hint(commandId)
            checkable: true
            checked: win.linkedSelection
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.linked-selection"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    KeyMenuItem {
        text: qsTr("Find…")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.find"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Paste Attributes…")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.attributes.canPaste) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.paste-attributes"
    }

    // One row for the pair keeps the menu within a laptop screen.
    AppMenu {
        title: qsTr("Remove")
        KeyMenuItem {
            text: qsTr("Remove Effects")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.remove-effects"
        }

        KeyMenuItem {
            text: qsTr("Remove Attributes…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.remove-attributes"
        }
    }

    KeyMenuItem {
        text: qsTr("Duplicate")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.duplicate"
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Place on Top")

        KeyMenuItem {
            text: qsTr("Picture and Sound")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0 && backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.connect-video"
        }

        KeyMenuItem {
            text: qsTr("Sound Only")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0 && backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.connect-audio"
        }
    }

    KeyMenuItem {
        text: qsTr("Insert at Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedLibraryIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.insert"
    }

    KeyMenuItem {
        text: qsTr("Append at End")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedLibraryIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.append"
    }

    KeyMenuItem {
        text: qsTr("Overwrite at Playhead")
        kbd: Ui.Commands.hint(commandId)
        enabled: (win.selectedLibraryIndex >= 0) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.overwrite"
    }

    AppMenu {
        title: qsTr("Replace")

        KeyMenuItem {
            text: qsTr("With Source Clip")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0 && (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.replace"
        }

        KeyMenuItem {
            text: qsTr("With Source Clip from Its End")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0 && (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.replace-from-end"
        }

        KeyMenuItem {
            text: qsTr("Fit to Fill")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0 && (win.hasTimelineRange || win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.fit-fill"
        }

        // Here rather than beside Overwrite: the Edit menu has no room for another line of its own.
        KeyMenuItem {
            text: qsTr("Overwrite Ending at Playhead")
            kbd: Ui.Commands.hint(commandId)
            enabled: (win.selectedLibraryIndex >= 0) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.overwrite-backtimed"
        }

        // What Insert, Append, Overwrite, Replace and a drag from the bin use.
        // Here for the same reason: the Edit menu has no room for another line.
        AppMenu {
            title: qsTr("Edits Use")

            KeyMenuItem {
                text: qsTr("Picture and Sound")
                kbd: Ui.Commands.hint(commandId)
                checkable: true
                checked: backend.editTakes === "both"
                // A click flips a checkable item by itself: bind it to the choice again.
                onTriggered: { Ui.Commands.trigger(commandId); checked = Qt.binding(function () { return backend.editTakes === "both" }) }
                commandId: "edit.takes-both"
            }

            KeyMenuItem {
                text: qsTr("Picture Only")
                kbd: Ui.Commands.hint(commandId)
                checkable: true
                checked: backend.editTakes === "picture"
                // A click flips a checkable item by itself: bind it to the choice again.
                onTriggered: { Ui.Commands.trigger(commandId); checked = Qt.binding(function () { return backend.editTakes === "picture" }) }
                commandId: "edit.takes-picture"
            }

            KeyMenuItem {
                text: qsTr("Sound Only")
                kbd: Ui.Commands.hint(commandId)
                checkable: true
                checked: backend.editTakes === "sound"
                // A click flips a checkable item by itself: bind it to the choice again.
                onTriggered: { Ui.Commands.trigger(commandId); checked = Qt.binding(function () { return backend.editTakes === "sound" }) }
                commandId: "edit.takes-sound"
            }
        }
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Add Title or Generated Clip")

        KeyMenuItem {
            text: qsTr("Gap")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.gap"
        }

        KeyMenuItem {
            text: qsTr("Solid Colour")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.generator-solid"
        }

        KeyMenuItem {
            text: qsTr("Colour Bars")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.generator-bars"
        }

        KeyMenuItem {
            text: qsTr("Noise")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.generator-noise"
        }

        KeyMenuItem {
            text: qsTr("Counting Leader")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.generator-count"
        }

        KeyMenuItem {
            text: qsTr("Effect Layer")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.generator-adjust"
        }

        KeyMenuItem {
            text: qsTr("Title…")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.title"
        }

        KeyMenuItem {
            // The default title and default lower third sit on
            // sibling shortcuts.
            text: qsTr("Lower Third…")
            kbd: Ui.Commands.hint(commandId)
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.lower-third"
        }
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Freeze Frame")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "edit.freeze"
    }
}
