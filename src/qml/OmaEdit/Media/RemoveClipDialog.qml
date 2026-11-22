import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui as Ui

// "Remove N clips from the project?" — the one destructive act on the Media
// page, so it asks. The files on disk are never touched.
Ui.MessageDialog {
    id: root
    objectName: "removeClipDialog"
    standardButtons: C.Dialog.NoButton
    title: qsTr("Remove from project")

    property int count: 0
    property bool anyUsed: false
    property var names: []
    signal confirmed()

    function openFor(count, anyUsed, names) {
        root.count = count
        root.anyUsed = anyUsed
        root.names = names || []
        open()
    }
    // "a, b and c" / "a, b, c and 4 more" — the exact targets, never "these".
    function named() {
        var n = root.names.map(function (s) { return "“" + s + "”" })
        if (n.length <= 3) return n.length <= 1 ? (n[0] || "") : n.slice(0, -1).join(", ") + qsTr(" and ") + n[n.length - 1]
        return n.slice(0, 3).join(", ") + qsTr(" and %1 more").arg(n.length - 3)
    }

    messageObjectName: "removeClipQuestion"
    message: root.count === 1 ? qsTr("Remove %1 from the project?").arg(root.named() || qsTr("this clip"))
                             : qsTr("Remove %1 clips from the project?").arg(root.count) + (root.names.length ? "\n" + root.named() : "")
    detail: root.anyUsed ? qsTr("Timeline clips keep playing — only the library entry goes.")
                        : qsTr("The files stay on disk.")

    footer: Ui.DialogButtons {
        Ui.ToolButton {
            objectName: "removeClipConfirm"
            text: qsTr("Remove")
            tip: qsTr("Take these clips out of the project; the files stay on disk")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.DestructiveRole
        }
        Ui.ToolButton {
            objectName: "removeClipCancel"
            text: qsTr("Cancel")
            tip: qsTr("Close without removing anything from the project")
            C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole
        }
        onClicked: function (button) {
            if (button.C.DialogButtonBox.buttonRole === C.DialogButtonBox.DestructiveRole) {
                root.close()
                root.confirmed()
            } else {
                root.close()
            }
        }
    }
}
