import QtQuick
import QtQuick.Controls
import OmaEdit.Ui as Ui

// Menus opened from the timeline toolbar; all actions follow the current selection.
Item {
    id: root
    property QtObject selection: ClipSelection {}
    property alias trimMenu: trimMenu
    property alias videoMenu: videoMenu
    property alias audioMenu: audioMenu
    property alias markersMenu: markersMenu
    property alias labelMenu: labelMenu
    readonly property bool canTrim: selection.editable && !selection.isGap
    readonly property bool canVideo: canTrim && !selection.isAudio && selection.singleClip
    readonly property bool canAudio: canTrim && selection.hasAudio && selection.singleClip
    readonly property bool canTransitions: canTrim && selection.singleClip
    readonly property bool canSpeed: canTransitions && !selection.isTitle
    readonly property bool canMarkers: backend.hasVideo && (!selection.entry || selection.editable)
    readonly property bool canLabel: canTrim

    AppMenu {
        id: trimMenu
        objectName: "timelineTrimMenu"
        enabled: root.canTrim
        title: qsTr("Trim & Arrange")
        KeyMenuItem {
            text: qsTr("Extend Edit to Playhead")
            commandId: "edit.extend"
            kbd: Ui.Commands.hint(commandId)
            showInMenu: root.selection.singleClip
            enabled: registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Trim Start to Playhead")
            commandId: "edit.trim-start"
            kbd: Ui.Commands.hint(commandId)
            showInMenu: root.selection.singleClip
            enabled: registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        KeyMenuItem {
            text: qsTr("Trim End to Playhead")
            commandId: "edit.trim-end"
            kbd: Ui.Commands.hint(commandId)
            showInMenu: root.selection.singleClip
            enabled: registeredCommand && registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
        }
        RollEditMenu { objectName: "timelineRollEditMenu" }
        KeyMenuItem {
            text: qsTr("Nudge Left"); kbd: "Alt+←"
            showInMenu: !root.selection.isGap
            enabled: root.selection.editable
            onTriggered: win.trimming.nudgeSelected(-1)
        }
        KeyMenuItem {
            text: qsTr("Nudge Right"); kbd: "Alt+→"
            showInMenu: !root.selection.isGap
            enabled: root.selection.editable
            onTriggered: win.trimming.nudgeSelected(1)
        }
        KeyMenuItem {
            text: qsTr("Join Clips"); kbd: "Ctrl+J"
            showInMenu: !root.selection.isGap && root.selection.singleClip
            enabled: root.selection.editable
            onTriggered: win.trimming.joinSelected()
        }
        KeyMenuItem {
            readonly property bool compound: root.selection.singleClip && !root.selection.onTrack
                && root.selection.entry !== null && root.selection.entry.compound === true
            text: compound ? qsTr("Ungroup Clip") : qsTr("New Nested Clip")
            kbd: compound ? "Alt+Shift+G" : "Alt+G"
            showInMenu: !root.selection.isGap && (!root.selection.onTrack || !root.selection.singleClip)
            enabled: root.selection.editable
            onTriggered: compound ? win.clipCommands.breakApartSelection() : win.clipCommands.compoundSelection()
        }
        KeyMenuItem {
            text: qsTr("Select Clips Forward on Track")
            onTriggered: {
                if (root.selection.entry !== null)
                    win.selection.selectTrackForward(root.selection.entry.track, root.selection.entry.index, false, false)
            }
        }
    }
    AppMenu {
        id: videoMenu
        objectName: "timelineVideoMenu"
        title: qsTr("Video")
        enabled: root.canVideo
        KeyMenuItem {
            text: qsTr("Transform / Picture in Picture…")
            onTriggered: win.showClipInspector(0)
        }
        KeyMenuItem {
            text: qsTr("Freeze Frame"); kbd: "Alt+F"
            showInMenu: !root.selection.isTitle
            onTriggered: {
                var e = root.selection.entry
                var inside = backend.position >= e.start && backend.position < e.start + e.frames
                backend.freezeFrameAt(e.track, e.index, inside ? backend.position : e.start)
            }
        }
        KeyMenuItem {
            text: win.videoAnimIndex === win.selectedConnectedIndex && win.videoAnimIndex >= 0
                ? qsTr("Hide Video Animation") : qsTr("Show Video Animation")
            kbd: "Ctrl+Shift+V"
            showInMenu: root.selection.onTrack
            onTriggered: win.videoAnimIndex = win.videoAnimIndex === win.selectedConnectedIndex
                ? -1 : win.selectedConnectedIndex
        }
        KeyMenuItem {
            text: root.selection.entry !== null && root.selection.entry.rendered === true
                ? qsTr("Restore Original") : qsTr("Render in Place")
            showInMenu: !root.selection.isTitle
            enabled: backend.renderJob.track === undefined
            onTriggered: win.clipCommands.renderOrRestoreSelected()
        }
    }
    AppMenu {
        id: audioMenu
        objectName: "timelineAudioMenu"
        title: qsTr("Audio")
        enabled: root.canAudio
        KeyMenuItem {
            objectName: "timelineSoundTiming"
            text: qsTr("Sound timing…")
            showInMenu: root.selection.isAudio
            onTriggered: audioTimingDialog.openFor(root.selection.entry.track, root.selection.entry.index)
        }
        KeyMenuItem {
            text: qsTr("Detach Audio"); kbd: "Alt+Shift+S"
            showInMenu: !root.selection.isAudio && root.selection.entry !== null && root.selection.entry.videoOnly !== true && root.selection.entry.silent !== true
            onTriggered: root.selection.onTrack
                ? backend.detachAudioOn(root.selection.entry.track, root.selection.entry.index)
                : backend.detachAudio(win.selectedTimelineIndex)
        }
        KeyMenuItem {
            text: qsTr("Silence Clip")
            onTriggered: win.sound.setVolume(-60)
        }
        KeyMenuItem {
            text: qsTr("Reset Volume")
            enabled: win.sound.volume() !== 0
            onTriggered: win.sound.setVolume(0)
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Audio Controls…")
            onTriggered: win.showClipInspector(1)
        }
    }
    AppMenu {
        id: markersMenu
        objectName: "timelineMarkersMenu"
        title: qsTr("Markers")
        enabled: root.canMarkers
        KeyMenuItem {
            text: win.clipMarkers.targetClip() !== null
                ? (win.clipMarkers.clipMarkerHere() !== null ? qsTr("Remove Clip Marker") : qsTr("Add Clip Marker at Playhead"))
                : (win.clipMarkers.timelineMarkerHere() !== null ? qsTr("Remove Timeline Marker") : qsTr("Add Timeline Marker at Playhead"))
            kbd: "M"
            onTriggered: win.clipMarkers.toggleHere()
        }
        KeyMenuItem {
            text: win.clipMarkers.clipMarkerHere() !== null ? qsTr("Modify Clip Marker…") : qsTr("Modify Timeline Marker…")
            kbd: "Shift+M"
            enabled: win.clipMarkers.clipMarkerHere() !== null || win.clipMarkers.timelineMarkerHere() !== null
            onTriggered: win.clipMarkers.modifyHere()
        }
    }
    LabelMenu { id: labelMenu; objectName: "timelineLabelMenu"; enabled: root.canLabel }
}
