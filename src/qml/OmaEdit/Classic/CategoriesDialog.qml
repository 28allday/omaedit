import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// The project's categories. The five it starts with can be renamed and
// recoloured; its own can also be removed, and their clips go back to the
// default for what they are.
Ui.FormDialog {
    id: root
    width: Math.min(460, parent ? parent.width - Ui.Theme.s5 * 2 : 460)
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Edit Categories")
    standardButtons: Dialog.Close

    readonly property var colourNames: Ui.Theme.labelNames

    ListView {
        id: rows
        objectName: "categoriesList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(320, contentHeight)
        clip: true
        model: win.categories.list
        delegate: RowLayout {
            id: row
            required property var modelData
            objectName: "categoryRow:" + modelData.id
            width: rows.width
            spacing: Ui.Theme.s2
            Rectangle {
                objectName: "categoryColour:" + row.modelData.id
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                radius: 9
                color: win.categories.tint(row.modelData.color)
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Change colour of %1").arg(row.modelData.name)
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        const at = root.colourNames.indexOf(row.modelData.color)
                        backend.mediaCatalog.editCategory(row.modelData.id, row.modelData.name,
                                                          root.colourNames[(at + 1) % root.colourNames.length])
                    }
                }
            }
            Ui.LineEdit {
                objectName: "categoryName:" + row.modelData.id
                Layout.fillWidth: true
                hoverEnabled: true
                tip: qsTr("The category's name. Click the dot to change its colour.")
                text: row.modelData.name
                onEditingFinished: if (text.trim() !== "" && text.trim() !== row.modelData.name)
                                       backend.mediaCatalog.editCategory(row.modelData.id, text, row.modelData.color)
            }
            Label {
                text: row.modelData.kind === "audio" ? qsTr("Sound") : qsTr("Picture")
                color: win.textDimColor
                font.pixelSize: 12
            }
            Ui.ToolButton {
                objectName: "categoryRemove:" + row.modelData.id
                text: qsTr("Remove")
                visible: row.modelData.builtIn !== true
                onClicked: backend.mediaCatalog.removeCategory(row.modelData.id)
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Ui.Theme.s2
        Ui.LineEdit {
            id: newName
            objectName: "categoryNewName"
            Layout.fillWidth: true
            hoverEnabled: true
            tip: qsTr("Name for a new category")
            hint: qsTr("New category")
        }
        Ui.ToolButton {
            objectName: "categoryAddSound"
            text: qsTr("Add for Sound")
            enabled: newName.text.trim() !== ""
            onClicked: { if (backend.mediaCatalog.addCategory(newName.text, "audio", "red") !== "") newName.text = "" }
        }
        Ui.ToolButton {
            objectName: "categoryAddPicture"
            text: qsTr("Add for Picture")
            enabled: newName.text.trim() !== ""
            onClicked: { if (backend.mediaCatalog.addCategory(newName.text, "video", "red") !== "") newName.text = "" }
        }
    }
}
