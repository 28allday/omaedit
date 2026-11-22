import QtQuick
import QtQuick.Controls
import omaedit
import OmaEdit.Ui as Ui

AppMenu {
    id: binContextMenu // the media bin itself, off any clip or timeline
    KeyMenuItem {
        text: qsTr("New Timeline…")
        enabled: !backend.projectBusy && !backend.exporting
        onTriggered: Ui.Commands.trigger("sequence.new")
    }
    MenuSeparator {}
    KeyMenuItem {
        text: qsTr("Import Media…")
        onTriggered: backend.importDialog()
    }
}
