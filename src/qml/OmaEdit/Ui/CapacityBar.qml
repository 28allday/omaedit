import QtQuick

// A thin used/total bar for the storage sidebar. Turns amber past 85% and red
// past 95% — a full scratch disk is the failure an editor meets first.
Rectangle {
    id: root

    property real fraction: 0    // 0..1 used

    implicitWidth: 80
    implicitHeight: 3
    radius: 1
    color: Theme.border

    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("%1% used").arg(Math.round(fraction * 100))

    Rectangle {
        width: Math.round(parent.width * Math.max(0, Math.min(1, root.fraction)))
        height: parent.height
        radius: parent.radius
        color: root.fraction > 0.95 ? Theme.danger
             : root.fraction > 0.85 ? Theme.warning : Theme.accent
    }
}
