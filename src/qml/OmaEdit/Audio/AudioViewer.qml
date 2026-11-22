import QtQuick
import OmaEdit.Ui

// The Audio page's centre: the programme picture with a transport, the
// current timecode and duration, and the master's horizontal meter.
Rectangle {
    id: root

    property string sequenceName: ""
    property bool hasMedia: false
    property bool playing: false
    property bool auditioning: false
    property int position: 0
    property int duration: 0
    property real imageAspect: 16 / 9
    property real fps: 25
    property real levelLeft: -100
    property real levelRight: -100
    property var monitorSettings: ({})
    signal monitorEdited(bool muted, bool dim, bool mono)
    property bool muted: false
    property bool loop: false
    property Component videoSurface: null
    property var timecoder: null

    signal playToggled()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal stepRequested(int frames)
    signal loopToggled(bool on)
    signal muteToggled(bool on)
    signal snapshotRequested()

    color: Theme.windowBg

    function tc(f) {
        if (timecoder) return timecoder(f)
        var rate = fps > 0 ? fps : 25
        var s = Math.floor(f / rate), ff = Math.round(f - s * rate)
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff)
    }

    Item {
        id: header
        width: parent.width
        height: Theme.hTab
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            text: root.auditioning ? qsTr("Audition") : qsTr("Program: %1").arg(root.sequenceName !== "" ? root.sequenceName : qsTr("Timeline"))
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }

    Item {
        id: picture
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.margins: Theme.s2
        readonly property real fitW: Math.min(width, height * root.imageAspect)
        readonly property real fitH: fitW / root.imageAspect
        Rectangle {
            anchors.centerIn: parent
            width: root.videoSurface !== null ? parent.width : parent.fitW
            height: root.videoSurface !== null ? parent.height : parent.fitH
            color: Theme.meterWell
            Loader {
                anchors.fill: parent
                active: root.hasMedia && root.videoSurface !== null
                sourceComponent: root.videoSurface
            }
            EmptyState {
                anchors.centerIn: parent
                visible: !root.hasMedia
                iconName: "video"
                title: qsTr("No media")
                description: qsTr("Import a clip or drop a sound on the timeline.")
            }
        }
    }

    Item {
        id: transport
        objectName: "audioTransport"
        readonly property bool stacked: width < playback.width + 2 * Math.max(tcNow.implicitWidth, durationRow.width) + Theme.s2 * 2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: monitorControls.top
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        height: (stacked ? 2 : 1) * Theme.hControl + Theme.s2

        Text {
            id: tcNow
            objectName: "audioTimecode"
            anchors.left: parent.left
            y: (Theme.hControl - height) / 2
            text: root.tc(root.position)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsTimecode
            color: Theme.accent
        }
        Row {
            id: playback
            objectName: "audioPlaybackButtons"
            anchors.horizontalCenter: parent.horizontalCenter
            y: transport.stacked ? Theme.hControl : 0
            spacing: 0
            IconButton { iconName: "skip-back"; shortcut: "Home"; enabled: root.hasMedia; Accessible.name: qsTr("Go to start"); onClicked: root.startRequested() }
            IconButton { iconName: "rewind"; shortcut: "J"; enabled: root.hasMedia; Accessible.name: qsTr("Shuttle back"); onClicked: root.shuttled(-1) }
            IconButton { iconName: "chevron-left"; shortcut: "←"; enabled: root.hasMedia; Accessible.name: qsTr("Step back"); onClicked: root.stepRequested(-1) }
            IconButton {
                objectName: "audioPlay"
                iconName: root.playing ? "pause" : "play"
                shortcut: "Space"
                enabled: root.hasMedia
                iconColor: Theme.textPrimary
                Accessible.name: root.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: root.playToggled()
            }
            IconButton { iconName: "chevron-right"; shortcut: "→"; enabled: root.hasMedia; Accessible.name: qsTr("Step forward"); onClicked: root.stepRequested(1) }
            IconButton { iconName: "fast-forward"; shortcut: "L"; enabled: root.hasMedia; Accessible.name: qsTr("Shuttle forward"); onClicked: root.shuttled(1) }
            IconButton { iconName: "skip-forward"; shortcut: "End"; enabled: root.hasMedia; Accessible.name: qsTr("Go to end"); onClicked: root.endRequested() }
            IconButton { objectName: "audioLoop"; iconName: "refresh-cw"; checked: root.loop; Accessible.name: qsTr("Loop range"); onClicked: root.loopToggled(!root.loop) }
        }
        Row {
            id: durationRow
            objectName: "audioDurationControls"
            anchors.right: parent.right
            y: 0
            spacing: Theme.s1
            IconButton { iconName: "camera"; enabled: root.hasMedia; Accessible.name: qsTr("Grab still"); onClicked: root.snapshotRequested() }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.tc(Math.max(0, root.duration - 1))
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsTimecode
                color: Theme.textSecondary
            }
        }
    }

    Row {
        id: monitorControls
        anchors.left: parent.left; anchors.bottom: meters.top; anchors.margins: Theme.s2
        spacing: Theme.s1
        ToolButton { text: qsTr("Mute"); showLabel: true; checked: root.monitorSettings.muted === true; onClicked: root.monitorEdited(!checked, root.monitorSettings.dim === true, root.monitorSettings.mono === true) }
        ToolButton { text: qsTr("Dim −20 dB"); showLabel: true; checked: root.monitorSettings.dim === true; onClicked: root.monitorEdited(root.monitorSettings.muted === true, !checked, root.monitorSettings.mono === true) }
        ToolButton { text: qsTr("Mono"); showLabel: true; checked: root.monitorSettings.mono === true; onClicked: root.monitorEdited(root.monitorSettings.muted === true, root.monitorSettings.dim === true, !checked) }
    }
    HMeter {
        id: meters
        objectName: "programMeters"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s3
        anchors.topMargin: 0
        levelLeft: root.levelLeft
        levelRight: root.levelRight
    }
}
