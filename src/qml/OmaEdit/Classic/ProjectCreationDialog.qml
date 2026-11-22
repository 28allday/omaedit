import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.FormDialog {
    id: root
    objectName: "projectCreationDialog"
    title: qsTr("New Project")
    width: Math.min(480,parent ? parent.width-Ui.Theme.s5*2 : 480)
    standardButtons: C.Dialog.NoButton
    closePolicy: busy ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property var context: ({})
    property var draft: ({})
    property bool awaiting: false
    property int pending: 0
    readonly property bool busy: awaiting || pending>0
    property string problem: ""
    property string error: ""
    function set(key,value) { var s=Object.assign({},draft); s[key]=value; draft=s }
    function refreshProblem() { problem=backend.projectCreationProblem(context,draft) }
    function openFor() {
        context=backend.creationContext(); awaiting=false; pending=0; error=""
        draft=backend.newProjectDefaults(); refreshProblem(); open()
    }
    function submit() {
        refreshProblem()
        if (busy || problem) return
        error=""; awaiting=true; win.project.requestNew()
    }
    onBusyChanged: if (!busy && visible) refreshProblem()
    onDraftChanged: { error=""; refreshProblem() }
    onOpened: { projectName.forceActiveFocus(); projectName.selectAll() }
    Connections {
        target: backend
        function onProjectBusyChanged() { if (root.visible && !root.busy) root.refreshProblem() }
        function onSequencesChanged() { if (root.visible && !root.busy) root.refreshProblem() }
        function onProjectPlaybackReset() { if (!root.busy) root.close() }
        function onProjectCreateFinished(request,created,message) {
            if (request!==root.pending) return
            root.pending=0
            if (created) root.close()
            else { root.error=message; root.refreshProblem() }
        }
    }
    component Caption: C.Label {
        color: Ui.Theme.textSecondary; font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel
        wrapMode: Text.Wrap
    }
    Caption { Layout.fillWidth: true; text: qsTr("A project holds your media and timelines. Import media whenever you like, then create a timeline with its own settings.") }
    Caption { Layout.fillWidth: true; text: qsTr("Project name") }
    Ui.LineEdit {
        id: projectName; objectName: "newProjectName"
        Layout.fillWidth: true; enabled: !root.busy
        hint: qsTr("Project name"); tip: qsTr("Name of the project; choose its file location when you save")
        text: root.draft.title || ""
        onTextEdited: root.set("title",text)
        onAccepted: root.submit()
    }
    Caption { Layout.fillWidth: true; text: qsTr("You can keep several timelines in one project and duplicate any timeline to try another version.") }
    Caption { objectName: "newProjectValidation"; Layout.fillWidth: true; visible: text!==""; color: Ui.Theme.warning; text: root.error || root.problem }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
            Item { Layout.fillWidth:true }
            Ui.ToolButton { objectName:"newProjectCancel"; text:qsTr("Cancel"); enabled:!root.busy; onClicked:root.close() }
            Ui.ToolButton { objectName:"newProjectCreate"; text:root.busy ? qsTr("Creating…") : qsTr("Create Project"); enabled:!root.busy && !root.problem; onClicked:root.submit() }
        }
    }
}
