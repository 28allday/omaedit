import QtQuick

// One of the project's timelines among the clips, in the Edit page's library
// and on the Media page. `item` is a row of Backend.timelineItems.
Item {
    id: root

    property var item: ({})
    property bool selected: false
    property bool listMode: false
    property bool draggable: false
    // The space under the poster, for the name and the details.
    property int captionHeight: 40

    signal activated()
    signal opened()
    signal contextRequested()
    signal dragChanged(bool active, var handler)

    Accessible.role: Accessible.ListItem
    Accessible.name: item.name !== undefined ? item.name : ""
    Accessible.selected: selected

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.controlBg
             : hover.hovered ? Theme.hovered(Theme.panelBg)
             : "transparent"
        radius: Theme.rControl
        border.width: root.selected ? Theme.selectionRing : 0
        border.color: Theme.accent
    }
    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated()
        onDoubleTapped: root.opened()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: {
            root.activated()
            root.contextRequested()
        }
    }
    DragHandler {
        id: drag
        target: null
        enabled: root.draggable
        acceptedButtons: Qt.LeftButton
        onActiveChanged: root.dragChanged(active, drag)
    }
    Rectangle {
        id: poster
        x: Theme.s1
        y: Theme.s1
        width: root.listMode ? Math.round(height * 16 / 9) : parent.width - Theme.s1 * 2
        height: root.listMode ? parent.height - Theme.s1 * 2 : parent.height - root.captionHeight
        radius: Theme.rControl
        clip: true
        color: Theme.controlBg
        Image {
            anchors.fill: parent
            source: root.item.thumb !== undefined ? root.item.thumb : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }
        Icon {
            anchors.centerIn: parent
            visible: !root.item.thumb
            name: "film"
            size: 18
            color: Theme.textMuted
        }
        // Tells it from the clips at a glance.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: Theme.s1
            width: badge.implicitWidth + Theme.s2
            height: 14
            radius: 2
            color: Theme.accent
            Text {
                id: badge
                anchors.centerIn: parent
                text: root.item.synced === true ? qsTr("SYNCED") : qsTr("TIMELINE")
                font.family: Theme.fontFamily
                font.pixelSize: 8
                font.weight: Theme.wHeading
                color: Theme.accentText
            }
        }
    }
    Text {
        id: name
        objectName: "timelineTileName"
        // Placed by hand: the view mode can change under a live tile, and
        // anchors that swap between edges leave it where it was.
        x: root.listMode ? poster.x + poster.width + Theme.s2 : poster.x
        y: root.listMode ? Math.round((root.height - height) / 2) - Theme.s2 : poster.y + poster.height + Theme.s1
        width: root.width - x - Theme.s2
        text: root.item.name !== undefined ? root.item.name : ""
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
        elide: Text.ElideMiddle
    }
    Row {
        x: name.x
        y: name.y + name.height
        spacing: Theme.s2
        Text {
            text: root.item.dur !== undefined ? root.item.dur : ""
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            text: root.item.height > 0 ? root.item.height + "p" : ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            objectName: "timelineOpenLabel"
            visible: root.item.active === true
            text: qsTr("Open")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.accent
        }
    }
}
