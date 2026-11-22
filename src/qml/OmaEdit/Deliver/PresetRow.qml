import QtQuick
import OmaEdit.Ui

// One preset in the browser: icon, name, its resolution (or a badge) at the
// right, a star when favourited, a ⋮ menu on hover. Selected rows carry the
// raised chrome and the cyan edge, like every list in the app.
Rectangle {
    id: root

    property string iconName: "film"
    property string label: ""
    property string detail: ""
    property string badge: ""
    property bool selected: false
    property bool favorite: false
    property bool modified: false
    property int depth: 0

    signal activated()
    signal menuRequested(var at)

    width: parent ? parent.width : 300
    implicitHeight: Theme.hRow + Theme.s2
    color: selected ? Theme.panelRaised
         : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
    border.width: selected || activeFocus ? Theme.hairline : 0
    border.color: Theme.accent
    radius: Theme.rControl

    activeFocusOnTab: true
    Keys.onSpacePressed: root.activated()
    Keys.onReturnPressed: root.activated()
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            root.menuRequested(Qt.point(width - 24, height)); event.accepted = true
        }
    }
    Accessible.onPressAction: root.activated()
    Accessible.role: Accessible.ListItem
    Accessible.name: label
    Accessible.selected: selected

    Behavior on color {
        ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
    }

    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: function (point) { root.menuRequested(point.position) }
    }

    Icon {
        id: glyph
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3 + root.depth * Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        name: root.iconName
        size: Theme.szIcon - 2
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
    }

    Text {
        anchors.left: glyph.right
        anchors.leftMargin: Theme.s2
        anchors.right: trailing.left
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        text: root.label + (root.modified ? qsTr(" (modified)") : "")
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsBase
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
    }

    Row {
        id: trailing
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Icon {
            visible: root.favorite
            name: "star"
            size: Theme.szIcon - 4
            color: Theme.accent
            anchors.verticalCenter: parent.verticalCenter
        }
        Rectangle {
            visible: root.badge !== ""
            width: badgeText.implicitWidth + Theme.s2
            height: Theme.hRow - 6
            radius: Theme.rControl
            color: "transparent"
            border.width: Theme.hairline
            border.color: root.selected ? Theme.accent : Theme.borderStrong
            anchors.verticalCenter: parent.verticalCenter
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: root.badge
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 1
                color: root.selected ? Theme.accent : Theme.textSecondary
            }
        }
        Text {
            visible: root.detail !== "" && root.badge === ""
            text: root.detail
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            anchors.verticalCenter: parent.verticalCenter
        }
        IconButton {
            iconName: "more-vertical"
            text: qsTr("Preset actions")
            visible: hover.hovered || root.selected
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.menuRequested(Qt.point(x + width, y))
        }
    }
}
