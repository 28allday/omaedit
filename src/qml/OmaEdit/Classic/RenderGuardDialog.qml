import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

// The window stays open unless the user explicitly keeps or cancels rendering.
Ui.MessageDialog {
    id: renderGuard
    objectName: "renderGuardDialog"
    title: qsTr("Rendering in progress")
    standardButtons: C.Dialog.NoButton
    signal keepRendering()
    signal cancelAndQuit()
    message: backend.renderQueue.pendingCount === 1 ? qsTr("A render is still in progress.")
             : qsTr("%1 renders are still in progress.").arg(backend.renderQueue.pendingCount)
    detail: qsTr("Keep Rendering hides the window until the queue finishes.")
    footer: Ui.DialogButtons {
        Ui.ToolButton {
            objectName: "renderGuardKeep"
            text: qsTr("Keep Rendering")
            tip: qsTr("Hide the window and let the queue finish rendering")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.AcceptRole
        }
        Ui.ToolButton {
            objectName: "renderGuardQuit"
            text: qsTr("Cancel Renders and Quit")
            tip: qsTr("Abandon the unfinished renders and close the app")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.DestructiveRole
        }
        Ui.ToolButton {
            objectName: "renderGuardStay"
            text: qsTr("Stay")
            tip: qsTr("Keep the window open and carry on rendering")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole
        }
        onClicked: function(button) {
            renderGuard.close()
            if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.AcceptRole)
                renderGuard.keepRendering()
            else if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.DestructiveRole)
                renderGuard.cancelAndQuit()
        }
    }
}
