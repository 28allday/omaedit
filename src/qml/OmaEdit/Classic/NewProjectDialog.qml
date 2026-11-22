import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.MessageDialog {
    id: root
    objectName: "newProjectUnsavedDialog"
    title: qsTr("Unsaved changes")
    standardButtons: C.Dialog.NoButton
    message: qsTr("Save “%1” before starting a new project?").arg(backend.projectName)
    function saveCurrent() { close(); win.newAfterSave=true; backend.saveProject() }
    function discardCurrent() { close(); win.project.commitNew() }
    function cancelCreation() { close(); projectCreationDialog.awaiting=false }
    onRejected: projectCreationDialog.awaiting=false
    footer: Ui.DialogButtons {
        Ui.ToolButton { objectName:"newProjectSaveCurrent"; text:qsTr("Save"); onClicked:root.saveCurrent() }
        Ui.ToolButton { objectName:"newProjectDiscardCurrent"; text:qsTr("Discard"); onClicked:root.discardCurrent() }
        Ui.ToolButton { objectName:"newProjectCancelCurrent"; text:qsTr("Cancel"); onClicked:root.cancelCreation() }
    }
}
