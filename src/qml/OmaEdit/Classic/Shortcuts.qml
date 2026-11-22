import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// Shared command handlers. Menus, toolbar actions, the palette and keyboard
// bindings dispatch these stable command IDs.
Item {
    id: handlers

    width: 0
    height: 0

    Ui.Command {
        commandId: "view.next-panel"
        text: qsTr("Focus Next Panel")
        category: qsTr("View")
        defaultShortcuts: ["F6"]
        shortcutEnabled: !Ui.Commands.popupOpen && !(win.activeLayout && win.activeLayout.detachedWindowActive)
        onTriggered: win.keyboardNavigation.cyclePanel(false)
    }
    Ui.Command {
        commandId: "view.previous-panel"
        text: qsTr("Focus Previous Panel")
        category: qsTr("View")
        defaultShortcuts: ["Shift+F6"]
        shortcutEnabled: !Ui.Commands.popupOpen && !(win.activeLayout && win.activeLayout.detachedWindowActive)
        onTriggered: win.keyboardNavigation.cyclePanel(true)
    }

    Ui.Command {
        commandId: "sequence.new"
        text: qsTr("New Timeline…")
        category: qsTr("Project")
        scope: "Global"
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: sequenceCreationDialog.openFor()
    }

    Ui.Command {
        commandId: "sequence.duplicate"
        text: qsTr("Duplicate Timeline")
        category: qsTr("Project")
        scope: "Global"
        available: backend.hasTimeline && !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: { win.selection.clearSelection(); backend.duplicateSequence(backend.activeSequence) }
    }

    // Application-context shortcuts keep working when focus is inside a control.
    Ui.Command {
        commandId: "project.new"
        text: qsTr("New Project…")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: [StandardKey.New]
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: win.project.startNew()
    }

    Ui.Command {
        commandId: "project.open"
        text: qsTr("Open Project\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: [StandardKey.Open]
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: win.project.guardedOpen("")
    }

    Ui.Command {
        commandId: "project.settings"
        text: qsTr("Timeline Settings\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Alt+J"]
        available: backend.hasTimeline && !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: projectSettingsDialog.openFor()
    }

    Ui.Command {
        commandId: "app.preferences"
        text: qsTr("Preferences\u2026")
        category: qsTr("Application")
        scope: "Global"
        defaultShortcuts: ["Ctrl+,"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: preferencesDialog.open()
    }

    Ui.Command {
        commandId: "project.save"
        text: qsTr("Save Project")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: [StandardKey.Save]
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: backend.saveProject()
    }

    Ui.Command {
        commandId: "project.save-as"
        text: qsTr("Save Project As\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+S"]
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: backend.saveProjectDialog()
    }

    Ui.Command {
        commandId: "media.import"
        text: qsTr("Import Media\u2026")
        category: qsTr("Media")
        scope: "Global"
        defaultShortcuts: ["Ctrl+I"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace === "Media" && mediaLoader.item ? mediaLoader.item.importMedia() : backend.importDialog()
    }

    Ui.Command {
        commandId: "transport.play"
        text: qsTr("Play / Pause")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Space"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.layoutTransport("play", 0)
    }

    Ui.Command {
        commandId: "transport.reverse"
        text: qsTr("Shuttle Backwards")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["J"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.layoutTransport("shuttle", -1)
    }

    Ui.Command {
        commandId: "transport.stop"
        text: qsTr("Stop Playback")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["K"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.layoutTransport("pause", 0)
    }

    Ui.Command {
        commandId: "transport.forward"
        text: qsTr("Shuttle Forwards")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["L"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.layoutTransport("shuttle", 1)
    }

    Ui.Command {
        commandId: "edit.append"
        text: qsTr("Append at End")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["E"]
        available: win.selectedLibraryIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedLibraryIndex >= 0)
                backend.appendClipAt(win.selectedLibraryIndex);

        }
    }

    Ui.Command {
        commandId: "edit.insert"
        text: qsTr("Insert at Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["W"]
        available: win.selectedLibraryIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedLibraryIndex >= 0)
                backend.insertClipAt(win.selectedLibraryIndex);

        }
    }

    Ui.Command {
        commandId: "edit.overwrite"
        text: qsTr("Overwrite at Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["D"]
        available: win.selectedLibraryIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedLibraryIndex >= 0)
                backend.overwriteClipAt(win.selectedLibraryIndex);

        }
    }

    Ui.Command {
        commandId: "edit.overwrite-backtimed"
        text: qsTr("Overwrite Ending at Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Shift+D"]
        available: win.selectedLibraryIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: if (win.selectedLibraryIndex >= 0) backend.overwriteClipBacktimed(win.selectedLibraryIndex)
    }

    Ui.Command {
        commandId: "edit.takes-both"
        text: qsTr("Edits Use Picture and Sound")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: backend.editTakes = "both"
    }

    Ui.Command {
        commandId: "edit.takes-picture"
        text: qsTr("Edits Use Picture Only")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: backend.editTakes = "picture"
    }

    Ui.Command {
        commandId: "edit.takes-sound"
        text: qsTr("Edits Use Sound Only")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: backend.editTakes = "sound"
    }

    Ui.Command {
        commandId: "edit.fade-in"
        text: qsTr("Toggle Fade In")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+["]
        available: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedConnectedIndex >= 0)
                backend.toggleConnectedFade(win.selectedConnectedIndex, false);
            else if (win.selectedTimelineIndex >= 0)
                backend.toggleFade(win.selectedTimelineIndex, false);
        }
    }

    Ui.Command {
        commandId: "edit.fade-out"
        text: qsTr("Toggle Fade Out")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+]"]
        available: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedConnectedIndex >= 0)
                backend.toggleConnectedFade(win.selectedConnectedIndex, true);
            else if (win.selectedTimelineIndex >= 0)
                backend.toggleFade(win.selectedTimelineIndex, true);
        }
    }

    Ui.Command {
        commandId: "marker.toggle"
        text: qsTr("Add / Remove Marker")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: ["M"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: win.clipMarkers.toggleHere() // a clip marker on the selected clip, else a timeline marker
    }

    Ui.Command {
        commandId: "audio.volume-up"
        text: qsTr("Volume Up 1 dB")
        category: qsTr("Audio")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Up"]
        available: win.sound.targetExists
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.adjustVolume(1)
    }

    Ui.Command {
        commandId: "audio.volume-down"
        text: qsTr("Volume Down 1 dB")
        category: qsTr("Audio")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Down"]
        available: win.sound.targetExists
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.adjustVolume(-1)
    }

    Ui.Command {
        commandId: "view.timeline-index"
        text: qsTr("Toggle Timeline List")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+2"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.showTimelineIndex = !win.showTimelineIndex
    }

    Ui.Command {
        commandId: "view.effects"
        text: qsTr("Toggle Effects Browser")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+5"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.showEffects(classicLayout.browserTab !== 1)
    }

    Ui.Command {
        commandId: "edit.detach-audio"
        text: qsTr("Detach Audio")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+Shift+S"]
        available: win.selectedTimelineIndex >= 0 && win.selectedTimelineIndex < backend.timeline.length && !backend.timeline[win.selectedTimelineIndex].gap && !backend.timeline[win.selectedTimelineIndex].videoOnly && backend.timeline[win.selectedTimelineIndex].silent !== true
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedTimelineIndex >= 0)
                backend.detachAudio(win.selectedTimelineIndex);

        }
    }

    Ui.Command {
        commandId: "view.escape"
        text: qsTr("Restore / Exit / Dismiss")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Escape"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: {
            const floating = win.activeLayout ? win.activeLayout.panels.find(function(p) {
                return p.window.active && p.window.visible;
            }) : null;
            if (floating) {
                if (floating.fullscreen)
                    floating.exitFullscreen();
                else
                    floating.setDetached(false);
            } else if (win.activeLayout && win.activeLayout.maximized !== "")
                win.activeLayout.maximized = "";
            else if (win.applicationFullscreen)
                win.toggleApplicationFullscreen();
            else if (win.videoAnimIndex >= 0)
                win.videoAnimIndex = -1;
            else if (win.precisionBoundary >= 0)
                win.precisionBoundary = -1;
            else
                win.setTool("select");
        }
    }

    Ui.Command {
        commandId: "edit.animation"
        text: qsTr("Toggle Video Animation")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+V"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.videoAnimIndex >= 0)
                win.videoAnimIndex = -1;
            else if (win.selectedConnectedIndex >= 0)
                win.videoAnimIndex = win.selectedConnectedIndex;
        }
    }

    Ui.Command {
        commandId: "edit.copy"
        text: qsTr("Copy Clips")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: [StandardKey.Copy]
        available: win.workspace === "Edit" || win.workspace === "Audio"
        shortcutEnabled: win.keysFree
        onTriggered: win.copying.copySelected()
    }

    Ui.Command {
        commandId: "edit.paste"
        text: qsTr("Paste at Playhead")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: [StandardKey.Paste]
        available: win.workspace === "Edit" || win.workspace === "Audio"
        shortcutEnabled: win.keysFree
        onTriggered: win.copying.pasteClipboard()
    }

    Ui.Command {
        commandId: "edit.paste-insert"
        text: qsTr("Paste Insert")
        category: qsTr("Edit")
        scope: "Edit"
        available: (win.workspace === "Edit" || win.workspace === "Audio") && win.hasClipboard
        shortcutEnabled: win.keysFree
        onTriggered: win.copying.pasteInsertClipboard()
    }

    // Clip ▸ Detect Edits: find the cuts inside the selected clip.
    Ui.Command {
        commandId: "clip.detect-edits-blade"
        text: qsTr("Detect Edits: Blade at Each Edit")
        category: qsTr("Clip")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.canDetectEdits()
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.detectEdits(e.track, e.index, true) }
    }
    Ui.Command {
        commandId: "clip.detect-edits-markers"
        text: qsTr("Detect Edits: Add Markers at Each Edit")
        category: qsTr("Clip")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.canDetectEdits()
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.detectEdits(e.track, e.index, false) }
    }
    Ui.Command {
        commandId: "clip.detect-edits-cancel"
        text: qsTr("Stop Detecting Edits")
        category: qsTr("Clip")
        scope: "Edit"
        available: backend.detectingEdits
        shortcutEnabled: win.keysFree
        onTriggered: backend.cancelDetectEdits()
    }

    // Captions: add at the playhead, split by line, join neighbours, or
    // generate from the timeline's speech.
    Ui.Command {
        commandId: "edit.caption-add"
        text: qsTr("Add Caption")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+C"]
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.addCaption()
    }
    Ui.Command {
        commandId: "edit.caption-split"
        text: qsTr("Split Caption")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.selectedCaptionIndices().length === 1
        shortcutEnabled: win.keysFree
        onTriggered: backend.splitCaption(win.clipCommands.selectedCaptionIndices()[0])
    }
    Ui.Command {
        commandId: "edit.caption-join"
        text: qsTr("Join Captions")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.selectedCaptionIndices().length > 1
        shortcutEnabled: win.keysFree
        onTriggered: backend.joinCaptions(win.clipCommands.selectedCaptionIndices())
    }
    Ui.Command {
        commandId: "edit.caption-generate"
        text: qsTr("Generate Captions")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.generateCaptions()
    }

    // Keyframes: a clip's whole animation (picture properties and opacity).
    Ui.Command {
        commandId: "edit.keyframes-cut"
        text: qsTr("Cut Keyframes")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.keyframesOnSelection()
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.cutKeyframes(e.track, e.index) }
    }
    Ui.Command {
        commandId: "edit.keyframes-copy"
        text: qsTr("Copy Keyframes")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && win.clipCommands.keyframesOnSelection()
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.copyKeyframes(e.track, e.index) }
    }
    Ui.Command {
        commandId: "edit.keyframes-paste"
        text: qsTr("Paste Keyframes")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.workspace === "Edit" && backend.hasCopiedKeyframes && win.selection.selectedEntry() !== null
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.pasteKeyframes(e.track, e.index, backend.position - e.start) }
    }

    Ui.Command {
        commandId: "edit.paste-attributes"
        text: qsTr("Paste Attributes\u2026")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+V"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.attributes.paste()
    }

    Ui.Command {
        commandId: "edit.duplicate"
        text: qsTr("Duplicate Clips")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+D"]
        available: win.workspace === "Edit" || win.workspace === "Audio"
        shortcutEnabled: win.keysFree
        onTriggered: win.copying.duplicateSelected()
    }

    Ui.Command {
        commandId: "edit.extend"
        text: qsTr("Extend Edit to Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Shift+X"]
        available: win.trimming.canTrimToPlayhead()
        shortcutEnabled: win.keysFree
        onTriggered: win.trimming.extendEditToPlayhead()
    }

    Ui.Command {
        commandId: "edit.trim-start"
        text: qsTr("Trim Start to Playhead")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+["]
        available: win.workspace === "Edit" && win.trimming.canTrimToPlayhead()
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.trimSelectedToPlayhead(false)
    }

    Ui.Command {
        commandId: "edit.trim-end"
        text: qsTr("Trim End to Playhead")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+]"]
        available: win.workspace === "Edit" && win.trimming.canTrimToPlayhead()
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.trimSelectedToPlayhead(true)
    }

    Ui.Command {
        commandId: "edit.roll-left"
        text: qsTr("Roll Edit Left 1 Frame")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+,"]
        available: win.workspace === "Edit" && win.trimming.canRollEdit(-1)
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.rollSelectedEdit(-1)
    }

    Ui.Command {
        commandId: "edit.roll-right"
        text: qsTr("Roll Edit Right 1 Frame")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+."]
        available: win.workspace === "Edit" && win.trimming.canRollEdit(1)
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.rollSelectedEdit(1)
    }

    Ui.Command {
        commandId: "edit.roll-left-ten"
        text: qsTr("Roll Edit Left 10 Frames")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Shift+,"]
        available: win.workspace === "Edit" && win.trimming.canRollEdit(-10)
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.rollSelectedEdit(-10)
    }

    Ui.Command {
        commandId: "edit.roll-right-ten"
        text: qsTr("Roll Edit Right 10 Frames")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Shift+."]
        available: win.workspace === "Edit" && win.trimming.canRollEdit(10)
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.rollSelectedEdit(10)
    }

    Ui.Command {
        commandId: "edit.roll-playhead"
        text: qsTr("Roll Edit to Playhead")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Shift+X"]
        available: win.workspace === "Edit" && win.trimming.canRollEdit()
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.rollSelectedEdit()
    }

    Ui.Command {
        commandId: "edit.nudge-left"
        text: qsTr("Nudge Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Left"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.nudgeSelected(-1)
    }

    Ui.Command {
        commandId: "edit.nudge-right"
        text: qsTr("Nudge Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Right"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.nudgeSelected(1)
    }

    Ui.Command {
        commandId: "edit.nudge-left-ten"
        text: qsTr("Nudge Left 10 Frames")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Shift+Left"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.nudgeSelected(-10)
    }

    Ui.Command {
        commandId: "edit.nudge-right-ten"
        text: qsTr("Nudge Right 10 Frames")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Alt+Shift+Right"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.trimming.nudgeSelected(10)
    }

    Ui.Command {
        commandId: "edit.replace"
        text: qsTr("Replace with Source Clip")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Shift+R"]
        available: win.selectedLibraryIndex >= 0 && (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)
        shortcutEnabled: win.keysFree
        onTriggered: win.matching.replaceSelected()
    }

    Ui.Command {
        commandId: "edit.replace-from-end"
        text: qsTr("Replace with Source Clip from Its End")
        category: qsTr("Edit")
        scope: "Global"
        available: win.selectedLibraryIndex >= 0 && (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)
        shortcutEnabled: win.keysFree
        onTriggered: win.matching.replaceSelected(true)
    }

    Ui.Command {
        commandId: "edit.dynamic-trim"
        text: qsTr("Toggle Dynamic Trim")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Shift+T"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.playing.toggleDynamicTrim()
    }

    Ui.Command {
        commandId: "transport.play-around"
        text: qsTr("Play Around Edit Point")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: ["/"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.playing.playAround()
    }

    Ui.Command {
        commandId: "edit.match-frame"
        text: qsTr("Match Frame")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["F"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.matching.matchFrame()
    }

    Ui.Command {
        commandId: "media.reveal-source"
        text: qsTr("Reveal Source Clip")
        category: qsTr("Media")
        scope: "Edit"
        defaultShortcuts: ["Shift+F"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.matching.revealSourceClip()
    }

    Ui.Command {
        commandId: "edit.fit-fill"
        text: qsTr("Fit to Fill")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Shift+F11"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.matching.fitToFill()
    }

    Ui.Command {
        commandId: "edit.compound"
        text: qsTr("New Nested Clip")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+G"]
        available: !backend.viewingSource && backend.hasVideo && (win.hasTimelineRange || win.selectionCount > 0)
        shortcutEnabled: win.keysFree
        onTriggered: win.clipCommands.compoundSelection()
    }

    Ui.Command {
        commandId: "edit.break-apart"
        text: qsTr("Ungroup Clip")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+Shift+G"]
        available: win.selectedTimelineIndex >= 0 && win.selectedTimelineIndex < backend.timeline.length && backend.timeline[win.selectedTimelineIndex].compound === true
        shortcutEnabled: win.keysFree
        onTriggered: win.clipCommands.breakApartSelection()
    }

    Ui.Command {
        commandId: "marker.edit"
        text: qsTr("Modify Marker\u2026")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: ["Shift+M"]
        available: win.clipMarkers.timelineMarkerHere() !== null || win.clipMarkers.clipMarkerHere() !== null
        shortcutEnabled: win.keysFree
        onTriggered: win.clipMarkers.modifyHere()
    }

    Ui.Command {
        commandId: "marker.previous"
        text: qsTr("Previous Marker")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: ["Ctrl+;"]
        available: win.snapping.markerFrames().length > 0
        shortcutEnabled: win.keysFree
        onTriggered: win.navigation.seekMarker(win.navigation.previousMarker())
    }

    Ui.Command {
        commandId: "marker.next"
        text: qsTr("Next Marker")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: ["Ctrl+'"]
        available: win.snapping.markerFrames().length > 0
        shortcutEnabled: win.keysFree
        onTriggered: win.navigation.seekMarker(win.navigation.nextMarker())
    }

    Ui.Command {
        commandId: "marker.previous-beat"
        text: qsTr("Previous Beat")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: []
        available: backend.beatMapRevision > 0 && backend.hasVideo
        shortcutEnabled: win.keysFree && available
        onTriggered: win.navigation.seekMarker(win.navigation.beatFrom(-1))
    }

    Ui.Command {
        commandId: "marker.next-beat"
        text: qsTr("Next Beat")
        category: qsTr("Markers")
        scope: "Global"
        defaultShortcuts: []
        available: backend.beatMapRevision > 0 && backend.hasVideo
        shortcutEnabled: win.keysFree && available
        onTriggered: win.navigation.seekMarker(win.navigation.beatFrom(1))
    }

    Ui.Command {
        commandId: "tool.trim"
        text: qsTr("Toggle Trim Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["T"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: backend.rippleTrim = !backend.rippleTrim
    }

    Ui.Command {
        commandId: "edit.title"
        text: qsTr("Add Title\u2026")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+T"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (backend.hasVideo)
                titleDialog.openFor(-1, "", "centre", "medium", "", "");

        }
    }

    Ui.Command {
        commandId: "edit.lower-third"
        text: qsTr("Add Lower Third\u2026")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+Shift+T"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (backend.hasVideo)
                titleDialog.openFor(-1, "", "lower", "medium", "", "");

        }
    }

    Ui.Command {
        commandId: "edit.connect-audio"
        text: qsTr("Place Audio on Top")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Shift+Q"]
        available: win.selectedLibraryIndex >= 0 && backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedLibraryIndex >= 0)
                backend.connectAudioClipAt(win.selectedLibraryIndex);

        }
    }

    Ui.Command {
        commandId: "edit.dissolve"
        text: qsTr("Toggle Cross Dissolve")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+T"]
        available: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedConnectedIndex >= 0) {
                // a track clip: into its next
                backend.toggleConnectedDissolve(win.selectedConnectedIndex);
                return ;
            }
            var target = win.trimming.dissolveTarget();
            if (target >= 0)
                backend.toggleDissolve(target);

        }
    }

    Ui.Command {
        commandId: "edit.connect-video"
        text: qsTr("Place Video on Top")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Q"]
        available: win.selectedLibraryIndex >= 0 && backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.selectedLibraryIndex >= 0)
                backend.connectClipAt(win.selectedLibraryIndex);

        }
    }

    Ui.Command {
        commandId: "edit.blade"
        text: qsTr("Blade at Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+B"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.bladeAtPlayhead()
    }

    Ui.Command {
        commandId: "edit.blade-all"
        text: qsTr("Blade All at Playhead")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+B"]
        available: backend.hasVideo && !backend.viewingSource && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: backend.bladeAllAtPlayhead()
    }
    // Tools (single-letter keys): A select, T trim, B blade, Y slip, H hand,

    // Z zoom; Escape returns to select.
    Ui.Command {
        commandId: "tool.select"
        text: qsTr("Select Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["A"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool("select")
    }

    // Selection: Ctrl+A everything on the timeline (the Media grid owns
    // Ctrl+A on its page), Ctrl+Shift+A none, Ctrl+Shift+L linked selection.
    Ui.Command {
        commandId: "edit.select-all"
        text: qsTr("Select All Clips")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+A"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.selection.selectAll()
    }

    Ui.Command {
        commandId: "edit.deselect-all"
        text: qsTr("Deselect All Clips")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Shift+A"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.selection.clearSelection()
    }

    Ui.Command {
        commandId: "edit.linked-selection"
        text: qsTr("Toggle Linked Selection")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+L"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: {
            win.linkedSelection = !win.linkedSelection;
            backend.announce(win.linkedSelection ? qsTr("Linked selection on") : qsTr("Linked selection off"));
        }
    }

    Ui.Command {
        commandId: "tool.blade"
        text: qsTr("Toggle Blade Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["B"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool(win.tool === "blade" ? "select" : "blade")
    }

    Ui.Command {
        commandId: "tool.trackselect"
        text: qsTr("Toggle Track Select Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool(win.tool === "trackselect" ? "select" : "trackselect")
    }

    Ui.Command {
        commandId: "tool.slip"
        text: qsTr("Toggle Slip Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["Y"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool(win.tool === "slip" ? "select" : "slip")
    }

    Ui.Command {
        commandId: "tool.slide"
        text: qsTr("Slide Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["U"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool("slide")
    }

    Ui.Command {
        commandId: "edit.join"
        text: qsTr("Join Clips")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Ctrl+J"]
        available: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
        shortcutEnabled: win.keysFree
        onTriggered: win.trimming.joinSelected()
    }

    Ui.Command {
        commandId: "tool.hand"
        text: qsTr("Toggle Hand Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["H"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool(win.tool === "hand" ? "select" : "hand")
    }

    Ui.Command {
        commandId: "tool.zoom"
        text: qsTr("Toggle Zoom Tool")
        category: qsTr("Tools")
        scope: "Global"
        defaultShortcuts: ["Z"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.setTool(win.tool === "zoom" ? "select" : "zoom")
    }

    Ui.Command {
        commandId: "edit.swap-left"
        text: qsTr("Swap Clip Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Shift+,"]
        available: win.workspace === "Edit" && win.selection.selectedEntry() !== null
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.selection.swapSelected(true)
    }

    Ui.Command {
        commandId: "edit.swap-right"
        text: qsTr("Swap Clip Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Shift+."]
        available: win.workspace === "Edit" && win.selection.selectedEntry() !== null
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.selection.swapSelected(false)
    }

    Ui.Command {
        commandId: "edit.delete-gaps"
        text: qsTr("Delete Gaps")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: backend.deleteGaps()
    }

    Ui.Command {
        commandId: "edit.gap"
        text: qsTr("Insert Gap")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+W"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.insertGapAtPlayhead()
    }

    Ui.Command {
        commandId: "edit.toggle-enabled"
        text: qsTr("Toggle Clip Enabled")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["V"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.clipCommands.toggleDisabledSelected()
    }

    Ui.Command {
        commandId: "range.in"
        text: qsTr("Set Range Start")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["I"]
        available: backend.previewLibraryIndex >= 0 || backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: win.ranges.setStart()
    }

    Ui.Command {
        commandId: "range.out"
        text: qsTr("Set Range End")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["O"]
        available: backend.previewLibraryIndex >= 0 || backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: win.ranges.setEnd()
    }

    Ui.Command {
        commandId: "range.clip"
        text: qsTr("Select Clip Range")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["X"]
        available: backend.hasVideo && !backend.viewingSource
        shortcutEnabled: win.keysFree
        onTriggered: win.ranges.selectClip()
    }

    Ui.Command {
        commandId: "range.clear"
        text: qsTr("Clear Range")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Alt+X"]
        available: backend.previewLibraryIndex >= 0 || backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: win.ranges.clearAny()
    }

    Ui.Command {
        commandId: "edit.nudge-edge-left"
        text: qsTr("Nudge Edge Left")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: [","]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.trimming.nudgeEdge(-1)
    }

    Ui.Command {
        commandId: "edit.nudge-edge-right"
        text: qsTr("Nudge Edge Right")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["."]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.trimming.nudgeEdge(1)
    }

    Ui.Command {
        commandId: "edit.ripple-delete"
        text: qsTr("Ripple Delete")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Delete", "Shift+Backspace"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.ranges.deleteSelectedOrRange()
    }

    Ui.Command {
        commandId: "edit.lift"
        text: qsTr("Delete and Leave Gap")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Backspace"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.clipCommands.liftSelected()
    }

    Ui.Command {
        commandId: "transport.previous-frame"
        text: qsTr("Previous Frame")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Left"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.layoutTransport("step", -1)
    }

    Ui.Command {
        commandId: "transport.next-frame"
        text: qsTr("Next Frame")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Right"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.layoutTransport("step", 1)
    }

    Ui.Command {
        commandId: "transport.previous-edit"
        text: qsTr("Previous Edit Point")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: ["Up"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: {
            if (win.precisionBoundary >= 0) {
                // walk edit points in place
                win.stepPrecision(-1);
                return ;
            }
            backend.viewTimeline(); // edit points are a timeline concept
            backend.pause();
            backend.seek(win.navigation.previousEditPoint());
        }
    }

    Ui.Command {
        commandId: "transport.next-edit"
        text: qsTr("Next Edit Point")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: ["Down"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: {
            if (win.precisionBoundary >= 0) {
                win.stepPrecision(1);
                return ;
            }
            backend.viewTimeline();
            backend.pause();
            backend.seek(win.navigation.nextEditPoint());
        }
    }

    Ui.Command {
        commandId: "view.skimming"
        text: qsTr("Toggle Hover Preview")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["S"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.skimEnabled = !win.skimEnabled
    }

    Ui.Command {
        commandId: "view.snapping"
        text: qsTr("Toggle Snapping")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["N"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.snapEnabled = !win.snapEnabled
    }

    Ui.Command {
        commandId: "view.rectified-waveforms"
        text: qsTr("Waveforms From the Base")
        category: qsTr("View")
        scope: "Global"
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.rectifiedWaveforms = !win.rectifiedWaveforms
    }

    Ui.Command {
        commandId: "view.beat-map"
        text: qsTr("Show Beat Map")
        category: qsTr("View")
        scope: "Global"
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.showBeatMap = !win.showBeatMap
    }

    Ui.Command {
        commandId: "transport.start"
        text: qsTr("Go to Start")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Home"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.layoutTransport("start", 0)
    }

    Ui.Command {
        commandId: "transport.go-to-timecode"
        text: qsTr("Go to Timecode…")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: ["="]
        available: win.workspace === "Edit" && (backend.hasVideo || backend.viewingSource)
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: classicLayout.enterTimecode("")
    }

    // The number pad types straight into the viewer's timecode: a digit, or
    // + or - for an amount, opens the entry with that key in it. While the
    // multicam viewer is up and the digits cut between its angles the number
    // pad's digits do that too, as they always have, and = or the pad's + and
    // - open the entry. With the viewer closed they type here, as anywhere.
    Repeater {
        model: ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "+", "-"]

        Item {
            required property string modelData

            Shortcut {
                // Bound only while it is this key's turn: a number-pad
                // binding that is merely switched off would still keep the
                // key from the digit commands it otherwise reaches.
                sequences: win.keysFree && win.workspace === "Edit" && (backend.hasVideo || backend.viewingSource)
                           && !(handlers.multicamKeys && backend.multicamMode && modelData >= "1" && modelData <= "9") ? ["Num+" + modelData] : []
                context: Qt.ApplicationShortcut
                onActivated: classicLayout.enterTimecode(modelData)
            }
        }
    }

    Ui.Command {
        commandId: "transport.end"
        text: qsTr("Go to End")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["End"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.layoutTransport("end", 0)
    }

    Ui.Command {
        commandId: "deliver.export"
        text: qsTr("Export Movie\u2026")
        category: qsTr("Export")
        scope: "Global"
        defaultShortcuts: ["Ctrl+E"]
        available: backend.hasVideo && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportDialog()
    }

    Ui.Command {
        commandId: "workspace.deliver"
        text: qsTr("Export Workspace")
        category: qsTr("Workspaces")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+E"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace = "Deliver"
    }

    Ui.Command {
        commandId: "workspace.audio"
        text: qsTr("Audio Workspace")
        category: qsTr("Workspaces")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+U"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace = "Audio"
    }

    Ui.Command {
        commandId: "workspace.color"
        text: qsTr("Colour Workspace")
        category: qsTr("Workspaces")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+C"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace = "Color"
    }

    Ui.Command {
        commandId: "edit.undo"
        text: backend.undoText ? qsTr("Undo %1").arg(backend.undoText) : qsTr("Undo")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: [StandardKey.Undo]
        available: backend.canUndo && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: {
            win.selection.clearSelection();
            backend.undo();
        }
    }

    Ui.Command {
        commandId: "edit.redo"
        text: backend.redoText ? qsTr("Redo %1").arg(backend.redoText) : qsTr("Redo")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: [StandardKey.Redo, "Ctrl+Y"]
        available: backend.canRedo && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: {
            win.selection.clearSelection();
            backend.redo();
        }
    }

    Ui.Command {
        commandId: "view.zoom-in"
        text: qsTr("Zoom Timeline In")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+=", "Ctrl++"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: win.navigation.zoomIn()
    }

    Ui.Command {
        commandId: "edit.freeze"
        text: qsTr("Freeze Frame")
        category: qsTr("Edit")
        scope: "Global"
        defaultShortcuts: ["Alt+F"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && backend.hasVideo
        onTriggered: backend.freezeFrame()
    }
    // Multicam: the viewer, and 1–9 to cut / Shift+1–9 to switch angles
    // while it is up, on the Edit page (where the viewer is). With it closed
    // and the playhead on a multicam clip a digit says how to show it; plain
    // digits stay free otherwise. The viewer opens on an empty timeline too
    // (it says there what it is for).
    readonly property bool multicamKeys: win.workspace === "Edit" && (backend.multicamMode || backend.multicamClip.has === true)
    function angleKey(angle, cut) {
        if (backend.multicamMode) {
            backend.cutToAngle(angle, cut)
            return
        }
        const key = Ui.Commands.hint("multicam.viewer")
        backend.explain(key !== "" ? qsTr("Show the multicam viewer (%1) to change angles with the number keys.").arg(key)
                                   : qsTr("Show the multicam viewer (Clip ▸ Show Multicam Viewer) to change angles with the number keys."))
    }

    Ui.Command {
        commandId: "multicam.viewer"
        text: qsTr("Toggle Multicam Viewer")
        category: qsTr("Multicam")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+M"]
        shortcutEnabled: win.keysFree
        onTriggered: backend.multicamMode = !backend.multicamMode
    }

    Repeater {
        model: 9

        Item {
            required property int index

            Ui.Command {
                commandId: "multicam.cut" + "." + String(index + 1)
                text: qsTr("Cut to Angle %1").arg(index + 1)
                category: qsTr("Multicam")
                scope: "Global"
                defaultShortcuts: [String(index + 1)]
                available: handlers.multicamKeys
                shortcutEnabled: win.keysFree && handlers.multicamKeys
                onTriggered: handlers.angleKey(index, true)
            }

            Ui.Command {
                commandId: "multicam.switch" + "." + String(index + 1)
                text: qsTr("Switch to Angle %1").arg(index + 1)
                category: qsTr("Multicam")
                scope: "Global"
                defaultShortcuts: ["Shift+" + String(index + 1)]
                available: handlers.multicamKeys
                shortcutEnabled: win.keysFree && handlers.multicamKeys
                onTriggered: handlers.angleKey(index, false)
            }

        }

    }

    Ui.Command {
        commandId: "view.tracks-taller"
        text: qsTr("Taller Tracks")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+=", "Ctrl+Shift++"]
        available: win.hLaneRow < win.hLaneRowMax
        shortcutEnabled: win.keysFree
        onTriggered: win.lanes.tallerTracks()
    }

    Ui.Command {
        commandId: "view.tracks-shorter"
        text: qsTr("Shorter Tracks")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+-", "Ctrl+Shift+_"]
        available: win.hLaneRow > win.hLaneRowMin
        shortcutEnabled: win.keysFree
        onTriggered: win.lanes.shorterTracks()
    }

    Ui.Command {
        commandId: "view.zoom-out"
        text: qsTr("Zoom Timeline Out")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+-"]
        available: win.pxPerFrame > 0
        shortcutEnabled: win.keysFree
        onTriggered: win.navigation.zoomOut()
    }

    Ui.Command {
        commandId: "view.zoom-fit"
        text: qsTr("Fit Timeline")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Shift+Z"]
        available: win.pxPerFrame > 0
        shortcutEnabled: win.keysFree
        onTriggered: win.pxPerFrame = 0
    }

    Ui.Command {
        commandId: "view.dual-viewer"
        text: qsTr("Toggle Source Viewer")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+3"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.dualViewer = !win.dualViewer
    }

    Ui.Command {
        commandId: "app.shortcuts"
        text: qsTr("Keyboard Shortcuts\u2026")
        category: qsTr("Application")
        scope: "Global"
        defaultShortcuts: ["?"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: shortcutEditor.openFor()
    }

    Ui.Command {
        commandId: "app.diagnostics"
        text: qsTr("About OmaEdit / Diagnostics…")
        category: qsTr("Application")
        scope: "Global"
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: diagnosticsDialog.openFor()
    }

    Ui.Command {
        commandId: "app.quit"
        text: qsTr("Quit")
        category: qsTr("Application")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Q"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.close()
    }

    Ui.Command {
        commandId: "view.tasks"
        text: qsTr("Background Tasks…")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Alt+T"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: backgroundTasksDialog.openFor(false)
    }

    Ui.Command {
        commandId: "view.layout"
        text: qsTr("Workspace Layout\u2026")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Alt+W"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: workspaceLayoutDialog.open()
    }

    Ui.Command {
        commandId: "view.fullscreen"
        text: qsTr("Full Screen Application")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["F11"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.toggleApplicationFullscreen()
    }

    Ui.Command {
        commandId: "view.viewer-fullscreen"
        text: qsTr("Full Screen Viewer")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: ["Ctrl+F11"]
        available: !!win.activeLayout
        shortcutEnabled: win.keysFree
        onTriggered: win.activeLayout.toggleViewerFullscreen()
    }

    Ui.Command {
        commandId: "app.palette"
        text: qsTr("Command Palette\u2026")
        category: qsTr("Application")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+P"]
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: commandPalette.openFor()
    }

    Ui.Command {
        commandId: "workspace.media"
        text: qsTr("Media Workspace")
        category: qsTr("Workspaces")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace = "Media"
    }

    Ui.Command {
        commandId: "workspace.edit"
        text: qsTr("Edit Workspace")
        category: qsTr("Workspaces")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: win.workspace = "Edit"
    }

    Ui.Command {
        commandId: "project.recovery-and-backups"
        text: qsTr("Recovery and Backups\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: projectSafetyDialog.openFor(0)
    }

    Ui.Command {
        commandId: "project.archive-project"
        text: qsTr("Archive Project\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: projectSafetyDialog.openFor(2)
    }

    Ui.Command {
        commandId: "project.import-timeline"
        text: qsTr("Import Timeline\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: backend.importTimelineDialog()
    }
    Repeater {
        model: ["otio", "edl", "fcpxml", "xmeml", "aaf"]
        Item {
            id: exportFormat
            required property string modelData
            Ui.Command {
                commandId: "project.export-timeline-" + exportFormat.modelData
                text: qsTr("Export Timeline as %1\u2026").arg(backend.timelineFormatName(exportFormat.modelData))
                category: qsTr("Project")
                scope: "Global"
                defaultShortcuts: []
                available: backend.hasVideo && !backend.exporting && backend.timelineFormatAvailable(exportFormat.modelData)
                shortcutEnabled: win.keysFree
                onTriggered: backend.exportTimelineDialog(exportFormat.modelData)
            }
        }
    }

    Ui.Command {
        commandId: "project.import-captions"
        text: qsTr("Import Captions\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.importCaptionsDialog()
    }

    Ui.Command {
        commandId: "project.export-captions"
        text: qsTr("Export Captions\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportCaptionsDialog()
    }

    Ui.Command {
        commandId: "project.h-264-high-quality"
        text: qsTr("H.264 High Quality\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: (backend.hasVideo && !backend.exporting) && (true)
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportWithPreset("h264-high")
    }

    Ui.Command {
        commandId: "project.h-264-smaller-file"
        text: qsTr("H.264 Smaller File\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: (backend.hasVideo && !backend.exporting) && (true)
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportWithPreset("h264-medium")
    }

    Ui.Command {
        commandId: "project.prores-422-master"
        text: qsTr("ProRes 422 Master\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: (backend.hasVideo && !backend.exporting) && (true)
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportWithPreset("prores")
    }

    Ui.Command {
        commandId: "project.audio-only"
        text: qsTr("Audio Only\u2026")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: []
        available: (backend.hasVideo && !backend.exporting) && (true)
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportWithPreset("audio")
    }

    Ui.Command {
        commandId: "edit.cross-dissolve"
        text: qsTr("Cross Dissolve")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut > 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setDissolveStyle(win.trimming.dissolveTarget(), "")
    }

    Ui.Command {
        commandId: "edit.wipe-right"
        text: qsTr("Wipe Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut > 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setDissolveStyle(win.trimming.dissolveTarget(), "wipe-right")
    }

    Ui.Command {
        commandId: "edit.wipe-left"
        text: qsTr("Wipe Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut > 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setDissolveStyle(win.trimming.dissolveTarget(), "wipe-left")
    }

    Ui.Command {
        commandId: "edit.wipe-down"
        text: qsTr("Wipe Down")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut > 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setDissolveStyle(win.trimming.dissolveTarget(), "wipe-down")
    }

    Ui.Command {
        commandId: "edit.iris"
        text: qsTr("Iris")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut > 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setDissolveStyle(win.trimming.dissolveTarget(), "iris")
    }

    Ui.Command {
        commandId: "modify.change-duration"
        text: qsTr("Change Duration…")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: [] // Ctrl+D is Duplicate here; bind one in the shortcut editor
        available: win.workspace === "Edit" && win.sound.targetExists
        shortcutEnabled: win.keysFree && win.sound.targetExists
        onTriggered: changeDurationDialog.openForSelection()
    }

    Ui.Command {
        commandId: "edit.speed-duration"
        text: qsTr("Speed / Duration…")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: speedDurationDialog.openForSelection()
    }
    // Modify ▸ Retime one-click edits through the speed-segment engine;
    // hold and blade act at the playhead.
    Ui.Command {
        commandId: "modify.retime-hold"
        text: qsTr("Hold")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: ["Shift+H"]
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("hold", 1)
    }
    Ui.Command {
        commandId: "modify.retime-blade"
        text: qsTr("Blade Speed")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: ["Shift+B"]
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("blade", 1)
    }
    Ui.Command {
        commandId: "modify.retime-replay"
        text: qsTr("Instant Replay")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("replay", 1)
    }
    Ui.Command {
        commandId: "modify.retime-rewind-1"
        text: qsTr("Rewind ×1")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("rewind", 1)
    }
    Ui.Command {
        commandId: "modify.retime-rewind-2"
        text: qsTr("Rewind ×2")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("rewind", 2)
    }
    Ui.Command {
        commandId: "modify.retime-rewind-4"
        text: qsTr("Rewind ×4")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.quickRetime("rewind", 4)
    }
    Ui.Command {
        commandId: "edit.speed-segments"
        text: qsTr("Speed Segments…")
        category: qsTr("Edit")
        scope: "Edit"
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree
        onTriggered: speedRampDialog.openForSelection()
    }

    Ui.Command {
        commandId: "edit.slow-50"
        text: qsTr("Slow 50%")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.setSelectedSpeed(0.5)
    }

    Ui.Command {
        commandId: "edit.slow-25"
        text: qsTr("Slow 25%")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.setSelectedSpeed(0.25)
    }

    Ui.Command {
        commandId: "edit.fast-2"
        text: qsTr("Fast 2\u00d7")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.setSelectedSpeed(2)
    }

    Ui.Command {
        commandId: "edit.fast-4"
        text: qsTr("Fast 4\u00d7")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.setSelectedSpeed(4)
    }

    Ui.Command {
        commandId: "edit.normal-100"
        text: qsTr("Normal 100%")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0) && (win.retiming.selectedSpeed() !== 1))
        shortcutEnabled: win.keysFree
        onTriggered: win.retiming.setSelectedSpeed(1)
    }

    Ui.Command {
        commandId: "edit.full-frame"
        text: qsTr("Full Frame")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0, 0, 1, 1)
    }

    Ui.Command {
        commandId: "edit.top-left"
        text: qsTr("Top Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0.03, 0.03, 0.42, 0.42)
    }

    Ui.Command {
        commandId: "edit.top-right"
        text: qsTr("Top Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0.55, 0.03, 0.42, 0.42)
    }

    Ui.Command {
        commandId: "edit.bottom-left"
        text: qsTr("Bottom Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0.03, 0.55, 0.42, 0.42)
    }

    Ui.Command {
        commandId: "edit.bottom-right"
        text: qsTr("Bottom Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0.55, 0.55, 0.42, 0.42)
    }

    Ui.Command {
        commandId: "edit.centre-small"
        text: qsTr("Centre Small")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.selectedConnectedIndex >= 0) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: backend.setConnectedRect(win.selectedConnectedIndex, 0.29, 0.29, 0.42, 0.42)
    }

    Ui.Command {
        commandId: "edit.new-multicam-clip"
        text: qsTr("New Multicam Clip\u2026")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (backend.library.length >= 2)
        shortcutEnabled: win.keysFree
        onTriggered: win.openMulticamDialog(win.selectedLibraryIndex >= 0 ? [win.selectedLibraryIndex] : [])
    }

    Ui.Command {
        commandId: "edit.multicam-audio-follows-video"
        text: qsTr("Multicam Audio Follows Video")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (true)
        shortcutEnabled: win.keysFree
        onTriggered: win.multicamAudioFollows = !win.multicamAudioFollows
    }

    Ui.Command {
        commandId: "edit.silence-clip"
        text: qsTr("Silence Clip")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (win.sound.targetExists)
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setVolume(-60)
    }

    Ui.Command {
        commandId: "edit.reset-volume"
        text: qsTr("Reset Volume")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (win.sound.targetExists && win.sound.volume() !== 0)
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setVolume(0)
    }

    Ui.Command {
        commandId: "edit.full-left"
        text: qsTr("Full Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setPan(-1)
    }

    Ui.Command {
        commandId: "edit.half-left"
        text: qsTr("Half Left")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setPan(-0.5)
    }

    Ui.Command {
        commandId: "edit.centre"
        text: qsTr("Centre")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (win.sound.targetExists && win.sound.pan() !== 0))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setPan(0)
    }

    Ui.Command {
        commandId: "edit.half-right"
        text: qsTr("Half Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setPan(0.5)
    }

    Ui.Command {
        commandId: "edit.full-right"
        text: qsTr("Full Right")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setPan(1)
    }

    Ui.Command {
        commandId: "edit.flat-off"
        text: qsTr("Flat (off)")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (win.sound.eq() !== ""))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("")
    }

    Ui.Command {
        commandId: "edit.voice-enhance"
        text: qsTr("Voice Enhance")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("voice")
    }

    Ui.Command {
        commandId: "edit.bass-boost"
        text: qsTr("Bass Boost")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("bass-boost")
    }

    Ui.Command {
        commandId: "edit.bass-reduce"
        text: qsTr("Bass Reduce")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("bass-reduce")
    }

    Ui.Command {
        commandId: "edit.treble-boost"
        text: qsTr("Treble Boost")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("treble-boost")
    }

    Ui.Command {
        commandId: "edit.treble-reduce"
        text: qsTr("Treble Reduce")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("treble-reduce")
    }

    Ui.Command {
        commandId: "edit.hum-reduction"
        text: qsTr("Hum Reduction")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && ((win.sound.targetExists) && (true))
        shortcutEnabled: win.keysFree
        onTriggered: win.sound.setEq("hum")
    }

    Ui.Command {
        commandId: "edit.select-clips-forward-on-track"
        text: qsTr("Select Clips Forward on Track")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0)
        shortcutEnabled: win.keysFree
        onTriggered: {
            var e = win.selection.selectedEntry();
            if (e !== null)
                win.selection.selectTrackForward(e.track, e.index, false, false);

        }
    }

    Ui.Command {
        commandId: "edit.toggle-precision-editor"
        text: qsTr("Toggle Trim Editor")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && (win.precisionBoundary >= 0 || (win.trimming.dissolveTarget() >= 0 && win.trimming.dissolveTarget() + 1 < backend.timeline.length && backend.timeline[win.trimming.dissolveTarget()].mixOut === 0))
        shortcutEnabled: win.keysFree
        onTriggered: win.precisionBoundary >= 0 ? win.precisionBoundary = -1 : win.openPrecision(win.trimming.dissolveTarget())
    }

    Ui.Command {
        commandId: "view.restore-all-panels"
        text: qsTr("Restore All Panels")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.activeLayout) {
                win.activeLayout.restorePanels();
            }
        }
    }

    Ui.Command {
        commandId: "view.reset-workspace"
        text: qsTr("Reset Workspace")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: {
            if (win.activeLayout) {
                win.activeLayout.reset();
            }
        }
    }

    Ui.Command {
        commandId: "view.reset-track-height"
        text: qsTr("Reset Track Height")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: []
        available: win.hLaneRow !== Ui.Theme.hLaneRow || win.trackSizedAlone
        shortcutEnabled: win.keysFree
        onTriggered: win.lanes.setTrackHeight(Ui.Theme.hLaneRow)
    }

    Ui.Command {
        commandId: "color.copy-node"
        text: qsTr("Copy Colour Layer")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: [StandardKey.Copy]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.hasClip
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.copyColor("node")
    }

    Ui.Command {
        commandId: "color.paste-node"
        text: qsTr("Paste Colour Layer")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: [StandardKey.Paste]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.versionsEditable && colorLoader.item.hasCopiedNode
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.pasteColor("node")
    }

    Ui.Command {
        commandId: "color.copy-grade"
        text: qsTr("Copy Grade")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: ["Ctrl+Alt+C"]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.hasClip
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.copyColor("grade")
    }

    Ui.Command {
        commandId: "color.paste-grade"
        text: qsTr("Paste Grade")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: ["Ctrl+Alt+P"]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.versionsEditable && colorLoader.item.hasCopiedGrade
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.pasteColor("grade")
    }

    Ui.Command {
        commandId: "color.new-version"
        text: qsTr("New Grade Version")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: ["Ctrl+Alt+V"]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.versionsEditable && !colorLoader.item.lean
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.newGradeVersion()
    }

    Ui.Command {
        commandId: "color.previous-version"
        text: qsTr("Previous Grade Version")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: ["Alt+["]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.versionsEditable && !colorLoader.item.lean
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.stepGradeVersion(-1)
    }

    Ui.Command {
        commandId: "color.next-version"
        text: qsTr("Next Grade Version")
        category: qsTr("Colour")
        scope: "Color"
        defaultShortcuts: ["Alt+]"]
        available: win.workspace === "Color" && !!colorLoader.item && colorLoader.item.versionsEditable && !colorLoader.item.lean
        shortcutEnabled: win.keysFree
        onTriggered: colorLoader.item.stepGradeVersion(1)
    }

    Ui.Command {
        commandId: "media.select-all"
        text: qsTr("Select All Media")
        category: qsTr("Media")
        scope: "Media"
        defaultShortcuts: ["Ctrl+A"]
        available: win.workspace === "Media" && !!mediaLoader.item && mediaLoader.item.filtered.length > 0
        shortcutEnabled: win.keysFree
        onTriggered: mediaLoader.item.selectAll()
    }


    Ui.Command {
        commandId: "project.close"
        text: qsTr("Close Project")
        category: qsTr("Project")
        scope: "Global"
        defaultShortcuts: ["Ctrl+Shift+W"]
        available: !backend.projectBusy && !backend.exporting
        shortcutEnabled: win.keysFree
        onTriggered: win.project.close()
    }
    Ui.Command {
        commandId: "view.playback"
        text: qsTr("Viewer and Playback…")
        category: qsTr("View")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree
        onTriggered: viewerPlaybackDialog.open()
    }
    Ui.Command {
        commandId: "transport.loop"
        text: qsTr("Loop In/Out")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: ["Ctrl+L"]
        available: backend.hasVideo || backend.sourceDuration > 0
        shortcutEnabled: win.keysFree
        onTriggered: backend.loop = !backend.loop
    }
    Ui.Command {
        commandId: "view.export-frame"
        text: qsTr("Export Current Frame…")
        category: qsTr("Playback")
        scope: "Global"
        defaultShortcuts: []
        available: backend.hasVideo || backend.sourceDuration > 0
        shortcutEnabled: win.keysFree
        onTriggered: backend.exportFrameDialog()
    }

    Ui.Command {
        commandId: "edit.cut"
        text: qsTr("Cut")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+X"]
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.copying.cutSelected()
    }

    Ui.Command {
        commandId: "edit.copy-timecode"
        text: qsTr("Copy Timecode")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.copyPlayheadTimecode()
    }

    Ui.Command {
        commandId: "edit.select-at-playhead"
        text: qsTr("Select Clip at Playhead")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && backend.hasVideo)
        onTriggered: win.selection.selectClipAtPlayhead()
    }

    Ui.Command {
        commandId: "edit.select-next"
        text: qsTr("Select Next Clip")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && backend.hasVideo)
        onTriggered: win.selection.selectNeighbourClip(1)
    }

    Ui.Command {
        commandId: "edit.select-previous"
        text: qsTr("Select Previous Clip")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && backend.hasVideo)
        onTriggered: win.selection.selectNeighbourClip(-1)
    }

    Ui.Command {
        commandId: "edit.select-above"
        text: qsTr("Select Clip Above")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.selection.selectClipOnNextTrack(1)
    }

    Ui.Command {
        commandId: "edit.select-below"
        text: qsTr("Select Clip Below")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.selection.selectClipOnNextTrack(-1)
    }

    Ui.Command {
        commandId: "edit.remove-effects"
        text: qsTr("Remove Effects")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selectionCount > 0
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selectionCount > 0)
        onTriggered: win.clipCommands.removeSelectedEffects()
    }

    Ui.Command {
        commandId: "edit.remove-attributes"
        text: qsTr("Remove Attributes\u2026")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selectionCount > 0
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selectionCount > 0)
        onTriggered: win.clipCommands.removeAttributes()
    }

    Ui.Command {
        commandId: "edit.generator-solid"
        text: qsTr("Insert Solid Colour")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: backend.addGenerator("solid")
    }

    Ui.Command {
        commandId: "edit.generator-bars"
        text: qsTr("Insert Colour Bars")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: backend.addGenerator("bars")
    }

    Ui.Command {
        commandId: "edit.generator-noise"
        text: qsTr("Insert Noise")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: backend.addGenerator("noise")
    }

    Ui.Command {
        commandId: "edit.generator-count"
        text: qsTr("Insert Counting Leader")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: backend.addGenerator("count")
    }

    Ui.Command {
        commandId: "edit.generator-adjust"
        text: qsTr("Insert Effect Layer")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && backend.hasVideo
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && backend.hasVideo)
        onTriggered: backend.addGenerator("adjust")
    }

    Ui.Command {
        commandId: "edit.find"
        text: qsTr("Find in Timeline\u2026")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+F"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: win.findInTimeline()
    }

    Ui.Command {
        commandId: "edit.trim-to-range"
        text: qsTr("Trim to Range")
        category: qsTr("Edit")
        scope: "Edit"
        defaultShortcuts: []
        available: win.ranges.canTrimTo()
        shortcutEnabled: win.keysFree && (win.ranges.canTrimTo())
        onTriggered: win.ranges.trimSelectedTo()
    }

    Ui.Command {
        commandId: "mark.favorite"
        text: qsTr("Favourite")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: win.clipCommands.ratingTarget() >= 0
        shortcutEnabled: win.keysFree && (win.clipCommands.ratingTarget() >= 0)
        onTriggered: backend.setClipFavorite(win.clipCommands.ratingTarget(), true)
    }

    Ui.Command {
        commandId: "mark.reject"
        text: qsTr("Reject")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: win.clipCommands.ratingTarget() >= 0
        shortcutEnabled: win.keysFree && (win.clipCommands.ratingTarget() >= 0)
        onTriggered: backend.setClipRejected(win.clipCommands.ratingTarget(), true)
    }

    Ui.Command {
        commandId: "mark.unrate"
        text: qsTr("Unrate")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: win.clipCommands.ratingTarget() >= 0
        shortcutEnabled: win.keysFree && (win.clipCommands.ratingTarget() >= 0)
        onTriggered: win.clipCommands.unrateTarget()
    }

    Ui.Command {
        commandId: "mark.remove-keywords"
        text: qsTr("Remove All Keywords")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: win.clipCommands.ratingTarget() >= 0
        shortcutEnabled: win.keysFree && (win.clipCommands.ratingTarget() >= 0)
        onTriggered: backend.setClipKeywords(win.clipCommands.ratingTarget(), [])
    }

    // M marks the selected clip when the playhead is on it; this one always
    // marks the timeline, whatever is selected.
    Ui.Command {
        commandId: "marker.toggle-timeline"
        text: qsTr("Add / Remove Timeline Marker")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: ["Alt+Shift+M"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: { backend.viewTimeline(); backend.toggleMarker() }
    }

    Ui.Command {
        commandId: "marker.add-modify"
        text: qsTr("Add Marker and Edit\u2026")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: ["Alt+M"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.addAndModify()
    }

    Ui.Command {
        commandId: "marker.add-todo"
        text: qsTr("Add To-Do Marker")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.addTyped("todo")
    }

    Ui.Command {
        commandId: "marker.add-chapter"
        text: qsTr("Add Chapter Marker")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.addTyped("chapter")
    }

    Ui.Command {
        commandId: "marker.delete"
        text: qsTr("Delete Marker")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+M"]
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.deleteHere()
    }

    Ui.Command {
        commandId: "marker.delete-range"
        text: qsTr("Delete Markers in Range")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: win.timelineRangeIn >= 0 && win.timelineRangeOut >= win.timelineRangeIn
        shortcutEnabled: win.keysFree && (win.timelineRangeIn >= 0 && win.timelineRangeOut >= win.timelineRangeIn)
        onTriggered: win.clipMarkers.deleteInRange()
    }

    Ui.Command {
        commandId: "marker.nudge-left"
        text: qsTr("Nudge Marker Left")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.nudgeHere(-1)
    }

    Ui.Command {
        commandId: "marker.nudge-right"
        text: qsTr("Nudge Marker Right")
        category: qsTr("Mark")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.clipMarkers.nudgeHere(1)
    }

    Ui.Command {
        commandId: "transport.range-start"
        text: qsTr("Go to Range Start")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: []
        available: win.timelineRangeIn >= 0
        shortcutEnabled: win.keysFree && (win.timelineRangeIn >= 0)
        onTriggered: win.navigation.seekMarker(win.timelineRangeIn)
    }

    Ui.Command {
        commandId: "transport.range-end"
        text: qsTr("Go to Range End")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: []
        available: win.timelineRangeOut >= 0
        shortcutEnabled: win.keysFree && (win.timelineRangeOut >= 0)
        onTriggered: win.navigation.seekMarker(win.timelineRangeOut)
    }

    Ui.Command {
        commandId: "transport.play-selection"
        text: qsTr("Play Selection")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: []
        available: win.playing.playSelectionSpan() !== null
        shortcutEnabled: win.keysFree && (win.playing.playSelectionSpan() !== null)
        onTriggered: win.playing.playSelection()
    }

    Ui.Command {
        commandId: "transport.play-from-start"
        text: qsTr("Play from Beginning")
        category: qsTr("Playback")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: win.playing.playFromBeginning()
    }

    Ui.Command {
        commandId: "clip.open"
        text: qsTr("Open Clip")
        category: qsTr("Clip")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null && win.selection.selectedClip().nestId !== undefined
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null && win.selection.selectedClip().nestId !== undefined)
        onTriggered: win.clipCommands.openNestedSequence(win.selection.selectedClip())
    }

    Ui.Command {
        commandId: "clip.reattach-audio"
        text: qsTr("Reattach Audio")
        category: qsTr("Clip")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.clipCommands.reattachSelectedAudio()
    }

    Ui.Command {
        commandId: "clip.link"
        text: qsTr("Link Picture and Sound")
        category: qsTr("Clip")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.clipCommands.linkTargets().length > 1
        shortcutEnabled: win.keysFree && available
        onTriggered: backend.linkClips(win.clipCommands.linkTargets())
    }

    Ui.Command {
        commandId: "clip.unlink"
        text: qsTr("Unlink Picture and Sound")
        category: qsTr("Clip")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.clipCommands.unlinkTargets().length > 0
        shortcutEnabled: win.keysFree && available
        onTriggered: backend.unlinkClips(win.clipCommands.unlinkTargets())
    }

    Ui.Command {
        commandId: "clip.map-beats"
        text: qsTr("Map Beats")
        category: qsTr("Clip")
        scope: "Global"
        defaultShortcuts: []
        available: win.clipCommands.canMapBeats()
        shortcutEnabled: win.keysFree && available
        onTriggered: win.clipCommands.mapBeats()
    }

    Ui.Command {
        commandId: "clip.clear-beat-map"
        text: qsTr("Clear Beat Map")
        category: qsTr("Clip")
        scope: "Global"
        defaultShortcuts: []
        available: win.clipCommands.beatsMapped()
        shortcutEnabled: win.keysFree && available
        onTriggered: win.clipCommands.clearBeatMap()
    }

    Ui.Command {
        commandId: "clip.solo-track"
        text: qsTr("Solo Track")
        category: qsTr("Clip")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.clipCommands.toggleSelectedTrackSolo()
    }

    Ui.Command {
        commandId: "modify.stabilize"
        text: qsTr("Stabilise")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.clipCommands.toggleSelectedStabilization()
    }

    Ui.Command {
        commandId: "modify.auto-balance"
        text: qsTr("Auto Balance Colour")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.clipCommands.autoBalanceSelected()
    }

    Ui.Command {
        commandId: "modify.reframe-to-fit"
        text: qsTr("Reframe to Fit")
        category: qsTr("Modify")
        scope: "Edit"
        available: win.workspace === "Edit" && backend.hasVideo && !backend.reframing
        shortcutEnabled: win.keysFree
        onTriggered: backend.reframeToFit(win.reframeToFitTargets())
    }
    Ui.Command {
        commandId: "modify.reframe-to-fit-stop"
        text: qsTr("Stop Reframe to Fit")
        category: qsTr("Modify")
        scope: "Edit"
        available: backend.reframing
        shortcutEnabled: win.keysFree
        onTriggered: backend.cancelReframeToFit()
    }

    // Match: keep a reference clip and frame, then match others to it.
    Ui.Command {
        commandId: "modify.match-reference"
        text: qsTr("Set Match Reference")
        category: qsTr("Modify")
        scope: "Edit"
        available: win.workspace === "Edit" && win.selection.selectedEntry() !== null && !win.selection.selectedEntry().gap && !win.selection.selectedEntry().title
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.setMatchReference(e.track, e.index, win.matching.matchFrameOf(e)) }
    }
    Ui.Command {
        commandId: "modify.match-color"
        text: qsTr("Match Colour")
        category: qsTr("Modify")
        scope: "Edit"
        available: win.workspace === "Edit" && win.matchReferenceSet && win.selection.selectedPicture() !== null && !win.matching.isMatchReference(win.selection.selectedPicture())
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedPicture(); if (e) backend.matchColor(e.track, e.index, win.matching.matchFrameOf(e)) }
    }
    Ui.Command {
        commandId: "modify.match-audio"
        text: qsTr("Match Audio")
        category: qsTr("Modify")
        scope: "Edit"
        available: win.workspace === "Edit" && win.matchReferenceSet && win.selection.selectedEntry() !== null && !win.matching.isMatchReference(win.selection.selectedEntry())
        shortcutEnabled: win.keysFree
        onTriggered: { const e = win.selection.selectedEntry(); if (e) backend.matchAudio(e.track, e.index) }
    }

    Ui.Command {
        commandId: "modify.volume-absolute"
        text: qsTr("Set Volume\u2026")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.sound.targetExists
        shortcutEnabled: win.keysFree && (win.sound.targetExists)
        onTriggered: volumeDialog.openFor(false)
    }

    Ui.Command {
        commandId: "modify.volume-relative"
        text: qsTr("Change Volume\u2026")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.sound.targetExists
        shortcutEnabled: win.keysFree && (win.sound.targetExists)
        onTriggered: volumeDialog.openFor(true)
    }

    Ui.Command {
        commandId: "modify.apply-fades"
        text: qsTr("Apply Fades")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.clipCommands.setSelectedFades(true)
    }

    Ui.Command {
        commandId: "modify.remove-fades"
        text: qsTr("Remove Fades")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedClip() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedClip() !== null)
        onTriggered: win.clipCommands.setSelectedFades(false)
    }

    Ui.Command {
        commandId: "modify.rotate-left"
        text: qsTr("Rotate Left")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.clipCommands.rotateSelected(-90)
    }

    Ui.Command {
        commandId: "modify.rotate-right"
        text: qsTr("Rotate Right")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.clipCommands.rotateSelected(90)
    }

    Ui.Command {
        commandId: "modify.reverse"
        text: qsTr("Reverse Clip")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.retiming.canRetimeSelection()
        shortcutEnabled: win.keysFree && (win.retiming.canRetimeSelection())
        onTriggered: win.retiming.setSelectedSpeed(-win.retiming.selectedSpeed())
    }

    Ui.Command {
        commandId: "modify.render-clip"
        text: qsTr("Render in Place")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.clipCommands.renderOrRestoreSelected()
    }

    Ui.Command {
        commandId: "view.compare"
        text: qsTr("Toggle Comparison")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: backend.compareMode = !backend.compareMode
    }

    Ui.Command {
        commandId: "view.video-scopes"
        text: qsTr("Video Scopes")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+7"]
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && win.workspace === "Edit"
        onTriggered: win.editScopes = !win.editScopes
    }

    Ui.Command {
        commandId: "view.safe-areas"
        text: qsTr("Toggle Safe Areas")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.hasVideo)
        onTriggered: backend.setViewerOption("safeAreas", !backend.viewerSettings.safeAreas)
    }

    Ui.Command {
        commandId: "view.history-back"
        text: qsTr("Previous Timeline Opened")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: []
        available: win.timelineHistoryAt > 0
        shortcutEnabled: win.keysFree && (win.timelineHistoryAt > 0)
        onTriggered: win.stepTimelineHistory(-1)
    }

    Ui.Command {
        commandId: "view.history-forward"
        text: qsTr("Next Timeline Opened")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: []
        available: win.timelineHistoryAt >= 0 && win.timelineHistoryAt < win.timelineHistory.length - 1
        shortcutEnabled: win.keysFree && (win.timelineHistoryAt >= 0 && win.timelineHistoryAt < win.timelineHistory.length - 1)
        onTriggered: win.stepTimelineHistory(1)
    }

    Ui.Command {
        commandId: "view.inspector"
        text: qsTr("Toggle Inspector")
        category: qsTr("View")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit"
        shortcutEnabled: win.keysFree && (win.workspace === "Edit")
        onTriggered: classicLayout.toggleInspector()
    }

    Ui.Command {
        commandId: "window.record-voiceover"
        text: qsTr("Record Voiceover")
        category: qsTr("Window")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.recordingTake || backend.hasVideo
        shortcutEnabled: win.keysFree && (backend.recordingTake || backend.hasVideo)
        onTriggered: win.recordVoiceover()
    }

    Ui.Command {
        commandId: "file.new-bin"
        text: qsTr("New Bin\u2026")
        category: qsTr("Project")
        scope: "Edit"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree && (true)
        onTriggered: win.onMediaPage(function (page) { page.binRequested("", false, false) })
    }

    Ui.Command {
        commandId: "file.create-proxies"
        text: qsTr("Create Proxies\u2026")
        category: qsTr("Project")
        scope: "Edit"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree && (true)
        onTriggered: win.onMediaPage(function (page) { page.operationRequested("proxy") })
    }

    Ui.Command {
        commandId: "file.split-photoshop-layers"
        text: qsTr("Split Photoshop Layers")
        category: qsTr("Project")
        scope: "Edit"
        defaultShortcuts: []
        available: backend.isPhotoshopMedia(win.selectedLibraryIndex)
        shortcutEnabled: win.keysFree && (available)
        onTriggered: backend.splitPhotoshopLayers(win.selectedLibraryIndex)
    }

    Ui.Command {
        commandId: "file.relink"
        text: qsTr("Relink Missing Media\u2026")
        category: qsTr("Project")
        scope: "Edit"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree && (true)
        onTriggered: win.onMediaPage(function (page) { page.operationRequested("relink") })
    }

    Ui.Command {
        commandId: "file.reveal-file"
        text: qsTr("Reveal in File Manager")
        category: qsTr("Project")
        scope: "Edit"
        defaultShortcuts: []
        available: win.clipCommands.ratingTarget() >= 0
        shortcutEnabled: win.keysFree && (win.clipCommands.ratingTarget() >= 0)
        onTriggered: backend.revealClip(win.clipCommands.ratingTarget())
    }

    Ui.Command {
        commandId: "modify.quality-3"
        text: qsTr("Frame Sampling: Floor")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.retiming.setSelectedFrameBlend(3)
    }

    Ui.Command {
        commandId: "modify.quality-4"
        text: qsTr("Frame Sampling: Nearest Frame")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.retiming.setSelectedFrameBlend(4)
    }

    Ui.Command {
        commandId: "modify.quality-0"
        text: qsTr("Frame Sampling: Original Sampling")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.retiming.setSelectedFrameBlend(0)
    }

    Ui.Command {
        commandId: "modify.quality-1"
        text: qsTr("Frame Sampling: Frame Blending")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.retiming.setSelectedFrameBlend(1)
    }

    Ui.Command {
        commandId: "modify.quality-2"
        text: qsTr("Frame Sampling: Optical Flow")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: win.workspace === "Edit" && win.selection.selectedPicture() !== null
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selection.selectedPicture() !== null)
        onTriggered: win.retiming.setSelectedFrameBlend(2)
    }

    Ui.Command {
        commandId: "modify.background-render"
        text: qsTr("Toggle Background Rendering")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree && (true)
        onTriggered: backend.backgroundRenderEnabled = !backend.backgroundRenderEnabled
    }

    Ui.Command {
        commandId: "clip.sync"
        text: qsTr("Sync Clips\u2026")
        category: qsTr("Clip")
        scope: "Global"
        defaultShortcuts: []
        available: backend.library.length >= 2
        shortcutEnabled: win.keysFree && (backend.library.length >= 2)
        onTriggered: win.openSyncDialog(win.selectedLibraryIndex >= 0 ? [win.selectedLibraryIndex] : [])
    }

    Ui.Command {
        commandId: "modify.edit-categories"
        text: qsTr("Edit Categories\u2026")
        category: qsTr("Modify")
        scope: "Global"
        defaultShortcuts: []
        available: true
        shortcutEnabled: win.keysFree && (true)
        onTriggered: categoriesDialog.open()
    }

    Ui.Command {
        commandId: "category.dialogue"
        text: qsTr("Category: Dialogue")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Alt+D"]
        available: win.workspace === "Edit" && win.selectionCount > 0
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selectionCount > 0)
        onTriggered: win.categories.assign("dialogue")
    }

    Ui.Command {
        commandId: "category.music"
        text: qsTr("Category: Music")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Alt+M"]
        available: win.workspace === "Edit" && win.selectionCount > 0
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selectionCount > 0)
        onTriggered: win.categories.assign("music")
    }

    Ui.Command {
        commandId: "category.effects"
        text: qsTr("Category: Effects")
        category: qsTr("Modify")
        scope: "Edit"
        defaultShortcuts: ["Ctrl+Alt+E"]
        available: win.workspace === "Edit" && win.selectionCount > 0
        shortcutEnabled: win.keysFree && (win.workspace === "Edit" && win.selectionCount > 0)
        onTriggered: win.categories.assign("effects")
    }
}
