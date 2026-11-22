import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import OmaEdit.Dialogs
import OmaEdit.Ui as Ui

Item {
    id: root
    objectName: "fileBrowser"
    required property var owner
    property var request: ({})
    property url folder
    property var selection: []
    property var history: []
    property int anchorIndex: -1
    property string error: ""
    property var overwriteTarget: ({})
    // The drives (Backend.volumes), and one being mounted from the places.
    readonly property var volumes: owner && owner.volumes ? owner.volumes : null
    property string mountingObject: ""
    readonly property bool saving: request.mode === "save"
    readonly property bool directoryOnly: request.mode === "directory"
    readonly property bool multiple: request.mode === "multiple"
    readonly property string actionText: saving ? qsTr("Save") : directoryOnly ? qsTr("Choose Folder") : qsTr("Open")

    function clearSelection() {
        selection = []; anchorIndex = -1; list.currentIndex = -1
    }
    function go(path, remember) {
        const info = host.pathInfo(path.toString(), folder)
        if (info.error || !info.directory || !info.readable) {
            error = info.error || qsTr("This folder is unavailable or cannot be read.")
            return false
        }
        if (remember && folder.toString() !== info.url.toString()) history = history.concat([folder])
        folder = info.url
        location.text = info.path
        search.text = ""
        clearSelection()
        error = ""
        return true
    }
    function back() {
        if (!history.length) return
        const target = history[history.length - 1]
        history = history.slice(0, -1)
        go(target, false)
    }
    function choose(index, modifiers) {
        if (index < 0 || index >= entries.count) return
        const url = entries.get(index, "fileUrl").toString()
        const isDir = entries.get(index, "fileIsDir")
        list.currentIndex = index
        error = ""
        if (multiple && (modifiers & Qt.ShiftModifier) && anchorIndex >= 0 && !isDir) {
            let picked = []
            for (let i = Math.min(index, anchorIndex); i <= Math.max(index, anchorIndex); ++i)
                if (!entries.get(i, "fileIsDir")) picked.push(entries.get(i, "fileUrl").toString())
            selection = picked
        } else if (multiple && (modifiers & Qt.ControlModifier) && !isDir) {
            if (selection.length && host.pathInfo(selection[0], folder).directory) selection = []
            selection = selection.indexOf(url) >= 0 ? selection.filter(p => p !== url) : selection.concat([url])
            anchorIndex = index
        } else { selection = [url]; anchorIndex = index }
        if (saving && !isDir) nameField.text = entries.get(index, "fileName")
    }
    function activate(index) {
        if (index < 0 || index >= entries.count) return
        if (entries.get(index, "fileIsDir")) go(entries.get(index, "fileUrl"), true)
        else { choose(index, 0); submit() }
    }
    function submit() {
        error = ""
        if (saving) {
            if (!nameField.text.length) { error = qsTr("Enter a filename."); nameField.forceActiveFocus(); return }
            const target = host.pathInfo(nameField.text, folder, request.suffix)
            if (target.directory) { go(target.url, true); return }
            if (target.error || !target.parentExists || !target.writable) {
                error = target.error || qsTr("This location cannot be written to."); return
            }
            // The same resolved URL is confirmed and sent to the writer.
            if (target.exists) { overwriteTarget = target; overwrite.open(); return }
            host.accept(request.id, [target.url])
        } else if (directoryOnly) {
            host.accept(request.id, selection.length ? selection : [folder])
        } else if (selection.length) {
            const first = host.pathInfo(selection[0], folder)
            if (first.directory) go(first.url, true)
            else host.accept(request.id, selection)
        }
    }

    Connections {
        target: root.volumes
        function onDiskActionFinished(object, action, ok, message, mountPoint) {
            if (action !== "mount" || object !== root.mountingObject) return
            root.mountingObject = ""
            if (ok) root.go(mountPoint, true)
            else root.error = message
        }
    }
    FileChooserHost {
        id: host
        objectName: "fileChooserHost"
        owner: root.owner
        onDismissed: { overwrite.close(); browser.close() }
        onRequested: function(options) {
            root.request = options
            root.history = []; root.selection = []; root.error = ""
            filter.currentIndex = 0; hidden.checked = false
            root.go(options.folder, false)
            nameField.text = options.file.toString() ? host.pathInfo(options.file.toString(), options.folder).name : ""
            browser.open()
            Qt.callLater(function() { if (root.saving) { nameField.forceActiveFocus(); nameField.selectAll() } else list.forceActiveFocus() })
        }
    }
    FolderListModel {
        id: folderEntries
        objectName: "fileBrowserFolderEntries"
        folder: browser.visible ? root.folder : ""
        showFiles: !root.directoryOnly
        showDotAndDotDot: false
        showHidden: hidden.checked
        showDirsFirst: true
        sortField: FolderListModel.Name
        sortCaseSensitive: false
        onStatusChanged: root.clearSelection()
    }
    FileBrowserFilterModel {
        id: entries
        objectName: "fileBrowserEntries"
        sourceModel: folderEntries
        query: search.text
        patterns: root.request.filterPatterns && filter.currentIndex >= 0
                  ? root.request.filterPatterns[filter.currentIndex] || [] : []
        onQueryChanged: root.clearSelection()
        onPatternsChanged: root.clearSelection()
        onCountChanged: root.clearSelection()
    }

    Ui.ModalDialog {
        id: browser
        objectName: "internalFileBrowser"
        title: root.request.title || qsTr("Choose a file")
        width: Math.min(Ui.Theme.wFileBrowser, parent ? parent.width - Ui.Theme.s5 * 2 : Ui.Theme.wFileBrowser)
        height: Math.min(Ui.Theme.hFileBrowser, parent ? parent.height - Ui.Theme.s5 * 2 : Ui.Theme.hFileBrowser)
        closePolicy: C.Popup.CloseOnEscape
        onRejected: host.cancel(root.request.id)
        footer: Item {}

        contentItem: ColumnLayout {
            spacing: Ui.Theme.s2
            RowLayout {
                Layout.fillWidth: true
                Ui.IconButton { iconName: "chevron-left"; text: qsTr("Back"); enabled: root.history.length > 0; onClicked: root.back() }
                Ui.IconButton { iconName: "chevron-up"; text: qsTr("Parent folder"); onClicked: root.go("..", true) }
                Ui.LineEdit {
                    id: location
                    objectName: "fileBrowserLocation"
                    Layout.fillWidth: true
                    hint: qsTr("Folder path")
                    tip: qsTr("Enter a folder path, then press Enter. Ctrl+L focuses this field.")
                    onAccepted: { if (root.go(text, true)) list.forceActiveFocus() }
                }
                Ui.ToolButton { text: qsTr("Go"); tip: qsTr("Open the folder entered in the path field."); onClicked: root.go(location.text, true) }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Ui.Theme.s2
                Ui.SearchField {
                    id: search
                    objectName: "fileBrowserSearch"
                    Layout.fillWidth: true
                    placeholder: qsTr("Search this folder…")
                    tip: qsTr("Search file and folder names here. Ctrl+F focuses search; subfolders are not searched.")
                }
                Ui.ComboField {
                    id: filter
                    objectName: "fileBrowserFilter"
                    Layout.preferredWidth: browser.availableWidth * 0.42
                    visible: !root.directoryOnly
                    model: root.request.filters || []
                    tip: qsTr("Show all files, a media category or one file format. Folders remain available for navigation.")
                    Accessible.name: qsTr("File type")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Ui.Theme.s2
                Rectangle {
                    Layout.preferredWidth: Ui.Theme.wFilePlaces
                    Layout.fillHeight: true
                    color: Ui.Theme.panelBg
                    radius: Ui.Theme.rControl
                    C.ScrollView {
                        C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                        anchors.fill: parent
                        contentWidth: availableWidth
                        Column {
                            width: parent.width
                            Repeater {
                                model: host.locations().concat(root.volumes ? root.volumes.drives : [])
                                delegate: Ui.ToolButton {
                                    required property var modelData
                                    readonly property bool mounted: modelData.mounted !== false
                                    objectName: "fileBrowserPlace:" + (mounted ? modelData.path : modelData.object)
                                    width: parent.width
                                    text: mounted ? modelData.name : modelData.name + " (" + modelData.totalLabel + ")"
                                    tip: modelData.tip || modelData.path
                                    // A drive not mounted is drawn dimmed and mounts when clicked.
                                    opacity: mounted ? 1.0 : Ui.Theme.disabledOpacity
                                    onClicked: {
                                        if (mounted) { root.go(modelData.path, true); return }
                                        root.error = ""
                                        root.mountingObject = modelData.object
                                        root.volumes.mount(modelData.object)
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Ui.Theme.panelBg
                    radius: Ui.Theme.rControl
                    border.color: list.activeFocus ? Ui.Theme.accent : Ui.Theme.border
                    border.width: Ui.Theme.hairline
                    ListView {
                        id: list
                        objectName: "fileBrowserList"
                        anchors.fill: parent
                        anchors.margins: Ui.Theme.hairline
                        clip: true
                        model: entries
                        enabled: folderEntries.status === FolderListModel.Ready
                        activeFocusOnTab: true
                        keyNavigationEnabled: false
                        C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.submit(); event.accepted = true }
                            else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                                const next = Math.max(0, Math.min(count - 1, currentIndex + (event.key === Qt.Key_Down ? 1 : -1)))
                                if (count) { root.choose(next, event.modifiers); positionViewAtIndex(next, ListView.Contain) }
                                event.accepted = true
                            } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier) && root.multiple) {
                                let all = []; for (let i = 0; i < count; ++i) if (!entries.get(i, "fileIsDir")) all.push(entries.get(i, "fileUrl").toString())
                                root.selection = all; event.accepted = true
                            }
                        }
                        delegate: C.ItemDelegate {
                            id: row
                            required property int index
                            required property string fileName
                            required property url fileUrl
                            required property bool fileIsDir
                            objectName: "fileBrowserRow" + index
                            width: list.width
                            height: Ui.Theme.hControl + Ui.Theme.s2
                            text: fileName
                            highlighted: root.selection.indexOf(fileUrl.toString()) >= 0
                            focusPolicy: Qt.NoFocus
                            Accessible.name: fileName
                            Accessible.description: fileIsDir ? qsTr("Folder. Double-click to open.") : qsTr("File. Double-click to choose.")
                            Accessible.selected: highlighted
                            Ui.Tooltip { text: row.fileName; visible: row.hovered }
                            background: Rectangle { color: row.highlighted ? Ui.Theme.controlBg : row.hovered ? Ui.Theme.panelRaised : Ui.Theme.panelBg; border.width: row.highlighted ? Ui.Theme.hairline : 0; border.color: Ui.Theme.accent }
                            contentItem: RowLayout {
                                spacing: Ui.Theme.s2
                                Ui.Icon { name: row.fileIsDir ? "folder" : "film"; color: row.highlighted ? Ui.Theme.accent : Ui.Theme.textSecondary; size: Ui.Theme.szIcon }
                                Text { Layout.fillWidth: true; text: row.fileName; elide: Text.ElideMiddle; color: Ui.Theme.textPrimary; font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsBase }
                                Text { text: row.fileIsDir ? qsTr("Folder") : ""; color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: function(mouse) { list.forceActiveFocus(); root.choose(row.index, mouse.modifiers) }
                                onDoubleClicked: root.activate(row.index)
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: folderEntries.status === FolderListModel.Loading ? qsTr("Loading…") : qsTr("No matches. Try another name or file type.")
                        width: parent.width - Ui.Theme.s3 * 2
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        visible: folderEntries.status === FolderListModel.Loading || entries.count === 0
                        color: Ui.Theme.textSecondary
                        font.pixelSize: Ui.Theme.fsBase
                    }
                }
            }
            RowLayout {
                visible: root.saving
                Text { text: qsTr("File name"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsBase }
                Ui.LineEdit { id: nameField; objectName: "fileBrowserName"; Layout.fillWidth: true; hint: qsTr("Filename"); tip: qsTr("Name the file to save. The file extension is added when needed."); onAccepted: root.submit() }
            }
            Text { Layout.fillWidth: true; visible: root.error.length > 0; text: root.error; color: Ui.Theme.dangerText; wrapMode: Text.Wrap; font.pixelSize: Ui.Theme.fsBase; Accessible.role: Accessible.AlertMessage }
            RowLayout {
                Layout.fillWidth: true
                Ui.ToolButton { id: hidden; text: qsTr("Hidden files"); tip: qsTr("Show files and folders whose names start with a dot."); checkable: true }
                Item { Layout.fillWidth: true }
                Text { text: root.multiple && root.selection.length ? qsTr("%1 selected").arg(root.selection.length) : ""; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
                Ui.ToolButton { objectName: "fileBrowserCancel"; text: qsTr("Cancel"); tip: qsTr("Close the browser without choosing a file."); onClicked: browser.reject() }
                Ui.ToolButton { objectName: "fileBrowserAccept"; text: root.actionText; tip: qsTr("Use the selected file or folder."); enabled: root.saving ? nameField.text.length > 0 : root.directoryOnly || root.selection.length > 0; onClicked: root.submit() }
            }
        }
        Shortcut { sequence: "Ctrl+F"; enabled: browser.visible && !overwrite.visible; onActivated: search.focusInput() }
        Shortcut { sequence: "Ctrl+L"; enabled: browser.visible && !overwrite.visible; onActivated: { location.forceActiveFocus(); location.selectAll() } }
        Shortcut { sequence: "Alt+Up"; enabled: browser.visible && !overwrite.visible; onActivated: root.go("..", true) }
        Shortcut { sequence: "Alt+Left"; enabled: browser.visible && !overwrite.visible; onActivated: root.back() }
    }
    Ui.MessageDialog {
        id: overwrite
        objectName: "fileBrowserOverwrite"
        title: qsTr("Replace existing file?")
        standardButtons: C.Dialog.Yes | C.Dialog.No
        closePolicy: C.Popup.CloseOnEscape
        message: qsTr("%1 already exists. Replace it?").arg(root.overwriteTarget.path || "")
        onAccepted: host.accept(root.request.id, [root.overwriteTarget.url])
    }
}
