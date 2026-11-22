import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Preferences ▸ Storage. Folders and limits are validated and saved together
// (Apply); the running process keeps the folders it started with, so the
// pane shows both what is saved and what is in use right now.
ScrollView {
    ScrollBar.vertical: Ui.ScrollBar { atRest: true }
    id: root
    required property var engine
    property var draft: ({})
    property string message: ""
    property bool error: false
    property var usage: ({})
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    readonly property var folders: [
        { key: "cache", label: qsTr("Processing cache"), note: qsTr("Playback renders, transcripts, nested sequences") },
        { key: "keptMedia", label: qsTr("Kept media"), note: qsTr("Render in Place files and stabilisation analyses, which saved projects point at; with the application's data by default") },
        { key: "thumbnails", label: qsTr("Thumbnail cache"), note: qsTr("Filmstrips and posters; inside the processing cache by default") },
        { key: "gallery", label: qsTr("Stills gallery"), note: qsTr("Beside the project; the app gallery for untitled work") },
        { key: "recordings", label: qsTr("Recordings"), note: qsTr("Beside the project; Music for untitled work") },
        { key: "proxies", label: qsTr("Proxy destination"), note: qsTr("Offered by Create Proxies; beside the project by default") }
    ]
    readonly property var limits: engine.storageLimits
    function set(key, value) {
        var copy = Object.assign({}, draft)
        copy[key] = value
        draft = copy
        message = ""
    }
    function refresh() { draft = Object.assign({}, engine.storageSettings); usage = engine.storageUsage(); message = ""; error = false }
    function mib(bytes) { return (bytes / 1048576).toFixed(bytes >= 10485760 ? 0 : 1) + " " + qsTr("MiB") }
    // Kept media nothing checkable uses: found first, removed only after the
    // editor has read what will go and what could not be checked.
    property bool sweeping: false
    property string keptMessage: ""
    property var unused: ({})
    Connections {
        target: root.engine
        function onUnusedKeptMediaFound(report) {
            root.sweeping = false
            root.unused = report
            if (report.files > 0) { removeUnusedConfirm.open(); return }
            root.keptMessage = qsTr("Nothing to remove: every kept file is used by a project OmaEdit can check.")
        }
        function onUnusedKeptMediaRemoved(report) {
            root.sweeping = false
            root.keptMessage = qsTr("Removed %n unused kept file(s), %1.", "", report.files).arg(root.mib(report.bytes))
                + (report.failed > 0 ? " " + qsTr("%n file(s) could not be removed.", "", report.failed) : "")
        }
    }
    Ui.MessageDialog {
        id: removeUnusedConfirm
        objectName: "prefs:removeUnusedKeptConfirm"
        title: qsTr("Remove unused kept media?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        message: qsTr("%n file(s), %1, in %2 will be deleted. This cannot be undone.", "", root.unused.files || 0).arg(root.mib(root.unused.bytes || 0)).arg(root.unused.folder || "")
        detail: qsTr("They are Render in Place files and stabilisation analyses that are not used by the open project (any of its timelines, or its undo history), by any project in the recent list, or by any saved version, recovery copy or queued render OmaEdit holds. OmaEdit cannot check a project that is not in the recent list: if one uses any of these files, its rendered clips will be missing their media until Restore Original is used, and its stabilised clips will need analysing again.")
        onAccepted: { root.sweeping = true; root.keptMessage = ""; root.engine.removeUnusedKeptMedia() }
    }
    onVisibleChanged: if (visible) refresh()
    Component.onCompleted: refresh()
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose, paths) {
            if (purpose.indexOf("preferences-storage:") === 0 && paths.length)
                root.set(purpose.substring(20), paths[0])
        }
    }
    ColumnLayout {
        width: root.availableWidth
        spacing: Ui.Theme.s2
        Label { text: qsTr("Storage"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel
            text: qsTr("Choose existing folders, or leave a field empty for its default. Saved folders are used from the next launch; files already in the old folders stay where they are, except kept media, which is moved to its new folder at that launch.")
        }
        Repeater {
            model: root.folders
            delegate: ColumnLayout {
                id: row
                required property var modelData
                readonly property string active: root.engine.storageActivePaths[modelData.key] || ""
                readonly property string fallback: root.engine.storageDefaultPaths[modelData.key] || ""
                Layout.fillWidth: true; spacing: 2
                RowLayout {
                    Layout.fillWidth: true; spacing: Ui.Theme.s2
                    Label {
                        Layout.preferredWidth: 150; Layout.alignment: Qt.AlignVCenter
                        text: row.modelData.label; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel
                    }
                    Ui.LineEdit {
                        objectName: "prefs:storage:" + row.modelData.key
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: root.draft[row.modelData.key] || ""
                        hint: row.fallback || qsTr("Ask when creating proxies")
                        Accessible.name: row.modelData.label
                        font.pixelSize: Ui.Theme.fsControl
                        tip: qsTr("Folder used for this purpose from the next launch; empty means the default shown")
                        onTextEdited: root.set(row.modelData.key, text)
                    }
                    FlatButton {
                        text: qsTr("Browse…"); quiet: true
                        Accessible.name: qsTr("Choose %1 folder").arg(row.modelData.label)
                        tip: qsTr("Choose the folder in a file picker instead of typing the path")
                        onClicked: root.engine.mediaTasks.browse("preferences-storage:" + row.modelData.key)
                    }
                }
                Label {
                    Layout.fillWidth: true; Layout.leftMargin: 150 + Ui.Theme.s2
                    wrapMode: Text.WordWrap; elide: Text.ElideMiddle; maximumLineCount: 2
                    color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel
                    // Only worth saying when the field (or its default) is not what this process runs with.
                    readonly property string shown: (root.draft[row.modelData.key] || row.fallback)
                    text: row.modelData.note + (row.active && row.active !== shown && row.active !== shown + "/thumbnails-v1" ? qsTr(" · In use now: %1").arg(row.active) : "")
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: Ui.Theme.s2
            Item { Layout.preferredWidth: 150 }
            FlatButton {
                objectName: "prefs:removeUnusedKept"
                text: qsTr("Remove Unused Kept Media…"); quiet: true
                enabled: !root.sweeping
                tip: qsTr("Find the kept media no project OmaEdit can check still uses, show how much there is, and ask before deleting it")
                onClicked: { root.sweeping = true; root.keptMessage = ""; root.engine.findUnusedKeptMedia() }
            }
            Label {
                objectName: "prefs:removeUnusedKeptMessage"
                Layout.fillWidth: true; wrapMode: Text.WordWrap
                color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel
                text: root.sweeping ? qsTr("Checking…") : root.keptMessage
                Accessible.name: text
            }
        }
        GridLayout {
            columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true
            Label { text: qsTr("Thumbnail memory per cache"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.ValueField {
                    objectName: "prefs:thumbnailMemory"; label: qsTr("Thumbnail memory per cache")
                    tip: qsTr("Memory each thumbnail cache may hold: library posters and timeline filmstrips")
                    value: root.draft.thumbnailMemoryMiB || 64; from: root.limits.thumbnailMemoryMiB[0]; to: root.limits.thumbnailMemoryMiB[1]; decimals: 0; suffix: qsTr("MiB")
                    onEdited: function(value) { root.set("thumbnailMemoryMiB", Math.round(value)) }
                }
                Label {
                    objectName: "prefs:thumbnailMemoryUsage"
                    color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel
                    text: root.usage.thumbnailMemoryLimitBytes ? qsTr("Library cache using %1 of %2").arg(root.mib(root.usage.thumbnailMemoryBytes || 0)).arg(root.mib(root.usage.thumbnailMemoryLimitBytes)) : ""
                }
            }
            Label { text: qsTr("Thumbnail disk limit"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.ValueField {
                    objectName: "prefs:thumbnailDisk"; label: qsTr("Thumbnail disk limit")
                    tip: qsTr("Disk space kept for generated thumbnails; 0 keeps them in memory only")
                    value: root.draft.thumbnailDiskMiB === undefined ? 512 : root.draft.thumbnailDiskMiB
                    from: root.limits.thumbnailDiskMiB[0]; to: root.limits.thumbnailDiskMiB[1]; decimals: 0; suffix: qsTr("MiB")
                    onEdited: function(value) { root.set("thumbnailDiskMiB", Math.round(value)) }
                }
                Label {
                    objectName: "prefs:thumbnailDiskUsage"
                    color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel
                    text: root.usage.thumbnailDiskLimitBytes === undefined ? "" : root.usage.thumbnailDiskLimitBytes === 0 ? qsTr("Disk thumbnails off this session") : qsTr("Using %1 of %2").arg(root.mib(root.usage.thumbnailDiskBytes || 0)).arg(root.mib(root.usage.thumbnailDiskLimitBytes))
                }
            }
            Label { text: qsTr("Waveform peak samples"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ValueField {
                objectName: "prefs:waveformSamples"; label: qsTr("Waveform peak samples")
                tip: qsTr("Waveform peak values held in memory; more keeps more audio drawn without recomputing")
                Layout.preferredWidth: 110
                value: root.draft.waveformSamples || 8000; from: root.limits.waveformSamples[0]; to: root.limits.waveformSamples[1]; decimals: 0; step: 100
                onEdited: function(value) { root.set("waveformSamples", Math.round(value)) }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel
            text: qsTr("Each of the two thumbnail caches (library posters and timeline filmstrips) may hold this much memory. Disk limit 0 keeps thumbnails in memory only; up to 8,192 generated files are retained. Waveform peaks are held in memory. Renders, proxies and gallery originals are kept until you remove them.")
        }
        RowLayout {
            FlatButton {
                objectName: "prefs:storageApply"; text: qsTr("Apply Storage Settings")
                tip: qsTr("Check the folders and save all storage settings; new folders apply at the next launch")
                onClicked: {
                    var problem = root.engine.saveStorageSettings(root.draft)
                    root.error = problem !== ""
                    root.message = problem || (root.engine.storageRestartRequired ? qsTr("Storage settings saved. Restart OmaEdit to use the new folders and limits.") : qsTr("Storage settings saved."))
                }
            }
            FlatButton {
                objectName: "prefs:storageDefaults"
                text: qsTr("Use Defaults"); quiet: true
                tip: qsTr("Fill every field with its built-in default; nothing is saved until you Apply")
                onClicked: { root.draft = Object.assign({}, root.engine.storageDefaults); root.message = qsTr("Defaults selected. Apply to save them."); root.error = false }
            }
        }
        Label {
            objectName: "prefs:storageMessage"
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            visible: text !== ""; color: root.error ? Ui.Theme.danger : Ui.Theme.textSecondary
            font.pixelSize: Ui.Theme.fsLabel
            text: root.message || (root.engine.storageRestartRequired ? qsTr("Saved storage settings differ from this session's. Restart OmaEdit to use them.") : "")
            Accessible.name: text
        }
        Item { Layout.fillHeight: true }
    }
}
