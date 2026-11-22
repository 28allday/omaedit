import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

Ui.MessageDialog {
    id: quitDialog
    objectName: "quitDialog"
    standardButtons: C.Dialog.NoButton
    title: qsTr("Unsaved changes")

    message: qsTr("Save “%1” before quitting?").arg(backend.projectName)

    footer: Ui.DialogButtons {
        Ui.ToolButton {
            objectName: "quitDialogSave"
            text: qsTr("Save")
            tip: qsTr("Save this project first, then quit")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.AcceptRole
        }
        Ui.ToolButton {
            objectName: "quitDialogDiscard"
            text: qsTr("Discard")
            tip: qsTr("Quit now and lose every unsaved change")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.DestructiveRole
        }
        Ui.ToolButton {
            objectName: "quitDialogCancel"
            text: qsTr("Cancel")
            tip: qsTr("Carry on working and do not quit")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole
        }
        onClicked: function (button) {
            if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.DestructiveRole) {
                quitDialog.close()
                backend.discardRecovery()
                win.forceClose = true
                win.close()
            }
        }
    }

    onAccepted: {
        win.quitAfterSave = true
        backend.saveProject() // no file yet → falls through to Save As
    }
}
