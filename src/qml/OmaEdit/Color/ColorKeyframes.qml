pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// Keyframes: every animatable parameter of the current node (the wheels'
// Y, contrast, saturation) with its keys, a diamond that toggles a key at
// the playhead, and a clear per row. Frames are clip-relative; the list
// shows them in programme timecode.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    readonly property string headTimecode: tc(clipFrame) // shown by the dock bar when docked
    property var node: ({})
    property int clipFrame: 0                 // playhead, clip-relative
    property int clipStart: 0
    property var timecoder: null
    signal keyToggled(string key)
    signal keysCleared(string key)
    signal frameRequested(int clipFrame)

    color: Theme.panelBg
    readonly property var keyframes: node.keyframes !== undefined ? node.keyframes : ({})
    readonly property var params: [
        { key: "lift.y", label: qsTr("Lift Y") },
        { key: "gamma.y", label: qsTr("Gamma Y") },
        { key: "gain.y", label: qsTr("Gain Y") },
        { key: "offset.y", label: qsTr("Offset Y") },
        { key: "contrast", label: qsTr("Contrast") },
        { key: "saturation", label: qsTr("Saturation") },
        { key: "lift.r", label: qsTr("Lift R") },
        { key: "lift.g", label: qsTr("Lift G") },
        { key: "lift.b", label: qsTr("Lift B") },
        { key: "gamma.r", label: qsTr("Gamma R") },
        { key: "gamma.g", label: qsTr("Gamma G") },
        { key: "gamma.b", label: qsTr("Gamma B") },
        { key: "gain.r", label: qsTr("Gain R") },
        { key: "gain.g", label: qsTr("Gain G") },
        { key: "gain.b", label: qsTr("Gain B") },
        { key: "offset.r", label: qsTr("Offset R") },
        { key: "offset.g", label: qsTr("Offset G") },
        { key: "offset.b", label: qsTr("Offset B") },
        { key: "temp", label: qsTr("Temperature") },
        { key: "tint", label: qsTr("Tint") },
        { key: "exposure", label: qsTr("Exposure") },
        { key: "pivot", label: qsTr("Pivot") },
        { key: "hue", label: qsTr("Hue") },
        { key: "highlights", label: qsTr("Highlights") },
        { key: "shadows", label: qsTr("Shadows") },
        { key: "whites", label: qsTr("Whites") },
        { key: "blacks", label: qsTr("Blacks") },
        { key: "midDetail", label: qsTr("Mid Detail") },
        { key: "window.x", label: qsTr("Window x") },
        { key: "window.y", label: qsTr("Window y") },
        { key: "window.w", label: qsTr("Window w") },
        { key: "window.h", label: qsTr("Window h") },
        { key: "window.rotation", label: qsTr("Window rotation") },
        { key: "window.softness", label: qsTr("Window softness") },
        { key: "qualifier.deltaH", label: node.qualifier && node.qualifier.subspace === "rgb" ? qsTr("Red range") : qsTr("Hue range") },
        { key: "qualifier.deltaS", label: node.qualifier && node.qualifier.subspace === "rgb" ? qsTr("Green range") : qsTr("Sat range") },
        { key: "qualifier.deltaL", label: node.qualifier && node.qualifier.subspace === "rgb" ? qsTr("Blue range") : qsTr("Lum range") },
        { key: "qualifier.softness", label: qsTr("Colour pick softness") },
        { key: "qualifier.blur", label: qsTr("Colour pick blur") },
        { key: "qualifier.grow", label: qsTr("Colour pick grow") },
        { key: "lut.intensity", label: qsTr("Lut intensity") },
        { key: "hdr.black.y", label: qsTr("Black Y") },
        { key: "hdr.black.r", label: qsTr("Black R") },
        { key: "hdr.black.g", label: qsTr("Black G") },
        { key: "hdr.black.b", label: qsTr("Black B") },
        { key: "hdr.dark.y", label: qsTr("Dark Y") },
        { key: "hdr.dark.r", label: qsTr("Dark R") },
        { key: "hdr.dark.g", label: qsTr("Dark G") },
        { key: "hdr.dark.b", label: qsTr("Dark B") },
        { key: "hdr.shadow.y", label: qsTr("Shadow Y") },
        { key: "hdr.shadow.r", label: qsTr("Shadow R") },
        { key: "hdr.shadow.g", label: qsTr("Shadow G") },
        { key: "hdr.shadow.b", label: qsTr("Shadow B") },
        { key: "hdr.light.y", label: qsTr("Light Y") },
        { key: "hdr.light.r", label: qsTr("Light R") },
        { key: "hdr.light.g", label: qsTr("Light G") },
        { key: "hdr.light.b", label: qsTr("Light B") },
        { key: "hdr.highlight.y", label: qsTr("Highlight Y") },
        { key: "hdr.highlight.r", label: qsTr("Highlight R") },
        { key: "hdr.highlight.g", label: qsTr("Highlight G") },
        { key: "hdr.highlight.b", label: qsTr("Highlight B") },
        { key: "hdr.specular.y", label: qsTr("Specular Y") },
        { key: "hdr.specular.r", label: qsTr("Specular R") },
        { key: "hdr.specular.g", label: qsTr("Specular G") },
        { key: "hdr.specular.b", label: qsTr("Specular B") }
    ]
    function keysOf(key) { return keyframes[key] !== undefined ? keyframes[key] : [] }
    function hasKeyAt(key, f) { var ks = keysOf(key); for (var i = 0; i < ks.length; ++i) if (ks[i].frame === f) return true; return false }
    function tc(f) { return timecoder ? timecoder(clipStart + f) : String(f) }

    Item {
        id: headerRow
        width: parent.width
        height: root.dockBar !== null ? 0 : Theme.hTab
        visible: root.dockBar === null
        Text { anchors.left: parent.left; anchors.leftMargin: Theme.s3; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Keyframes"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading; color: Theme.textPrimary }
        Text { anchors.right: parent.right; anchors.rightMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; text: root.tc(root.clipFrame); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }
    ListView {
        id: list
        objectName: "keyframeList"
        anchors.top: headerRow.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: root.params.length
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}
        delegate: Item {
            id: row
            required property int index
            readonly property var p: root.params[index]
            readonly property var keys: root.keysOf(p.key)
            readonly property bool atHead: root.hasKeyAt(p.key, root.clipFrame)
            objectName: "kf:" + p.key
            width: list.width
            height: Theme.hRow + 4
            Rectangle { anchors.fill: parent; color: rowHover.hovered ? Theme.hovered(Theme.panelBg) : "transparent" }
            HoverHandler { id: rowHover }
            Text { anchors.left: parent.left; anchors.leftMargin: Theme.s3; anchors.verticalCenter: parent.verticalCenter; width: 80; text: row.p.label; elide: Text.ElideRight; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; color: row.keys.length > 0 ? Theme.textPrimary : Theme.textSecondary }
            // The keys as diamonds along a mini timeline of the clip.
            Item {
                id: lane
                anchors.left: parent.left
                anchors.leftMargin: 90
                anchors.right: buttons.left
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                height: parent.height
                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: Theme.hairline; color: Theme.border }
                Repeater {
                    id: keyRepeater
                    model: row.keys.length
                    delegate: IconButton {
                        id: key
                        required property int index
                        // Within a row, arrows walk the keys (the row's first
                        // key is its tab stop's natural entry); Enter and Space
                        // do exactly what a click does.
                        function focusKey(i) {
                            var target = keyRepeater.itemAt(Math.max(0, Math.min(keyRepeater.count - 1, i)))
                            if (target && target !== key) target.forceActiveFocus(Qt.TabFocusReason)
                        }
                        Keys.onPressed: function(event) {
                            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) { event.accepted = false; return }
                            if (event.key === Qt.Key_Left) key.focusKey(key.index - 1)
                            else if (event.key === Qt.Key_Right) key.focusKey(key.index + 1)
                            else if (event.key === Qt.Key_Home) key.focusKey(0)
                            else if (event.key === Qt.Key_End) key.focusKey(keyRepeater.count - 1)
                            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) key.clicked()
                            else { event.accepted = false; return }
                            event.accepted = true
                        }
                        Accessible.description: qsTr("Keyframe %1 of %2. Enter or Space moves the playhead to it; Left and Right move between this row's keyframes; Home and End go to the first and last.")
                                                .arg(key.index + 1).arg(keyRepeater.count)
                        readonly property var k: row.keys[index]
                        readonly property int span: Math.max(1, root.node.frames !== undefined ? root.node.frames : 1)
                        objectName: "kfSeek:" + row.p.key + ":" + (k ? k.frame : -1)
                        iconName: "diamond"
                        width: 20; height: Math.min(24, lane.height)
                        visible: k !== undefined
                        text: k ? qsTr("%1 keyframe at %2").arg(row.p.label).arg(root.tc(k.frame)) : ""
                        iconColor: k && k.frame === root.clipFrame ? Theme.accent : Theme.textSecondary
                        contentItem: Item { Icon { anchors.centerIn: parent; name: "diamond"; size: 10; color: key.iconColor } }
                        x: Math.max(0, Math.min(lane.width - width, lane.width * (k ? k.frame : 0) / span))
                        anchors.verticalCenter: lane.verticalCenter
                        onClicked: if (key.k) root.frameRequested(key.k.frame)
                    }
                }
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                Text { anchors.verticalCenter: parent.verticalCenter; width: 22; horizontalAlignment: Text.AlignRight; text: row.keys.length > 0 ? row.keys.length : ""; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; rightPadding: Theme.s1 }
                IconButton { objectName: "kfToggle:" + row.p.key; iconName: "diamond"; iconColor: row.atHead ? Theme.accent : Theme.textMuted; Accessible.name: row.atHead ? qsTr("Remove %1 keyframe").arg(row.p.label) : qsTr("Add %1 keyframe").arg(row.p.label); onClicked: root.keyToggled(row.p.key) }
                IconButton { iconName: "x"; visible: row.keys.length > 0; Accessible.name: qsTr("Clear %1 keyframes").arg(row.p.label); onClicked: root.keysCleared(row.p.key) }
            }
        }
    }
}
