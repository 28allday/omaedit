pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

Item {
    id: root
    required property var catalog
    property var selection: []
    property var activeRules: ({})
    readonly property bool textActive: editor.visible || removePrompt.visible || locationPrompt.visible || binMenu.visible || locationMenu.visible || watchPrompt.visible || watchMenu.visible || providerPrompt.visible || providerMenu.visible
    property var tree: { var dependency = catalog.bins; return catalog.tree() }
    property var parents: [{id: "", name: qsTr("Library root")}].concat(tree.filter(function (b) { return !b.smart }))
    signal moveRequested(var indexes, string binId)
    signal filterRequested(string kind, string value)
    signal rulesApplied(var rules)
    function openBin(id, smart, assign) {
        var b = catalog.bin(id)
        editor.binId = id; editor.assign = !!assign; editor.smart = id ? !!b.smart : !!smart; editor.filterOnly = false
        nameField.text = b.name || ""
        var p = parents.findIndex(function (entry) { return entry.id === (b.parent || "") })
        parentField.currentIndex = Math.max(0, p)
        rules.reset(b.rules || activeRules)
        editor.open(); nameField.forceActiveFocus()
    }
    function openFilters() {
        editor.binId = ""; editor.assign = false; editor.smart = true; editor.filterOnly = true
        rules.reset(activeRules); editor.open()
    }
    function context(id) { binMenu.binId = id; binMenu.popup() }
    function locationContext(index) { locationMenu.locationIndex = index; locationMenu.popup() }
    signal providerManifestRequested()
    function addProvider() { providerName.text = ""; providerRoot.text = ""; providerPrompt.open() }
    function providerContext(id) { providerMenu.providerId = id; providerMenu.popup() }
    function addWatch() { watchPath.text = ""; watchPrompt.open() }
    function watchContext(id) { watchMenu.watchId = id; watchMenu.popup() }
    function addLocation() { locationPrompt.locationIndex = -1; locationName.text = ""; locationPath.text = ""; locationPrompt.open() }

    ContextMenu {
        id: binMenu
        property string binId: ""
        MenuAction { text: qsTr("Edit / Move Bin…"); onTriggered: root.openBin(binMenu.binId, false, false) }
        MenuAction { text: qsTr("Move Selected Here"); enabled: root.selection.length > 0 && !root.catalog.bin(binMenu.binId).smart; onTriggered: root.moveRequested(root.selection, binMenu.binId) }
        MenuAction { text: qsTr("New Nested Bin…"); enabled: !root.catalog.bin(binMenu.binId).smart; onTriggered: { root.openBin("", false, false); parentField.currentIndex = root.parents.findIndex(function (p) { return p.id === binMenu.binId }) } }
        MenuAction { text: qsTr("Delete Bin…"); onTriggered: { removePrompt.binId = binMenu.binId; removePrompt.binName = root.catalog.bin(binMenu.binId).name || ""; removePrompt.open() } }
    }
    ContextMenu {
        id: locationMenu
        property int locationIndex: -1
        MenuAction { text: qsTr("Rename…"); onTriggered: { locationPrompt.locationIndex = locationMenu.locationIndex; locationName.text = root.catalog.locations[locationMenu.locationIndex].name; locationPath.text = root.catalog.locations[locationMenu.locationIndex].path; locationPrompt.open() } }
        MenuAction { text: qsTr("Move Up"); enabled: locationMenu.locationIndex > 0; onTriggered: root.catalog.moveLocation(locationMenu.locationIndex, -1) }
        MenuAction { text: qsTr("Move Down"); enabled: locationMenu.locationIndex + 1 < root.catalog.locations.length; onTriggered: root.catalog.moveLocation(locationMenu.locationIndex, 1) }
        MenuAction { text: qsTr("Remove Favourite"); onTriggered: root.catalog.removeLocation(locationMenu.locationIndex) }
    }
    ModalDialog {
        id: editor
        objectName: "mediaBinDialog"
        property string binId: ""
        property bool smart: false
        property bool assign: false
        property bool filterOnly: false
        title: filterOnly ? qsTr("Filter Media") : assign ? qsTr("Move Clips to Bin") : binId ? qsTr("Edit Bin") : smart ? qsTr("New Saved Search") : qsTr("New Bin")
        onAccepted: {
            if (filterOnly) { root.rulesApplied(rules.rules()); return }
            var parentId = root.parents[parentField.currentIndex].id
            if (assign) { root.moveRequested(root.selection, parentId); return }
            var id = binId
            if (id) root.catalog.edit(id, nameField.text, parentId, rules.rules())
            else id = root.catalog.create(nameField.text, parentId, rules.rules(), smart)
            if (id) root.filterRequested("bin", id)
        }
        ColumnLayout {
            width: parent.width
            spacing: Theme.s2
            LineEdit { id: nameField; objectName: "mediaBinName"; tip: qsTr("Name this bin is listed under in the sidebar"); visible: !editor.assign && !editor.filterOnly; Layout.fillWidth: true; hint: qsTr("Bin name"); Accessible.name: qsTr("Bin name") }
            ComboField { id: parentField; tip: qsTr("Bin this one sits inside, or where the clips are moved to"); visible: !editor.filterOnly; Layout.fillWidth: true; caption: editor.assign ? qsTr("Destination") : qsTr("Parent"); model: root.parents.map(function (b) { return "  ".repeat(b.depth || 0) + b.name }) }
            SearchRules { id: rules; visible: editor.smart; Layout.fillWidth: true }
            Text { Layout.fillWidth: true; visible: editor.smart; text: qsTr("All rules must match. Empty fields allow any value."); wrapMode: Text.Wrap; color: Theme.textMuted; font.pixelSize: Theme.fsLabel; font.family: Theme.fontFamily }
        }
        Component.onCompleted: standardButton(C.Dialog.Ok).enabled = Qt.binding(function () { return editor.assign || editor.filterOnly || nameField.text.trim() !== "" })
    }
    ModalDialog {
        id: removePrompt
        property string binId: ""
        property string binName: ""
        objectName: "deleteBinDialog"
        title: qsTr("Delete bin “%1”?").arg(binName)
        onAccepted: { root.catalog.remove(binId); root.filterRequested("", "") }
        Text { width: parent.width; text: qsTr("Delete “%1” and its nested bins? Clips and timelines return to the library root. Source files and timeline edits are kept.").arg(removePrompt.binName); wrapMode: Text.Wrap; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
    }
    ModalDialog {
        id: locationPrompt
        title: locationIndex < 0 ? qsTr("Favourite Location") : qsTr("Rename Location")
        property int locationIndex: -1
        onAccepted: if (locationIndex < 0) root.catalog.addLocation(locationPath.text, locationName.text); else root.catalog.editLocation(locationIndex, locationName.text)
        ColumnLayout {
            width: parent.width
            LineEdit { id: locationName; tip: qsTr("Name this favourite folder is listed under"); Layout.fillWidth: true; hint: qsTr("Location name") }
            LineEdit { id: locationPath; tip: qsTr("Folder on disk the favourite opens at"); Layout.fillWidth: true; visible: locationPrompt.locationIndex < 0; hint: qsTr("Folder path, e.g. /media/camera") }
        }
    }
    ContextMenu {
        id: watchMenu
        property string watchId: ""
        MenuAction { text: qsTr("Resume Automatic Ingest"); onTriggered: root.catalog.enableWatch(watchMenu.watchId, true) }
        MenuAction { text: qsTr("Pause Automatic Ingest"); onTriggered: root.catalog.enableWatch(watchMenu.watchId, false) }
        MenuAction { text: qsTr("Remove Watch Folder"); onTriggered: root.catalog.removeWatch(watchMenu.watchId) }
    }
    ModalDialog {
        id: watchPrompt
        title: qsTr("Watch Folder")
        onAccepted: root.catalog.watch(watchPath.text.trim(), root.parents[watchBin.currentIndex].id)
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            Text { Layout.fillWidth: true; text: qsTr("Automatically import files after their size and modification time are stable across two scans, five seconds apart. Files stay in place. This watch belongs to the project."); wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
            LineEdit { id: watchPath; tip: qsTr("Folder scanned for new files to import in place"); Layout.fillWidth: true; hint: qsTr("Folder path") }
            ComboField { id: watchBin; tip: qsTr("Bin the watched folder\u2019s new files are added to"); Layout.fillWidth: true; caption: qsTr("Import into"); model: root.parents.map(function (b) { return b.name }) }
        }
    }
    ContextMenu {
        id: providerMenu
        property string providerId: ""
        MenuAction { text: qsTr("Remove Provider"); onTriggered: root.catalog.removeProvider(providerMenu.providerId) }
    }
    ModalDialog {
        id: providerPrompt
        title: qsTr("Mounted Storage Provider")
        onAccepted: root.catalog.registerProvider({name: providerName.text, root: providerRoot.text, adapter: "mounted", kind: providerKind.currentIndex === 0 ? "cloud" : "network"})
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            Text { Layout.fillWidth: true; text: qsTr("Use a cloud or network folder already mounted on this computer. Account connection and mounting stay with your storage provider. Provider manifests can register these locations without changing the app."); wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
            LineEdit { id: providerName; tip: qsTr("Name this storage location is listed under"); Layout.fillWidth: true; hint: qsTr("Provider name") }
            LineEdit { id: providerRoot; tip: qsTr("Folder the mounted storage is already available at"); Layout.fillWidth: true; hint: qsTr("Mounted root folder") }
            ComboField { id: providerKind; tip: qsTr("Whether the storage is listed as cloud or network"); Layout.fillWidth: true; model: [qsTr("Cloud"), qsTr("Network")] }
            ToolButton { text: qsTr("Import Provider Manifest…"); onClicked: { providerPrompt.close(); root.providerManifestRequested() } }
        }
    }
}
