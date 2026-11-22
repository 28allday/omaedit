import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

Ui.MessageDialog {
    id: openDialog
    objectName: "openProjectUnsavedDialog"
    standardButtons: C.Dialog.NoButton
    title: qsTr("Unsaved changes")

    message: qsTr("Save “%1” before opening another project?").arg(backend.projectName)

    footer: Ui.DialogButtons {
        Ui.ToolButton {
            objectName: "openDialogSave"
            text: qsTr("Save")
            tip: qsTr("Save this project first, then open the other one")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.AcceptRole
        }
        Ui.ToolButton {
            objectName: "openDialogDiscard"
            text: qsTr("Discard")
            tip: qsTr("Open the other project and lose unsaved changes")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.DestructiveRole
        }
        Ui.ToolButton {
            objectName: "openDialogCancel"
            text: qsTr("Cancel")
            tip: qsTr("Keep this project open and open nothing")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole
        }
        onClicked: function (button) {
            if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.DestructiveRole) {
                openDialog.close()
                win.project.proceedOpen(win.pendingOpenPath)
            }
        }
    }

    onAccepted: {
        win.openAfterSave = true
        backend.saveProject() // no file yet → falls through to Save As
    }
}
