import QtQuick
import OmaEdit.Ui

// One job in the render queue table. Columns are laid out from the widths
// the panel hands down so the header and every row agree to the pixel.
Rectangle {
    id: root

    property var job: ({})
    property int row: 0
    property var columns: ({})   // {index, name, preset, status, progress, duration, destination}
    property bool selected: false

    signal activated()
    signal menuRequested(var at)
    signal doubleClicked()

    readonly property string status: job.status !== undefined ? job.status : "queued"
    readonly property int percent: job.percent !== undefined ? job.percent : 0

    width: parent ? parent.width : 800
    implicitHeight: Theme.hRow + Theme.s3
    color: selected ? Theme.panelRaised : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"

    activeFocusOnTab: true
    Keys.onSpacePressed: root.activated()
    Keys.onReturnPressed: root.doubleClicked()
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            root.activated(); root.menuRequested(Qt.point(Math.min(width - 24, columns.actions), height)); event.accepted = true
        }
    }
    Accessible.onPressAction: root.activated()
    border.width: activeFocus ? 1 : 0
    border.color: Theme.accent
    Accessible.role: Accessible.ListItem
    Accessible.name: (job.name || "") + " " + statusLabel(status)

    function statusLabel(s) {
        switch (s) {
        case "queued": return qsTr("Queued")
        case "rendering": return qsTr("Rendering")
        case "paused": return qsTr("Paused")
        case "completed": return qsTr("Completed")
        case "warning": return qsTr("Completed")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        }
        return s
    }
    function statusIcon(s) {
        switch (s) {
        case "queued": return "clock"
        case "rendering": return "loader"
        case "paused": return "pause"
        case "completed": return "circle-check"
        case "warning": return "circle-alert"
        case "failed": return "circle-alert"
        case "cancelled": return "x"
        }
        return "circle"
    }
    function statusColor(s) {
        switch (s) {
        case "rendering": return Theme.accent
        case "paused": return Theme.warning
        case "completed": return Theme.audio
        case "warning": return Theme.warning
        case "failed": return Theme.danger
        case "cancelled": return Theme.textMuted
        }
        return Theme.textSecondary
    }
    function clock(seconds) {
        if (seconds === undefined || seconds < 0) return "--:--"
        var s = Math.max(0, Math.round(seconds))
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60)
    }
    function durationText() {
        if (status === "rendering" && job.remaining !== undefined && job.remaining > 0)
            return clock(job.elapsed) + " · " + qsTr("%1 left").arg(clock(job.remaining))
        return clock(job.elapsed)
    }
    function stageText() {
        switch (job.stage) {
        case "check": return qsTr("Checking encoder")
        case "pre": return qsTr("Baking retimes")
        case "render": return qsTr("Rendering")
        case "join": return qsTr("Joining")
        case "encode": return qsTr("Encoding")
        case "captions": return qsTr("Captions")
        case "tags": return qsTr("Tagging colour")
        }
        return statusLabel(status)
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border; opacity: 0.6 }

    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated()
        onDoubleTapped: root.doubleClicked()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: function (point) { root.activated(); root.menuRequested(point.position) }
    }

    Text {
        x: root.columns.index
        anchors.verticalCenter: parent.verticalCenter
        text: root.row + 1
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    Text {
        x: root.columns.name
        width: root.columns.preset - root.columns.name - Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        text: root.job.name !== undefined ? root.job.name : ""
        elide: Text.ElideMiddle
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsBase
        color: Theme.textPrimary
    }
    Text {
        visible: root.columns.showPreset !== false
        x: root.columns.preset
        width: root.columns.status - root.columns.preset - Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        text: root.job.preset !== undefined ? root.job.preset : ""
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsBase
        color: Theme.textSecondary
    }
    Row {
        x: root.columns.status
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
        Icon {
            name: root.statusIcon(root.status)
            size: Theme.szIcon - 2
            color: root.statusColor(root.status)
            anchors.verticalCenter: parent.verticalCenter
            RotationAnimation on rotation {
                running: root.status === "rendering" && !Theme.reducedMotion
                from: 0; to: 360; duration: 1200; loops: Animation.Infinite
            }
        }
        Text {
            objectName: "queueStatus"
            text: root.status === "rendering" ? root.stageText() : root.statusLabel(root.status)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsBase
            color: root.status === "failed" ? Theme.dangerText : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    Row {
        x: root.columns.progress
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2
        Text {
            width: 34
            text: root.percent + "%"
            horizontalAlignment: Text.AlignRight
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }
        Rectangle {
            width: root.columns.duration - root.columns.progress - 34 - Theme.s2 * 3
            height: 4
            radius: 2
            color: Theme.controlBg
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                width: parent.width * root.percent / 100
                height: parent.height
                radius: 2
                color: root.status === "failed" ? Theme.danger : root.status === "paused" ? Theme.warning
                     : root.status === "cancelled" ? Theme.textMuted : Theme.accent
                Behavior on width { NumberAnimation { duration: Theme.dNormal; easing.type: Theme.easing } }
            }
        }
    }
    Text {
        visible: root.columns.showDuration !== false
        x: root.columns.duration
        anchors.verticalCenter: parent.verticalCenter
        text: root.durationText()
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }
    Text {
        visible: root.columns.showDestination !== false
        x: root.columns.destination
        width: root.columns.actions - root.columns.destination - Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        text: root.status === "failed" && root.job.error !== undefined && root.job.error !== ""
              ? root.job.error : (root.job.path !== undefined ? root.job.path : "")
        elide: Text.ElideMiddle
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: root.status === "failed" ? Theme.danger : Theme.textMuted
    }
    Tooltip { visible: hover.hovered; text: (root.job.name || "") + "\n" + (root.job.path || "") + (root.job.error ? "\n" + root.job.error : "") }
    IconButton {
        objectName: "queueJobActions"
        x: root.columns.actions
        anchors.verticalCenter: parent.verticalCenter
        iconName: "more-vertical"
        text: qsTr("Job actions")
        implicitWidth: Theme.szIcon + Theme.s1
        implicitHeight: Theme.szIcon + Theme.s1
        onClicked: { root.activated(); root.menuRequested(Qt.point(x, y + height)) }
    }
}
