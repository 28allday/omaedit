import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.ModalDialog {
    id: root
    objectName: "projectSafetyDialog"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(620, parent ? parent.height - 40 : 620)
    modal: true
    background: Rectangle { color: Ui.Theme.panelBg; radius: Ui.Theme.rMenu; border.color: Ui.Theme.border; border.width: 1 }
    title: qsTr("Project safety")
    standardButtons: C.Dialog.Close
    property int page: 0
    property int selectedCopy: -1
    readonly property var copies: page === 0 ? backend.recoveryCopies : backend.projectBackups
    readonly property var currentCopy: selectedCopy >= 0 && selectedCopy < copies.length ? copies[selectedCopy] : null
    function openFor(which) {
        page = which === undefined ? 0 : which;
        selectedCopy = -1;
        archiveName.text = backend.projectName + "-archive";
        backend.refreshProjectSafety();
        open();
    }
    onPageChanged: selectedCopy = -1
    Connections {
        target: backend
        function onArchiveFolderChosen(folder) {
            archiveFolder.text = folder;
        }
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2
        RowLayout {
            Repeater {
                model: [qsTr("Recovery"), qsTr("Backups"), qsTr("Archive")]
                FlatButton {
                    required property int index
                    required property string modelData
                    text: modelData
                    tip: qsTr("Switch between the recovery copies, the backups and the archive tools")
                    checkable: true
                    checked: root.page === index
                    highlighted: checked
                    quiet: true
                    onClicked: root.page = index
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }
        C.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textSecondary
            text: root.page === 0 ? qsTr("Autosave keeps separate recovery copies, including untitled work. Restoring opens a copy for Save As.") : root.page === 1 ? qsTr("Each manual save preserves the previous saved version. Open a backup as a copy to inspect it before saving.") : qsTr("Create a portable project folder with its media, grades and nested sequences. The current edit stays open.")
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: root.page < 2
            ListView {
                id: copiesList
                objectName: "safetyCopies"
                model: root.copies
                spacing: Ui.Theme.s1
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: copiesList.width
                    height: 74
                    color: index === root.selectedCopy ? Ui.Theme.controlBg : Ui.Theme.panelBg
                    border.color: index === root.selectedCopy ? Ui.Theme.accent : Ui.Theme.border
                    radius: Ui.Theme.rControl
                    Column {
                        anchors.fill: parent
                        anchors.margins: Ui.Theme.s2
                        spacing: 2
                        Text {
                            width: parent.width
                            text: modelData.name
                            color: Ui.Theme.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: Ui.Theme.fsControl
                        }
                        Text {
                            width: parent.width
                            text: new Date(modelData.time).toLocaleString()
                            color: Ui.Theme.textSecondary
                            elide: Text.ElideRight
                            font.pixelSize: Ui.Theme.fsLabel
                        }
                        Text {
                            width: parent.width
                            text: modelData.error || modelData.origin || modelData.path
                            color: modelData.valid === false ? Ui.Theme.danger : Ui.Theme.textMuted
                            elide: Text.ElideMiddle
                            font.pixelSize: Ui.Theme.fsLabel
                        }
                    }
                    TapHandler {
                        onTapped: root.selectedCopy = index
                    }
                }
                C.Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: root.copies.length === 0
                    text: root.page === 0 ? qsTr("No interrupted-session recovery copies.") : qsTr("No previous saved versions for this project yet.")
                    color: Ui.Theme.textMuted
                }
            }
        }
        RowLayout {
            visible: root.page < 2
            FlatButton {
                objectName: "restoreProjectCopy"
                text: qsTr("Open Copy")
                tip: qsTr("Open the selected copy as a separate unsaved project")
                enabled: root.currentCopy !== null && root.currentCopy.valid !== false
                onClicked: {
                    const path = root.currentCopy.path;
                    root.close();
                    win.project.guardedOpen("recovery:" + path);
                }
            }
            FlatButton {
                text: qsTr("Delete Recovery Copy…")
                tip: qsTr("Permanently remove the selected recovery copy from disk")
                visible: root.page === 0
                enabled: root.currentCopy !== null
                onClicked: {
                    deleteCopy.path = root.currentCopy.path;
                    deleteCopy.name = root.currentCopy.name;
                    deleteCopy.savedAt = root.currentCopy.time;
                    deleteCopy.open();
                }
            }
            Item {
                Layout.fillWidth: true
            }
            FlatButton {
                text: qsTr("Refresh")
                tip: qsTr("Re-read the recovery copies and backups from disk")
                onClicked: backend.refreshProjectSafety()
            }
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            visible: root.page === 2
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: Ui.Theme.s2
                C.Label {
                    text: qsTr("Destination parent folder")
                    color: Ui.Theme.textSecondary
                }
                RowLayout {
                    Layout.fillWidth: true
                    C.TextField {
                        id: archiveFolder
                        objectName: "archiveFolder"
                        Layout.fillWidth: true
                        hoverEnabled: true
                        C.ToolTip.text: qsTr("Existing folder the new archive folder is created inside")
                        C.ToolTip.visible: hovered && !activeFocus
                        C.ToolTip.delay: 450
                        placeholderText: qsTr("Choose a folder…")
                    }
                    FlatButton {
                        text: qsTr("Browse…")
                        tip: qsTr("Pick the destination folder with a file chooser")
                        enabled: !backend.projectArchive.busy
                        onClicked: backend.chooseArchiveFolder()
                    }
                }
                C.Label {
                    text: qsTr("New archive folder name")
                    color: Ui.Theme.textSecondary
                }
                C.TextField {
                    id: archiveName
                    objectName: "archiveName"
                    Layout.fillWidth: true
                    hoverEnabled: true
                    C.ToolTip.text: qsTr("Name of the archive folder that gets created")
                    C.ToolTip.visible: hovered && !activeFocus
                    C.ToolTip.delay: 450
                    text: qsTr("Project-archive")
                }
                C.CheckBox {
                    id: unused
                    text: qsTr("Include unused library media")
                    C.ToolTip.text: qsTr("Copy library media the edit never uses into the archive too")
                    C.ToolTip.visible: hovered
                    C.ToolTip.delay: 450
                    enabled: !backend.projectArchive.busy
                }
                C.CheckBox {
                    id: trim
                    objectName: "archiveTrim"
                    text: qsTr("Trim used sources and keep handles")
                    C.ToolTip.text: qsTr("Copy only the used part of each source instead of whole files")
                    C.ToolTip.visible: hovered
                    C.ToolTip.delay: 450
                    enabled: !unused.checked && !backend.projectArchive.busy
                }
                RowLayout {
                    visible: trim.checked && !unused.checked
                    C.Label {
                        text: qsTr("Handles on each side")
                        color: Ui.Theme.textSecondary
                    }
                    Ui.ValueField {
                        id: handles
                        objectName: "archiveHandles"
                        tip: qsTr("Spare media kept either side of each trimmed clip")
                        fps: backend.fps > 0 ? backend.fps : 25
                        value: Math.round(fps * 2)
                        from: 0
                        to: 25000
                        onEdited: function (v) {
                            value = Math.round(v);
                        }
                    }
                }
                C.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Ui.Theme.textMuted
                    text: qsTr("Whole sources are copied unchanged. Trimming makes lossless video/PCM audio intermediates; retimed, stabilised or incompatible sources stay whole.")
                }
                RowLayout {
                    FlatButton {
                        objectName: "startProjectArchive"
                        text: qsTr("Create Archive")
                        tip: qsTr("Copy the project and its media into the new folder")
                        enabled: !backend.exporting && !backend.projectArchive.busy && archiveFolder.text.length > 0 && archiveName.text.trim().length > 0
                        onClicked: backend.archiveProject(archiveFolder.text, archiveName.text.trim(), unused.checked, trim.checked && !unused.checked, handles.value)
                    }
                    FlatButton {
                        text: qsTr("Cancel Archive")
                        tip: qsTr("Stop the archive in progress and leave it unfinished")
                        enabled: backend.projectArchive.busy
                        onClicked: backend.projectArchive.cancel()
                    }
                }
                C.ProgressBar {
                    Layout.fillWidth: true
                    visible: backend.projectArchive.busy
                    value: (backend.projectArchive.state.progress || 0) / 100
                }
                C.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    text: backend.projectArchive.state.message || ""
                    color: backend.projectArchive.state.error ? Ui.Theme.danger : Ui.Theme.textSecondary
                }
                C.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    text: backend.projectArchive.state.path || ""
                    color: Ui.Theme.textSecondary
                }
            }
        }
        C.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.page === 0
            text: backend.lastRecoverySave ? qsTr("Current project's recovery copy saved at %1.").arg(backend.lastRecoverySave) : qsTr("Autosave interval is set in Preferences. Manual Save keeps your named project up to date.")
            color: Ui.Theme.textMuted
        }
    }
    Ui.MessageDialog {
        id: deleteCopy
        parent: C.Overlay.overlay
        anchors.centerIn: parent
        property string path: ""
        property string name: ""
        property string savedAt: ""
        objectName: "deleteRecoveryCopyDialog"
        title: qsTr("Delete recovery copy?")
        modal: true
        standardButtons: C.Dialog.Yes | C.Dialog.Cancel
        message: qsTr("Delete the recovery copy of “%1”?\nSaved %2. The named project and its backups will remain available.").arg(deleteCopy.name).arg(deleteCopy.savedAt ? new Date(deleteCopy.savedAt).toLocaleString(Qt.locale(), Locale.ShortFormat) : qsTr("at an unknown time"))
        onAccepted: {
            backend.removeRecoveryCopy(path);
            root.selectedCopy = -1;
        }
    }
}
