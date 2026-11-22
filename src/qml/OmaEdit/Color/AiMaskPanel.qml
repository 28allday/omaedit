pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// The people in the picture as this node's mask, found on this machine, frame
// by frame. Edits the node's `aiMask`; a shape on the same node narrows it to
// one person, and the tracker keeps that shape on them.
Item {
    id: root

    property var node: ({})
    property bool highlight: false
    signal aiMaskEdited(var mask)
    signal highlightToggled(bool on)
    signal committed()

    readonly property var m: node.aiMask !== undefined ? node.aiMask : ({})
    readonly property bool on: m.enabled === true
    readonly property var qualities: ["fast", "balanced", "best"]
    function put(key, value) {
        var next = {}
        for (var k in m) next[k] = m[k]
        next[key] = value
        root.aiMaskEdited(next)
    }

    Column {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Math.max(Theme.s1, Math.min(Theme.s3, (root.height - 2 * Theme.s2 - 6 * Theme.hControl) / 5))
        Toggle { objectName: "aiMaskOn"; label: qsTr("AI Mask"); tip: qsTr("Limit this layer's correction to the people in the picture, found on every frame. To pick one person, add a shape around them and track it."); checked: root.on; onToggled: { root.put("enabled", checked); root.committed() } }
        Row {
            spacing: Theme.s1
            Text { width: 54; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Quality"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
            ComboField {
                objectName: "aiMaskQuality"
                width: root.width - 2 * Theme.s2 - 54 - Theme.s1
                enabled: root.on
                tip: qsTr("Best follows hair and edges most closely; Fast leaves more room for other work during playback")
                model: [qsTr("Fast"), qsTr("Balanced"), qsTr("Best")]
                currentIndex: Math.max(0, root.qualities.indexOf(root.m.quality !== undefined ? root.m.quality : "best"))
                onActivated: function (i) { root.put("quality", root.qualities[i]); root.committed() }
            }
        }
        SliderField { liveEdit: true; objectName: "aiMaskTighten"; width: parent.width; labelWidth: 54; label: qsTr("Tighten"); tip: qsTr("Narrows the soft edge of the mask towards its middle"); from: 0; to: 1; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.m.tighten !== undefined ? root.m.tighten : 0; onEdited: function (v) { root.put("tighten", v) }; onEditingFinished: root.committed() }
        SliderField { liveEdit: true; objectName: "aiMaskGrow"; width: parent.width; labelWidth: 54; label: qsTr("Grow"); tip: qsTr("Grows or shrinks the mask, in pixels of a 1920-wide picture"); from: -24; to: 24; stepSize: 1; decimals: 0; origin: 0; enabled: root.on; value: root.m.grow !== undefined ? root.m.grow : 0; onEdited: function (v) { root.put("grow", v) }; onEditingFinished: root.committed() }
        SliderField { liveEdit: true; objectName: "aiMaskBlur"; width: parent.width; labelWidth: 54; label: qsTr("Soften"); tip: qsTr("Blurs the mask, in pixels of a 1920-wide picture"); from: 0; to: 64; stepSize: 1; decimals: 0; enabled: root.on; value: root.m.blur !== undefined ? root.m.blur : 0; onEdited: function (v) { root.put("blur", v) }; onEditingFinished: root.committed() }
        Row {
            spacing: Theme.s2
            Toggle { objectName: "aiMaskInvert"; label: qsTr("Invert"); tip: qsTr("Grade everything except the people"); enabled: root.on; checked: root.m.invert === true; onToggled: { root.put("invert", checked); root.committed() } }
            ToolButton { objectName: "aiMaskPreview"; showLabel: true; iconName: "sparkles"; text: qsTr("Show Mask"); checkable: true; checked: root.highlight; enabled: root.on; onClicked: root.highlightToggled(!root.highlight) }
        }
    }
}
