import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// Inspector ▸ Applied ▸ Save Preset…: the selected clip's set attributes
// as a named preset (Effects tab ▸ Presets; Paste Attributes reads them).
// Existing presets are listed here with a delete each.
Ui.FormDialog {
    id: presetDialog
    width: Math.min(520, parent ? parent.width - Ui.Theme.s5 * 2 : 520)
    title: qsTr("Save Preset")
    standardButtons: Dialog.Ok | Dialog.Cancel
    property var source: null
    property var rows: []

    function openFor(entry) {
        source = entry
        var defs = win.attributes.groupDefs
        var avail = win.attributes.groupsOf(entry)
        var list = []
        for (var i = 0; i < defs.length; i++)
            if (avail.indexOf(defs[i].key) >= 0)
                list.push({ key: defs[i].key, name: defs[i].name })
        rows = list
        nameField.text = ""
        open()
        nameField.forceActiveFocus()
    }
    function chosenGroups() {
        var out = []
        for (var i = 0; i < groupRepeater.count; i++) {
            var box = groupRepeater.itemAt(i)
            if (box !== null && box.checked)
                out.push(box.groupKey)
        }
        return out
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8
        Ui.LineEdit {
            id: nameField
            objectName: "presetNameField"
            hoverEnabled: true
            tip: qsTr("Name the preset is saved and listed under")
            Layout.fillWidth: true
            Layout.minimumWidth: 340
            hint: qsTr("Preset name")
            onAccepted: presetDialog.accept()
        }
        Label {
            visible: presetDialog.rows.length === 0
            text: qsTr("This clip has no attributes set.")
            color: win.textDimColor
        }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 0
            Repeater {
                id: groupRepeater
                model: presetDialog.rows
                Ui.Toggle {
                    required property var modelData
                    readonly property string groupKey: modelData.key
                    label: modelData.name
                    tip: qsTr("Include this group of attributes in the preset")
                    checked: true
                }
            }
        }
        Label {
            visible: backend.presets.length > 0
            text: qsTr("Saved presets")
            color: win.textDimColor
            font.pixelSize: 12
        }
        Repeater {
            model: backend.presets
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Label { text: modelData.name; color: win.textColor; Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText }
                Ui.ToolButton {
                    text: qsTr("Delete")
                    tip: qsTr("Remove this saved preset from the list")
                    onClicked: backend.deletePreset(modelData.name)
                }
            }
        }
    }

    onAccepted: {
        var groups = chosenGroups()
        if (presetDialog.source === null || groups.length === 0 || nameField.text.trim() === "")
            return
        backend.savePreset(nameField.text.trim(), presetDialog.source, groups)
    }
}
