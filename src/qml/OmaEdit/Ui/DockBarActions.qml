import QtQuick

// A panel's header actions. Inside a DockPanel they ride the dock bar, so the
// panel gives its whole body to content; standalone (a preview scene, a floating
// tool), or when the bar is too narrow to hold them, they stay in the panel's
// own header. The decision reads this row's implicit width and the bar's free
// width — neither depends on where the row is parented, so it cannot loop.
Row {
    id: root

    property Item dockBar: null     // DockPanel.barSlot, or null when standalone
    required property Item fallback // the panel's own header
    property real fallbackMargin: Theme.s1
    readonly property bool inBar: dockBar !== null && implicitWidth > 0
                                  && implicitWidth <= dockBar.available

    parent: inBar ? dockBar : fallback
    spacing: 2
    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    anchors.right: !inBar && parent ? parent.right : undefined
    anchors.rightMargin: inBar ? 0 : fallbackMargin
    anchors.left: inBar && parent ? parent.left : undefined
}
