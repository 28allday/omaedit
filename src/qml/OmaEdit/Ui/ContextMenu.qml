import QtQuick
import QtQuick.Window
import QtQuick.Controls as C

// Styled menu, used both for right-click context menus and the title bar's
// application menus. Items come from MenuAction, which lays the shortcut out
// in a right-aligned column so a column of hints stays scannable.
C.Menu {
    id: root
    // A submenu's popup visibility is separate from the row in its parent.
    property bool showInParent: true
    onVisibleChanged: Commands.popupVisibility(root, visible)
    Component.onDestruction: Commands.popupVisibility(root, false)
    onAboutToShow: {
        // Clicking a hovered submenu can open it before its parent's delayed
        // hover action fires. Reselect the same row to cancel that pending
        // action, or it can reopen this menu immediately after Escape.
        if (parent && parent.subMenu === root && parent.menu) {
            const owner = parent.menu
            const index = owner.currentIndex
            if (index >= 0) {
                owner.currentIndex = -1
                owner.currentIndex = index
            }
        }
    }

    readonly property real preferredWidth: {
        var widest = 220
        for (var i = 0; i < count; ++i) {
            var row = itemAt(i)
            if (row) widest = Math.max(widest, row.implicitWidth + leftPadding + rightPadding)
        }
        return widest
    }
    readonly property var hostWindow: parent ? parent.Window.window : null
    implicitWidth: Math.min(preferredWidth, hostWindow ? hostWindow.width - Theme.s2 * 2 : preferredWidth)
    // Keep long menus scrollable within the window, including at larger densities.
    implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                             hostWindow ? hostWindow.height - Theme.s2 * 2 : 10000)
    padding: Theme.s1
    // Menus are one of the few places the brief allows motion, so a menu that
    // opens instantly reads as a glitch rather than as speed.
    topPadding: Theme.s1
    bottomPadding: Theme.s1

    delegate: MenuAction {
        showInMenu: !subMenu || subMenu.showInParent !== false
    }
    contentItem: ListView {
        implicitHeight: contentHeight
        model: root.contentModel
        currentIndex: root.currentIndex
        // Menu items are an object model; explicitly reveal keyboard
        // selection even though this list has no highlight delegate.
        onCurrentIndexChanged: Qt.callLater(function() {
            if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
        })
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        // Only a menu taller than the window has one: it sits over the rows.
        C.ScrollBar.vertical: ScrollBar {
            visible: parent.contentHeight > parent.height
            policy: C.ScrollBar.AlwaysOn
        }
    }

    background: Rectangle {
        implicitWidth: 220
        color: Theme.panelRaised
        radius: Theme.rMenu
        border.width: Theme.hairline
        border.color: Theme.border
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: Theme.dFast; easing.type: Theme.easing
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: Theme.dFast; easing.type: Theme.easing
        }
    }
}
