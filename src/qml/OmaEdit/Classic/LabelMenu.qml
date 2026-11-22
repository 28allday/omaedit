import QtQuick
import QtQuick.Controls
import OmaEdit.Ui as Ui

// Clip ▸ Label: the six colour labels plus None, routed to whichever clip
// is selected. Shared by the Clip menu and both clip context menus.
AppMenu {
    id: labelMenu
    title: qsTr("Label")
    enabled: win.selectedTimelineIndex >= 0 || win.selectedConnectedIndex >= 0
    KeyMenuItem {
        text: qsTr("None")
        enabled: win.clipCommands.selectedLabel() !== ""
        onTriggered: win.clipCommands.setSelectedLabel("")
    }
    MenuSeparator {}
    Repeater {
        model: Ui.Theme.labelNames
        KeyMenuItem {
            id: labelItem
            required property string modelData
            text: Ui.Theme.labelTitle(modelData)
            onTriggered: win.clipCommands.setSelectedLabel(labelItem.modelData)
            // A swatch stands in for the check mark: the colour itself.
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: Ui.Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                width: Ui.Theme.szIcon - 2
                height: width
                radius: width / 2
                color: Ui.Theme.labelColor(labelItem.modelData)
                border.width: win.clipCommands.selectedLabel() === labelItem.modelData
                              ? Ui.Theme.selectionRing : 0
                border.color: Ui.Theme.accent
            }
        }
    }
}
