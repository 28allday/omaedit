import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

Ui.MessageDialog {
    id: root
    objectName: "closeProjectDialog"
    standardButtons: C.Dialog.NoButton
    title: qsTr("Close Project")
    message: qsTr("Save “%1” before closing this project?").arg(backend.projectName)
    footer: Ui.DialogButtons {
        Ui.ToolButton { objectName: "closeProjectSave"; text: qsTr("Save"); tip: qsTr("Write the project to disk, then close it"); C.DialogButtonBox.buttonRole: C.DialogButtonBox.AcceptRole }
        Ui.ToolButton { objectName: "closeProjectDiscard"; text: qsTr("Discard"); tip: qsTr("Close the project and lose every change since the last save"); C.DialogButtonBox.buttonRole: C.DialogButtonBox.DestructiveRole }
        Ui.ToolButton { objectName: "closeProjectCancel"; text: qsTr("Cancel"); tip: qsTr("Keep the project open and change nothing"); C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole }
        onClicked: function(button) {
            if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.DestructiveRole) {
                root.close()
                backend.closeProject()
            }
        }
    }
    onAccepted: { win.closeAfterSave = true; backend.saveProject() }
}
