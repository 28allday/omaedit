pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "lutBrowser"
    title: qsTr("LUT Browser")
    width: Math.min(960, parent ? parent.width - Theme.s5 * 2 : 960)
    height: Math.min(640, parent ? parent.height - Theme.s5 * 2 : 640)
    standardButtons: C.Dialog.NoButton

    property var luts: []
    property var lut: ({})
    property var preview: ({})
    property string targetKey: ""
    property string clipName: ""
    property string nodeName: ""
    property bool editable: false
    property string selectedPath: ""
    property real amount: 1
    property string folder: ""
    property bool favoritesOnly: false
    property bool applying: false
    property string applyError: ""
    signal previewRequested(string path, real intensity)
    signal applyRequested(string path, real intensity)
    signal favoriteToggled(string path, bool on)
    signal folderRequested()
    signal fileRequested()
    signal rescanRequested()
    signal previewClosed()

    readonly property var folders: {
        var seen = {}, list = []
        for (var i = 0; i < luts.length; ++i) {
            var item = luts[i]
            if (!seen[item.folderPath]) { seen[item.folderPath] = true; list.push({ path: item.folderPath, name: item.folder }) }
        }
        list.sort(function (a, b) { return a.name.localeCompare(b.name) })
        return list
    }
    readonly property var rows: {
        var query = search.text.trim().toLowerCase()
        var list = luts.filter(function (item) {
            return (!root.favoritesOnly || item.favorite === true)
                && (root.folder === "" || item.folderPath === root.folder)
                && (query === "" || (item.name + " " + item.folder + " " + item.path).toLowerCase().indexOf(query) >= 0)
        })
        return list
    }
    readonly property bool previewMatches: preview.path === selectedPath && Math.abs((preview.intensity || 0) - amount) < 0.000001
    readonly property bool ready: previewMatches && preview.ready === true && !previewDelay.running
    readonly property string selectedName: selectedPath === "" ? qsTr("None") : selectedPath.split("/").pop()
    function select(path) { selectedPath = path; refreshPreview() }
    function refreshPreview() { applyError = ""; if (visible) previewDelay.restart() }
    function openForNode() {
        selectedPath = lut.path || ""
        amount = lut.intensity !== undefined ? lut.intensity : 1
        applying = false
        applyError = ""
        open()
        refreshPreview()
    }
    function finishApply(changed, message) {
        applying = false
        if (changed || message === "LUT is unchanged") accept()
        else applyError = message
    }
    onTargetKeyChanged: close()
    onEditableChanged: if (!editable) close()
    onClosed: { previewDelay.stop(); applying = false; root.previewClosed() }
    Timer {
        id: previewDelay
        interval: 180
        onTriggered: root.previewRequested(root.selectedPath, root.amount)
    }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text {
            Layout.fillWidth: true
            text: root.clipName + " · " + root.nodeName
            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            elide: Text.ElideRight
        }
        RowLayout {
            Layout.fillWidth: true
            SearchField { id: search; objectName: "lutSearch"; Layout.fillWidth: true; placeholder: qsTr("Search LUTs…"); tip: qsTr("Filter the LUT list by file or folder name") }
            ToolButton {
                objectName: "lutFavoritesOnly"
                iconName: "star"; showLabel: true; text: qsTr("Favourites")
                checkable: true; checked: root.favoritesOnly
                onClicked: root.favoritesOnly = !root.favoritesOnly
            }
            IconButton { objectName: "lutRescan"; iconName: "refresh-cw"; text: qsTr("Refresh LUT folders"); onClicked: { root.rescanRequested(); root.refreshPreview() } }
        }
        ComboField {
            objectName: "lutFolderFilter"
            Layout.fillWidth: true
            model: [qsTr("All folders")].concat(root.folders.map(function (f) { return f.name }))
            currentIndex: [""].concat(root.folders.map(function (f) { return f.path })).indexOf(root.folder)
            tip: qsTr("Limit the list to LUTs from one scanned folder")
            onActivated: function (i) { root.folder = i === 0 ? "" : root.folders[i - 1].path }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s3
            Rectangle {
                Layout.preferredWidth: root.width * 0.36
                Layout.fillHeight: true
                color: Theme.panelBg; radius: Theme.rControl
                border.width: Theme.hairline; border.color: Theme.border
                ListView {
                    id: list
                    objectName: "lutList"
                    anchors.fill: parent; anchors.margins: Theme.hairline
                    clip: true; model: root.rows
                    activeFocusOnTab: true
                    Keys.onDownPressed: { currentIndex = Math.min(count - 1, currentIndex + 1); if (currentIndex >= 0) root.select(root.rows[currentIndex].path) }
                    Keys.onUpPressed: { currentIndex = Math.max(0, currentIndex - 1); if (count > 0) root.select(root.rows[currentIndex].path) }
                    Keys.onReturnPressed: if (currentIndex >= 0 && currentIndex < count) root.select(root.rows[currentIndex].path)
                    C.ScrollBar.vertical: ScrollBar { atRest: true }
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        width: list.width; height: 60
                        objectName: "lutItem:" + modelData.path
                        color: modelData.path === root.selectedPath ? Theme.controlBg : hover.containsMouse ? Theme.panelRaised : "transparent"
                        border.width: modelData.path === root.selectedPath ? Theme.hairline : 0
                        border.color: Theme.accent
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.name + " · " + modelData.folder
                        MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; onClicked: { list.currentIndex = row.index; list.forceActiveFocus(); root.select(row.modelData.path) } }
                        Column {
                            anchors.left: parent.left; anchors.leftMargin: Theme.s2
                            anchors.right: star.left; anchors.rightMargin: Theme.s1
                            anchors.verticalCenter: parent.verticalCenter; spacing: Theme.s1
                            Text {
                                width: parent.width; text: row.modelData.name
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                                color: Theme.textPrimary; elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width; text: row.modelData.missing ? qsTr("Missing · %1").arg(row.modelData.folder) : row.modelData.folder
                                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                                color: row.modelData.missing ? Theme.warning : Theme.textMuted; elide: Text.ElideMiddle
                            }
                        }
                        IconButton {
                            id: star
                            objectName: "lutFavorite:" + row.modelData.path
                            anchors.right: parent.right; anchors.rightMargin: Theme.s1; anchors.verticalCenter: parent.verticalCenter
                            iconName: "star"; iconColor: row.modelData.favorite ? Theme.accent : Theme.textMuted
                            text: row.modelData.favorite ? qsTr("Remove favourite") : qsTr("Favourite LUT")
                            onClicked: root.favoriteToggled(row.modelData.path, !row.modelData.favorite)
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent; width: parent.width - Theme.s4
                    visible: root.rows.length === 0
                    text: root.luts.length === 0 ? qsTr("No LUTs found. Choose a LUT folder or locate a file.") : qsTr("No LUTs match this search.")
                    wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; color: Theme.textMuted
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: Theme.s1
                Text {
                    Layout.fillWidth: true; text: root.selectedName
                    color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                    elide: Text.ElideMiddle
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Repeater {
                        model: 2
                        delegate: ColumnLayout {
                            required property int index
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                            Text { text: parent.index === 0 ? qsTr("Current") : qsTr("Preview"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                            Rectangle {
                                Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.windowBg
                                Image {
                                    objectName: parent.parent.index === 0 ? "lutBeforeImage" : "lutAfterImage"
                                    anchors.fill: parent; fillMode: Image.PreserveAspectFit; cache: false
                                    source: root.ready ? (parent.parent.index === 0 ? root.preview.before || "" : root.preview.after || "") : ""
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.selectedPath
                    elide: Text.ElideMiddle; color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                }
                SliderField {
                    objectName: "lutPreviewIntensity"
                    tip: qsTr("How strongly the previewed LUT is blended in, 0-100%")
                    Layout.fillWidth: true; label: qsTr("Intensity"); labelWidth: 64
                    from: 0; to: 100; stepSize: 1; decimals: 0; suffix: "%"
                    value: root.amount * 100; enabled: root.selectedPath !== "" && !root.applying
                    onEdited: function (v) { root.amount = v / 100; root.refreshPreview() }
                }
                Text {
                    Layout.fillWidth: true; Layout.minimumHeight: 38
                    text: root.applyError !== "" ? root.applyError : !root.previewMatches || root.preview.loading === true || previewDelay.running
                        ? qsTr("Preparing preview…") : root.preview.error || qsTr("Preview only. Apply keeps this LUT on the selected layer.")
                    wrapMode: Text.WordWrap; color: root.applyError !== "" || root.preview.error ? Theme.warning : Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                }
            }
        }
    }
    footer: Item {
        implicitHeight: buttons.implicitHeight + Theme.s3 * 2
        RowLayout {
            id: buttons
            anchors.fill: parent; anchors.margins: Theme.s3
            spacing: Theme.s1
            ToolButton { objectName: "lutChooseFolder"; text: qsTr("LUT Folder…"); iconName: "folder"; showLabel: true; onClicked: root.folderRequested() }
            ToolButton { objectName: "lutLocate"; text: qsTr("Locate LUT…"); onClicked: root.fileRequested() }
            ToolButton { objectName: "lutNone"; text: qsTr("None"); onClicked: root.select("") }
            Item { Layout.fillWidth: true }
            ToolButton { objectName: "lutCancel"; text: qsTr("Cancel"); enabled: !root.applying; onClicked: root.reject() }
            ToolButton {
                objectName: "lutApply"; text: root.applying ? qsTr("Applying…") : qsTr("Apply")
                enabled: root.editable && root.ready && !root.applying
                onClicked: { root.applying = true; root.applyRequested(root.selectedPath, root.amount) }
            }
        }
    }
}
