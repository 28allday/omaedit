import QtQuick
import OmaEdit.Ui

// The Media page's source viewer: the stage is a slot (the app drops its
// VideoView in; the preview scene leaves it black), a centred timecode, and
// the transport row beneath.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property string clipName: ""
    property bool hasMedia: false
    property bool offline: false
    property bool playing: false
    property string timecode: "00:00:00:00"
    property string duration: "00:00:00:00"
    property real imageAspect: 16 / 9
    property int nativeWidth: 1920
    property int zoomMode: 0
    property bool sharedView: false
    signal zoomRequested(int index)
    readonly property bool textActive: viewerMenu.visible
    default property alias stageContent: stage.data

    signal snapshotRequested()
    signal volumeRequested()
    signal fullscreenRequested()
    signal playToggled()
    signal paused()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal stepped(int frames)

    color: Theme.panelBg

    SectionTitle {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        height: root.dockBar !== null ? Theme.s1 : implicitHeight // docked, the bar names the panel and the clip
        visible: root.dockBar === null
        text: qsTr("Source Viewer")

        Text {
            text: root.clipName
            elide: Text.ElideMiddle
            width: Math.min(implicitWidth, title.width * 0.6)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Rectangle {
        id: stageFrame
        anchors.top: title.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        anchors.bottomMargin: Theme.s1
        color: "black"
        radius: 2
        clip: true

        Item {
            id: stage
            // Letterboxed to the clip's aspect inside the well.
            readonly property real fitW: Math.min(parent.width, parent.height * root.imageAspect)
            width: root.sharedView ? parent.width : Math.floor(root.zoomMode === 0 ? fitW : root.zoomMode === 1 ? Math.max(parent.width, parent.height * root.imageAspect) : root.nativeWidth * [0, 0, 0.25, 0.5, 1, 2][root.zoomMode])
            height: root.sharedView ? parent.height : Math.floor(width / root.imageAspect)
            anchors.centerIn: parent
        }

        Column {
            anchors.centerIn: parent
            visible: !root.hasMedia
            spacing: Theme.s2
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: root.offline ? "circle-alert" : "film"; size: 28; color: root.offline ? Theme.danger : Qt.rgba(1, 1, 1, 0.12) }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.offline ? qsTr("Media offline") : qsTr("Select a clip to preview it")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }

        // Timecode over the image, as the concept places it.
        Text {
            objectName: "sourceTimecode"
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.s2
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.hasMedia
            text: root.timecode
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsTimecode
            color: Theme.accent
            style: Text.Outline
            styleColor: Qt.rgba(0, 0, 0, 0.8)
        }
    }

    ContextMenu {
        id: viewerMenu
        MenuAction { text: qsTr("Save Source Snapshot…"); enabled: root.hasMedia; onTriggered: root.snapshotRequested() }
        MenuAction { text: qsTr("Monitor Volume…"); onTriggered: root.volumeRequested() }
        MenuAction { text: qsTr("Full Screen Viewer"); enabled: root.hasMedia; onTriggered: root.fullscreenRequested() }
    }
    Item {
        id: transport
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        height: Theme.hControl + Theme.s2

        ComboField {
            objectName: "sourceZoom"
            tip: qsTr("How the source frame is scaled to fit the viewer")
            width: 72
            model: [qsTr("Fit"), qsTr("Fill"), "25%", "50%", "100%", "200%", qsTr("Custom…")]
            currentIndex: root.zoomMode
            onActivated: function (index) { if (index === 6) Commands.trigger("view.playback"); else if (root.sharedView) root.zoomRequested(index); else root.zoomMode = index }
            enabled: root.hasMedia
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
        }

        Row {
            anchors.centerIn: parent
            spacing: 0
            enabled: root.hasMedia
            IconButton { iconName: "skip-back";    text: qsTr("Go to start"); shortcut: "Home"; onClicked: root.startRequested() }
            IconButton { iconName: "rewind";       text: qsTr("Shuttle backwards"); shortcut: "J"; onClicked: root.shuttled(-1) }
            IconButton {
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
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1
            // Extra buttons yield first; the menu keeps every action reachable.
            readonly property bool roomy: transport.width >= 440
            IconButton { visible: parent.roomy; iconName: "camera";   text: qsTr("Save Source Snapshot…"); enabled: root.hasMedia; onClicked: root.snapshotRequested() }
            IconButton { visible: parent.roomy; iconName: "volume-2"; text: qsTr("Monitor Volume…"); onClicked: root.volumeRequested() }
            Text {
                visible: transport.width >= 360
                text: root.duration
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
                opacity: root.hasMedia ? 1 : Theme.disabledOpacity
                anchors.verticalCenter: parent.verticalCenter
            }
            IconButton { objectName: "sourceViewerMenu"; iconName: "more-horizontal"; text: qsTr("Source Viewer Options"); onClicked: viewerMenu.popup() }
        }
    }
}
