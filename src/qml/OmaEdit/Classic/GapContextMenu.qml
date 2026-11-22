import QtQuick
import QtQuick.Controls

// Right-click on empty track space: the gap is already selected (and
// outlined), so the menu only has to close it.
AppMenu {
    id: root
    KeyMenuItem {
        objectName: "gapDelete"
        text: qsTr("Ripple Delete Gap")
        kbd: "Del"
        enabled: win.selectedGap !== null
        onTriggered: win.selection.closeSelectedGap()
    }
    KeyMenuItem {
        text: qsTr("Paste at Playhead"); kbd: "Ctrl+V"
        enabled: win.hasClipboard
        onTriggered: win.copying.pasteClipboard()
    }
}
