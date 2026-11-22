import QtQuick
import QtQuick.Controls as C

C.Dialog {
    id: dialog
    onVisibleChanged: Commands.popupVisibility(dialog, visible)
    Component.onDestruction: Commands.popupVisibility(dialog, false)
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.wMediaDialog, parent ? parent.width - Theme.s5 * 2 : Theme.wMediaDialog)
    modal: true
    // Confirmation blocks background input without changing the picture.
    dim: false
    focus: true
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBase
    padding: Theme.s3
    topPadding: Theme.s3
    topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
    standardButtons: C.Dialog.Ok | C.Dialog.Cancel
    background: Rectangle { color: Theme.panelRaised; radius: Theme.rMenu; border.width: Theme.hairline; border.color: Theme.borderStrong }
    header: Text {
        text: dialog.title
        visible: text !== ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        padding: Theme.s3; bottomPadding: Theme.s2
        color: Theme.textPrimary
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading
    }
    footer: DialogButtons {}
    // OK takes no focus from a field still being typed in, so a dialog that
    // reads its form on OK lands what is typed there first, as Enter would.
    function landTyped() {
        for (let item = contentItem ? contentItem.Window.activeFocusItem : null; item; item = item.parent)
            if (typeof item.commitTyped === "function") { item.commitTyped(); return }
    }
}
