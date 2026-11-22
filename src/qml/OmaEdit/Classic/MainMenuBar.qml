import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls as C

// The application menus across the title bar. Moving the pointer along the
// bar, or the left and right keys, goes from one open menu to the next.
C.MenuBar {
    id: root
    objectName: "mainMenuBar"
    padding: 0
    spacing: 0
    background: Item {}

    delegate: C.MenuBarItem {
        id: item
        objectName: "menuBarItem:" + text
        implicitHeight: Ui.Theme.hControl
        leftPadding: Ui.Theme.s2
        rightPadding: Ui.Theme.s2
        topPadding: 0
        bottomPadding: 0
        Accessible.name: text
        contentItem: Text {
            text: item.text
            verticalAlignment: Text.AlignVCenter
            font.family: Ui.Theme.fontFamily
            font.pixelSize: Ui.Theme.fsControl
            color: item.highlighted ? Ui.Theme.textPrimary : Ui.Theme.textSecondary
        }
        background: Rectangle {
            radius: Ui.Theme.rControl
            color: item.highlighted ? Ui.Theme.controlBg : "transparent"
        }
    }

    FileMenu {}
    EditMenu {}
    TrimMenu {}
    MarkMenu {}
    ClipMenu {}
    ModifyMenu {}
    ViewMenu {}
    WindowMenu {}
    HelpMenu {}
}
