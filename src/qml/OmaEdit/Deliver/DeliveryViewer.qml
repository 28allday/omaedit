import QtQuick
import OmaEdit.Ui
import OmaEdit.Media as Media
import "../Ui/MediaTime.js" as MediaTime

// The delivery viewer: the program picture (a slot the app fills with its
// VideoView), the timecode, the transport, and the A/B range bar with the
// range duration beside it. What renders is exactly what plays here.
Rectangle {
    id: root

    property string sequenceName: ""
    property bool hasMedia: false
    property bool playing: false
    property int position: 0
    property int duration: 0
    property int rangeIn: -1
    property int rangeOut: -1
    property real imageAspect: 16 / 9
    property var timecoder: null
    property real fps: 25
    default property alias stageContent: stage.data

    signal fullscreenRequested()
    signal playToggled()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal seekRequested(int frame)
    signal rangeEdited(int rangeIn, int rangeOut)
    signal rangeCleared()

    function tc(frame) { return timecoder ? timecoder(frame) : "00:00:00:00" }
    readonly property int effIn: rangeIn >= 0 ? rangeIn : 0
    readonly property int effOut: rangeOut >= 0 ? rangeOut : Math.max(0, duration - 1)

    color: Theme.panelBg

    Media.SectionTitle {
        id: sectionTitle
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        text: root.sequenceName !== "" ? root.sequenceName : qsTr("Timeline")

        IconButton {
            iconName: "menu"
            text: qsTr("Viewer options")
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            onClicked: viewerMenu.popup()
        }
    }

    Rectangle {
        id: stageFrame
        anchors.top: sectionTitle.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        anchors.bottomMargin: Theme.s1
        color: "black"
        radius: 2

        Item {
            id: stage
            readonly property real fitW: Math.min(parent.width, parent.height * root.imageAspect)
            width: parent.width
            height: parent.height
            clip: true
            anchors.centerIn: parent
        }

        Column {
            anchors.centerIn: parent
            visible: !root.hasMedia
            spacing: Theme.s2
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "film"; size: 28; color: Qt.rgba(1, 1, 1, 0.12) }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("The timeline is empty — nothing to deliver yet")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    Item {
        id: transport
        objectName: "deliverTransport"
        readonly property bool stacked: width < playback.width + 2 * Math.max(tcNow.implicitWidth, durationRow.width) + Theme.s2 * 2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: rangeBar.top
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        height: (stacked ? 2 : 1) * Theme.hControl + Theme.s2

        Text {
            id: tcNow
            objectName: "deliverTimecode"
            anchors.left: parent.left
            y: (Theme.hControl - height) / 2
            text: root.tc(root.position)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsTimecode
            color: Theme.accent
            opacity: root.hasMedia ? 1 : Theme.disabledOpacity
        }

        Row {
            id: playback
            objectName: "deliverPlaybackButtons"
            anchors.horizontalCenter: parent.horizontalCenter
            y: transport.stacked ? Theme.hControl : 0
            spacing: 0
            enabled: root.hasMedia
            IconButton { iconName: "skip-back";    text: qsTr("Go to start"); shortcut: "Home"; onClicked: root.startRequested() }
            IconButton { iconName: "rewind";       text: qsTr("Shuttle backwards"); shortcut: "J"; onClicked: root.shuttled(-1) }
            IconButton { iconName: "square";       text: qsTr("Stop"); shortcut: "K"; onClicked: if (root.playing) root.playToggled() }
            IconButton {
                objectName: "deliverPlay"
                iconName: root.playing ? "pause" : "play"
                text: root.playing ? qsTr("Pause") : qsTr("Play")
                shortcut: "Space"
                iconColor: Theme.textPrimary
                onClicked: root.playToggled()
            }
            IconButton { iconName: "fast-forward"; text: qsTr("Shuttle forwards"); shortcut: "L"; onClicked: root.shuttled(1) }
            IconButton { iconName: "skip-forward"; text: qsTr("Go to end"); shortcut: "End"; onClicked: root.endRequested() }
        }

        Row {
            id: durationRow
            objectName: "deliverDurationControls"
            anchors.right: parent.right
            y: 0
            spacing: Theme.s2
            Text {
                text: root.tc(Math.max(0, root.duration - 1))
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
                opacity: root.hasMedia ? 1 : Theme.disabledOpacity
                anchors.verticalCenter: parent.verticalCenter
            }
            IconButton { iconName: "maximize"; text: qsTr("Full screen viewer"); onClicked: root.fullscreenRequested() }
        }
    }

    Item {
        id: rangeBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        height: 84

        RangeSelector {
            id: selector
            objectName: "rangeSelector"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: Theme.s3
            duration: root.duration
            rangeIn: root.rangeIn
            rangeOut: root.rangeOut
            position: root.position
            timecoder: root.timecoder
            onRangeEdited: function (a, b) { root.rangeEdited(a, b) }
            onSeekRequested: function (f) { root.seekRequested(f) }
        }

        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            height: Theme.hControl + Theme.s1

            // One action, so a button rather than a one-item list (whose
            // name was cut to "Viewe…").
            ToolButton {
                objectName: "deliverViewerSettings"
                showLabel: true
                iconName: "monitor"
                text: qsTr("Viewer…")
                onClicked: Commands.trigger("view.playback")
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                Tooltip { text: qsTr("Open the viewer and playback preferences"); visible: parent.hovered && !parent.down }
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2
                IconButton {
                    iconName: "x"
                    text: qsTr("Clear range (whole timeline)")
                    shortcut: "Alt+X"
                    visible: root.rangeIn >= 0 || root.rangeOut >= 0
                    onClicked: root.rangeCleared()
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    visible: rangeBar.width >= 400
                    text: qsTr("Range Duration")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    width: 112
                    height: Theme.hControl
                    radius: Theme.rControl
                    color: Theme.controlBg
                    border.width: Theme.hairline
                    border.color: Theme.border
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        objectName: "rangeDuration"
                        anchors.centerIn: parent
                        text: root.hasMedia ? MediaTime.durationText(Math.max(0, root.effOut - root.effIn + 1), root.fps) : "–"
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textPrimary
                    }
                }
            }
        }
    }

    ContextMenu {
        id: viewerMenu
        parent: sectionTitle
        y: sectionTitle.height
        MenuAction { text: qsTr("Set In at Playhead"); shortcut: "I"; enabled: root.hasMedia; onTriggered: root.rangeEdited(root.position, Math.max(root.position, root.effOut)) }
        MenuAction { text: qsTr("Set Out at Playhead"); shortcut: "O"; enabled: root.hasMedia; onTriggered: root.rangeEdited(Math.min(root.effIn, root.position), root.position) }
        MenuAction { text: qsTr("Clear Range"); shortcut: "Alt+X"; enabled: root.rangeIn >= 0 || root.rangeOut >= 0; onTriggered: root.rangeCleared() }
    }
}
