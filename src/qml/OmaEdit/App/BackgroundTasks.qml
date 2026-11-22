import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.ModalDialog {
    id: root
    objectName: "backgroundTasksDialog"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(920, parent ? parent.width - 32 : 920)
    height: Math.min(660, parent ? parent.height - 48 : 660)
    modal: false
    closePolicy: C.Popup.CloseOnEscape
    title: qsTr("Background Tasks")
    standardButtons: C.Dialog.Close
    property var tasks: backend.backgroundTasks
    property string selectedId: ""
    readonly property var selected: {
        if (!visible) return null
        const rows = tasks.jobs
        for (let i = 0; i < rows.length; ++i) if (rows[i].id === selectedId) return rows[i]
        return null
    }
    readonly property var rows: {
        if (!visible) return []
        const query = search.text.trim().toLowerCase()
        return tasks.jobs.filter(function(job) {
            const active = ["queued", "running", "rendering", "paused", "waiting", "applying"].indexOf(job.state) >= 0
            return (filter.currentIndex === 0 || (filter.currentIndex === 1 ? active : !active))
                    && (!query || [job.name, job.type, job.project, job.statusText].join(" ").toLowerCase().indexOf(query) >= 0)
        })
    }
    function openFor(messages) {
        tabs.currentIndex = messages ? 1 : 0
        open()
        search.forceActiveFocus()
    }
    function moveSelection(delta) {
        const index = rows.findIndex(function(job) { return job.id === root.selectedId })
        const next = Math.max(0, Math.min(rows.length - 1, index + delta))
        if (rows.length) {
            selectedId = rows[next].id
            list.currentIndex = next
            list.positionViewAtIndex(next, ListView.Contain)
        }
    }
    function duration(seconds) {
        if (seconds < 0) return qsTr("Estimating…")
        return seconds < 60 ? qsTr("%1s").arg(seconds) : qsTr("%1m %2s").arg(Math.floor(seconds / 60)).arg(seconds % 60)
    }
    background: Rectangle { color: Ui.Theme.panelBg; radius: Ui.Theme.rMenu; border.color: Ui.Theme.border }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2
        C.TabBar {
            id: tabs
            Layout.fillWidth: true
            C.TabButton { C.ToolTip.text: qsTr("List of imports, renders and analysis jobs running in the background"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Tasks (%1 active)").arg(root.tasks.activeCount) }
            C.TabButton { C.ToolTip.text: qsTr("Notices logged this session; click one to jump to its task"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Messages (%1)").arg(root.tasks.notices.length) }
        }
        RowLayout {
            visible: tabs.currentIndex === 0
            Layout.fillWidth: true
            Ui.LineEdit { id: search; objectName: "taskSearch"; Layout.fillWidth: true; tip: qsTr("Filter the list by task name, type, project or status"); hint: qsTr("Search tasks or projects…"); Accessible.name: qsTr("Search background tasks") }
            C.ComboBox { id: filter; objectName: "taskFilter"; model: [qsTr("All tasks"), qsTr("Active"), qsTr("Finished")]; Accessible.name: qsTr("Task status filter"); C.ToolTip.text: qsTr("Show all tasks, only those still working, or only completed ones"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450 }
            C.Button { visible: backend.renderQueue.pendingCount > 0; text: backend.renderQueue.paused ? qsTr("Resume render queue") : qsTr("Start render queue"); enabled: backend.renderQueue.pendingCount > 0 && (backend.renderQueue.paused || !backend.renderQueue.active); C.ToolTip.text: qsTr("Begin rendering the jobs waiting in the render queue"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; onClicked: backend.renderQueue.resumeAll() }
            C.Button { text: qsTr("Clear finished"); C.ToolTip.text: qsTr("Remove completed and failed tasks from the list"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; onClicked: root.tasks.clearFinished() }
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            visible: tabs.currentIndex === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ListView {
                id: list
                objectName: "backgroundTaskList"
                model: root.rows
                spacing: Ui.Theme.s1
                keyNavigationEnabled: false
                Keys.onDownPressed: root.moveSelection(1)
                Keys.onUpPressed: root.moveSelection(-1)
                // Model refreshes may reset currentIndex. Only user navigation
                // changes selection; cancellation keeps targeting the chosen ID.
                C.Label { anchors.centerIn: parent; visible: list.count === 0; text: search.text ? qsTr("No matching tasks") : qsTr("No background tasks. Import media or add a render to begin."); color: Ui.Theme.textMuted }
                delegate: C.ItemDelegate {
                    required property var modelData
                    required property int index
                    objectName: "backgroundTask:" + modelData.id
                    width: ListView.view.width
                    height: 102
                    highlighted: root.selectedId === modelData.id
                    onClicked: { root.selectedId = modelData.id; list.currentIndex = index }
                    Accessible.name: modelData.name + ", " + modelData.type + ", " + modelData.statusText
                    contentItem: ColumnLayout {
                        spacing: Ui.Theme.s1
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideMiddle; color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsControl; font.bold: true }
                            Text { text: modelData.statusText; color: modelData.state === "failed" ? Ui.Theme.danger : modelData.state === "warning" || modelData.state === "partial" ? Ui.Theme.warning : Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
                        }
                        Text { text: modelData.type + " · " + modelData.project; Layout.fillWidth: true; elide: Text.ElideRight; color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
                        RowLayout {
                            Layout.fillWidth: true
                            C.ProgressBar { Layout.fillWidth: true; from: 0; to: 100; value: Math.max(0, modelData.percent); indeterminate: modelData.percent < 0 && modelData.state === "running"; Accessible.name: qsTr("Task progress") }
                            Text { text: modelData.percent < 0 ? qsTr("Progress unknown") : modelData.percent + "%"; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; font.family: Ui.Theme.monoFamily }
                            Text { text: root.duration(modelData.elapsed) + (modelData.remaining >= 0 ? qsTr(" · ~%1 left").arg(root.duration(modelData.remaining)) : "") + (modelData.speed ? " · " + modelData.speed : ""); color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
                        }
                        Text { text: modelData.message.replace(/\n/g, " · "); Layout.fillWidth: true; elide: Text.ElideMiddle; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
                    }
                    background: Rectangle { color: parent.highlighted ? Ui.Theme.controlBg : parent.hovered ? Ui.Theme.panelRaised : Ui.Theme.panelBg; border.color: parent.visualFocus ? Ui.Theme.accent : Ui.Theme.border; radius: Ui.Theme.rControl }
                }
            }
        }
        RowLayout {
            visible: tabs.currentIndex === 0
            Layout.fillWidth: true
            C.Label { text: root.selected ? root.selected.name : qsTr("Select a task for controls and details"); Layout.fillWidth: true; elide: Text.ElideMiddle; color: Ui.Theme.textSecondary }
            C.Button { objectName: "taskPause"; text: qsTr("Pause"); C.ToolTip.text: qsTr("Hold the selected task so it stops using the machine"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; enabled: !!root.selected && root.selected.canPause; onClicked: root.tasks.act(root.selectedId, "pause") }
            C.Button { objectName: "taskResume"; text: qsTr("Resume"); C.ToolTip.text: qsTr("Continue the selected task from where it was paused"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; enabled: !!root.selected && root.selected.canResume; onClicked: root.tasks.act(root.selectedId, "resume") }
            C.Button { objectName: "taskCancel"; text: qsTr("Cancel task"); C.ToolTip.text: qsTr("Stop the selected task and discard its unfinished work"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; enabled: !!root.selected && root.selected.canCancel; onClicked: root.tasks.act(root.selectedId, "cancel") }
            C.Button { objectName: "taskRetry"; text: qsTr("Retry"); C.ToolTip.text: qsTr("Run the selected task again after a failure"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; enabled: !!root.selected && root.selected.canRetry; onClicked: root.tasks.act(root.selectedId, "retry") }
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            visible: tabs.currentIndex === 0 && !!root.selected && (root.selected.details !== "" || root.selected.message !== "")
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(130, root.height / 5)
            C.TextArea { objectName: "taskDetails"; text: root.selected ? (root.selected.details || root.selected.message) : ""; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; Accessible.name: qsTr("Task details and failure log") }
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            visible: tabs.currentIndex === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ListView {
                id: messages
                objectName: "taskMessages"
                model: root.visible ? root.tasks.notices : []
                spacing: Ui.Theme.s2
                C.Label { anchors.centerIn: parent; visible: messages.count === 0; text: qsTr("No messages this session"); color: Ui.Theme.textMuted }
                delegate: C.ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    height: notice.implicitHeight + 24
                    contentItem: ColumnLayout {
                        id: notice
                        spacing: Ui.Theme.s1
                        Text { text: modelData.time + " · " + (modelData.severity === "error" ? qsTr("Error") : modelData.severity === "warning" ? qsTr("Warning") : modelData.severity === "success" ? qsTr("Success") : qsTr("Information")); color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
                        Text { text: modelData.title; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsControl; font.bold: true }
                        Text { text: modelData.message; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
                    }
                    onClicked: { if (modelData.taskId) { root.selectedId = modelData.taskId; tabs.currentIndex = 0; filter.currentIndex = 0; search.text = "" } }
                    background: Rectangle { color: parent.hovered ? Ui.Theme.panelRaised : Ui.Theme.panelBg; border.color: parent.visualFocus ? Ui.Theme.accent : Ui.Theme.border; radius: Ui.Theme.rControl }
                }
            }
        }
        C.Button { visible: tabs.currentIndex === 1; text: qsTr("Clear messages"); C.ToolTip.text: qsTr("Empty the message log for this session"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; onClicked: root.tasks.clearNotices() }
        C.Label { text: qsTr("You can keep editing while tasks run. Controls are enabled when supported; estimates use reported progress."); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
    }
}
