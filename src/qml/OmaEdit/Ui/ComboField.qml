pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C

// Dropdown. Styled rather than themed, because the Material combo brings its
// own ripple, elevation and radius that all fight the brief.
C.ComboBox {
    id: root
    readonly property bool commandPopupVisible: popup && popup.visible
    onCommandPopupVisibleChanged: Commands.popupVisibility(root, commandPopupVisible)
    Component.onDestruction: Commands.popupVisibility(root, false)

    // Optional leading caption rendered inside the control, for the dense
    // Render Settings rows where a separate label column would waste width.
    property string caption: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    // Off where the page already says the same beside the control: the tip
    // is then only read out, and no bubble covers the row above.
    property bool showsTip: true
    // As wide as its longest choice (a list of words), so none is cut short.
    property bool fitsChoices: false
    FontMetrics { id: choiceMetrics; font: root.font }
    readonly property real choicesWidth: {
        if (!fitsChoices || !model || model.length === undefined) return 0
        var widest = 0
        for (var i = 0; i < model.length; ++i)
            widest = Math.max(widest, choiceMetrics.advanceWidth(String(model[i])))
        return Math.ceil(widest)
    }
    // A click never takes the keyboard from the editing shortcuts; Tab still reaches it.
    focusPolicy: Qt.TabFocus
    // And so Esc, which reaches a list only through the focus, needs saying.
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: root.commandPopupVisible
        onActivated: root.popup.close()
    }

    implicitWidth: choicesWidth > 0 ? choicesWidth + indicator.width + Theme.s2 * 3 + 2 : 140
    implicitHeight: Theme.hControl
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.ComboBox
    Accessible.name: caption
    Accessible.description: tip

    background: Rectangle {
        color: root.pressed ? Theme.pressedOn(Theme.controlBg)
             : root.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
        radius: Theme.rControl
        border.width: root.visualFocus ? Theme.focusRing : Theme.hairline
        border.color: root.visualFocus ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    contentItem: Row {
        id: content
        leftPadding: Theme.s2
        spacing: Theme.s2

        // The value comes first: it is shown whole while the control has
        // the room, and the caption takes what is left (none, in a narrow one).
        // Neither is ever drawn past the arrow or the control's edge.
        readonly property real room: Math.max(0, root.width - root.indicator.width - Theme.s2 * 3)
        readonly property real captionRoom: room - Theme.s2 - Math.min(value.implicitWidth, room)

        Text {
            id: captionText
            text: root.caption
            visible: root.caption !== "" && content.captionRoom >= Math.min(implicitWidth, Theme.s5)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, content.captionRoom))
        }

        Text {
            id: value
            text: root.displayText
            font: root.font
            color: Theme.textPrimary
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            width: content.room - (captionText.visible ? captionText.width + Theme.s2 : 0)
        }
    }

    indicator: Icon {
        name: "chevron-down"
        size: Theme.szIcon
        color: Theme.textMuted
        x: root.width - width - Theme.s2
        y: root.topPadding + (root.availableHeight - height) / 2
    }

    delegate: C.ItemDelegate {
        id: option
        required property int index
        required property var modelData

        width: root.width
        height: Theme.hRow + Theme.s1 * 2
        highlighted: root.highlightedIndex === index

        contentItem: Text {
            text: option.modelData
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: option.highlighted ? Theme.textPrimary : Theme.textSecondary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: option.highlighted ? Theme.controlBg : "transparent"
        }
    }

    popup: C.Popup {
        y: root.height
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight + Theme.s1 * 2, 320)
        padding: Theme.s1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollIndicator.vertical: C.ScrollIndicator {}
        }

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.rMenu
            border.width: Theme.hairline
            border.color: Theme.border
        }

        enter: Transition {
            NumberAnimation {
                property: "opacity"; from: 0; to: 1
                duration: Theme.dFast; easing.type: Theme.easing
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "opacity"; from: 1; to: 0
                duration: Theme.dFast; easing.type: Theme.easing
            }
        }
    }

    Tooltip { text: root.tip; visible: root.showsTip && root.hovered && !root.pressed && !root.popup.visible && root.tip !== "" }
}
