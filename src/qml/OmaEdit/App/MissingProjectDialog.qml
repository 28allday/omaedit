import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

Ui.MessageDialog {
    id: root
    objectName: "missingProjectDialog"
    property string projectPath: ""
    function openFor(path) { projectPath = path; open() }
    standardButtons: C.Dialog.NoButton
    title: qsTr("Project not found")
    message: qsTr("This recent project has moved or is unavailable. Locate its project file, or remove this entry from the recent list.")
    detail: root.projectPath
    footer: Ui.DialogButtons {
        Ui.ToolButton { objectName: "missingProjectLocate"; text: qsTr("Locate…"); tip: qsTr("Browse for the moved project file and reopen it"); onClicked: { root.close(); backend.locateRecentProject(root.projectPath) } }
        Ui.ToolButton { objectName: "missingProjectRemove"; text: qsTr("Remove from Recent"); tip: qsTr("Drop this entry from the recent list; the file itself is untouched"); onClicked: { backend.removeRecentProject(root.projectPath); root.close() } }
        Ui.ToolButton { text: qsTr("Cancel"); tip: qsTr("Close this notice and leave the recent list as it is"); C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole }
    }
}
