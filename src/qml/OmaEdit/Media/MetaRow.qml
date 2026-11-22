import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// One label/value line of the metadata inspector. A plain fact is text; an
// editable field becomes a text entry on click; `custom` rows host their own
// control (stars, chips) in the value column.
Item {
    id: root

    property string label: ""
    property string value: ""
    property string placeholder: "—"
    property bool editable: false
    property bool multiline: false
    property bool custom: false
    property int labelWidth: 96
    default property alias content: slot.data

    signal edited(string text)

    readonly property bool textActive: input.activeFocus

    width: parent ? parent.width : 240
    implicitHeight: Math.max(Theme.hRow, custom ? slot.childrenRect.height + Theme.s1
                                                  : multiline && editable ? input.implicitHeight + Theme.s1
                                                  : valueText.implicitHeight + Theme.s1)

    Text {
        id: caption
        width: root.labelWidth
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: (Theme.hRow - implicitHeight) / 2
        text: root.label
        textFormat: Text.PlainText // labels and values can come from a file
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }

    Item {
        id: slot
        visible: root.custom
        anchors.left: caption.right
        anchors.right: parent.right
        anchors.top: parent.top
    }

    Text {
        id: valueText
        visible: !root.custom && !(root.editable && input.activeFocus)
        anchors.left: caption.right
        anchors.right: editIcon.visible ? editIcon.left : parent.right
        anchors.top: parent.top
        anchors.topMargin: (Theme.hRow - Theme.fsLabel - 2) / 2
        text: root.value !== "" ? root.value : root.placeholder
        textFormat: Text.PlainText
        wrapMode: root.multiline ? Text.Wrap : Text.NoWrap
        elide: root.multiline ? Text.ElideNone : Text.ElideMiddle
        maximumLineCount: root.multiline ? 4 : 1
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: root.value !== "" ? Theme.textPrimary : Theme.textMuted

        TapHandler {
            enabled: root.editable
            onTapped: { input.forceActiveFocus(); input.selectAll() }
        }
    }

    Icon {
        id: editIcon
        visible: root.editable && !input.activeFocus
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: (Theme.hRow - size) / 2
        name: "pencil"
        size: Theme.szIcon - 4
        color: Theme.textMuted
        TapHandler { onTapped: { input.forceActiveFocus(); input.selectAll() } }
    }

    C.TextArea {
        id: input
        visible: root.editable && activeFocus
        anchors.left: caption.right
        anchors.right: parent.right
        anchors.top: parent.top
        text: root.value
        placeholderText: root.placeholder === "—" ? "" : root.placeholder
        placeholderTextColor: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        wrapMode: root.multiline ? TextEdit.Wrap : TextEdit.NoWrap
        leftPadding: Theme.s1; rightPadding: Theme.s1; topPadding: 3; bottomPadding: 3
        background: Rectangle {
            color: Theme.controlBg
            radius: Theme.rControl
            border.width: Theme.focusRing
            border.color: Theme.accent
        }
        Accessible.name: root.label

        Keys.onReturnPressed: function (event) {
            if (root.multiline && (event.modifiers & Qt.ShiftModifier)) { event.accepted = false; return }
            commit()
            event.accepted = true
        }
        Keys.onEscapePressed: { text = Qt.binding(() => root.value); focus = false }
        onActiveFocusChanged: if (!activeFocus) commit()
        function commit() {
            if (text !== root.value) root.edited(text)
            focus = false
        }
    }
}
