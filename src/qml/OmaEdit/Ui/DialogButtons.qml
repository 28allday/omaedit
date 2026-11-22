import QtQuick
import QtQuick.Controls as C

// No Material button wells or oversized footer behind the app's controls.
C.DialogButtonBox {
    padding: Theme.s2
    verticalPadding: Theme.s2
    spacing: Theme.s2
    delegate: ToolButton {}
    background: Rectangle { color: Theme.panelRaised; radius: Theme.rMenu }
}
