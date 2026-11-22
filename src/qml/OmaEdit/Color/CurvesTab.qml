pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Curves: the custom RGB curves (Master / R / G / B, editable together or
// one at a time) and the six secondaries — Hue vs Hue, Hue vs Sat, Hue vs
// Lum, Luma vs Luma, Sat vs Sat. One editor, a selector, a reset.
// All curves render in preview, playback and export.
Item {
    id: root

    property var node: ({})
    property bool linearCurves: false
    signal curvesEdited(var curves)
    signal committed()

    readonly property var curves: node.curves !== undefined ? node.curves : ({})
    readonly property var kinds: [
        { key: "master", label: qsTr("Master"), tint: Theme.textPrimary, hue: false, rest: -1 },
        { key: "red", label: qsTr("Red"), tint: Theme.danger, hue: false, rest: -1 },
        { key: "green", label: qsTr("Green"), tint: Theme.audio, hue: false, rest: -1 },
        { key: "blue", label: qsTr("Blue"), tint: Theme.accent, hue: false, rest: -1 },
        { key: "hueHue", label: qsTr("Hue vs Hue"), tint: Theme.warning, hue: true, rest: 0.5 },
        { key: "hueSat", label: qsTr("Hue vs Sat"), tint: Theme.warning, hue: true, rest: 0.5 },
        { key: "hueLum", label: qsTr("Hue vs Lum"), tint: Theme.warning, hue: true, rest: 0.5 },
        { key: "lumaLuma", label: qsTr("Luma vs Luma"), tint: Theme.textPrimary, hue: false, rest: -1 },
        { key: "satSat", label: qsTr("Sat vs Sat"), tint: Theme.aiAccent, hue: false, rest: -1 }
    ]
    property int which: 0
    readonly property var kind: kinds[which]
    function defaultFor(k) { return k.rest >= 0 ? [[0, 0.5], [1, 0.5]] : [[0, 0], [1, 1]] }
    function put(key, pts) {
        var m = {}
        for (var k in curves) m[k] = curves[k]
        m[key] = pts
        root.curvesEdited(m)
    }

    // The nine curves list in one column, or two when the panel is too
    // short for nine rows, so every curve stays reachable without scrolling.
    readonly property int rowStep: Theme.hRow + 2
    readonly property int selectorColumns: height - 2 * Theme.s2 >= kinds.length * rowStep ? 1 : 2
    readonly property real minimumHeight: Math.ceil(kinds.length / 2) * rowStep + 2 * Theme.s2
    // The width at which the editor is as wide as it gets (three times its height).
    readonly property real wantedWidth: 2 * Theme.s2 + selector.width + Theme.s3
                                        + Math.max(240, (height - 2 * Theme.s2 - Theme.hControl - Theme.s1) * 3)
    Row {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3

        Grid {
            id: selector
            objectName: "curveSelector"
            columns: root.selectorColumns
            flow: Grid.TopToBottom
            rows: Math.ceil(root.kinds.length / columns)
            readonly property real cell: columns === 1 ? 150 : 124
            width: columns * cell + (columns - 1) * columnSpacing
            rowSpacing: 2
            columnSpacing: Theme.s1
            Repeater {
                model: root.kinds.length
                delegate: Rectangle {
                    id: row
                    required property int index
                    activeFocusOnTab: true
                    readonly property bool keyboardConsumesKeys: true
                    Accessible.role: Accessible.PageTab
                    Accessible.name: k.label
                    Accessible.selected: root.which === index
                    border.color: Theme.accent
                    border.width: activeFocus ? Theme.focusRing : 0
                    Keys.onSpacePressed: root.which = index
                    Keys.onReturnPressed: root.which = index
                    readonly property var k: root.kinds[index]
                    readonly property bool identity: {
                        var pts = root.curves[k.key]
                        if (pts === undefined || pts.length !== 2) return pts === undefined
                        var d = root.defaultFor(k)
                        return pts[0][0] === d[0][0] && pts[0][1] === d[0][1] && pts[1][0] === d[1][0] && pts[1][1] === d[1][1]
                    }
                    objectName: "curve:" + k.key
                    width: selector.cell
                    height: Theme.hRow
                    radius: Theme.rControl
                    color: root.which === index ? Theme.controlBg : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
                    Rectangle { anchors.left: parent.left; anchors.leftMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; width: 8; height: 8; radius: 4; color: row.k.tint }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s2 + 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.k.label
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        color: root.which === index ? Theme.textPrimary : Theme.textSecondary
                    }
                    Rectangle { anchors.right: parent.right; anchors.rightMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; width: 6; height: 6; radius: 3; color: Theme.accent; visible: !row.identity }
                    HoverHandler { id: hover }
                    TapHandler { onTapped: root.which = row.index }
                }
            }
        }

        Item {
            width: parent.width - selector.width - Theme.s3
            height: parent.height
            CurveEditor {
                id: editor
                objectName: "curveEditor"
                accessibleLabel: qsTr("%1 curve").arg(root.kind.label)
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.bottom: footer.top
                anchors.bottomMargin: Theme.s1
                width: Math.min(parent.width, height * 3)
                interpolation: root.linearCurves || root.which >= 4 ? "linear" : "smooth"
                points: root.curves[root.kind.key] !== undefined ? root.curves[root.kind.key] : root.defaultFor(root.kind)
                tint: root.kind.tint
                hueBackground: root.kind.hue
                restY: root.kind.rest
                onEdited: function (pts) { root.put(root.kind.key, pts) }
                onEditingFinished: function (pts) { root.put(root.kind.key, pts); root.committed() }
            }
            Row {
                id: footer
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                spacing: Theme.s2
                // One line of a fixed height: the editor above is as wide as
                // it is tall allows, so a hint that wrapped by the editor's
                // width made the two chase each other and the page collapse.
                height: Theme.hControl
                ToolButton { showLabel: true; iconName: "rotate-ccw"; text: qsTr("Reset Curve"); onClicked: { root.put(root.kind.key, root.defaultFor(root.kind)); root.committed() } }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, footer.parent.width - 120)
                    elide: Text.ElideRight
                    text: editor.activeFocus ? qsTr("Page Up/Down selects; arrows adjust; Insert adds; Delete removes.")
                                             : qsTr("Double-click adds a point, Alt-click removes one, drag to shape.")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
            }
        }
    }
}
