import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui

// "Name this preset" — for New Preset and Rename. Refuses an empty or
// taken name inline rather than failing after the fact.
FormDialog {
    id: root
    width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
    title: qsTr("New Preset")
    standardButtons: Dialog.Ok | Dialog.Cancel

    property string context: ""       // an id the opener wants back
    property var taken: null          // function(name) → bool
    property string initial: ""
    readonly property bool textActive: field.activeFocus
    readonly property bool nameOk: field.text.trim() !== "" && !(taken && taken(field.text.trim()) && field.text.trim() !== initial)

    signal accepted2(string name, string context)

    function openFor(title, initialName, ctx) {
        root.title = title
        root.initial = initialName
        root.context = ctx
        field.text = initialName
        open()
        field.forceActiveFocus()
        field.selectAll()
    }

    onAccepted: if (nameOk) accepted2(field.text.trim(), context)

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.s2
        TextField {
            id: field
            objectName: "presetNameField"
            Layout.fillWidth: true
            Layout.minimumWidth: 320
            placeholderText: qsTr("Preset name")
            hoverEnabled: true
            ToolTip.text: qsTr("Name the preset is saved under; it cannot be empty or already in use"); ToolTip.visible: hovered && !activeFocus; ToolTip.delay: 450
            color: Theme.textPrimary
            Keys.onReturnPressed: if (root.nameOk) root.accept()
        }
        Label {
            visible: !root.nameOk && field.text.trim() !== ""
            text: qsTr("A preset with that name exists")
            color: Theme.warning
            font.pixelSize: Theme.fsLabel
        }
    }
    Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(function () { return root.nameOk })
}
