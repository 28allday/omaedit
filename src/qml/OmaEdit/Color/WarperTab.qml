pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Hue mesh (P1): a hue / saturation mesh — a hue ring with rings of
// control points; drag a point to push that hue and saturation. The mesh
// lives in the node and renders with circular hue interpolation.
Item {
    id: root

    property var node: ({})
    signal warperEdited(var mesh)
    signal committed()

    readonly property var mesh: node.warper !== undefined ? node.warper : []
    readonly property int hues: 12
    readonly property int rings: 3
    property int selectedPoint: 0
    // The last keyboard limit reached, spoken and shown until the next key.
    property string announcement: ""
    function baseMesh() {
        var m = []
        for (var r = 1; r <= rings; ++r)
            for (var h = 0; h < hues; ++h)
                m.push({ hue: h / hues, sat: r / rings, dh: 0, ds: 0 })
        return m
    }
    readonly property var points: mesh.length === hues * rings ? mesh : baseMesh()
    readonly property real hueLimit: 0.08
    readonly property real satLimit: 0.33
    function move(i, dh, ds) {
        var m = points.map(function (p) { return { hue: p.hue, sat: p.sat, dh: p.dh, ds: p.ds } })
        m[i].dh = Math.max(-hueLimit, Math.min(hueLimit, dh))
        m[i].ds = Math.max(-satLimit, Math.min(satLimit, ds))
        root.warperEdited(m)
    }
    // Keyboard step: one edit and one commit, like a finished drag. A step
    // that cannot move the point any further says so instead.
    function nudge(i, dh, ds) {
        var p = points[i]
        var nh = Math.max(-hueLimit, Math.min(hueLimit, p.dh + dh))
        var ns = Math.max(-satLimit, Math.min(satLimit, p.ds + ds))
        if (nh === p.dh && ns === p.ds) {
            say(dh !== 0 ? qsTr("Hue shift is at its limit.") : qsTr("Saturation shift is at its limit."))
            return
        }
        announcement = ""
        move(i, nh, ns)
        committed()
    }
    function say(text) {
        announcement = text
        if (typeof backend !== "undefined" && backend && backend.announce) backend.announce(text)
    }
    function pointText(i) {
        var p = points[i]
        if (!p) return ""
        return qsTr("Point %1 of %2: hue %3°, saturation ring %4; hue shift %5°, saturation shift %6%.")
            .arg(i + 1).arg(points.length).arg(Math.round(p.hue * 360)).arg(Math.round(p.sat * rings))
            .arg((p.dh * 360).toFixed(1)).arg(Math.round(p.ds * 100))
    }

    Row {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3
        Item {
            id: wheel
            objectName: "warperWheel"
            activeFocusOnTab: true
            readonly property bool keyboardConsumesKeys: true
            Accessible.role: Accessible.Pane
            Accessible.name: qsTr("Hue Mesh")
            Accessible.description: qsTr("Page Up and Page Down select points; Home and End select the first and last. Left and Right change hue; Up and Down change saturation. Shift makes fine adjustments. Delete resets the selected point. Tab leaves the mesh.")
                                    + " " + root.pointText(root.selectedPoint)
                                    + (root.announcement !== "" ? " " + root.announcement : "")
            Keys.onPressed: function(event) {
                var i = root.selectedPoint, p = root.points[i], fine = !!(event.modifiers & Qt.ShiftModifier)
                // Tab and Backtab are never taken: the mesh is one tab stop.
                if (event.key === Qt.Key_PageUp) { root.selectedPoint = Math.max(0, i - 1); root.announcement = "" }
                else if (event.key === Qt.Key_PageDown) { root.selectedPoint = Math.min(root.points.length - 1, i + 1); root.announcement = "" }
                else if (event.key === Qt.Key_Home) { root.selectedPoint = 0; root.announcement = "" }
                else if (event.key === Qt.Key_End) { root.selectedPoint = root.points.length - 1; root.announcement = "" }
                else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right)
                    root.nudge(i, (event.key === Qt.Key_Right ? 1 : -1) * (fine ? .00025 : .0025), 0)
                else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                    root.nudge(i, 0, (event.key === Qt.Key_Up ? 1 : -1) * (fine ? .001 : .01))
                else if (event.key === Qt.Key_Delete) {
                    if (p.dh === 0 && p.ds === 0) root.say(qsTr("This point is already at its original position."))
                    else { root.announcement = ""; root.move(i, 0, 0); root.committed() }
                }
                else { event.accepted = false; return }
                event.accepted = true
            }
            Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.accent; border.width: wheel.activeFocus ? Theme.focusRing : 0; radius: Theme.rControl }
            width: parent.height
            height: parent.height
            Canvas {
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var cx = width / 2, cy = height / 2, r = width / 2 - 2
                    for (var a = 0; a < 360; a += 3) {
                        ctx.beginPath(); ctx.moveTo(cx, cy)
                        ctx.arc(cx, cy, r, (a - 1) * Math.PI / 180, (a + 3) * Math.PI / 180)
                        ctx.closePath()
                        ctx.fillStyle = Qt.hsla(a / 360, 0.8, 0.5, 0.35)
                        ctx.fill()
                    }
                    ctx.strokeStyle = String(Theme.border)
                    for (var k = 1; k <= root.rings; ++k) { ctx.beginPath(); ctx.arc(cx, cy, r * k / root.rings, 0, Math.PI * 2); ctx.stroke() }
                }
            }
            Repeater {
                model: root.points.length
                delegate: Rectangle {
                    id: pt
                    required property int index
                    readonly property var p: root.points[index]
                    readonly property real ang: (p.hue + p.dh) * Math.PI * 2
                    readonly property real rad: Math.max(0.05, Math.min(1, p.sat + p.ds)) * (wheel.width / 2 - 2)
                    x: wheel.width / 2 + Math.cos(ang) * rad - width / 2
                    y: wheel.height / 2 + Math.sin(ang) * rad - height / 2
                    width: 8; height: 8; radius: 4
                    color: p.dh !== 0 || p.ds !== 0 ? Theme.accent : Theme.textPrimary
                    border.color: Theme.windowBg
                    border.width: 1
                    Rectangle { anchors.centerIn: parent; width: 16; height: 16; radius: 8; color: "transparent"; border.color: Theme.textPrimary; border.width: 2; visible: wheel.activeFocus && root.selectedPoint === pt.index }
                    TapHandler { onTapped: { root.selectedPoint = pt.index; wheel.forceActiveFocus(Qt.MouseFocusReason) } }
                    DragHandler {
                        target: null
                        onCentroidChanged: {
                            if (!active) return
                            var dx = centroid.position.x + pt.x - wheel.width / 2, dy = centroid.position.y + pt.y - wheel.height / 2
                            var hue = Math.atan2(dy, dx) / (Math.PI * 2); if (hue < 0) hue += 1
                            var sat = Math.sqrt(dx * dx + dy * dy) / (wheel.width / 2 - 2)
                            var dh = hue - pt.p.hue; if (dh > 0.5) dh -= 1; if (dh < -0.5) dh += 1
                            root.move(pt.index, dh, sat - pt.p.sat)
                        }
                        onActiveChanged: if (active) root.selectedPoint = pt.index; else root.committed()
                    }
                }
            }
        }
        Column {
            spacing: Theme.s2
            width: 300
            Rectangle {
                width: parent.width
                height: Theme.hControl
                radius: Theme.rControl
                color: Theme.controlBg
                Row {
                    anchors.left: parent.left; anchors.leftMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; spacing: Theme.s2
                    Icon { name: "circle-check"; size: 13; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Hue and saturation mesh"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: wheel.activeFocus ? qsTr("Page Up/Down selects; arrows adjust hue and saturation; Shift adjusts finely; Delete resets the point.") : qsTr("Drag a point to push that hue and saturation. Twelve hues, three saturation rings."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            Text { objectName: "warperPointReadout"; width: parent.width; wrapMode: Text.WordWrap; visible: wheel.activeFocus; text: root.pointText(root.selectedPoint); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
            Text { objectName: "warperAnnouncement"; width: parent.width; wrapMode: Text.WordWrap; visible: root.announcement !== ""; text: root.announcement; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary }
            ToolButton { showLabel: true; iconName: "rotate-ccw"; text: qsTr("Reset Mesh"); onClicked: { root.warperEdited(root.baseMesh()); root.committed() } }
        }
    }
}
