import QtQuick
import OmaEdit.Input as Input

Item {
    id: root
    property alias window: navigation.window // qmllint disable unresolved-alias
    readonly property alias textFocused: navigation.textFocused // qmllint disable unresolved-alias
    function cyclePanel(backwards) { return navigation.cyclePanel(backwards) }

    Input.KeyboardNavigation { id: navigation }
    Rectangle {
        objectName: "keyboardFocusOutline"
        // ApplicationWindow has an additional content item below its header.
        // Use the same window surface as the C++ coordinate mapping.
        parent: navigation.overlayParent
        x: navigation.focusRect.x + Theme.focusRing / 2
        y: navigation.focusRect.y + Theme.focusRing / 2
        width: Math.max(0, navigation.focusRect.width - Theme.focusRing)
        height: Math.max(0, navigation.focusRect.height - Theme.focusRing)
        z: 10000000
        visible: width > 0 && height > 0
        color: "transparent"
        radius: Theme.rControl
        border.width: Theme.focusRing
        border.color: Theme.accent
    }
}
