import QtQuick
import QtQuick.Templates as T

// Thin overlay scrollbar. Stays out of the way in a dense layout: shown while
// scrolling (or always, when asked), thickening on hover so it can actually be
// grabbed. In a box (atRest) it is a thin line whenever there is more than the
// view shows, so a list cut at its edge says it scrolls; nothing when it all
// fits. Built on the bare template: the app style's own scroll bar animates
// the track to full opacity while active, which painted an opaque strip over
// the content beside it.
T.ScrollBar {
    id: root

    // Set in boxes; the main window's panels leave it off.
    property bool atRest: false
    // The most it takes across, thickened under the pointer: what a view
    // leaves clear at its edge so the bar covers none of its content.
    readonly property real reach: 8 + (orientation === Qt.Horizontal ? topPadding + bottomPadding : leftPadding + rightPadding)

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    visible: policy !== T.ScrollBar.AlwaysOff
    padding: 2
    minimumSize: 0.06

    contentItem: Rectangle {
        implicitWidth: root.hovered || root.pressed ? 8 : 4
        implicitHeight: root.hovered || root.pressed ? 8 : 4
        radius: width / 2
        color: root.pressed ? Theme.textSecondary
             : root.hovered ? Theme.textMuted : Theme.borderStrong
        opacity: root.size < 1.0 && (root.atRest || root.policy === T.ScrollBar.AlwaysOn || root.active) ? 1.0 : 0.0

        Behavior on implicitWidth {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
        Behavior on implicitHeight {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
        Behavior on opacity {
            NumberAnimation { duration: Theme.dNormal; easing.type: Theme.easing }
        }
        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    background: Rectangle {
        color: Theme.windowBg
        opacity: root.hovered || root.pressed ? 0.6 : 0.0

        Behavior on opacity {
            NumberAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }
}
