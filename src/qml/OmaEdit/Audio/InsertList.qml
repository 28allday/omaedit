pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// A channel strip's insert slots: one row per fx entry ("EQ ›"), struck
// through when bypassed; click opens it in the Effects tab, the dot
// toggles bypass, "+" adds. Sends use the same rows with a level knob.
Column {
    id: root

    property var fx: []               // [{id, bypass, mix, params}]
    property var catalogue: []        // mixEffects
    property bool compact: false
    property int maxRows: 4
    signal opened(int index)
    signal bypassToggled(int index, bool bypass)
    signal addRequested()

    spacing: 2

    function nameFor(id) {
        for (var i = 0; i < catalogue.length; ++i) if (catalogue[i].id === id) return shortName(catalogue[i].name)
        return id
    }
    function shortName(name) {
        if (name === "Equaliser") return "EQ"
        if (name === "Compressor") return "Comp"
        if (name === "Gate / Expander") return "Gate"
        if (name === "Noise Reduction") return "NR"
        if (name === "High-pass") return "HPF"
        if (name === "Low-pass") return "LPF"
        return name
    }

    Repeater {
        model: Math.min(root.fx.length, root.maxRows)
        delegate: Rectangle {
            id: row
            required property int index
            readonly property var entry: root.fx[index] !== undefined ? root.fx[index] : ({})
            readonly property bool bypassed: entry.bypass === true
            width: root.width
            height: Theme.hRow - 4
            radius: Theme.rControl
            color: hover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
            border.color: entry.ai === true ? Theme.aiAccent : Theme.border
            border.width: Theme.hairline
            Rectangle {
                id: bypassDot
                anchors.left: parent.left
                anchors.leftMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: row.bypassed ? Theme.textMuted : Theme.audio
                TapHandler { onTapped: root.bypassToggled(row.index, !row.bypassed) }
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s1 + 10
                anchors.right: chevron.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.nameFor(row.entry.id)
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.strikeout: row.bypassed
                color: row.bypassed ? Theme.textMuted : Theme.textPrimary
            }
            Icon {
                id: chevron
                anchors.right: parent.right
                anchors.rightMargin: 2
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-right"
                size: 12
                visible: !root.compact
            }
            HoverHandler { id: hover }
            // Both handlers see a tap on the dot; that tap only bypasses.
            TapHandler {
                onTapped: function (point) {
                    const p = bypassDot.mapFromItem(parent, point.position)
                    if (!bypassDot.contains(p)) root.opened(row.index)
                }
            }
        }
    }
    Rectangle {
        width: root.width
        height: Theme.hRow - 6
        radius: Theme.rControl
        color: addHover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
        border.color: Theme.border
        border.width: Theme.hairline
        visible: root.fx.length < root.maxRows || root.fx.length === 0
        Icon { anchors.centerIn: parent; name: "plus"; size: 12; color: Theme.textMuted }
        HoverHandler { id: addHover }
        TapHandler { onTapped: root.addRequested() }
    }
    Text {
        visible: root.fx.length > root.maxRows
        width: root.width
        text: qsTr("+%1 more").arg(root.fx.length - root.maxRows)
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel - 1
        color: Theme.textMuted
        horizontalAlignment: Text.AlignHCenter
        TapHandler { onTapped: root.opened(root.maxRows) }
    }
}
