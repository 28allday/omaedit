import QtQuick
import OmaEdit.Ui

// A sidebar row: icon, label, and either a count or a capacity readout. The
// depth indents nested drives; the selected row carries the raised chrome and
// a 2 px cyan edge, as the brief's active-tab rule.
Rectangle {
    id: root

    property string iconName: "folder"
    property string label: ""
    property string detail: ""        // second line (e.g. "1.2 TB free")
    property int count: -1            // -1 = no count
    property real capacity: -1        // 0..1 = show the bar
    property int depth: 0
    property bool selected: false
    property bool dimmed: false
    // Drawn as dimmed, yet still taken: a drive that is not mounted mounts
    // when activated.
    property bool faded: false
    property bool starred: false
    property string tip: ""

    signal activated()
    signal contextRequested()

    width: parent ? parent.width : 200
    implicitHeight: detail !== "" ? Theme.hRow + Theme.s3 : Theme.hRow
    color: selected ? Theme.panelRaised
         : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
    opacity: dimmed || faded ? Theme.disabledOpacity : 1

    activeFocusOnTab: enabled && !dimmed
    readonly property bool keyboardConsumesKeys: true
    Keys.onSpacePressed: if (enabled && !dimmed) root.activated()
    Keys.onReturnPressed: if (enabled && !dimmed) root.activated()
    Keys.onEnterPressed: if (enabled && !dimmed) root.activated()
    Keys.onMenuPressed: if (enabled && !dimmed) root.contextRequested()

    Accessible.role: Accessible.ListItem
    Accessible.name: label
    Accessible.description: tip
    Accessible.selected: selected

    Behavior on color {
        ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
    }

    Rectangle {
        visible: root.selected
        width: Theme.activeUnderline
        height: parent.height
        color: Theme.accent
    }

    HoverHandler { id: hover }
    Tooltip { text: root.tip; visible: root.tip !== "" && hover.hovered }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.contextRequested()
    }

    Icon {
        id: glyph
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3 + root.depth * Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        name: root.iconName
        size: Theme.szIcon - 2
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
    }

    Column {
        anchors.left: glyph.right
        anchors.leftMargin: Theme.s2
        anchors.right: trailing.left
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1

        Text {
            width: parent.width
            text: root.label
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsBase
            color: root.selected ? Theme.textPrimary : Theme.textSecondary
        }
        Text {
            visible: root.detail !== ""
            width: parent.width
            text: root.detail
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textMuted
        }
        CapacityBar {
            visible: root.capacity >= 0
            width: Math.min(parent.width, 96)
            fraction: root.capacity
        }
    }

    Item {
        id: trailing
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        width: countText.visible ? countText.implicitWidth : star.visible ? star.width : 0
        height: parent.height

        Text {
            id: countText
            visible: root.count >= 0
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.count
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: root.selected ? Theme.accent : Theme.textMuted
        }
        Icon {
            id: star
            visible: root.starred
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            name: "star"
            size: Theme.szIcon - 4
            color: Theme.accent
        }
    }
}
