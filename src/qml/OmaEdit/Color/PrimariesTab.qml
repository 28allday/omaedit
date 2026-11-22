pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Primaries: the four wheels (Lift / Gamma / Gain / Offset), the two
// columns of fields (Temp, Tint, Exposure, Contrast, Pivot, Mid Detail;
// Saturation, Hue, Highlights, Shadows, Whites, Blacks) and the right
// column — Bypass, Split Screen, Comparison, LUT, Intensity. Everything
// edits the node's `primaries` map and leaves as one signal.
Item {
    id: root

    property bool compact: width < 1380
    // Smallest workable size; above it the wheels grow with the panel height
    // and the spare width, and the field rows spread down the panel.
    readonly property real minimumContentWidth: 4 * wheelLeast + 3 * Theme.s2 + fieldsWidth + rightWidth + 5 * Theme.s3 + 2 * Theme.hairline + Theme.s2 * 2
    readonly property real minimumContentHeight: Math.max(184, Theme.hControl * 6 + Theme.s1 * 5 + Theme.s2 * 2)
    // The height that gives the wheels a ring big enough to steer with ease.
    readonly property real wantedHeight: 180 + wheelChrome + 2 * Theme.s2
    // The width at which the rings stop growing.
    readonly property real wantedWidth: 4 * ringMost + 3 * Theme.s2 + fieldsWidth + rightWidth + 5 * Theme.s3 + 2 * Theme.hairline + Theme.s2 * 2
    // The least width at the compact sizes: what the qualifier has to leave
    // before it may sit beside the wheels.
    readonly property real compactWidth: 4 * wheelLeast + 3 * Theme.s2 + 2 * (labelWidth + 74 + Theme.szIconHit - 6 + 2 * Theme.s1) + 220 + 5 * Theme.s3 + 2 * Theme.hairline + Theme.s2 * 2
    // A wheel is never narrower than its four value fields need.
    readonly property int wheelLeast: 150
    readonly property int ringMost: 230
    readonly property int labelWidth: 70
    readonly property int valueWidth: compact ? 74 : 86
    readonly property int fieldsWidth: 2 * (labelWidth + valueWidth + Theme.szIconHit - 6 + 2 * Theme.s1)
    readonly property int rightWidth: compact ? 220 : 260
    // The row's six items (wheels, rule, two field columns, rule, right
    // column) leave five gaps.
    readonly property real wheelsWidth: controls.width - fieldsWidth - rightWidth - 5 * Theme.s3 - 2 * Theme.hairline
    // Ring size limited by the height (title and fields around it) and by a
    // quarter of the wheel area.
    readonly property int ringSize: Math.max(64, Math.min(controls.height - wheelChrome, (wheelsWidth - 3 * Theme.s2) / 4, ringMost))
    // A wheel is as wide as its ring, and wider where there is the room:
    // the four fields under the ring are easier to read and to drag.
    readonly property int wheelWidth: Math.max(wheelLeast, ringSize, Math.min(ringSize + 48, Math.floor((wheelsWidth - 3 * Theme.s2) / 4)))
    // Around a ring: its title, then the master dial and the lettered fields.
    readonly property int wheelChrome: Theme.fsHeading + Theme.s2 + Theme.s2 + (Theme.hControl - 2) + Theme.s2 + Theme.fsLabel + 4 + Theme.hControl + Theme.s2
    readonly property real rowSpacing: Math.max(Theme.s1, Math.min(Theme.s3, (controls.height - 6 * Theme.hControl) / 5))
    property var node: ({})
    property var luts: []
    property bool lutEditable: false
    property bool bypass: false
    property string comparison: "none"      // none | wipe | split | still
    property var keyedKeys: []              // primaries keys that hold keyframes
    property bool keyable: true             // false: no keyframe buttons
    signal primariesEdited(var primaries)
    signal lutEdited(var lut)
    signal lutFolderRequested()
    signal lutBrowseRequested()
    signal bypassToggled(bool on)
    signal comparisonEdited(string mode)
    signal keyToggled(string key)
    signal committed()                      // a drag ended / a field landed

    readonly property var p: node.primaries !== undefined ? node.primaries : ({})
    readonly property var wheelDefs: [
        { key: "lift", title: qsTr("Lift"), origin: 0, min: -1, max: 1 },
        { key: "gamma", title: qsTr("Gamma"), origin: 0, min: -1, max: 1 },
        { key: "gain", title: qsTr("Gain"), origin: 1, min: 0, max: 4 },
        { key: "offset", title: qsTr("Offset"), origin: 0, min: -1, max: 1 }
    ]
    readonly property var leftFields: [
        { key: "temp", label: qsTr("Temp"), def: 6500, min: 1000, max: 20000, step: 100, dec: 0 },
        { key: "tint", label: qsTr("Tint"), def: 0, min: -100, max: 100, step: 1, dec: 1 },
        { key: "exposure", label: qsTr("Exposure"), def: 0, min: -3, max: 3, step: 0.05, dec: 2 },
        { key: "contrast", label: qsTr("Contrast"), def: 1, min: 0, max: 3, step: 0.01, dec: 3 },
        { key: "pivot", label: qsTr("Pivot"), def: 0.435, min: 0, max: 1, step: 0.005, dec: 3 },
        { key: "midDetail", label: qsTr("Mid Detail"), def: 0, min: -1, max: 1, step: 0.01, dec: 2 }
    ]
    readonly property var rightFields: [
        { key: "saturation", label: qsTr("Saturation"), def: 1, min: 0, max: 4, step: 0.01, dec: 2 },
        { key: "hue", label: qsTr("Hue"), def: 0, min: -180, max: 180, step: 1, dec: 1 },
        { key: "highlights", label: qsTr("Highlights"), def: 0, min: -1, max: 1, step: 0.01, dec: 2 },
        { key: "shadows", label: qsTr("Shadows"), def: 0, min: -1, max: 1, step: 0.01, dec: 2 },
        { key: "whites", label: qsTr("Whites"), def: 0, min: -1, max: 1, step: 0.01, dec: 2 },
        { key: "blacks", label: qsTr("Blacks"), def: 0, min: -1, max: 1, step: 0.01, dec: 2 }
    ]
    function put(key, value) {
        var m = {}
        for (var k in p) m[k] = p[k]
        m[key] = value
        root.primariesEdited(m)
    }
    function isKeyed(key) { return keyedKeys.indexOf(key) >= 0 }

    // Width the wheels, fields and right column leave over, shared out
    // between the five gaps so the controls span the panel.
    readonly property real spare: controls.width - (4 * wheelWidth + 3 * Theme.s2) - fieldsWidth - rightWidth - 5 * Theme.s3 - 2 * Theme.hairline
    Row {
        id: controls
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3 + Math.max(0, Math.min(Theme.s5 * 4, Math.floor(root.spare / 5)))

        // ── Wheels ──────────────────────────────────────────────────────────
        Row {
            id: wheels
            spacing: Theme.s2
            height: parent.height
            Repeater {
                model: root.wheelDefs.length
                delegate: ColorWheel {
                    id: wheel
                    required property int index
                    readonly property var def: root.wheelDefs[index]
                    objectName: "wheel:" + def.key
                    width: root.wheelWidth
                    stacked: true
                    maxWheel: root.ringSize
                    title: def.title
                    yOrigin: def.origin
                    yMin: def.min
                    yMax: def.max
                    range: def.key === "gain" ? 1 : 0.5
                    keyed: root.isKeyed(def.key + ".y")
                    keyable: root.keyable
                    value: root.p[def.key] !== undefined ? root.p[def.key] : [def.origin, 0, 0, 0]
                    onEdited: function (v) { root.put(def.key, v) }
                    onEditingFinished: root.committed()
                    onResetRequested: { root.put(def.key, [def.origin, 0, 0, 0]); root.committed() }
                    onKeyToggled: root.keyToggled(def.key + ".y")
                }
            }
        }

        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.border }

        // ── Field columns ──────────────────────────────────────────────────
        Repeater {
            model: 2
            delegate: Column {
                id: column
                required property int index
                readonly property var fields: index === 0 ? root.leftFields : root.rightFields
                spacing: root.rowSpacing
                Repeater {
                    model: column.fields.length
                    delegate: Row {
                        id: fieldRow
                        required property int index
                        readonly property var f: column.fields[index]
                        spacing: Theme.s1
                        height: Theme.hControl
                        Text {
                            width: root.labelWidth
                            anchors.verticalCenter: parent.verticalCenter
                            text: fieldRow.f.label
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                        }
                        ValueField {
                            objectName: "field:" + fieldRow.f.key
                            tip: qsTr("Type an exact value for this primaries control")
                            width: root.valueWidth
                            height: Theme.hControl
                            anchors.verticalCenter: parent.verticalCenter
                            decimals: fieldRow.f.dec
                            step: fieldRow.f.step
                            from: fieldRow.f.min
                            to: fieldRow.f.max
                            readonly property real modelValue: root.p[fieldRow.f.key] !== undefined ? root.p[fieldRow.f.key] : fieldRow.f.def
                            value: modelValue
                            onModelValueChanged: value = modelValue
                            onEdited: function (v) { root.put(fieldRow.f.key, v); if (!scrubbing) root.committed() }
                            onEditFinished: root.committed()
                        }
                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "diamond"
                            width: Theme.szIconHit - 6
                            visible: root.keyable && (fieldRow.f.key === "contrast" || fieldRow.f.key === "saturation")
                            iconColor: root.isKeyed(fieldRow.f.key) ? Theme.accent : Theme.textMuted
                            Accessible.name: qsTr("Keyframe %1").arg(fieldRow.f.label)
                            onClicked: root.keyToggled(fieldRow.f.key)
                        }
                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "rotate-ccw"
                            width: Theme.szIconHit - 6
                            visible: fieldRow.f.key !== "contrast" && fieldRow.f.key !== "saturation"
                            Accessible.name: qsTr("Reset %1").arg(fieldRow.f.label)
                            onClicked: { root.put(fieldRow.f.key, fieldRow.f.def); root.committed() }
                        }
                    }
                }
            }
        }

        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.border }

        // ── Right column ───────────────────────────────────────────────────
        Column {
            width: root.rightWidth
            spacing: Math.max(Theme.s2, root.rowSpacing)
            ToolButton {
                objectName: "bypassGrade"
                showLabel: true
                iconName: "eye-off"
                text: root.bypass ? qsTr("Bypassed") : qsTr("Bypass")
                checkable: true
                checked: root.bypass
                onClicked: root.bypassToggled(!root.bypass)
            }
            Row {
                spacing: Theme.s2
                Text { width: 60; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Split Screen"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary; wrapMode: Text.WordWrap }
                SegmentedControl {
                    anchors.verticalCenter: parent.verticalCenter
                    icons: ["square", "panel-left", "compare"]
                    tip: qsTr("How the viewer splits the graded and ungraded picture")
                    names: ["none", "split", "wipe"]
                    currentIndex: Math.max(0, names.indexOf(root.comparison === "still" ? "wipe" : root.comparison))
                    onActivated: function (i) { root.comparisonEdited(names[i]) }
                }
            }
            Row {
                spacing: Theme.s2
                Text { width: 60; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Comparison"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                ComboField {
                    objectName: "comparisonCombo"
                    tip: qsTr("Which before and after comparison the viewer shows")
                    anchors.verticalCenter: parent.verticalCenter
                    width: 150
                    Accessible.name: qsTr("Comparison")
                    readonly property var modes: ["none", "wipe", "split", "still"]
                    model: [qsTr("Off"), qsTr("Wipe"), qsTr("Split"), qsTr("Reference still")]
                    currentIndex: Math.max(0, modes.indexOf(root.comparison))
                    onActivated: function (i) { root.comparisonEdited(modes[i]) }
                }
            }
            LutRow {
                width: parent.width
                enabled: root.lutEditable
                luts: root.luts
                lut: root.node.lut !== undefined ? root.node.lut : ({ path: "", intensity: 1 })
                onLutEdited: function (l) { root.lutEdited(l) }
                onCommitted: root.committed()
                onFolderRequested: root.lutFolderRequested()
                onBrowseRequested: root.lutBrowseRequested()
            }
        }
    }
}
