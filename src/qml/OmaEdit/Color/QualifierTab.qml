pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Qualifier: an HSL / RGB key. The eyedropper arms a pick in the viewer
// (the page reads the pixel under the click and sends it back as
// `picked`); + / − samples widen or narrow the hue / sat / lum deltas
// around the chosen colour; range sliders, softness, invert; the matte
// preview is the viewer's highlight mode. Edits the node's `qualifier`.
Item {
    id: root

    property var node: ({})
    property bool picking: false
    property string pickMode: "set"
    signal pickModeEdited(string mode)
    property bool highlight: false
    signal qualifierEdited(var qualifier)
    signal pickToggled(bool on)
    signal highlightToggled(bool on)
    signal committed()

    readonly property var q: node.qualifier !== undefined ? node.qualifier : ({})
    readonly property bool on: q.enabled === true
    readonly property bool rgb: q.subspace === "rgb"
    function put(key, value) {
        var m = {}
        for (var k in q) m[k] = q[k]
        m[key] = value
        root.qualifierEdited(m)
    }
    function putMany(map) {
        var m = {}
        for (var k in q) m[k] = q[k]
        for (var j in map) m[j] = map[j]
        root.qualifierEdited(m)
    }
    function reset() {
        putMany({ color: "#ff0000", deltaH: 0.2, deltaS: 0.3, deltaL: 0.3, softness: 0.1, invert: false, subspace: "hsl", blur: 0, grow: 0 })
        root.committed()
    }
    // A picked colour. mode "set": the key colour (and the qualifier on);
    // "add": widen the deltas just enough to take the sample in;
    // "subtract": narrow them so the sample falls outside.
    function picked(hex, mode) {
        if (mode === "set" || !on || q.color === undefined) {
            putMany({ enabled: true, color: hex })
            root.committed()
            return
        }
        var a = Qt.color(q.color), b = Qt.color(hex)
        var hueDistance = a.hslSaturation === 0 || b.hslSaturation === 0 ? 0 : Math.abs(a.hslHue - b.hslHue) % 1
        var distances = rgb ? [Math.abs(a.r - b.r), Math.abs(a.g - b.g), Math.abs(a.b - b.b)]
                            : [Math.min(hueDistance, 1 - hueDistance), Math.abs(a.hslSaturation - b.hslSaturation), Math.abs(a.hslLightness - b.hslLightness)]
        var keys = ["deltaH", "deltaS", "deltaL"], defaults = [0.2, 0.3, 0.3]
        var ranges = keys.map(function(k, i) { return q[k] !== undefined ? q[k] : defaults[i] })
        var changes = {}
        if (mode === "add") {
            for (var i = 0; i < 3; ++i)
                changes[keys[i]] = Math.min(i === 0 && !rgb ? 0.5 : 1,
                                          Math.max(ranges[i], distances[i] + (i === 0 ? 0.02 : 0.05)))
        } else {
            // A rectangular range only needs one edge narrowed to exclude a
            // sample. Preserve the other axes, and account for the feather.
            var axis = 0, softness = q.softness !== undefined ? q.softness : 0.1
            for (var j = 1; j < 3; ++j)
                if (distances[j] / Math.max(0.001, ranges[j] + softness)
                    > distances[axis] / Math.max(0.001, ranges[axis] + softness)) axis = j
            changes[keys[axis]] = Math.max(axis === 0 ? 0.01 : 0.02,
                                          Math.min(ranges[axis], distances[axis] - softness - 0.001))
        }
        putMany(changes)
        root.committed()
    }

    // Three columns share the panel width; the sliders stretch with them. Beside
    // the wheels on a narrower panel there are two: the matte's tools join the others.
    readonly property bool narrow: width < 880
    readonly property real usable: width - 2 * Theme.s2 - (narrow ? 2 : 4) * Theme.s3 - (narrow ? 1 : 2) * Theme.hairline
    readonly property real pickWidth: narrow ? 220 : Math.max(220, Math.min(320, usable * 0.24))
    readonly property real matteWidth: Math.max(240, Math.min(420, usable * 0.30))
    readonly property real rangeWidth: narrow ? usable - pickWidth : Math.max(320, Math.min(640, usable - pickWidth - matteWidth))
    readonly property real rowSpacing: Math.max(Theme.s1, Math.min(Theme.s3, (height - 2 * Theme.s2 - (narrow ? 6 : 5) * Theme.hControl) / (narrow ? 5 : 4)))

    Row {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3

        // Swatch + pick controls.
        Column {
            width: root.pickWidth
            spacing: Theme.s2
            Row {
                spacing: Theme.s2
                Toggle { objectName: "qualifierOn"; anchors.verticalCenter: parent.verticalCenter; label: qsTr("Colour Pick"); tip: qsTr("Limit this layer's correction to the keyed colour range"); checked: root.on; onToggled: { root.put("enabled", checked); root.committed() } }
            }
            Rectangle {
                objectName: "qualifierSwatch"
                width: parent.width
                height: Math.max(56, Math.min(88, root.height * 0.24))
                radius: Theme.rControl
                color: root.q.color !== undefined ? root.q.color : "#ff0000"
                border.color: Theme.border
                border.width: Theme.hairline
                Text { anchors.centerIn: parent; text: root.q.color !== undefined ? String(root.q.color).toUpperCase() : ""; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary; style: Text.Outline; styleColor: Theme.windowBg }
            }
            Row {
                spacing: Theme.s1
                ToolButton { objectName: "eyedropper"; showLabel: true; iconName: "mouse-pointer-2"; text: qsTr("Pick"); checkable: true; checked: root.picking; onClicked: {root.pickModeEdited("set");root.pickToggled(!root.picking)} }
                ToolButton { showLabel: true; iconName: "plus"; text: qsTr("Add"); checked:root.picking&&root.pickMode==="add"; onClicked:{root.pickModeEdited("add");root.pickToggled(true)} }
                ToolButton { showLabel: true; iconName: "minus"; text: qsTr("Subtract"); checked:root.picking&&root.pickMode==="subtract"; onClicked:{root.pickModeEdited("subtract");root.pickToggled(true)} }
            }
            Row {
                visible: root.narrow
                spacing: Theme.s1
                ToolButton { objectName: root.narrow ? "mattePreview" : ""; showLabel: true; iconName: "sparkles"; text: qsTr("Matte Preview"); checkable: true; checked: root.highlight; enabled: root.on; onClicked: root.highlightToggled(!root.highlight) }
                ToolButton { objectName: root.narrow ? "qualifierReset" : ""; showLabel: true; iconName: "rotate-ccw"; text: qsTr("Reset"); onClicked: root.reset() }
            }
            Text { visible: !root.narrow || root.picking; width: parent.width; wrapMode: Text.WordWrap; text: root.picking ? qsTr("Click the viewer to sample. Ctrl-click adds to the range, Shift-click narrows it.") : qsTr("Pick a colour from the viewer, then widen the range with more samples."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: root.picking ? Theme.accent : Theme.textMuted }
        }

        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.border }

        // Ranges.
        Column {
            width: root.rangeWidth
            spacing: root.rowSpacing
            SliderField { liveEdit: true; objectName: "deltaH"; width: parent.width; label: root.rgb ? qsTr("Red range") : qsTr("Hue range"); tip: root.rgb ? qsTr("How far either side of the picked red value is keyed") : qsTr("How far either side of the picked hue is keyed"); from: 0.01; to: root.rgb ? 1 : 0.5; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.q.deltaH !== undefined ? root.q.deltaH : 0.2; onEdited: function (v) { root.put("deltaH", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: root.rgb ? qsTr("Green range") : qsTr("Sat range"); tip: root.rgb ? qsTr("How far either side of the picked green value is keyed") : qsTr("How far either side of the picked saturation is keyed"); from: 0.02; to: 1; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.q.deltaS !== undefined ? root.q.deltaS : 0.3; onEdited: function (v) { root.put("deltaS", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: root.rgb ? qsTr("Blue range") : qsTr("Lum range"); tip: root.rgb ? qsTr("How far either side of the picked blue value is keyed") : qsTr("How far either side of the picked brightness is keyed"); from: 0.02; to: 1; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.q.deltaL !== undefined ? root.q.deltaL : 0.3; onEdited: function (v) { root.put("deltaL", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Softness"); tip: qsTr("Feathers the key edge so the correction blends in"); from: 0; to: 1; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.q.softness !== undefined ? root.q.softness : 0.1; onEdited: function (v) { root.put("softness", v) }; onEditingFinished: root.committed() }
            Row {
                spacing: Theme.s3
                Toggle { label: qsTr("Invert"); tip: qsTr("Key everything outside the sampled range instead of inside it"); enabled: root.on; checked: root.q.invert === true; onToggled: { root.put("invert", checked); root.committed() } }
                Row {
                    spacing: Theme.s1
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Space"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                    SegmentedControl {
                        anchors.verticalCenter: parent.verticalCenter
                        labels: ["HSL", "RGB"]
                        tip: qsTr("Whether the key is measured in HSL or in RGB")
                        names: ["hsl", "rgb"]
                        enabled: root.on
                        currentIndex: root.q.subspace === "rgb" ? 1 : 0
                        onActivated: function (i) { root.put("subspace", names[i]); root.committed() }
                    }
                }
            }
            Row {
                visible: root.narrow
                spacing: Theme.s2
                readonly property real half: (parent.width - spacing) / 2
                SliderField { liveEdit: true; objectName: root.narrow ? "matteBlur" : ""; width: parent.half; labelWidth: 54; label: qsTr("Blur"); tip: qsTr("Softens the finished matte; sizes assume a 1920-pixel frame"); from: 0; to: 64; value: root.q.blur || 0; stepSize: 1; decimals: 0; onEdited: function(v) { root.put("blur",v) }; onEditingFinished: root.committed() }
                SliderField { liveEdit: true; objectName: root.narrow ? "matteGrow" : ""; width: parent.half; labelWidth: 54; label: qsTr("Grow"); tip: qsTr("Expands or contracts the matte edge, in pixels"); from: -24; to: 24; value: root.q.grow || 0; stepSize: 1; decimals: 0; onEdited: function(v) { root.put("grow",v) }; onEditingFinished: root.committed() }
            }
        }

        Rectangle { visible: !root.narrow; width: Theme.hairline; height: parent.height; color: Theme.border }

        Column {
            visible: !root.narrow
            width: root.matteWidth
            spacing: Theme.s2
            ToolButton { objectName: root.narrow ? "" : "mattePreview"; showLabel: true; iconName: "sparkles"; text: qsTr("Matte Preview"); checkable: true; checked: root.highlight; enabled: root.on; onClicked: root.highlightToggled(!root.highlight) }
            ToolButton { objectName: root.narrow ? "" : "qualifierReset"; showLabel: true; iconName: "rotate-ccw"; text: qsTr("Reset"); onClicked: root.reset() }
            SliderField { liveEdit: true; objectName: root.narrow ? "" : "matteBlur"; width: parent.width; label: qsTr("Blur"); tip: qsTr("Softens the finished matte; sizes assume a 1920-pixel frame"); from: 0; to: 64; value: root.q.blur || 0; stepSize: 1; decimals: 0; onEdited: function(v) { root.put("blur",v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; objectName: root.narrow ? "" : "matteGrow"; width: parent.width; label: qsTr("Grow / shrink"); tip: qsTr("Expands or contracts the matte edge, in pixels"); from: -24; to: 24; value: root.q.grow || 0; stepSize: 1; decimals: 0; onEdited: function(v) { root.put("grow",v) }; onEditingFinished: root.committed() }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("Cleanup sizes use 1920-pixel frame width and scale with the picture."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        }
    }
}
