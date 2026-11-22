import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import "../Ui/Timecode.js" as Timecode

// The title bar's gear and File ▸ Preferences… (Ctrl+,): application-wide
// settings, a category rail on the left and one page per category. Every
// control here writes through immediately (there is no Apply), and every
// row is honest — what the app cannot do yet says so in place rather than
// offering a control that does nothing.
Ui.ModalDialog {
    id: root
    objectName: "preferences"
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    background: Rectangle { color: Ui.Theme.panelBg; radius: Ui.Theme.rMenu; border.color: Ui.Theme.border; border.width: 1 }
    title: qsTr("Preferences")
    width: Math.min(840, parent ? parent.width - Ui.Theme.s5 * 2 : 840)
    height: Math.min(620, parent ? parent.height - Ui.Theme.s5 * 2 : 620)
    standardButtons: Dialog.Close

    property bool openShortcutEditor: false
    onClosed: { if (openShortcutEditor) { openShortcutEditor = false; Qt.callLater(function() { Ui.Commands.trigger("app.shortcuts") }) } }
    property int page: 0
    readonly property var categories: [
        { key: "general", name: qsTr("General"), icon: "sliders-horizontal" },
        { key: "playback", name: qsTr("Playback"), icon: "play" },
        { key: "media", name: qsTr("Media"), icon: "film" },
        { key: "storage", name: qsTr("Storage"), icon: "hard-drive" },
        { key: "audio", name: qsTr("Audio"), icon: "volume-2" },
        { key: "color", name: qsTr("Colour"), icon: "palette" },
        { key: "keyboard", name: qsTr("Keyboard"), icon: "type" },
        { key: "hardware", name: qsTr("Hardware"), icon: "monitor" },
        { key: "input", name: qsTr("Input"), icon: "mouse-pointer-2" },
        { key: "plugins", name: qsTr("Plug-ins"), icon: "sparkles" }
    ]
    readonly property bool textActive: activeFocus || transcriptCommand.activeFocus
                                       || transcriptModel.activeFocus || transcriptLanguage.activeFocus

    function openAt(key) {
        var i = 0
        for (var c = 0; c < categories.length; ++c)
            if (categories[c].key === key) i = c
        page = i
        open()
    }
    onOpened: {
        newTimelineStart.clearRefused() // an entry refused last time is not still warned of
        backend.hardware.probe()
        backend.recorder.refreshInputs()
        outputs = backend.audioOutputs()
    }
    property var outputs: []
    readonly property var outputIds: [""].concat(outputs.map(function (o) { return o.id }))

    // Reset: the backend restores its own categories through their setters;
    // the window-owned bits (density, skimming, snapping) and shortcuts are
    // reset here. "All" asks first and names everything it will touch.
    function resetPage(key) {
        if (key === "general") {
            win.density = "compact"
            Ui.Theme.highContrast = false; Ui.Theme.reducedMotion = false
        }
        if (key === "playback") { win.skimEnabled = true; win.snapEnabled = true }
        if (key === "keyboard") { backend.commands.resetAll(); return }
        if (key === "input") { win.resetInputPreferences(); return }
        backend.resetPreferences(key)
    }
    function resetAll() {
        win.density = "compact"; win.skimEnabled = true; win.snapEnabled = true
        Ui.Theme.highContrast = false; Ui.Theme.reducedMotion = false
        win.resetInputPreferences()
        backend.commands.resetAll()
        backend.resetPreferences("all")
    }
    Ui.MessageDialog {
        id: resetAllConfirm
        objectName: "prefs:resetAllConfirm"
        title: qsTr("Reset all preferences to their defaults?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.resetAll()
        message: qsTr("General (density, contrast, motion, autosave, backups), Playback (audio output, buffer and monitor mapping, viewer options, hover preview, snapping), Media (proxy playback, transcription), Storage (folders and limits, from the next launch), Audio input device and channels, Colour (default colour space, LUT folder), all keyboard shortcuts, Hardware choices, Input (wheel and double-click) and Plug-ins (every provider enabled, quarantine cleared). Recent projects, LUT favourites, presets and saved layouts are kept.")
    }

    component Heading: Label {
        Layout.fillWidth: true
        color: Ui.Theme.textPrimary
        font.pixelSize: Ui.Theme.fsHeading
        font.weight: Ui.Theme.wHeading
    }
    component Caption: Label {
        color: Ui.Theme.textSecondary
        font.pixelSize: Ui.Theme.fsLabel
        Layout.alignment: Qt.AlignVCenter
    }
    component Note: Label {
        Layout.fillWidth: true
        // (a folder's path has no spaces to break at)
        wrapMode: Text.Wrap
        color: Ui.Theme.textMuted
        font.pixelSize: Ui.Theme.fsLabel
    }

    contentItem: RowLayout {
        spacing: Ui.Theme.s3

        // ── Category rail ─────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 150
            Layout.fillHeight: true
            color: Ui.Theme.panelBg
            radius: Ui.Theme.rControl
            Column {
                anchors.fill: parent
                anchors.margins: Ui.Theme.s1
                spacing: 2
                Repeater {
                    model: root.categories
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        objectName: "prefs:tab:" + modelData.key
                        activeFocusOnTab: true
                        readonly property bool keyboardConsumesKeys: true
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.name
                        Accessible.selectable: true
                        Accessible.selected: root.page === index
                        Accessible.onPressAction: root.page = index
                        border.width: activeFocus ? Ui.Theme.focusRing : 0
                        border.color: Ui.Theme.accent
                        Keys.onSpacePressed: root.page = index
                        Keys.onReturnPressed: root.page = index
                        Keys.onEnterPressed: root.page = index
                        width: parent.width
                        height: Ui.Theme.hRow + Ui.Theme.s1
                        radius: Ui.Theme.rControl
                        color: root.page === index ? Ui.Theme.controlBg
                             : tabHover.hovered ? Ui.Theme.hovered(Ui.Theme.panelBg) : "transparent"
                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: Ui.Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Ui.Theme.s2
                            Ui.Icon {
                                name: modelData.icon
                                size: 14
                                color: root.page === index ? Ui.Theme.accent : Ui.Theme.textSecondary
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: modelData.name
                                font.family: Ui.Theme.fontFamily
                                font.pixelSize: Ui.Theme.fsControl
                                color: root.page === index ? Ui.Theme.textPrimary : Ui.Theme.textSecondary
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        HoverHandler { id: tabHover }
                        TapHandler { onTapped: root.page = index }
                    }
                }
            }
        }

        // ── Pages ─────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.minimumHeight: 0
            spacing: Ui.Theme.s2
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.minimumHeight: 0
            currentIndex: root.page

            // General
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("General") }
                GridLayout {
                    columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                    Layout.fillWidth: true
                    Caption { text: qsTr("Density") }
                    Ui.SegmentedControl {
                        objectName: "prefs:density"
                        labels: [qsTr("Compact"), qsTr("Normal"), qsTr("Comfortable")]
                        names: ["compact", "normal", "comfortable"]
                        tip: qsTr("Spacing and control height across the interface; type size stays the same")
                        currentIndex: Math.max(0, names.indexOf(win.density))
                        onActivated: function (i) { win.density = names[i] }
                    }
                    Caption { text: qsTr("High contrast") }
                    Ui.Toggle {
                        objectName: "prefs:highContrast"
                        Accessible.name: qsTr("High contrast")
                        tip: qsTr("Stronger control borders and brighter labels; the viewer surround stays unchanged")
                        checked: Ui.Theme.highContrast
                        onToggled: Ui.Theme.highContrast = checked
                    }
                    Caption { text: qsTr("Reduce motion") }
                    Ui.Toggle {
                        objectName: "prefs:reducedMotion"
                        Accessible.name: qsTr("Reduce motion")
                        tip: qsTr("Turn off interface transitions and spinning progress icons; video playback is unaffected")
                        checked: Ui.Theme.reducedMotion
                        onToggled: Ui.Theme.reducedMotion = checked
                    }
                    Caption { text: qsTr("Autosave") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:autosave"
                            fitsChoices: true
                            Accessible.name: qsTr("Autosave")
                            showsTip: false
                            tip: qsTr("How often a recovery copy is written while playback is stopped")
                            readonly property var minutes: [0, 1, 2, 5, 10, 15]
                            model: [qsTr("Off"), qsTr("Every minute"), qsTr("Every 2 minutes"), qsTr("Every 5 minutes"),
                                    qsTr("Every 10 minutes"), qsTr("Every 15 minutes")]
                            currentIndex: Math.max(0, minutes.indexOf(backend.autosaveMinutes))
                            onActivated: function (i) { backend.autosaveMinutes = minutes[i] }
                        }
                        Note { text: qsTr("Keeps a separate recovery copy while playback is stopped, including untitled work. Your named project changes only when you save.") }
                    }
                    Caption { text: qsTr("New timelines start at") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.TimecodeField {
                            id: newTimelineStart
                            objectName: "prefs:newTimelineStart"
                            start: true
                            // Counted with room for any rate's frame numbers; what is kept is the timecode.
                            fps: 60
                            anyRate: true
                            label: qsTr("New timelines start at")
                            tip: qsTr("Starting timecode the New Timeline box offers, such as 09:59:50:00")
                            frame: Math.max(0, Timecode.read(backend.newTimelineStart, fps))
                            onEdited: function (f) { backend.newTimelineStart = Timecode.format(f, fps) }
                        }
                        Note { text: qsTr("The starting timecode offered for each new timeline, whatever its frame rate. Timeline Settings changes it for one timeline.") }
                    }
                    Caption { text: qsTr("Project backups") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ValueField { objectName: "prefs:backupLimit"; showsTip: false; value: backend.backupLimit; from: 1; to: 100; decimals: 0; tip: qsTr("Previous saved versions kept per project before the oldest is removed"); onEdited: function(v) { backend.backupLimit = Math.round(v) } }
                        Note { text: qsTr("Previous saved versions to keep per project (1–100). Older versions are removed only after a successful save.") }
                    }
                    Caption { text: qsTr("Recent projects") }
                    RowLayout {
                        spacing: Ui.Theme.s2
                        Label {
                            text: backend.recentProjects.length === 1 ? qsTr("1 remembered")
                                                                      : qsTr("%1 remembered").arg(backend.recentProjects.length)
                            color: Ui.Theme.textPrimary
                            font.pixelSize: Ui.Theme.fsControl
                        }
                        FlatButton {
                            objectName: "prefs:clearRecent"
                            text: qsTr("Clear List")
                            quiet: true
                            tip: qsTr("Forget the recent projects list; the project files themselves stay")
                            enabled: backend.recentProjects.length > 0
                            onClicked: backend.clearRecentProjects()
                        }
                    }
                    Caption { text: qsTr("On launch") }
                    Note { text: qsTr("The last workspace, window size and track height come back as they were.") }
                }
                Item { Layout.fillHeight: true }
            }

            // Playback
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("Playback") }
                GridLayout {
                    columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                    Layout.fillWidth: true
                    Caption { text: qsTr("Audio output") }
                    Ui.ComboField {
                        objectName: "prefs:audioOutput"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Audio output")
                        tip: qsTr("Device that plays the timeline and monitoring audio")
                        model: [qsTr("System default")].concat(root.outputs.map(function (o) {
                            return o.name + (o.default ? qsTr(" (default)") : "") }))
                        currentIndex: Math.max(0, root.outputIds.indexOf(backend.audioOutput))
                        onActivated: function (i) { backend.audioOutput = root.outputIds[i] }
                    }
                    Caption { text: qsTr("Audio buffer") }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: Ui.Theme.s2
                        Ui.ValueField {
                            objectName: "prefs:audioBuffer"
                            label: qsTr("Audio buffer")
                            showsTip: false
                            tip: qsTr("Audio queued ahead of the picture; more resists dropouts, less responds sooner")
                            fps: backend.fps > 0 ? backend.fps : 25
                            value: backend.audioBufferFrames; from: 2; to: 30
                            onEdited: function (v) { backend.audioBufferFrames = Math.round(v) }
                        }
                        Note { Layout.minimumWidth: 0; Layout.preferredWidth: 1; text: qsTr("Audio queued ahead of the picture while playing. More survives a busy machine without dropouts; less starts and stops sooner. Applies at once.") }
                    }
                    Caption { text: qsTr("Playback resolution") }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            fitsChoices: true
                            Accessible.name: qsTr("Playback resolution")
                            tip: qsTr("Play at half or quarter of the timeline's resolution so a heavy timeline keeps its frame rate. Scopes and exports are unaffected.")
                            model: [qsTr("Full"), qsTr("Half"), qsTr("Quarter")]
                            currentIndex: [1,2,4].indexOf(backend.viewerSettings.previewDivisor)
                            onActivated: function(index) { backend.setViewerOption("previewDivisor", [1,2,4][index]) }
                        }
                        Note { Layout.minimumWidth: 0; Layout.preferredWidth: 1; text: qsTr("Viewer and Playback controls zoom, preview quality, proxy preference and monitoring. Full-resolution exports are unaffected.") }
                    }
                    Caption { text: qsTr("Background render") }
                    Ui.Toggle {
                        objectName: "prefs:backgroundRender"
                        label: qsTr("Render effects while idle")
                        tip: qsTr("Automatically render affected timeline pictures after editing pauses; playback and exports take priority")
                        checked: backend.backgroundRenderEnabled
                        onToggled: backend.backgroundRenderEnabled = checked
                    }
                    Caption { text: qsTr("Playback cache") }
                    RowLayout {
                        Ui.ValueField {
                            objectName: "prefs:backgroundRenderLimit"
                            label: qsTr("Playback cache limit")
                            tip: qsTr("Maximum disk space for this session's lossless playback pictures; changing the limit rebuilds the cache")
                            value: backend.backgroundRenderLimit; from: 1; to: 200; decimals: 0; suffix: qsTr("GiB")
                            onEdited: function(v) { backend.backgroundRenderLimit = Math.round(v) }
                        }
                        Ui.ToolButton {
                            objectName: "prefs:clearPlaybackCache"
                            text: qsTr("Clear")
                            tip: qsTr("Delete the current playback cache; automatic rendering starts again after the next edit")
                            onClicked: backend.backgroundRenderAction("clear")
                        }
                        Ui.ToolButton {
                            objectName: "prefs:rebuildPlaybackCache"
                            text: qsTr("Render")
                            tip: qsTr("Build playback pictures for effects in the current sequence")
                            enabled: backend.backgroundRenderEnabled && backend.hasVideo
                            onClicked: backend.backgroundRenderAction("retry")
                        }
                    }
                    Caption { text: qsTr("Hover Preview") }
                    Ui.Toggle {
                        objectName: "prefs:skimming"
                        label: qsTr("Hovering the timeline scrubs the picture (S)")
                        tip: qsTr("Move the picture as the pointer hovers the timeline, without clicking")
                        checked: win.skimEnabled
                        onToggled: win.skimEnabled = checked
                    }
                    Caption { text: qsTr("Snapping") }
                    Ui.Toggle {
                        objectName: "prefs:snapping"
                        label: qsTr("Drags snap to edits, the playhead and markers (N)")
                        tip: qsTr("Pull dragged clips and edges onto edits, the playhead and markers")
                        checked: win.snapEnabled
                        onToggled: win.snapEnabled = checked
                    }
                }
                Item { Layout.fillHeight: true }
            }

            // Media (it scrolls in a short window, as Storage does)
            ScrollView {
                ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                id: mediaScroll
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.minimumHeight: 0
                implicitWidth: 0; implicitHeight: 0
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: mediaScroll.availableWidth
                    spacing: Ui.Theme.s3
                    Heading { text: qsTr("Media") }
                    GridLayout {
                        columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                        Layout.fillWidth: true
                        Caption { text: qsTr("Cache") }
                        Note { text: qsTr("Playback renders, transcripts and nested sequences are cached under %1. Change folders and limits on the Storage page.").arg(backend.storageActivePaths.cache) }
                        Caption { text: qsTr("Recordings") }
                        Note { text: backend.recordingsDir() }
                        Caption { text: qsTr("Proxies") }
                        Note { text: (backend.defaultProxyDir() ? qsTr("Create Proxies in Media offers %1; pick another folder there if you like.").arg(backend.defaultProxyDir())
                                                               : qsTr("Create Proxies in Media asks for a folder while the project is untitled; a fixed folder can be set on the Storage page."))
                                     + " " + qsTr("Choose Original, Proxy or Proxy preferred in Viewer and Playback.") }
                    }
                    Heading { text: qsTr("Transcription") }
                    GridLayout {
                        columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                        Layout.fillWidth: true
                        Caption { text: qsTr("Command") }
                        TextField {
                            id: transcriptCommand
                            objectName: "prefs:transcriptCommand"
                            Layout.fillWidth: true
                            text: backend.transcriber.command
                            color: Ui.Theme.textPrimary
                            font.pixelSize: Ui.Theme.fsControl
                            hoverEnabled: true
                            ToolTip.text: qsTr("Command run to transcribe clips; any whisper.cpp-compatible CLI works"); ToolTip.visible: hovered && !activeFocus; ToolTip.delay: 450
                            onEditingFinished: backend.transcriber.command = text
                        }
                        Caption { text: qsTr("Model") }
                        TextField {
                            id: transcriptModel
                            objectName: "prefs:transcriptModel"
                            Layout.fillWidth: true
                            text: backend.transcriber.model
                            color: Ui.Theme.textPrimary
                            font.pixelSize: Ui.Theme.fsControl
                            hoverEnabled: true
                            ToolTip.text: qsTr("Path to the ggml speech model the transcription command loads; empty for the one that comes with OmaEdit"); ToolTip.visible: hovered && !activeFocus; ToolTip.delay: 450
                            onEditingFinished: backend.transcriber.model = text
                        }
                        Caption { text: qsTr("Language") }
                        TextField {
                            id: transcriptLanguage
                            objectName: "prefs:transcriptLanguage"
                            Layout.preferredWidth: 80
                            text: backend.transcriber.language
                            color: Ui.Theme.textPrimary
                            font.pixelSize: Ui.Theme.fsControl
                            hoverEnabled: true
                            ToolTip.text: qsTr("Language code the speech is in, such as en, or auto to have it found"); ToolTip.visible: hovered && !activeFocus; ToolTip.delay: 450
                            onEditingFinished: backend.transcriber.language = text
                        }
                        Item { Layout.preferredWidth: 1; Layout.preferredHeight: 1 }
                        Note { text: qsTr("OmaEdit comes with whisper.cpp and its multilingual base model, which finds the language itself. Any whisper.cpp-compatible command and ggml model can take their place.") }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Storage
            StoragePreferencesPane {
                objectName: "storagePane"
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.minimumHeight: 0
                implicitWidth: 0; implicitHeight: 0
                engine: backend
            }

            // Audio
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("Audio") }
                GridLayout {
                    columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                    Layout.fillWidth: true
                    Caption { text: qsTr("Recording input") }
                    Ui.ComboField {
                        objectName: "prefs:audioInput"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Recording input")
                        tip: qsTr("Microphone or interface that voice-over and audio recordings capture from")
                        readonly property var ids: backend.recorder.inputs.map(function (d) { return d.id })
                        enabled: backend.recorder.available && ids.length > 0
                        model: ids.length > 0 ? backend.recorder.inputs.map(function (d) { return d.name })
                                              : [qsTr("No input found")]
                        currentIndex: Math.max(0, ids.indexOf(backend.recorder.inputDevice))
                        onActivated: function (i) { backend.recorder.inputDevice = ids[i] }
                    }
                    Caption { text: qsTr("Input channels") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:inputChannel"
                            fitsChoices: true
                            showsTip: false
                            Accessible.name: qsTr("Input channels")
                            tip: qsTr("Which side of a stereo input is recorded; one side is copied to both channels")
                            readonly property var values: ["both", "left", "right"]
                            model: [qsTr("Both (stereo)"), qsTr("Left only, on both"), qsTr("Right only, on both")]
                            currentIndex: Math.max(0, values.indexOf(backend.inputChannel))
                            onActivated: function (i) { backend.inputChannel = values[i] }
                        }
                        Note { text: qsTr("A mono microphone on one side of a stereo interface records to both channels.") }
                    }
                    Caption { text: qsTr("Monitor output") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:monitorMapping"
                            fitsChoices: true
                            showsTip: false
                            Accessible.name: qsTr("Monitor output")
                            tip: qsTr("How the left and right monitoring channels reach the output device")
                            readonly property var values: ["stereo", "swap", "left", "right"]
                            model: [qsTr("Stereo"), qsTr("Swapped L ↔ R"), qsTr("Left on both"), qsTr("Right on both")]
                            currentIndex: Math.max(0, values.indexOf(backend.monitorMapping))
                            onActivated: function (i) { backend.monitorMapping = values[i] }
                        }
                        Note { text: qsTr("How the two monitoring channels reach the output device; Mono in the viewer's monitor menu folds them. Applies at once.") }
                    }
                    Caption { text: qsTr("Monitoring") }
                    Note { text: qsTr("48 kHz, 16-bit stereo; the buffer is set under Playback. The project's own sample rate and channels are set in Timeline Settings and used by exports.") }
                }
                Item { Layout.fillHeight: true }
            }

            // Color
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("Colour") }
                GridLayout {
                    columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                    Layout.fillWidth: true
                    Caption { text: qsTr("New projects") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:defaultColorspace"
                            fitsChoices: true
                            showsTip: false
                            Accessible.name: qsTr("Default colour space")
                            tip: qsTr("Colour space new projects start in; Timeline Settings changes it per project")
                            readonly property var values: [601, 709, 2020]
                            model: [qsTr("Rec.601 (SD)"), qsTr("Rec.709 (HD)"), qsTr("Rec.2020 (UHD / wide gamut)")]
                            currentIndex: Math.max(0, values.indexOf(backend.defaultColorspace))
                            onActivated: function (i) { backend.defaultColorspace = values[i] }
                        }
                        Note { text: qsTr("The colour space a new project starts in; Timeline Settings changes it per project.") }
                    }
                    Caption { text: qsTr("LUT folder") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: backend.lutLibrary.folder !== "" ? backend.lutLibrary.folder : qsTr("~/LUTs and the app's own folder")
                            color: Ui.Theme.textPrimary
                            font.pixelSize: Ui.Theme.fsControl
                            elide: Text.ElideMiddle
                        }
                        FlatButton { text: qsTr("Choose…"); quiet: true; tip: qsTr("Pick the folder the Colour page lists LUTs from"); onClicked: backend.chooseLutFolder() }
                        FlatButton { text: qsTr("Rescan"); quiet: true; tip: qsTr("Read the LUT folder again to pick up files added since launch"); onClicked: backend.lutLibrary.rescan() }
                    }
                    Caption { text: qsTr("Monitor profile") }
                    Note { text: qsTr("The viewer shows the project's colour space as-is; display profiles are not applied.") }
                }
                Item { Layout.fillHeight: true }
            }

            // Keyboard
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("Keyboard") }
                Note { text: qsTr("Search commands, change keys, resolve conflicts and save shortcut presets. Changes update menus and tooltips immediately.") }
                FlatButton {
                    objectName: "prefs:showShortcuts"
                    text: qsTr("Edit Keyboard Shortcuts…")
                    quiet: true
                    tip: qsTr("Close Preferences and open the keyboard shortcut editor")
                    onClicked: { root.openShortcutEditor = true; root.close() }
                }
                Item { Layout.fillHeight: true }
            }

            // Hardware
            ScrollView {
                ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                id: hardwareScroll
                objectName: "hardwareScroll"
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.minimumHeight: 0
                implicitWidth: 0; implicitHeight: 0
                clip: true
                contentWidth: availableWidth
                HardwareDetails { width: hardwareScroll.availableWidth }
            }

            // Input
            ColumnLayout {
                spacing: Ui.Theme.s3
                Heading { text: qsTr("Input") }
                GridLayout {
                    columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
                    Layout.fillWidth: true
                    Caption { text: qsTr("Wheel over the timeline") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:wheelAction"
                            fitsChoices: true
                            showsTip: false
                            Accessible.name: qsTr("Wheel over the timeline")
                            tip: qsTr("What a plain wheel step does over the timeline; Ctrl+wheel does the other")
                            readonly property var values: ["scroll", "zoom"]
                            model: [qsTr("Scrolls time"), qsTr("Zooms")]
                            currentIndex: Math.max(0, values.indexOf(win.wheelAction))
                            onActivated: function (i) { win.wheelAction = values[i] }
                        }
                        Note { Layout.fillWidth: true; text: qsTr("Ctrl+wheel does the other one; Shift+wheel scrolls the track stack.") }
                    }
                    Caption { text: qsTr("Wheel direction") }
                    Ui.Toggle {
                        objectName: "prefs:wheelInvert"
                        label: qsTr("Reverse the wheel (natural scrolling)")
                        tip: qsTr("Flip the wheel direction for scrolling and zooming the timeline")
                        checked: win.wheelInvert
                        onToggled: win.wheelInvert = checked
                    }
                    Caption { text: qsTr("Wheel sensitivity") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ValueField { objectName: "prefs:wheelSensitivity"; showsTip: false; label: qsTr("Wheel sensitivity"); value: win.wheelSensitivity; from: 0.25; to: 4; decimals: 2; step: 0.05; suffix: "×"; tip: qsTr("Multiplier on the distance each wheel step scrolls or zooms (0.25–4×)"); onEdited: function (v) { win.wheelSensitivity = v } }
                        Note { Layout.fillWidth: true; text: qsTr("Scroll distance per wheel step. Trackpads and pen wheels send smaller steps.") }
                    }
                    Caption { text: qsTr("Double-click a clip") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Ui.ComboField {
                            objectName: "prefs:doubleClickClip"
                            fitsChoices: true
                            showsTip: false
                            Accessible.name: qsTr("Double-click a clip")
                            tip: qsTr("What a double-click on a timeline clip does; edit points always open the trim editor")
                            readonly property var values: ["open", "precision", "select"]
                            model: [qsTr("Opens a nested sequence or title"), qsTr("Opens the trim editor at the nearer edit"), qsTr("Selects only")]
                            currentIndex: Math.max(0, values.indexOf(win.doubleClickClip))
                            onActivated: function (i) { win.doubleClickClip = values[i] }
                        }
                        Note { Layout.fillWidth: true; text: qsTr("Double-clicking an edit point always opens the trim editor.") }
                    }
                    Caption { text: qsTr("Tablet and trackpad") }
                    Note { text: qsTr("Pen hover previews when Hover Preview is on (Playback); pinch and pressure are not used.") }
                }
                Item { Layout.fillHeight: true }
            }

            // Plug-ins
            ScrollView {
                ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                id: pluginsScroll
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.minimumHeight: 0
                implicitWidth: 0; implicitHeight: 0
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: pluginsScroll.availableWidth
                    spacing: Ui.Theme.s2
                    Heading { text: qsTr("Plug-ins") }
                    Note { text: qsTr("The tools outside OmaEdit that some features run on. Every effect, decoder and encoder is built in.") }
                    Repeater {
                        model: backend.plugins
                        delegate: ColumnLayout {
                            id: provider
                            required property var modelData
                            Layout.fillWidth: true; spacing: 2
                            RowLayout {
                                Layout.fillWidth: true; spacing: Ui.Theme.s2
                                Ui.Toggle {
                                    objectName: "prefs:plugin:" + provider.modelData.id
                                    label: provider.modelData.name
                                    enabled: !provider.modelData.core && provider.modelData.present
                                    checked: provider.modelData.enabled
                                    tip: qsTr("Let OmaEdit offer this provider's services; off warns at render, clips keep their settings")
                                    onToggled: backend.setPluginEnabled(provider.modelData.id, checked)
                                }
                                Item { Layout.fillWidth: true }
                                Ui.Chip { label: provider.modelData.present ? (provider.modelData.enabled ? qsTr("Ready") : qsTr("Off")) : qsTr("Missing"); tip: qsTr("Whether this provider loaded: Ready, switched Off, or Missing from this machine"); tint: provider.modelData.present ? (provider.modelData.enabled ? "green" : "") : "red" }
                            }
                            Note { Layout.leftMargin: Ui.Theme.s4; text: provider.modelData.kind + " · " + provider.modelData.detail + (provider.modelData.paths.length ? "\n" + provider.modelData.paths.join(", ") : "") }
                            Repeater {
                                model: provider.modelData.quarantined
                                delegate: Note { required property var modelData; Layout.leftMargin: Ui.Theme.s4; color: Ui.Theme.warning; text: qsTr("Quarantined: %1").arg(modelData.reason) }
                            }
                        }
                    }
                    RowLayout {
                        spacing: Ui.Theme.s2
                        FlatButton { objectName: "prefs:rescanPlugins"; text: qsTr("Rescan"); quiet: true; tip: qsTr("Look for these tools again"); onClicked: backend.rescanPlugins() }
                        FlatButton { text: qsTr("Clear Quarantine"); quiet: true; tip: qsTr("Forget every quarantined plug-in service so all are tried again"); onClicked: backend.clearPluginQuarantine("") }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Ui.Theme.s2
            FlatButton {
                objectName: "prefs:resetPage"
                quiet: true
                text: qsTr("Reset %1").arg(root.categories[root.page].name)
                Accessible.name: qsTr("Reset the %1 preferences to their defaults").arg(root.categories[root.page].name)
                tip: qsTr("Return every setting on this page to its default; other pages are untouched")
                onClicked: root.resetPage(root.categories[root.page].key)
            }
            Item { Layout.fillWidth: true }
            FlatButton {
                objectName: "prefs:resetAll"
                quiet: true
                text: qsTr("Reset All Preferences…")
                tip: qsTr("Return every preference on every page to its default, after confirming")
                onClicked: resetAllConfirm.open()
            }
        }
        }
    }

    function encoderState(v) {
        if (v === undefined) return qsTr("Probing…")
        if (v === "yes" || v === true) return qsTr("Available")
        if (v === "probing") return qsTr("Probing…")
        return qsTr("Not available")
    }
}
