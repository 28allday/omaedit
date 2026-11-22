import QtQuick
import QtQuick.Controls
import OmaEdit.Ui as Ui

// What the selected clips are for. Sound categories go on the sound of the
// selection and picture categories on its pictures, whichever is picked.
AppMenu {
    id: categoryMenu
    title: qsTr("Category")
    enabled: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
    Repeater {
        model: win.categories.list
        KeyMenuItem {
            id: categoryItem
            required property var modelData
            objectName: "categoryItem:" + modelData.id
            text: (modelData.kind === "audio" ? qsTr("Sound: %1") : qsTr("Picture: %1")).arg(modelData.name)
            kbd: Ui.Commands.hint("category." + modelData.id)
            onTriggered: win.categories.assign(categoryItem.modelData.id)
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: Ui.Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                width: Ui.Theme.szIcon - 2
                height: width
                radius: width / 2
                color: win.categories.tint(categoryItem.modelData.color)
                border.width: win.categories.selected(categoryItem.modelData.kind) === categoryItem.modelData.id
                              ? Ui.Theme.selectionRing : 0
                border.color: Ui.Theme.accent
            }
        }
    }
}
