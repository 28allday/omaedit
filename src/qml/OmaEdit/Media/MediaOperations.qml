pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

Item {
    id: root
    required property var engine
    property string binId: ""
    readonly property bool textActive: setup.visible || tasks.visible || report.visible
    property string reportId: ""
    // The chosen sources, in the order they were added, one entry per path.
    property var chosen: []
    // The per-import clip details fold away; they open by themselves when
    // the project's import defaults already fill one in.
    property bool detailsOpen: false
    readonly property var chosenInfo: root.engine.mediaTasks.describePaths(chosen)
    // The removable drives the chosen sources are on, and the one the
    // destination is on ("" = the computer's own disks).
    readonly property var sourceDrives: {
        var dependency = root.engine.volumes.drives
        var out = []
        for (var i = 0; i < chosen.length; ++i) {
            var d = root.engine.volumes.driveOf(chosen[i])
            if (d !== "" && out.indexOf(d) < 0) out.push(d)
        }
        return out
    }
    readonly property string destinationDrive: {
        var dependency = root.engine.volumes.drives
        return root.engine.volumes.driveOf(destination.text.trim())
    }
    readonly property bool copying: setup.importMode && mode.currentIndex > 0
    readonly property bool destinationOnSource: copying && destinationDrive !== "" && sourceDrives.indexOf(destinationDrive) >= 0
    // An import from a drive starts on copying to the computer's own disk;
    // the handling and destination it found are put back when the box is
    // next opened for anything else.
    property var beforeDrive: null
    function takeDriveDefaults() {
        if (!setup.importMode || beforeDrive !== null || sourceDrives.length === 0) return
        beforeDrive = {mode: mode.currentIndex, destination: destination.text}
        mode.currentIndex = 1
        destination.text = root.engine.copyImportDestination(sourceDrives[0])
    }
    function open(kind, paths) {
        if (beforeDrive !== null) {
            mode.currentIndex = beforeDrive.mode
            destination.text = beforeDrive.destination
            beforeDrive = null
        }
        setup.kind = kind
        chosen = []
        addPaths(paths || [])
        takeDriveDefaults()
        setup.open()
    }
    function showTasks() { tasks.open() }
    function paths() { return chosen.slice() }
    function addPaths(list) {
        var next = chosen.slice()
        for (var i = 0; i < list.length; ++i) {
            var p = String(list[i]).trim()
            if (p.length > 0 && next.indexOf(p) < 0) next.push(p)
        }
        chosen = next
    }
    function removePath(path) { chosen = chosen.filter(function (p) { return p !== path }) }
    function bytesText(n) {
        if (n < 1024) return qsTr("%1 B").arg(n)
        var units = [qsTr("KB"), qsTr("MB"), qsTr("GB"), qsTr("TB")], v = n / 1024, u = 0
        while (v >= 1024 && u < units.length - 1) { v /= 1024; ++u }
        return (v >= 100 ? v.toFixed(0) : v.toFixed(1)) + " " + units[u]
    }
    readonly property string chosenSummary: {
        var files = 0, folders = 0, bytes = 0
        for (var i = 0; i < chosenInfo.length; ++i) {
            if (chosenInfo[i].isDir) ++folders
            else { ++files; bytes += chosenInfo[i].bytes }
        }
        var parts = []
        if (files > 0) parts.push(files === 1 ? qsTr("1 file") : qsTr("%1 files").arg(files))
        if (folders > 0) parts.push(folders === 1 ? qsTr("1 folder") : qsTr("%1 folders").arg(folders))
        if (bytes > 0) parts.push(bytesText(bytes))
        return parts.join(" · ")
    }
    function tags(text) { return text.split(",").map(function (s) { return s.trim() }).filter(function (s) { return s.length > 0 }) }
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose, paths) {
            if (!paths.length) return
            if (purpose === "media-input") { root.addPaths(paths); root.takeDriveDefaults() }
            else if (purpose === "media-destination") destination.text = paths[0]
            else if (purpose === "media-destination2") destination2.text = paths[0]
            else if (purpose === "media-input-lut") inputLut.text = paths[0]
            else if (purpose.indexOf("media-report:") === 0) root.engine.mediaTasks.saveReport(purpose.substring(13), paths[0])
        }
    }
    component Caption: Text { color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; wrapMode: Text.Wrap; Layout.fillWidth: true }
    component MultiEdit: C.TextArea {
        color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase
        selectionColor: Theme.accent; selectedTextColor: Theme.accentText
        padding: Theme.s2; wrapMode: TextEdit.Wrap
        topPadding: Theme.s2; bottomPadding: Theme.s2; leftPadding: Theme.s2; rightPadding: Theme.s2
        topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
        background: Rectangle { color: Theme.controlBg; border.color: Theme.border; border.width: Theme.hairline; radius: Theme.rControl }
    }
    // The form's own length up to the window's; past that the whole form
    // scrolls between the title and the buttons.
    FormDialog {
        id: setup
        objectName: "mediaOperationDialog"
        property string kind: "import"
        readonly property bool importMode: kind === "import"
        readonly property bool cloneMode: kind === "clone"
        readonly property bool proxyMode: kind === "proxy"
        readonly property bool reframeMode: kind === "reframe360"
        title: reframeMode ? qsTr("Reframe 360° Video") : kind === "import" ? qsTr("Import Media") : kind === "clone" ? qsTr("Verified Clone") : kind === "proxy" ? qsTr("Create Proxies") : kind === "verify" ? qsTr("Verify Checksums") : qsTr("Find Duplicates")
        onAccepted: {
            var options = {mode: ["leave", "copy", "move"][mode.currentIndex], destination: destination.text.trim(),
                destination2: destination2.text.trim(), hierarchy: hierarchy.checked, pattern: pattern.text.trim(),
                duplicates: ["path", "size", "hash"][duplicates.currentIndex], preset: ["h264-540", "h264-720", "prores-1080"][preset.currentIndex], binId: root.binId,
                acceleration: ["auto", "software"][acceleration.currentIndex],
                projection: ["e", "dfisheye"][projection.currentIndex], yaw: yaw.value, pitch: pitch.value, roll: roll.value, fov: fov.value,
                width: resolution.currentIndex === 0 ? 1920 : 3840, height: resolution.currentIndex === 0 ? 1080 : 2160,
                metadata: {tags: root.tags(tagField.text), reel: reel.text, scene: scene.text, notes: notes.text, inputLut: inputLut.text.trim(), inputSpace: [0, 601, 709, 2020][inputSpace.currentIndex]}}
            if (kind === "duplicates" && options.duplicates === "path") options.duplicates = "hash"
            root.engine.startMediaTask(kind, root.paths(), options)
            tasks.open()
        }
        onOpened: {
            pattern.text = proxyMode ? "{name}_proxy" : reframeMode ? "{name}_reframed" : "{name}"
            if (proxyMode && destination.text === "") destination.text = root.engine.defaultProxyDir()
            if (importMode) { // the project's import defaults, editable per import
                var s = root.engine.projectSettings
                inputLut.text = s.importLut || ""
                inputSpace.currentIndex = Math.max(0, [0, 601, 709, 2020].indexOf(Number(s.importSpace || 0)))
                root.detailsOpen = inputLut.text !== "" || inputSpace.currentIndex > 0 || tagField.text !== "" || reel.text !== "" || scene.text !== "" || notes.text !== ""
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            ColumnLayout {
                id: form
                Layout.fillWidth: true
                spacing: Theme.s2
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "mediaInputSummary"
                        Layout.fillWidth: true
                        text: root.chosen.length > 0 ? root.chosenSummary : qsTr("No files chosen yet")
                        color: root.chosen.length > 0 ? Theme.textPrimary : Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; font.weight: Theme.wHeading
                        elide: Text.ElideRight
                    }
                    ToolButton { iconName: "plus"; showLabel: true; text: qsTr("Add Files…"); tip: qsTr("Choose media files to add to the list"); onClicked: root.engine.mediaTasks.browse("media-input", "files") }
                    ToolButton { iconName: "folder-plus"; showLabel: true; text: qsTr("Add Folder…"); tip: qsTr("Add a folder; camera-card folders are scanned recursively"); onClicked: root.engine.mediaTasks.browse("media-input") }
                    ToolButton { iconName: "trash-2"; showLabel: true; text: qsTr("Clear"); tip: qsTr("Remove every file and folder from the list"); enabled: root.chosen.length > 0; onClicked: root.chosen = [] }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(2, Math.min(6, root.chosen.length)) * Theme.hRow * 2 + Theme.s1 * 2
                    color: Theme.controlBg; border.color: Theme.border; border.width: Theme.hairline; radius: Theme.rControl
                    ListView {
                        id: input
                        objectName: "mediaInputPaths"
                        anchors.fill: parent; anchors.margins: Theme.s1
                        clip: true
                        model: root.chosenInfo
                        Accessible.name: qsTr("Source files and folders")
                        boundsBehavior: Flickable.StopAtBounds
                        C.ScrollBar.vertical: ScrollBar { atRest: true }
                        delegate: Item {
                            id: sourceRow
                            required property var modelData
                            required property int index
                            objectName: "mediaInputRow:" + index
                            width: ListView.view.width
                            height: Theme.hRow * 2
                            Rectangle { anchors.fill: parent; radius: Theme.rControl; color: rowHover.hovered ? Theme.panelBg : "transparent" }
                            HoverHandler { id: rowHover }
                            Icon {
                                id: sourceIcon
                                anchors.left: parent.left; anchors.leftMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter
                                name: sourceRow.modelData.isDir ? "folder" : "film"
                                size: 14
                                color: sourceRow.modelData.exists ? Theme.textSecondary : Theme.warning
                            }
                            Column {
                                anchors.left: sourceIcon.right; anchors.leftMargin: Theme.s2
                                anchors.right: sourceSize.left; anchors.rightMargin: Theme.s2
                                anchors.verticalCenter: parent.verticalCenter
                                Text { width: parent.width; text: sourceRow.modelData.name; elide: Text.ElideMiddle; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
                                Text {
                                    width: parent.width; elide: Text.ElideMiddle
                                    text: !sourceRow.modelData.exists ? qsTr("Not found · %1").arg(sourceRow.modelData.folder) : sourceRow.modelData.folder
                                    color: sourceRow.modelData.exists ? Theme.textMuted : Theme.warning
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                }
                            }
                            Text {
                                id: sourceSize
                                anchors.right: removeSource.left; anchors.rightMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter
                                text: sourceRow.modelData.isDir ? qsTr("Folder") : root.bytesText(sourceRow.modelData.bytes)
                                color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                            }
                            IconButton {
                                id: removeSource
                                objectName: "mediaInputRemove:" + sourceRow.index
                                anchors.right: parent.right; anchors.rightMargin: Theme.s1; anchors.verticalCenter: parent.verticalCenter
                                iconName: "x"
                                tip: qsTr("Remove %1 from the list").arg(sourceRow.modelData.name)
                                Accessible.name: qsTr("Remove %1").arg(sourceRow.modelData.name)
                                onClicked: root.removePath(sourceRow.modelData.path)
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: root.chosen.length === 0
                            text: qsTr("Add files or a folder. Camera-card folders are scanned recursively.")
                            color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                        }
                    }
                }
                ComboField { id: mode; objectName: "mediaHandling"; tip: qsTr("Whether files are left where they are, copied, or moved after verifying"); Layout.fillWidth: true; visible: setup.importMode; caption: qsTr("Media handling"); model: [qsTr("Leave in place"), qsTr("Copy to destination"), qsTr("Move after verified copy")] }
                Caption { visible: setup.importMode && mode.currentIndex === 2; text: qsTr("Move removes each original only after its copy has passed SHA-256 verification."); color: Theme.warning }
                Caption { visible: setup.cloneMode; text: qsTr("Clone copies all files, including card metadata, and verifies each destination. Originals are kept.") }
                ComboField { id: preset; tip: qsTr("Codec and size the proxy files are encoded at"); Layout.fillWidth: true; visible: setup.proxyMode; caption: qsTr("Preset"); currentIndex: 1; model: [qsTr("H.264 · up to 540p"), qsTr("H.264 · up to 720p"), qsTr("ProRes Proxy · up to 1080p")] }
                ComboField { id: acceleration; Layout.fillWidth: true; visible: setup.reframeMode || (setup.proxyMode && preset.currentIndex === 2); caption: qsTr("Processing"); model: [qsTr("Auto · GPU with CPU fallback"), qsTr("CPU")]; tip: qsTr("Use the GPU when available; retry on the CPU if it fails") }
                Caption { visible: setup.reframeMode; text: qsTr("Create and import a new ProRes 422 HQ clip with a fixed viewing direction. The original is kept.") }
                ComboField { id: projection; tip: qsTr("The layout of the original 360° image"); Layout.fillWidth: true; visible: setup.reframeMode; caption: qsTr("Source projection"); model: [qsTr("Equirectangular"), qsTr("Dual fisheye")] }
                ComboField { id: resolution; tip: qsTr("The size of the new flat video"); Layout.fillWidth: true; visible: setup.reframeMode; caption: qsTr("Output size"); model: ["1920 × 1080", "3840 × 2160"] }
                SliderField { id: yaw; tip: qsTr("Turn the viewing direction horizontally, in degrees"); Layout.fillWidth: true; visible: setup.reframeMode; label: qsTr("Look left / right"); from: -180; to: 180; stepSize: 1; value: 0; onEdited: function(v) { value=v } }
                SliderField { id: pitch; tip: qsTr("Tilt the viewing direction vertically, in degrees"); Layout.fillWidth: true; visible: setup.reframeMode; label: qsTr("Look up / down"); from: -90; to: 90; stepSize: 1; value: 0; onEdited: function(v) { value=v } }
                SliderField { id: roll; tip: qsTr("Rotate the view around its centre, in degrees"); Layout.fillWidth: true; visible: setup.reframeMode; label: qsTr("Roll"); from: -180; to: 180; stepSize: 1; value: 0; onEdited: function(v) { value=v } }
                SliderField { id: fov; tip: qsTr("How much of the scene the view includes horizontally, in degrees"); Layout.fillWidth: true; visible: setup.reframeMode; label: qsTr("Horizontal field of view"); from: 10; to: 170; stepSize: 1; value: 90; onEdited: function(v) { value=v } }
                RowLayout {
                    visible: setup.cloneMode || setup.proxyMode || setup.reframeMode || (setup.importMode && mode.currentIndex > 0)
                    LineEdit { id: destination; objectName: "mediaDestination"; tip: qsTr("Folder the copied, cloned or proxy files are written to"); Layout.fillWidth: true; hint: qsTr("Destination folder"); Accessible.name: qsTr("Destination folder") }
                    ToolButton { text: qsTr("Browse…"); onClicked: root.engine.mediaTasks.browse("media-destination") }
                }
                Caption {
                    objectName: "mediaDestinationOnSource"
                    visible: root.destinationOnSource
                    text: qsTr("This folder is on the drive being imported from. Choose a folder on another disk.")
                    color: Theme.warning
                }
                RowLayout {
                    visible: setup.cloneMode
                    LineEdit { id: destination2; tip: qsTr("A second folder to clone to, verified the same way"); Layout.fillWidth: true; hint: qsTr("Optional second destination") }
                    ToolButton { text: qsTr("Browse…"); onClicked: root.engine.mediaTasks.browse("media-destination2") }
                }
                Toggle { id: hierarchy; tip: qsTr("Recreate the source folder structure under the destination"); label: qsTr("Preserve folder hierarchy"); checked: true; visible: setup.cloneMode || (setup.importMode && mode.currentIndex > 0) }
                LineEdit { id: pattern; tip: qsTr("How the new filenames are built from {name}, {index} and {date}"); Layout.fillWidth: true; visible: setup.proxyMode || setup.reframeMode || (setup.importMode && mode.currentIndex > 0); hint: qsTr("Filename pattern: {name}, {index}, {date}") }
                Caption { visible: pattern.visible; text: qsTr("Use {name}, {index}, {date}; the extension is added automatically. Existing files are kept.") }
                ComboField { id: duplicates; tip: qsTr("What is compared to decide a file is already in the project"); visible: setup.importMode || setup.kind === "duplicates"; Layout.fillWidth: true; caption: qsTr("Duplicate check"); model: [qsTr("Path"), qsTr("Size"), qsTr("SHA-256 checksum")] }
                Caption { visible: duplicates.visible && duplicates.currentIndex === 1; text: qsTr("Files of equal size may have different content. Choose SHA-256 to compare contents.") }
                Caption { visible: setup.kind === "verify"; text: qsTr("Calculate SHA-256 hashes and compare against any previous verification stored in this project.") }
                ToolButton {
                    objectName: "mediaClipDetails"
                    visible: setup.importMode
                    showLabel: true
                    iconName: root.detailsOpen ? "chevron-down" : "chevron-right"
                    text: qsTr("Clip details")
                    tip: qsTr("Tags, reel, scene, input LUT, input space and notes given to every clip in this import")
                    onClicked: root.detailsOpen = !root.detailsOpen
                }
                LineEdit { id: tagField; tip: qsTr("Tags given to every clip brought in by this import"); Layout.fillWidth: true; visible: setup.importMode && root.detailsOpen; hint: qsTr("Tags, separated by commas") }
                RowLayout {
                    visible: setup.importMode && root.detailsOpen
                    LineEdit { id: reel; tip: qsTr("Reel logged on every clip brought in by this import"); Layout.fillWidth: true; hint: qsTr("Reel") }
                    LineEdit { id: scene; tip: qsTr("Scene logged on every clip brought in by this import"); Layout.fillWidth: true; hint: qsTr("Scene") }
                }
                RowLayout {
                    visible: setup.importMode && root.detailsOpen
                    LineEdit { id: inputLut; tip: qsTr("A .cube LUT applied to everything brought in by this import"); Layout.fillWidth: true; hint: qsTr("Optional input LUT (.cube)") }
                    ToolButton { text: qsTr("Browse…"); onClicked: root.engine.mediaTasks.browse("media-input-lut", "file") }
                }
                ComboField { id: inputSpace; tip: qsTr("How Y\u2032CbCr video is read when the file states nothing"); visible: setup.importMode && root.detailsOpen; Layout.fillWidth: true; caption: qsTr("Input space"); model: [qsTr("Use file metadata"), "Rec.601", "Rec.709", "Rec.2020"] }
                LineEdit { id: notes; tip: qsTr("Note stored on every clip brought in by this import"); Layout.fillWidth: true; visible: setup.importMode && root.detailsOpen; hint: qsTr("Import notes") }
            }
        }
        Component.onCompleted: standardButton(C.Dialog.Ok).enabled = Qt.binding(function () {
            return root.paths().length > 0 && !root.destinationOnSource
                && (!(setup.cloneMode || setup.proxyMode || setup.reframeMode || root.copying) || destination.text.trim() !== "")
        })
    }
    ModalDialog {
        id: tasks
        objectName: "mediaTasksDialog"
        title: qsTr("Media Tasks")
        standardButtons: C.Dialog.Close
        ColumnLayout {
            width: parent.width
            spacing: Theme.s2
            Caption { text: root.engine.mediaTasks.running ? qsTr("Working in the background. You can close this window and keep editing.") : qsTr("Completed tasks keep their reports for this session.") }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: Theme.hMediaTasks
                clip: true; spacing: Theme.s2
                model: root.engine.mediaTasks.jobs.slice().reverse()
                C.ScrollBar.vertical: ScrollBar { atRest: true }
                delegate: Rectangle {
                    id: taskRow
                    required property var modelData
                    width: ListView.view.width
                    height: content.implicitHeight + Theme.s2 * 2
                    color: Theme.controlBg; radius: Theme.rControl
                    ColumnLayout {
                        id: content
                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: Theme.s2
                        RowLayout {
                            Caption { text: taskRow.modelData.kind.toUpperCase() + " · " + taskRow.modelData.label; elide: Text.ElideMiddle; maximumLineCount: 1 }
                            Text { text: taskRow.modelData.state; color: taskRow.modelData.state === "failed" || taskRow.modelData.state === "partial" ? Theme.warning : Theme.textSecondary; font.pixelSize: Theme.fsLabel }
                        }
                        Caption { text: taskRow.modelData.message || qsTr("Queued") }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Theme.s1; color: Theme.border; Rectangle { width: parent.width * taskRow.modelData.percent / 100; height: parent.height; color: Theme.accent } }
                        RowLayout {
                            ToolButton { text: qsTr("Cancel"); visible: taskRow.modelData.state === "running" || taskRow.modelData.state === "queued"; onClicked: root.engine.mediaTasks.cancel(taskRow.modelData.id) }
                            ToolButton { text: qsTr("Retry"); visible: ["failed", "partial", "cancelled"].indexOf(taskRow.modelData.state) >= 0; onClicked: root.engine.mediaTasks.retry(taskRow.modelData.id) }
                            ToolButton { text: qsTr("Report…"); enabled: (taskRow.modelData.report || []).length > 0; onClicked: { root.reportId = taskRow.modelData.id; report.open() } }
                            ToolButton { text: qsTr("Save CSV…"); enabled: (taskRow.modelData.report || []).length > 0; onClicked: root.engine.mediaTasks.browse("media-report:" + taskRow.modelData.id, "save") }
                        }
                    }
                }
                Text { anchors.centerIn: parent; visible: root.engine.mediaTasks.jobs.length === 0; text: qsTr("No media tasks yet"); color: Theme.textMuted; font.pixelSize: Theme.fsBase }
            }
            ToolButton { text: qsTr("Clear Finished Tasks"); onClicked: root.engine.mediaTasks.clearFinished() }
        }
    }
    ModalDialog {
        id: report
        title: qsTr("Media Task Report")
        standardButtons: C.Dialog.Close
        C.ScrollView {
            width: parent.width; implicitHeight: Theme.hMediaTasks; contentWidth: availableWidth
            C.ScrollBar.vertical: ScrollBar { atRest: true }
            MultiEdit {
                width: parent.width
                readOnly: true
                text: {
                    var dependency = root.engine.mediaTasks.jobs
                    return (root.engine.mediaTasks.record(root.reportId).report || []).map(function (r) {
                        return r.state.toUpperCase() + " · " + r.source + (r.destination ? "\n→ " + r.destination : "") + (r.message ? "\n" + r.message : "") + (r.sha256 ? "\nSHA-256 " + r.sha256 : "")
                    }).join("\n\n")
                }
            }
        }
    }
}
