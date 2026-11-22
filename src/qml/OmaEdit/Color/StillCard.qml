import QtQuick
import OmaEdit.Ui

// A still in the gallery: its picture, name, timecode, resolution and a
// star; the current one ringed.
Rectangle {
    id: root

    property var still: ({})
    property bool selected: false
    property string thumbSource: ""
    property var timecoder: null
    signal activated()
    signal doubleActivated()
    signal contextRequested()
    signal favoriteToggled(bool on)

    color: selected ? Theme.controlBg : hover.hovered ? Theme.hovered(Theme.panelBg) : Theme.panelBg
    border.color: selected ? Theme.accent : Theme.border
    border.width: selected ? Theme.selectionRing : Theme.hairline
    radius: Theme.rControl
    implicitHeight: pic.height + Theme.s1 + name.height + meta.height + Theme.s2

    HoverHandler { id: hover }
    TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.activated(); onDoubleTapped: root.doubleActivated() }
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: { root.activated(); root.contextRequested() } }

    Rectangle {
        id: pic
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.hairline
        height: width * 9 / 16
        color: Theme.meterWell
        clip: true
        Image {
            objectName: "stillImage"
            anchors.fill: parent
            source: root.thumbSource
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            visible: status === Image.Ready
        }
        Icon { anchors.centerIn: parent; name: "image"; color: Theme.textMuted; visible: root.thumbSource === "" }
        IconButton {
            anchors.top: parent.top
            anchors.right: parent.right
            iconName: "star"
            iconColor: root.still.favorite === true ? Theme.warning : hover.hovered ? Theme.textPrimary : "transparent"
            Accessible.name: qsTr("Favourite")
            onClicked: root.favoriteToggled(!(root.still.favorite === true))
        }
    }
    Text {
        id: name
        anchors.top: pic.bottom
        anchors.topMargin: Theme.s1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s1
        anchors.rightMargin: Theme.s1
        text: root.still.name !== undefined ? root.still.name : ""
        elide: Text.ElideMiddle
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textPrimary
    }
    Row {
        id: meta
        anchors.top: name.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s1
        anchors.rightMargin: Theme.s1
        height: Theme.fsLabel + 4
        // The timecode comes first; the picture size only when both fit.
        Text {
            id: stillTimecode
            objectName: "stillTimecode"
            width: parent.width - (res.visible ? res.width : 0)
            elide: Text.ElideRight
            text: root.timecoder ? root.timecoder(root.still.frame !== undefined ? root.still.frame : 0) : ""
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textMuted
        }
        Text {
            id: res
            objectName: "stillResolution"
            visible: stillTimecode.implicitWidth + Theme.s1 + implicitWidth <= meta.width
            width: implicitWidth + Theme.s1
            horizontalAlignment: Text.AlignRight
            text: root.still.height >= 2160 ? "4K" : root.still.height >= 1080 ? qsTr("HD") : root.still.height !== undefined ? root.still.height + "p" : ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textMuted
        }
    }
}
