import QtQuick
import OmaEdit.Ui

// A role group's header row across the track column and the lane area:
// chevron, role name, track count, tinted by the role.
Rectangle {
    id: root

    property string role: ""
    property int trackCount: 0
    property bool expanded: true
    property bool lane: false          // the wide, quieter version behind the clips
    signal toggled()

    height: Theme.hAudioGroupHeader
    color: lane ? Qt.rgba(Theme.roleColor(role).r, Theme.roleColor(role).g, Theme.roleColor(role).b, 0.10)
                : Qt.darker(Theme.roleColor(role), 1.6)

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }

    Row {
        visible: !root.lane
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
        Icon { anchors.verticalCenter: parent.verticalCenter; name: root.expanded ? "chevron-down" : "chevron-right"; size: 12; color: Theme.textPrimary }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.roleTitle(root.role)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.trackCount > 1 ? String(root.trackCount) : ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textSecondary
        }
    }
    TapHandler { enabled: !root.lane; onTapped: root.toggled() }
}
