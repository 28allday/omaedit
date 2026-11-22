pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui
Item {
    id: root
    required property var engine
    property var selection: []
    property var exportIds: []
    property var fields: []
    readonly property bool textActive: schemaDialog.visible
    function openSchema() { fields = engine.mediaCatalog.schema.map(function (f) { return Object.assign({}, f) }); schemaDialog.open() }
    function importCsv() { engine.mediaTasks.browse("metadata-import", "file") }
    function exportCsv() { exportIds = selection.map(function (i) { return root.engine.library[i].libraryId }); engine.mediaTasks.browse("metadata-export", "save") }
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose, paths) {
            if (!paths.length) return
            if (purpose === "metadata-import") root.engine.importMediaCsv(paths[0])
            else if (purpose === "metadata-export") {
                var indexes = []
                root.engine.library.forEach(function (entry, index) { if (root.exportIds.indexOf(entry.libraryId) >= 0) indexes.push(index) })
                root.engine.exportMediaCsv(paths[0], indexes)
            }
        }
    }
    ModalDialog {
        id: schemaDialog
        objectName: "mediaSchemaDialog"
        title: qsTr("Custom Metadata Fields")
        onAccepted: root.engine.mediaCatalog.setSchema(root.fields)
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            Text { Layout.fillWidth: true; text: qsTr("Fields belong to this project. CSV columns use custom.key. Removing a field hides its values; adding the same key restores them."); wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: Theme.hMediaTasks / 2; clip: true; model: root.fields
                C.ScrollBar.vertical: ScrollBar { atRest: true }
                delegate: RowLayout {
                    id: fieldRow
                    required property var modelData
                    required property int index
                    width: ListView.view.width; height: Theme.hControl + Theme.s2
                    Text { Layout.preferredWidth: 120; text: fieldRow.modelData.key; elide: Text.ElideRight; color: Theme.textSecondary; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel }
                    LineEdit { tip: qsTr("Name this custom field is shown under in the inspector"); Layout.fillWidth: true; text: fieldRow.modelData.label; onEditingFinished: { var copy = root.fields.slice(); copy[fieldRow.index] = Object.assign({}, copy[fieldRow.index], {label: text}); root.fields = copy } }
                    Text { text: fieldRow.modelData.type; color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
                    IconButton { iconName: "x"; text: qsTr("Remove field"); onClicked: root.fields = root.fields.filter(function (f, i) { return i !== fieldRow.index }) }
                }
            }
            RowLayout {
                LineEdit { id: key; tip: qsTr("Key the field is stored and exported under, as custom.key"); Layout.fillWidth: true; hint: qsTr("Field key"); validator: RegularExpressionValidator { regularExpression: /^[A-Za-z][A-Za-z0-9_]{0,31}$/ } }
                LineEdit { id: label; tip: qsTr("Name the new field is shown under in the inspector"); Layout.fillWidth: true; hint: qsTr("Display label") }
                ComboField { id: type; tip: qsTr("Whether the field holds text, a number or a yes/no value"); model: ["text", "number", "boolean"] }
                IconButton {
                    iconName: "plus"; text: qsTr("Add custom field")
                    enabled: key.acceptableInput && key.text.length > 0 && !root.fields.some(function (f) { return f.key === key.text })
                    onClicked: { root.fields = root.fields.concat([{key: key.text, label: label.text || key.text, type: type.currentText}]); key.text = ""; label.text = "" }
                }
            }
        }
    }
}
