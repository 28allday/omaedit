import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// The File menu of the menu bar. Menus only mirror the Shortcut items (single
// source of truth for key handling): no Action shortcuts here, or every key
// would register twice as ambiguous.
AppMenu {
    title: qsTr("File")

    AppMenu {
        title: qsTr("New")

        KeyMenuItem {
            text: qsTr("Project…")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "project.new"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Timeline…")
            commandId: "sequence.new"
            onTriggered: Ui.Commands.trigger(commandId)
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Duplicate Timeline")
            commandId: "sequence.duplicate"
            onTriggered: Ui.Commands.trigger(commandId)
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Bin…")
            kbd: Ui.Commands.hint(commandId)
            enabled: !registeredCommand || registeredCommand.available
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "file.new-bin"
        }

        KeyMenuItem {
            text: qsTr("Nested Clip")
            kbd: Ui.Commands.hint(commandId)
            enabled: (!backend.viewingSource && backend.hasVideo && (win.hasTimelineRange || win.selectionCount > 0)) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "edit.compound"
        }

        KeyMenuItem {
            text: qsTr("Multicam Clip…")
            enabled: (backend.library.length >= 2) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "edit.new-multicam-clip"
        }
    }

    KeyMenuItem {
        text: qsTr("Open Project…")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "project.open"
        enabled: !registeredCommand || registeredCommand.available
    }

    AppMenu {
        title: qsTr("Open Recent")
        onAboutToShow: backend.refreshRecentProjects()
        enabled: backend.recentProjects.length > 0 && !backend.projectBusy && !backend.exporting

        Repeater {
            model: backend.recentProjectEntries

            KeyMenuItem {
                required property var modelData

                text: modelData.name + (modelData.missing ? qsTr(" — Missing") : "")
                onTriggered: win.project.guardedOpen(modelData.path)
            }
        }
    }

    KeyMenuItem {
        text: qsTr("Close Project")
        commandId: "project.close"
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Save")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "project.save"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Save As…")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "project.save-as"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {}

    AppMenu {
        title: qsTr("Import")

        KeyMenuItem {
            text: qsTr("Media…")
            kbd: Ui.Commands.hint(commandId)
            onTriggered: Ui.Commands.trigger(commandId)
            commandId: "media.import"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Timeline…")
            enabled: !backend.exporting && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.import-timeline"
        }

        KeyMenuItem {
            text: qsTr("Captions…")
            enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.import-captions"
        }
    }

    KeyMenuItem {
        text: qsTr("Create Proxies…")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "file.create-proxies"
    }

    KeyMenuItem {
        text: qsTr("Split Photoshop Layers")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "file.split-photoshop-layers"
    }

    KeyMenuItem {
        text: qsTr("Relink Missing Media…")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "file.relink"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Export Movie…")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo && !backend.exporting) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "deliver.export"
    }

    AppMenu {
        title: qsTr("Export As")
        enabled: backend.hasVideo && !backend.exporting

        KeyMenuItem {
            text: qsTr("OpenTimelineIO…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.export-timeline-otio"
            enabled: !registeredCommand || registeredCommand.available
        }
        KeyMenuItem {
            text: qsTr("CMX 3600 EDL…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.export-timeline-edl"
            enabled: !registeredCommand || registeredCommand.available
        }
        KeyMenuItem {
            text: qsTr("XML Timeline (.fcpxml)…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.export-timeline-fcpxml"
            enabled: !registeredCommand || registeredCommand.available
        }
        KeyMenuItem {
            text: qsTr("Legacy XML Timeline (.xml)…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.export-timeline-xmeml"
            enabled: !registeredCommand || registeredCommand.available
        }
        KeyMenuItem {
            text: qsTr("AAF…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.export-timeline-aaf"
            enabled: (!registeredCommand || registeredCommand.available) && backend.timelineFormatAvailable("aaf")
        }
        MenuSeparator {}

        KeyMenuItem {
            text: qsTr("H.264 High Quality…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.h-264-high-quality"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("H.264 Smaller File…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.h-264-smaller-file"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("ProRes 422 Master…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.prores-422-master"
            enabled: !registeredCommand || registeredCommand.available
        }

        KeyMenuItem {
            text: qsTr("Audio Only…")
            onTriggered: Ui.Commands.trigger(commandId)
            kbd: Ui.Commands.hint(commandId)
            commandId: "project.audio-only"
            enabled: !registeredCommand || registeredCommand.available
        }
    }

    KeyMenuItem {
        text: qsTr("Export Current Frame…")
        commandId: "view.export-frame"
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }

    KeyMenuItem {
        text: qsTr("Export Captions…")
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "project.export-captions"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Reveal Source Clip")
        kbd: Ui.Commands.hint(commandId)
        enabled: (backend.hasVideo) && (!registeredCommand || registeredCommand.available)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "media.reveal-source"
    }

    KeyMenuItem {
        text: qsTr("Reveal in File Manager")
        kbd: Ui.Commands.hint(commandId)
        enabled: !registeredCommand || registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "file.reveal-file"
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Recovery and Backups…")
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "project.recovery-and-backups"
        enabled: !registeredCommand || registeredCommand.available
    }

    KeyMenuItem {
        text: qsTr("Archive Project…")
        onTriggered: Ui.Commands.trigger(commandId)
        kbd: Ui.Commands.hint(commandId)
        commandId: "project.archive-project"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Preferences…")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "app.preferences"
        enabled: !registeredCommand || registeredCommand.available
    }

    MenuSeparator {}

    KeyMenuItem {
        text: qsTr("Quit")
        kbd: Ui.Commands.hint(commandId)
        onTriggered: Ui.Commands.trigger(commandId)
        commandId: "app.quit"
        enabled: !registeredCommand || registeredCommand.available
    }
}
