import QtQuick

// 0–5 star rating. Click a star to set it, click the lit star again to clear,
// arrow keys when focused. The glyphs are text, not icons: a filled star has
// no stroke to bake, and the font renders it crisp at any size.
Item {
    id: root

    property int value: 0
    property bool editable: true
    readonly property bool keyboardConsumesKeys: editable
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    property int starSize: Theme.fsBase + 2
    // Read-only ratings are dimmer, so they read as a fact, not a control.
    property color litColor: editable ? Theme.warning : Theme.textSecondary
    property color dimColor: Theme.textMuted

    signal edited(int value)

    implicitWidth: row.implicitWidth
    implicitHeight: Math.max(starSize + Theme.s1, Theme.hRow)
    activeFocusOnTab: editable
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Rating")
    Accessible.description: root.tip !== "" ? root.tip + ". " + qsTr("%1 of 5 stars").arg(value) : qsTr("%1 of 5 stars").arg(value)

    Keys.onLeftPressed: if (editable) root.edited(Math.max(0, value - 1))
    Keys.onRightPressed: if (editable) root.edited(Math.min(5, value + 1))

    Rectangle {
        anchors.fill: parent
        radius: Theme.rControl
        color: "transparent"
        border.width: root.activeFocus ? Theme.focusRing : 0
        border.color: Theme.accent
    }

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1

        Repeater {
            model: 5
            delegate: Text {
                required property int index
                readonly property bool lit: index < (hover.hovered && root.editable
                                                     ? hover.point.position.x >= 0 ? root.hoverValue : root.value
                                                     : root.value)
                text: lit ? "★" : "☆"
                font.pixelSize: root.starSize
                color: lit ? root.litColor : root.dimColor
                width: root.starSize + 1
                horizontalAlignment: Text.AlignHCenter

                TapHandler {
                    enabled: root.editable
                    onTapped: root.edited(root.value === index + 1 ? 0 : index + 1)
                }
            }
        }
    }

    // Hovering previews the rating a click would set.
    readonly property int hoverValue: hover.hovered
        ? Math.max(0, Math.min(5, Math.ceil((hover.point.position.x - row.x) / (root.starSize + 1))))
        : value
    HoverHandler { id: hover; enabled: root.editable }

    HoverHandler { id: tipHover }
    Tooltip { text: root.tip; visible: tipHover.hovered && root.tip !== "" }
}
