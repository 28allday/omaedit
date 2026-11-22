import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

Rectangle {
    id: root
    objectName: "appStatusBar"
    property string proxyState: qsTr("No media")
    property bool proxyReady: true
    property string proxyDetails: ""
    property real renderCache: -1
    property string cacheText: qsTr("Cache idle")
    property int activeTasks: 0
    property bool rendering: false
    property bool diskLow: false
    property string resolution: ""
    property string frameRate: ""
    property string aspect: ""
    property string colorSpace: ""
    property string diskFree: ""
    property string message: ""
    property var tasks
    readonly property bool infoControlsFocused: info.controlsFocused
    signal messagesRequested()
    signal tasksRequested()
    signal playbackRequested()
    property string playbackText: qsTr("Viewer")
    property bool playbackWarning: false
    implicitHeight: Theme.hStatusBar
    color: Theme.panelBg
    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        spacing: Theme.s2
        C.ToolButton {
            objectName: "backgroundTasksButton"
            Layout.preferredHeight: root.height - 2
            text: root.rendering ? qsTr("Rendering · %1 tasks").arg(root.activeTasks) : root.activeTasks ? qsTr("Tasks · %1 active").arg(root.activeTasks) : qsTr("Tasks")
            font.pixelSize: Theme.fsLabel
            Accessible.name: qsTr("Open Background Tasks")
            onClicked: root.tasksRequested()
            C.ToolTip.visible: hovered
            C.ToolTip.delay: Theme.tooltipDelay
            C.ToolTip.text: qsTr("Background Tasks and messages")
        }
        C.ToolButton {
            objectName: "viewerPlaybackButton"
            Layout.preferredHeight: root.height - 2
            text: root.playbackText
            font.pixelSize: Theme.fsLabel
            palette.buttonText: root.playbackWarning ? Theme.warning : Theme.textSecondary
            onClicked: root.playbackRequested()
            Accessible.name: qsTr("Open Viewer and Playback")
            C.ToolTip.visible: hovered
            C.ToolTip.delay: Theme.tooltipDelay
            C.ToolTip.text: qsTr("Viewer zoom, preview quality, monitoring and frame export")
        }
        C.Label {
            objectName: "proxyStatus"
            Layout.maximumWidth: root.width * .19
            text: (root.proxyReady ? "" : qsTr("Warning: ")) + root.proxyState
            elide: Text.ElideRight
            color: root.proxyReady ? Theme.textSecondary : Theme.warning
            font.pixelSize: Theme.fsLabel
            HoverHandler { id: proxyHover }
            C.ToolTip.visible: proxyHover.hovered
            C.ToolTip.delay: 450
            C.ToolTip.text: root.proxyDetails
            Accessible.description: root.proxyDetails
        }
        Text { text: root.cacheText + (root.renderCache >= 0 ? " " + Math.round(root.renderCache * 100) + "%" : ""); font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        InfoArea {
            id: info
            Layout.fillWidth: true
            Layout.minimumWidth: 140
            Layout.preferredHeight: root.height - 2
            tasks: root.tasks
            message: root.message
            onMessagesRequested: root.messagesRequested()
        }
        Text { text: [root.resolution, root.frameRate, root.aspect, root.colorSpace].filter(function(s) { return s !== "" }).join(" | "); visible: root.width >= 1000; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
        Text { text: (root.diskLow ? qsTr("Low space · ") : "") + root.diskFree; font.pixelSize: Theme.fsLabel; color: root.diskLow ? Theme.warning : Theme.textSecondary }
    }
}
