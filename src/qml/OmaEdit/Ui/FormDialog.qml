import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// Size to the form, up to the window bounds. Long forms scroll while the
// title and actions stay in view. Children use Layout.fillWidth to reflow.
ModalDialog {
    id: dialog
    default property alias body: column.data
    // How far the form may be squeezed before it scrolls: a list in it that
    // scrolls by itself (Layout.fillHeight, its whole height preferred) gives
    // the difference between that and the least it can show.
    property real bodySlack: 0
    readonly property alias bodyScroll: scroll
    height: Math.min(implicitHeight, parent ? Math.max(0, parent.height - Theme.s5 * 2) : implicitHeight)
    contentItem: C.ScrollView {
        id: scroll
        implicitHeight: column.implicitHeight
        contentWidth: availableWidth
        contentHeight: column.height
        clip: true
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        // In the box's margin, clear of the form, and there whenever the
        // form is longer than the box.
        C.ScrollBar.vertical: ScrollBar {
            parent: scroll.parent
            x: scroll.x + scroll.width + Math.max(0, (dialog.rightPadding - width) / 2)
            y: scroll.y
            height: scroll.height
            policy: C.ScrollBar.AlwaysOn
        }
        ColumnLayout {
            id: column
            width: scroll.availableWidth
            height: Math.max(implicitHeight - dialog.bodySlack, Math.min(implicitHeight, scroll.availableHeight))
            spacing: Theme.s2
        }
    }
}
