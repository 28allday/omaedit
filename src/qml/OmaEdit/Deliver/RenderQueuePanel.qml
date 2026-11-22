pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The render queue: a header with Add to Queue / Render All, the job table,
// and a ⋮ menu per job (pause, resume, cancel, retry, remove, duplicate,
// move, open, reveal, log). After-completion action lives in the header
// menu, as does Clear Finished.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    // What the queue is doing, for the dock bar when docked.
    readonly property string statusLine: !(active || pendingCount > 0) ? ""
        : paused ? qsTr("Paused · %1 waiting").arg(pendingCount)
        : active ? qsTr("Rendering · %1 waiting").arg(pendingCount)
                 : qsTr("%1 waiting").arg(pendingCount)
    property var jobs: []
    property bool active: false
    property bool paused: false
    property int activeJob: -1
    property bool canQueue: false
    property string queueHint: ""
    property string afterCompletion: "none"
    property int selectedJob: -1
    // The timeline as an edit file for another application: [{ id, name, available }].
    property var timelineFormats: []
    property bool canExportTimeline: false

    signal addRequested()
    signal renderAllRequested()
    signal pauseAllRequested()
    signal resumeAllRequested()
    signal clearFinishedRequested()
    signal jobAction(string action, int id)
    signal afterCompletionEdited(string action)
    signal logRequested(int id)
    signal timelineExportRequested(string format)

    color: Theme.panelBg

    readonly property int pendingCount: jobs.filter(function (j) { return j.status === "queued" }).length
    readonly property int finishedCount: jobs.filter(function (j) { return j.status === "completed" || j.status === "warning" || j.status === "failed" || j.status === "cancelled" }).length
    readonly property var columns: {
        var w = table.width, preset = w >= 760, duration = w >= 920, destination = w >= 1100
        var actions = Math.max(0, w - Theme.s3 - Theme.szIcon - Theme.s1)
        var name = Theme.s3 + 30
        var statusWidth = 126, progressWidth = 116
        var remaining = actions - name - statusWidth - progressWidth - (preset ? 140 : 0) - (duration ? 150 : 0)
        var nameWidth = destination ? remaining * 0.48 : remaining
        var p = name + Math.max(90, nameWidth), st = p + (preset ? 140 : 0)
        var pr = st + statusWidth, du = pr + progressWidth, dest = du + (duration ? 150 : 0)
        return { index: Theme.s3, name: name, preset: p, status: st, progress: pr,
                 duration: du, destination: dest, actions: actions,
                 showPreset: preset, showDuration: duration, showDestination: destination }
    }
    function jobById(id) {
        for (var i = 0; i < jobs.length; ++i) if (jobs[i].id === id) return jobs[i]
        return {}
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }

    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        height: queueActions.inBar ? Theme.s2 : Theme.hControl + Theme.s3 * 2

        Media.SectionTitle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            visible: root.dockBar === null
            text: qsTr("Render Queue")
            width: 200
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 120
            anchors.verticalCenter: parent.verticalCenter
            visible: root.statusLine !== "" && !queueActions.inBar
            text: root.statusLine
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }

        DockBarActions {
            id: queueActions
            dockBar: root.dockBar
            fallback: header
            fallbackMargin: 0
            spacing: Theme.s2

            ToolButton {
                id: timelineButton
                objectName: "exportTimeline"
                visible: root.timelineFormats.length > 0
                enabled: root.canExportTimeline
                iconName: "share-2"
                text: qsTr("Export Timeline")
                tip: qsTr("Write the timeline's edit as a file another application opens")
                showLabel: true
                anchors.verticalCenter: parent.verticalCenter
                onClicked: timelineMenu.popup(timelineButton, 0, timelineButton.height)
            }
            IconButton {
                iconName: "menu"
                text: qsTr("Queue options")
                anchors.verticalCenter: parent.verticalCenter
                onClicked: queueMenu.popup()
            }
            ToolButton {
                objectName: "pauseQueue"
                visible: root.active || root.paused
                iconName: root.paused ? "play" : "pause"
                text: root.paused ? qsTr("Resume Queue") : qsTr("Pause Queue")
                showLabel: true
                anchors.verticalCenter: parent.verticalCenter
                onClicked: root.paused ? root.resumeAllRequested() : root.pauseAllRequested()
            }
            C.AbstractButton {
                id: addButton
                objectName: "addToQueue"
                enabled: root.canQueue
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                implicitHeight: queueActions.inBar ? Theme.hControl - 2 : Theme.hControl + Theme.s1
                implicitWidth: addLabel.implicitWidth + Theme.s4 * 2 + Theme.szIcon
                opacity: enabled ? 1 : Theme.disabledOpacity
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: qsTr("Add to Queue")
                background: Rectangle {
                    radius: Theme.rControl
                    color: addButton.down ? Theme.pressedOn(Theme.controlBg) : addButton.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
                    border.width: Theme.hairline
                    border.color: addButton.visualFocus ? Theme.accent : Theme.accent
                }
                contentItem: Row {
                    spacing: Theme.s1
                    leftPadding: Theme.s2
                    Icon { name: "plus"; size: Theme.szIcon; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        id: addLabel
                        text: qsTr("Add to Queue")
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        color: Theme.accent
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                onClicked: root.addRequested()
                Tooltip { text: root.canQueue ? qsTr("Queue these settings as a job") : root.queueHint; visible: addButton.hovered && text !== "" }
            }
            C.AbstractButton {
                id: renderButton
                objectName: "renderAll"
                enabled: root.pendingCount > 0 && !root.active
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                implicitHeight: Theme.hControl + Theme.s1
                implicitWidth: renderLabel.implicitWidth + Theme.s4 * 2 + Theme.szIcon
                opacity: enabled ? 1 : Theme.disabledOpacity
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: qsTr("Render All")
                background: Rectangle {
                    radius: Theme.rControl
                    color: renderButton.down ? Theme.accentPressed : renderButton.hovered ? Qt.lighter(Theme.accent, 1.06) : Theme.accent
                }
                contentItem: Row {
                    spacing: Theme.s1
                    leftPadding: Theme.s2
                    Icon { name: "play"; size: Theme.szIcon; color: Theme.accentText; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        id: renderLabel
                        text: qsTr("Render All")
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.weight: Theme.wHeading
                        color: Theme.accentText
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                onClicked: root.renderAllRequested()
            }
        }
    }

    // Column headings.
    Item {
        id: headings
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.hRow
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        Repeater {
            model: [
                { key: "index", label: "#" }, { key: "name", label: qsTr("Job Name") }, { key: "preset", label: qsTr("Preset") },
                { key: "status", label: qsTr("Status") }, { key: "progress", label: qsTr("Progress") },
                { key: "duration", label: qsTr("Render Time") }, { key: "destination", label: qsTr("Destination") }
            ]
            delegate: Text {
                required property var modelData
                visible: modelData.key !== "preset" && modelData.key !== "duration" && modelData.key !== "destination"
                    || root.columns["show" + modelData.key.charAt(0).toUpperCase() + modelData.key.slice(1)]
                x: root.columns[modelData.key]
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.label
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    ListView {
        id: table
        anchors.top: headings.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: root.jobs
        cacheBuffer: Math.max(0, height)
        C.ScrollBar.vertical: ScrollBar {}
        delegate: QueueRow {
                    id: line
                    required property var modelData
                    required property int index
                    objectName: "queueJob:" + modelData.id
                    width: table.width
                    job: modelData
                    row: index
                    columns: root.columns
                    selected: modelData.id === root.selectedJob
                    onActivated: root.selectedJob = modelData.id
                    onDoubleClicked: if (modelData.status === "completed" || modelData.status === "warning") root.jobAction("open", modelData.id)
                    onMenuRequested: function (at) {
                        jobMenu.job = modelData
                        jobMenu.parent = line
                        jobMenu.popup(at)
                    }
                }
        EmptyState {
            anchors.fill: parent
            visible: root.jobs.length === 0
            iconName: "share-2"
            title: qsTr("No render jobs")
            description: qsTr("Choose a preset, check the settings, then Add to Queue.")
        }
    }

    ContextMenu {
        id: queueMenu
        parent: header
        x: header.width - width
        y: header.height
        MenuAction { text: qsTr("Clear Finished Jobs"); iconName: "trash-2"; enabled: root.finishedCount > 0; onTriggered: root.clearFinishedRequested() }
        C.MenuSeparator { }
        MenuAction { text: qsTr("After the queue finishes:"); enabled: false }
        MenuAction { text: qsTr("Do nothing"); checkable: true; checked: root.afterCompletion === "none"; onTriggered: root.afterCompletionEdited("none") }
        MenuAction { text: qsTr("Suspend the computer"); checkable: true; checked: root.afterCompletion === "suspend"; onTriggered: root.afterCompletionEdited("suspend") }
        MenuAction { text: qsTr("Shut down the computer"); checkable: true; checked: root.afterCompletion === "shutdown"; onTriggered: root.afterCompletionEdited("shutdown") }
    }

    ContextMenu {
        id: timelineMenu
        objectName: "exportTimelineMenu"
        Instantiator {
            model: root.timelineFormats
            delegate: MenuAction {
                required property var modelData
                objectName: "exportTimelineAs:" + modelData.id
                text: qsTr("Export Timeline as %1\u2026").arg(modelData.name)
                enabled: modelData.available
                onTriggered: root.timelineExportRequested(modelData.id)
            }
            onObjectAdded: function (index, object) { timelineMenu.insertItem(index, object) }
            onObjectRemoved: function (index, object) { timelineMenu.removeItem(object) }
        }
    }

    ContextMenu {
        id: jobMenu
        objectName: "jobMenu"
        property var job: ({})
        readonly property string status: job.status !== undefined ? job.status : ""
        readonly property bool running: status === "rendering" || status === "paused"
        readonly property bool finished: status === "completed" || status === "warning" || status === "failed" || status === "cancelled"
        MenuAction { text: qsTr("Pause"); iconName: "pause"; visible: jobMenu.status === "rendering"; height: visible ? implicitHeight : 0; onTriggered: root.jobAction("pause", jobMenu.job.id) }
        MenuAction { text: qsTr("Resume"); iconName: "play"; visible: jobMenu.status === "paused"; height: visible ? implicitHeight : 0; onTriggered: root.jobAction("resume", jobMenu.job.id) }
        MenuAction { text: qsTr("Cancel"); iconName: "x"; visible: jobMenu.running || jobMenu.status === "queued"; height: visible ? implicitHeight : 0; onTriggered: root.jobAction("cancel", jobMenu.job.id) }
        MenuAction { text: qsTr("Retry"); iconName: "refresh-cw"; visible: jobMenu.finished; height: visible ? implicitHeight : 0; onTriggered: root.jobAction("retry", jobMenu.job.id) }
        MenuAction { text: qsTr("Duplicate"); iconName: "copy"; onTriggered: root.jobAction("duplicate", jobMenu.job.id) }
        MenuAction { text: qsTr("Move Up"); iconName: "chevron-up"; enabled: jobMenu.status === "queued"; onTriggered: root.jobAction("up", jobMenu.job.id) }
        MenuAction { text: qsTr("Move Down"); iconName: "chevron-down"; enabled: jobMenu.status === "queued"; onTriggered: root.jobAction("down", jobMenu.job.id) }
        C.MenuSeparator { }
        MenuAction { text: qsTr("Open Output"); iconName: "circle-play"; enabled: jobMenu.status === "completed" || jobMenu.status === "warning"; onTriggered: root.jobAction("open", jobMenu.job.id) }
        MenuAction { text: qsTr("Reveal in File Manager"); iconName: "folder-open"; enabled: jobMenu.status === "completed" || jobMenu.status === "warning"; onTriggered: root.jobAction("reveal", jobMenu.job.id) }
        MenuAction { text: qsTr("Show Log…"); iconName: "list"; onTriggered: root.logRequested(jobMenu.job.id) }
        C.MenuSeparator { }
        MenuAction { text: qsTr("Remove"); iconName: "trash-2"; enabled: !jobMenu.running; onTriggered: root.jobAction("remove", jobMenu.job.id) }
    }
}
