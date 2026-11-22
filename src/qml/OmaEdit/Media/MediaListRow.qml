import QtQuick
import OmaEdit.Ui

// The list-mode row: a small poster, then columns — name, duration,
// resolution, frame rate, rating, tags, created, size.
Item {
    id: root

    property var entry: ({})
    property bool selected: false
    property bool used: false
    property string durationText: ""
    property string resolutionText: ""
    property string fpsText: ""
    property string createdText: ""
    property string sizeText: ""

    signal activated(int modifiers)
    signal opened()
    signal contextRequested()

    readonly property bool offline: entry.offline !== undefined && entry.offline

    Accessible.role: Accessible.ListItem
    Accessible.name: entry.name !== undefined ? entry.name : ""
    Accessible.selected: selected

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.panelRaised
             : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
        Rectangle { visible: root.selected; width: Theme.activeUnderline; height: parent.height; color: Theme.accent }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border; opacity: 0.6 }
    }

    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated(point.modifiers)
        onDoubleTapped: root.opened()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.contextRequested()
    }

    Row {
        anchors.fill: parent
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        spacing: Theme.s3

        Rectangle {
            width: Math.round((parent.height - Theme.s2) * 16 / 9)
            height: parent.height - Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            radius: 2
            clip: true
            color: Theme.clipGap
            opacity: root.offline ? 0.5 : 1
            Image {
                anchors.fill: parent
                source: root.entry.thumb !== undefined ? root.entry.thumb : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
        }
        Text {
            width: Math.max(120, parent.width * 0.30)
            anchors.verticalCenter: parent.verticalCenter
            text: root.entry.name !== undefined ? root.entry.name : ""
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsBase
            color: root.offline ? Theme.danger : root.selected ? Theme.textPrimary : Theme.textSecondary
        }
        Text { width: 84; anchors.verticalCenter: parent.verticalCenter; text: root.durationText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        Text { objectName: "listResolution"; width: 72; anchors.verticalCenter: parent.verticalCenter; text: root.resolutionText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; elide: Text.ElideRight }
        Text { width: 56; anchors.verticalCenter: parent.verticalCenter; text: root.fpsText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        StarRating {
            tip: qsTr("Rating given to this clip; set it in the inspector")
            width: 70
            anchors.verticalCenter: parent.verticalCenter
            value: root.entry.rating !== undefined ? root.entry.rating : 0
            editable: false
            starSize: Theme.fsLabel
        }
        Row {
            width: Math.max(80, parent.width * 0.14)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1
            clip: true
            Repeater {
                model: root.entry.tags !== undefined ? root.entry.tags : []
                delegate: Chip { required property string modelData; tip: qsTr("A tag on this clip; filter by tag in the bar above"); label: modelData }
            }
        }
        Text { width: 118; anchors.verticalCenter: parent.verticalCenter; text: root.createdText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; elide: Text.ElideRight }
        Text { objectName: "listSize"; width: 76; anchors.verticalCenter: parent.verticalCenter; text: root.sizeText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; elide: Text.ElideRight }
    }
}
