import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls

// The context menu keeps clipboard, source and removal actions close to the clip.
// Editing, picture, sound, markers and labels live above the timeline; transitions
// are dragged from the Effects tab.
// Right-click promotion preserves an existing multi-selection before opening.
AppMenu {
    id: root
    property QtObject selection: ClipSelection {}

    KeyMenuItem {
        text: qsTr("Edit Title…")
        showInMenu: root.selection.isTitle && root.selection.singleClip
        enabled: root.selection.editable && root.selection.onTrack
        onTriggered: titleDialog.openFor(win.selectedConnectedIndex, root.selection.entry.title,
            root.selection.entry.titlePlace, root.selection.entry.titleSize, root.selection.entry.titleFont,
            root.selection.entry.titleColor, win.catalogue.titleExtras(root.selection.entry))
    }
    KeyMenuItem {
        text: qsTr("Copy"); kbd: "Ctrl+C"
        showInMenu: !root.selection.isGap
        onTriggered: win.copying.copySelected()
    }
    KeyMenuItem {
        text: qsTr("Paste at Playhead"); kbd: "Ctrl+V"
        enabled: root.selection.editable && win.hasClipboard
        onTriggered: win.copying.pasteClipboard()
    }
    KeyMenuItem {
        text: qsTr("Duplicate"); kbd: "Ctrl+D"
        showInMenu: !root.selection.isGap && root.selection.singleClip
        enabled: root.selection.editable
        onTriggered: win.copying.duplicateSelected()
    }
    KeyMenuItem {
        text: qsTr("Paste Attributes…"); kbd: "Alt+V"
        showInMenu: !root.selection.isGap
        enabled: root.selection.editable && win.attributes.canPaste
        onTriggered: win.attributes.paste()
    }
    MenuSeparator {
        visible: !root.selection.isGap && !root.selection.isTitle && root.selection.singleClip
        height: !root.selection.isGap && !root.selection.isTitle && root.selection.singleClip ? implicitHeight : 0
    }
    AngleMenu {
        entry: root.selection.entry
        track: entry !== null ? entry.track : 0
        index: entry !== null ? entry.index : -1
        showInParent: isMulticam && root.selection.singleClip
        enabled: root.selection.editable
    }
    // Where the whole placed multicam clip's sound comes from (every piece of
    // it across its angle cuts), beside the angle of this piece.
    SoundFromMenu {
        id: soundFrom
        objectName: "clipSoundFromMenu"
        readonly property var entry: root.selection.entry
        readonly property bool isMulticam: entry !== null && entry !== undefined
                                           && entry.mcam !== undefined && entry.mcam !== ""
        rows: isMulticam && root.visible ? backend.multicamSoundSources(entry.mcam) : []
        showInParent: isMulticam && root.selection.singleClip && rows.length > 0
        enabled: root.selection.editable && rows.length > 0
        onChosen: function (source) { backend.setMulticamSound(soundFrom.entry.track, soundFrom.entry.index, source) }
    }
    AppMenu {
        title: qsTr("Source")
        showInParent: !root.selection.isGap && !root.selection.isTitle && root.selection.singleClip
        KeyMenuItem {
            text: qsTr("Match Frame"); kbd: "F"
            onTriggered: win.matching.matchFrame()
        }
        KeyMenuItem {
            text: qsTr("Reveal Source Clip"); kbd: "Shift+F"
            onTriggered: win.matching.revealSourceClip()
        }
        KeyMenuItem {
            text: qsTr("Replace with Source Clip"); kbd: "Shift+R"
            enabled: root.selection.editable && win.selectedLibraryIndex >= 0
            onTriggered: win.matching.replaceSelected()
        }
    }
    MenuSeparator {}
    // Parting a clip from its sound, as on the Clip menu.
    KeyMenuItem {
        text: qsTr("Unlink Picture and Sound")
        commandId: "clip.unlink"
        kbd: Ui.Commands.hint(commandId)
        showInMenu: !root.selection.isGap
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Link Picture and Sound")
        commandId: "clip.link"
        kbd: Ui.Commands.hint(commandId)
        showInMenu: !root.selection.isGap && !!registeredCommand && registeredCommand.available
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Detach Audio")
        commandId: "edit.detach-audio"
        kbd: Ui.Commands.hint(commandId)
        showInMenu: !root.selection.isGap && !!registeredCommand && registeredCommand.available
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    // The beats of the clip's music, from its recording (as on the Clip menu).
    KeyMenuItem {
        text: qsTr("Map Beats")
        commandId: "clip.map-beats"
        kbd: Ui.Commands.hint(commandId)
        showInMenu: !root.selection.isGap && !!registeredCommand && registeredCommand.available
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Clear Beat Map")
        commandId: "clip.clear-beat-map"
        kbd: Ui.Commands.hint(commandId)
        showInMenu: !root.selection.isGap && !!registeredCommand && registeredCommand.available
        enabled: !!registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: root.selection.entry !== null && root.selection.entry.disabled ? qsTr("Enable Clip") : qsTr("Disable Clip")
        kbd: "V"
        showInMenu: !root.selection.isGap
        enabled: root.selection.editable
        onTriggered: win.clipCommands.toggleDisabledSelected()
    }
    KeyMenuItem {
        text: qsTr("Delete (Leave Gap)"); kbd: "Backspace"
        showInMenu: !root.selection.isGap
        enabled: root.selection.editable
        onTriggered: win.clipCommands.liftSelected()
    }
    KeyMenuItem {
        text: qsTr("Ripple Delete")
        kbd: "Del"
        enabled: root.selection.editable
        onTriggered: win.clipCommands.deleteSelected()
    }
}
