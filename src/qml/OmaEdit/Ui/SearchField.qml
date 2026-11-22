import QtQuick
import QtQuick.Controls as C

// Search input with a leading glyph and a clear affordance that only appears
// once there is something to clear.
Rectangle {
    id: root

    property alias text: input.text
    property string placeholder: qsTr("Search…")
    // Search-as-you-type is the default; set false where a query is expensive
    // and should wait for Return.
    property bool live: true
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""

    // True while the field owns the keyboard — owners use it to stand the
    // editing shortcuts down so typing a name never blades a clip.
    readonly property bool active: input.activeFocus

    signal accepted(string text)
    signal cleared()

    function focusInput() { input.forceActiveFocus(); input.selectAll() }

    implicitWidth: 200
    implicitHeight: Theme.hControl
    color: Theme.controlBg
    radius: Theme.rControl
    border.width: Theme.hairline
    border.color: input.activeFocus ? Theme.accent : Theme.border

    Behavior on border.color {
        ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
    }

    Icon {
        id: glyph
        name: "search"
        size: Theme.szIcon
        color: Theme.textMuted
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
    }

    C.TextField {
        id: input
        anchors.left: glyph.right
        anchors.leftMargin: Theme.s2
        anchors.right: clear.visible ? clear.left : parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height

        // A stable inline prompt; Material's floating label crosses the
        // compact field border when focus enters an empty search.
        placeholderText: ""
        placeholderTextColor: Theme.textMuted
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsControl
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        verticalAlignment: Text.AlignVCenter
        // Material sets each side explicitly, so padding alone cannot override
        // its tall-field defaults in this compact control.
        padding: 0
        leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
        topInset: 0; bottomInset: 0
        background: null

        Text {
            objectName: "searchPlaceholder"
            anchors.fill: parent
            text: root.placeholder
            visible: input.text === "" && input.preeditText === ""
            font: input.font
            color: Theme.textMuted
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        Accessible.role: Accessible.EditableText
        Accessible.name: root.placeholder
        Accessible.description: root.tip

        onTextChanged: if (root.live) root.accepted(text)
        onAccepted: root.accepted(text)
        // Escape clears the search; in an empty field it leaves the field
        // and passes the key on, so the dialog or panel around it answers
        // the same press (a browser closes) instead of waiting for another.
        Keys.onEscapePressed: function (event) {
            if (text !== "") {
                text = ""
                root.cleared()
            } else {
                focus = false
                event.accepted = false
            }
        }
    }

    IconButton {
        id: clear
        iconName: "x"
        text: qsTr("Clear search")
        visible: input.text !== ""
        implicitWidth: Theme.szIcon + Theme.s1 * 2
        implicitHeight: Theme.szIcon + Theme.s1 * 2
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        onClicked: {
            input.text = ""
            input.forceActiveFocus()
            root.cleared()
        }
    }

    HoverHandler { id: tipHover }
    Tooltip { text: root.tip; visible: tipHover.hovered && !input.activeFocus && root.tip !== "" }
}
