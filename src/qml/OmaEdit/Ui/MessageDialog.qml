import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// A short confirmation, with filenames treated as text rather than markup.
FormDialog {
    id: dialog
    property string message: ""
    property string detail: ""
    property string messageObjectName: "dialogMessage"
    width: Math.min(480, parent ? Math.max(0, parent.width - Theme.s5 * 2) : 480)
    closePolicy: C.Popup.CloseOnEscape
    C.Label {
        objectName: dialog.messageObjectName
        Layout.fillWidth: true
        text: dialog.message
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
    }
    C.Label {
        Layout.fillWidth: true
        visible: dialog.detail !== ""
        text: dialog.detail
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
    }
}
