import QtQuick
import QtQuick.Layouts
import OmaEdit.Ui as Ui

RowLayout {
    id: root
    objectName: "appInfoArea"
    property var tasks
    property string message: ""
    readonly property var notice: tasks ? tasks.currentNotice : ({})
    readonly property bool hasNotice: (notice.id || "") !== ""
    readonly property bool controlsFocused: messages.activeFocus || dismiss.activeFocus
    readonly property string fullText: hasNotice
        ? [notice.title || "", notice.message || ""].filter(function(s) { return s !== "" }).join(" · ")
        : message
    signal messagesRequested()
    spacing: Ui.Theme.s1

    Ui.ToolButton {
        id: messages
        objectName: "infoMessages"
        text: qsTr("Info") + (root.tasks && root.tasks.notices.length ? " · " + root.tasks.notices.length : "")
        tip: qsTr("Read messages and task results from this session")
        Accessible.name: qsTr("Open messages")
        onClicked: root.messagesRequested()
        Layout.preferredHeight: root.height
    }
    Text {
        objectName: "infoMessage"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        text: root.fullText.replace(/\s+/g, " ")
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: Ui.Theme.fontFamily
        font.pixelSize: Ui.Theme.fsLabel
        color: root.notice.severity === "error" ? Ui.Theme.danger
               : root.notice.severity === "warning" ? Ui.Theme.warning : Ui.Theme.textPrimary
        Accessible.role: Accessible.StaticText
        Accessible.name: root.fullText
    }
    Ui.IconButton {
        id: dismiss
        objectName: "dismissInfoMessage"
        iconName: "x"
        text: qsTr("Dismiss message")
        tip: qsTr("Clear the info area; the message stays in the session history")
        visible: root.hasNotice
        implicitWidth: Ui.Theme.szIconHit
        implicitHeight: root.height
        onClicked: root.tasks.dismissNotice()
    }
}
