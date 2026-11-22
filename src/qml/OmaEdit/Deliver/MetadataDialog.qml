import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui

// Render Settings ▸ Metadata ▸ Edit…: the container tags the file carries
// (title, artist, album, comment, copyright, description, date).
FormDialog {
    id: root
    objectName: "renderMetadataDialog"
    width: Math.min(560, parent ? parent.width - Theme.s5 * 2 : 560)
    title: qsTr("Metadata")
    standardButtons: Dialog.Ok | Dialog.Cancel

    property var metadata: ({})
    readonly property bool textActive: activeFocus || fields.activeFocus
    signal edited(var metadata)

    readonly property var keys: [
        { key: "title", label: qsTr("Title") }, { key: "artist", label: qsTr("Artist") },
        { key: "album", label: qsTr("Album / Show") }, { key: "comment", label: qsTr("Comment") },
        { key: "copyright", label: qsTr("Copyright") }, { key: "description", label: qsTr("Description") },
        { key: "date", label: qsTr("Date") }
    ]

    function openFor(meta) {
        metadata = meta || {}
        for (var i = 0; i < fieldRepeater.count; ++i) {
            var f = fieldRepeater.itemAt(i)
            if (f) f.text = metadata[f.key] !== undefined ? metadata[f.key] : ""
        }
        open()
    }
    onAccepted: {
        var out = {}
        for (var i = 0; i < fieldRepeater.count; ++i) {
            var f = fieldRepeater.itemAt(i)
            if (f && f.text.trim() !== "") out[f.key] = f.text.trim()
        }
        edited(out)
    }

    GridLayout {
        Layout.fillWidth: true
        id: fields
        columns: 2
        columnSpacing: Theme.s3
        rowSpacing: Theme.s2
        Repeater {
            id: fieldRepeater
            model: root.keys
            delegate: LineEdit {
                required property var modelData
                required property int index
                readonly property string key: modelData.key
                objectName: "metadata:" + key
                Layout.row: index
                Layout.column: 1
                Layout.fillWidth: true
                Layout.minimumWidth: 320
                hint: modelData.label
                hoverEnabled: true
                tip: qsTr("Tag written into the exported file's metadata; blank fields are left out");
                color: Theme.textPrimary
            }
        }
        Repeater {
            model: root.keys
            delegate: Label {
                required property var modelData
                required property int index
                Layout.row: index
                Layout.column: 0
                text: modelData.label
                color: Theme.textSecondary
                font.pixelSize: Theme.fsLabel
            }
        }
    }
}
