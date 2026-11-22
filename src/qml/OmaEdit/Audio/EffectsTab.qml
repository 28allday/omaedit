pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import OmaEdit.Ui

// The selected strip's insert chain: add (from the catalogue), reorder,
// bypass, remove, wet/dry where the filter has it, and every parameter as
// a slider in operator units. Presets: the catalogue's defaults plus the
// user's saved chains.
Rectangle {
    id: root

    property string stripName: ""
    property var fx: []              // [{id, bypass, mix, params, group, ai}]
    property var catalogue: []
    property var audioServices: ({})
    property var presets: []         // [{name, fx}]
    property int currentIndex: -1
    property bool enabled2: true

    signal fxEdited(var fx)
    signal editingStarted()
    signal editingFinished()
    property bool gestureActive: false
    function beginGesture() { if (!gestureActive) { gestureActive = true; editingStarted() } }
    function endGesture() { if (gestureActive) { gestureActive = false; editingFinished() } }
    function publishFx(value) {
        if (!gestureActive) editingStarted()
        fxEdited(value)
        if (!gestureActive) editingFinished()
    }
    component EffectSlider: SliderField {
        labelWidth: 64
        liveEdit: true
        onEditingStarted: root.beginGesture()
        onEditingEnded: root.endGesture()
    }
    signal presetSaveRequested(string name, var fx)
    signal presetDeleteRequested(string name)

    color: Theme.panelBg

    readonly property bool textActive: presetName.activeFocus

    function copyFx() { return fx.map(function (e) { var c = {}; for (var k in e) c[k] = e[k]; if (e.params) { c.params = {}; for (var p in e.params) c.params[p] = e.params[p] } return c }) }
    function catalogueEntry(id) { for (var i = 0; i < catalogue.length; ++i) if (catalogue[i].id === id) return catalogue[i]; return null }
    function nameFor(id) { var e = catalogueEntry(id); return e ? e.name : id }
    function paramValue(entry, key) {
        if (entry.params !== undefined && entry.params[key] !== undefined) return entry.params[key]
        var c = catalogueEntry(entry.id)
        return c && c.params[key] !== undefined ? c.params[key] : 0
    }
    function setParam(index, key, value) {
        var f = copyFx()
        if (!f[index].params) f[index].params = {}
        f[index].params[key] = value
        publishFx(f)
    }
    function add(id) {
        var f = copyFx()
        f.push({ id: id, bypass: false, params: {} })
        publishFx(f)
        currentIndex = f.length - 1
    }
    function remove(index) {
        var f = copyFx(); f.splice(index, 1); publishFx(f)
        // The open insert stays open when one above it goes.
        currentIndex = index < currentIndex ? currentIndex - 1 : Math.min(currentIndex, f.length - 1)
    }
    function move(index, delta) {
        var f = copyFx()
        var j = index + delta
        if (j < 0 || j >= f.length) return
        var t = f[index]; f[index] = f[j]; f[j] = t
        publishFx(f)
        currentIndex = j
    }
    function setBypass(index, on) { var f = copyFx(); f[index].bypass = on; publishFx(f) }
    function setMix(index, v) { var f = copyFx(); f[index].mix = v; publishFx(f) }
    function resetParams(index) { var f = copyFx(); f[index].params = {}; publishFx(f) }

    // Parameter specs in operator units: key → label/range.
    readonly property var specs: ({
        threshold: { label: qsTr("Threshold"), from: -60, to: 0, step: 0.5, suffix: " dB", decimals: 1 },
        ratio: { label: qsTr("Ratio"), from: 1, to: 20, step: 0.1, suffix: ":1", decimals: 1 },
        attack: { label: qsTr("Attack"), from: 0.1, to: 500, step: 0.1, suffix: " ms", decimals: 1 },
        release: { label: qsTr("Release"), from: 5, to: 2000, step: 1, suffix: " ms", decimals: 0 },
        knee: { label: qsTr("Knee"), from: 0, to: 18, step: 0.5, suffix: " dB", decimals: 1 },
        makeup: { label: qsTr("Makeup"), from: 0, to: 24, step: 0.5, suffix: " dB", decimals: 1 },
        range: { label: qsTr("Range"), from: -90, to: 0, step: 1, suffix: " dB", decimals: 0 },
        ceiling: { label: qsTr("Ceiling"), from: -12, to: 0, step: 0.1, suffix: " dBFS", decimals: 1 },
        frequency: { label: qsTr("Frequency"), from: 20, to: 20000, step: 10, suffix: " Hz", decimals: 0 },
        intensity: { label: qsTr("Intensity"), from: 0, to: 1, step: 0.01, suffix: "", decimals: 2 },
        max: { label: qsTr("Max reduction"), from: 0, to: 1, step: 0.01, suffix: "", decimals: 2 },
        reduction: { label: qsTr("Reduction"), from: 0, to: 40, step: 0.5, suffix: " dB", decimals: 1 },
        floor: { label: qsTr("Noise floor"), from: -80, to: -20, step: 1, suffix: " dB", decimals: 0 }
    })

    function specFor(id, key) {
        var s = Object.assign({}, specs[key] || {label:key, from:0, to:100, step:1, suffix:"", decimals:1})
        if (id === "hum" && key === "frequency") { s.from=40; s.to=120; s.step=1 }
        if (id === "hum" && key === "reduction") s.to=60
        if (id === "limiter" && key === "attack") s.to=80
        if (id === "limiter" && key === "release") { s.from=1; s.to=8000 }
        if (id === "gate" && key === "threshold") s.from=-80
        if (id === "deesser" && key === "frequency") { s.label=qsTr("Tuning"); s.from=0; s.to=100; s.step=1; s.suffix=" %" }
        if (id === "deesser" && key === "max") { s.label=qsTr("Max cut"); s.from=0; s.to=48; s.step=1; s.suffix=" dB"; s.decimals=0 }
        return s
    }
    function displayParam(entry, key) {
        var v=paramValue(entry,key)
        return entry.id === "deesser" ? (key === "max" ? (1-v)*48 : key === "frequency" ? v/240 : v) : v
    }
    function storedParam(id, key, v) {
        return id === "deesser" ? (key === "max" ? 1-v/48 : key === "frequency" ? v*240 : v) : v
    }

    Column {
        enabled: root.enabled2
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s2

        Row {
            width: parent.width
            spacing: Theme.s2
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - addButton.width - presetsButton.width - Theme.s2 * 2
                elide: Text.ElideRight
                text: root.stripName !== "" ? qsTr("Inserts · %1").arg(root.stripName) : qsTr("Inserts")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsHeading
                font.weight: Theme.wHeading
                color: Theme.textPrimary
            }
            ToolButton {
                id: presetsButton
                enabled: root.enabled2
                iconName: "bookmark"
                text: qsTr("Presets")
                showLabel: true
                onClicked: presetMenu.popup()
            }
            ToolButton {
                id: addButton
                objectName: "fxAdd"
                iconName: "plus"
                text: qsTr("Add")
                showLabel: true
                enabled: root.enabled2
                onClicked: addMenu.popup()
            }
        }

        Text {
            width: parent.width;wrapMode: Text.WordWrap;font.pixelSize: Theme.fsLabel;color: Theme.textMuted
            text: root.audioServices.pluginHost ? root.audioServices.pluginHost.reason : qsTr("Third-party plug-ins unavailable")
        }
        // Keep the insert list scrollable without crowding out its controls.
        Flickable {
            width: parent.width
            height: Math.min(chain.height, Math.max(70, root.height * 0.4))
            contentHeight: chain.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}
        Column {
            id: chain
            width: parent.width
            spacing: 2
            Repeater {
                model: root.fx.length
                delegate: Rectangle {
                    id: rowItem
                    required property int index
                    readonly property var entry: root.fx[index] !== undefined ? root.fx[index] : ({})
                    readonly property bool current: root.currentIndex === index
                    width: chain.width
                    height: Theme.hRow + 4
                    radius: Theme.rControl
                    color: current ? Theme.controlBg : rowHover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
                    border.color: current ? Theme.accent : entry.ai === true ? Theme.aiAccent : Theme.border
                    border.width: Theme.hairline
                    objectName: "fxRow:" + index
                    HoverHandler { id: rowHover }
                    TapHandler { onTapped: root.currentIndex = rowItem.index }
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s1
                        anchors.rightMargin: Theme.s1
                        spacing: Theme.s1
                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: rowItem.entry.bypass === true ? "eye-off" : "eye"
                            iconColor: rowItem.entry.bypass === true ? Theme.textMuted : Theme.audio
                            Accessible.name: qsTr("Bypass")
                            onClicked: root.setBypass(rowItem.index, !(rowItem.entry.bypass === true))
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - Theme.szIconHit * 5 - Theme.s1 * 6
                            elide: Text.ElideRight
                            text: (rowItem.index + 1) + ". " + root.nameFor(rowItem.entry.id) + (rowItem.entry.ai === true ? "  ✦" : "")
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsBase
                            font.strikeout: rowItem.entry.bypass === true
                            color: rowItem.entry.bypass === true ? Theme.textMuted : Theme.textPrimary
                        }
                        IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "chevron-up"; enabled: rowItem.index > 0; Accessible.name: qsTr("Move up"); onClicked: root.move(rowItem.index, -1) }
                        IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "chevron-down"; enabled: rowItem.index < root.fx.length - 1; Accessible.name: qsTr("Move down"); onClicked: root.move(rowItem.index, 1) }
                        IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "rotate-ccw"; Accessible.name: qsTr("Reset parameters"); onClicked: root.resetParams(rowItem.index) }
                        IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "x"; Accessible.name: qsTr("Remove"); onClicked: root.remove(rowItem.index) }
                    }
                }
            }
            EmptyState {
                width: chain.width
                visible: root.fx.length === 0
                iconName: "sliders-horizontal"
                title: root.stripName !== "" ? qsTr("No inserts on %1").arg(root.stripName) : qsTr("Select a track or the master")
                description: root.stripName !== "" ? qsTr("Add an equaliser, compressor, gate, limiter, de-esser or noise reduction.") : ""
                actionText: root.stripName !== "" ? qsTr("Add Insert") : ""
                onActionTriggered: addMenu.popup()
            }
        }

        }
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border; visible: root.currentIndex >= 0 && root.currentIndex < root.fx.length }

        // Parameters of the current insert.
        Flickable {
            id: paramsFlick
            objectName: "effectParameters"
            width: parent.width
            height: Math.max(0, parent.height - y)
            visible: root.currentIndex >= 0 && root.currentIndex < root.fx.length
            contentHeight: params.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}
            readonly property var entry: root.currentIndex >= 0 && root.currentIndex < root.fx.length ? root.fx[root.currentIndex] : ({})
            readonly property var cat: root.catalogueEntry(entry.id !== undefined ? entry.id : "")
            Column {
                id: params
                width: paramsFlick.width
                spacing: Theme.s1
                Text {
                    text: paramsFlick.cat ? paramsFlick.cat.name : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsHeading
                    font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                // EQ bands.
                Repeater {
                    model: paramsFlick.entry.id === "eq" ? 5 : 0
                    delegate: Column {
                        id: bandCol
                        required property int index
                        readonly property var bands: {
                            var b = paramsFlick.entry.params !== undefined && paramsFlick.entry.params.bands !== undefined ? paramsFlick.entry.params.bands
                                  : (paramsFlick.cat ? paramsFlick.cat.params.bands : [])
                            return b
                        }
                        readonly property var band: bands[index] !== undefined ? bands[index] : ({ f: 1000, g: 0, q: 1 })
                        width: params.width
                        spacing: 2
                        function setBand(key, value) {
                            var f = root.copyFx()
                            var list = bands.map(function (x) { return { f: x.f, g: x.g, q: x.q } })
                            list[index][key] = value
                            if (!f[root.currentIndex].params) f[root.currentIndex].params = {}
                            f[root.currentIndex].params.bands = list
                            root.publishFx(f)
                        }
                        Text { text: ["LF", "LMF", "MF", "HMF", "HF"][index]; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                        EffectSlider { width: params.width; label: qsTr("Gain"); tip: qsTr("Boost or cut this band, -18 to +18 dB"); from: -18; to: 18; origin: 0; stepSize: 0.1; suffix: " dB"; value: bandCol.band.g; onEdited: function (v) { bandCol.setBand("g", v) } }
                        EffectSlider { width: params.width; label: qsTr("Freq"); tip: qsTr("Centre frequency this band is anchored on"); from: 20; to: 20000; stepSize: 1; decimals: 0; suffix: " Hz"; value: bandCol.band.f; onEdited: function (v) { bandCol.setBand("f", v) } }
                        EffectSlider { width: params.width; label: qsTr("Q"); tip: qsTr("Width of this band; a higher Q touches fewer frequencies"); from: 0.1; to: 10; stepSize: 0.1; value: bandCol.band.q; onEdited: function (v) { bandCol.setBand("q", v) } }
                    }
                }
                // Scalar params.
                Repeater {
                    model: paramsFlick.cat && paramsFlick.entry.id !== "eq" ? Object.keys(paramsFlick.cat.params).filter(function (k) { return typeof paramsFlick.cat.params[k] === "number" }) : []
                    delegate: EffectSlider {
                        required property string modelData
                        readonly property var spec: root.specFor(paramsFlick.entry.id, modelData)
                        width: params.width
                        tip: qsTr("Set this parameter on the selected insert, in its own units")
                        label: spec.label
                        from: spec.from
                        to: spec.to
                        stepSize: spec.step
                        suffix: spec.suffix
                        decimals: spec.decimals
                        value: root.displayParam(paramsFlick.entry, modelData)
                        onEdited: function (v) { root.setParam(root.currentIndex, modelData, root.storedParam(paramsFlick.entry.id, modelData, v)) }
                    }
                }
                Toggle {
                    visible: paramsFlick.cat && paramsFlick.cat.params.track !== undefined
                    label: qsTr("Track the noise floor")
                    tip: qsTr("Keep following the noise floor as the background changes")
                    checked: root.paramValue(paramsFlick.entry, "track") === true
                    onToggled: root.setParam(root.currentIndex, "track", checked)
                }
                EffectSlider {
                    visible: paramsFlick.cat && paramsFlick.cat.mixable === true
                    width: params.width
                    label: qsTr("Mix")
                    tip: qsTr("Blend of treated and untreated sound; 100% is fully treated")
                    from: 0; to: 100; stepSize: 1; decimals: 0; suffix: " %"
                    value: (paramsFlick.entry.mix !== undefined ? paramsFlick.entry.mix : 1) * 100
                    onEdited: function (v) { root.setMix(root.currentIndex, v / 100) }
                }
            }
        }
    }

    ContextMenu {
        id: addMenu
        Repeater {
            model: root.catalogue
            delegate: MenuAction {
                required property var modelData
                text: modelData.name
                onTriggered: root.add(modelData.id)
            }
        }
    }
    ContextMenu {
        id: presetMenu
        MenuAction { text: qsTr("Save Chain as Preset…"); iconName: "save"; enabled: root.fx.length > 0; onTriggered: presetPrompt.open() }
        C.MenuSeparator {}
        MenuAction { text: qsTr("Dialogue Clean"); onTriggered: root.publishFx([{ id: "highpass", params: { frequency: 80 } }, { id: "eq", params: { bands: [{ f: 100, g: 0, q: 0.7 }, { f: 300, g: -2, q: 1 }, { f: 1000, g: 0, q: 1 }, { f: 3000, g: 2, q: 1 }, { f: 10000, g: 1, q: 0.7 }] } }, { id: "comp", params: { threshold: -18, ratio: 3, attack: 10, release: 120, makeup: 2 } }, { id: "deesser" }]) }
        MenuAction { text: qsTr("Music Glue"); onTriggered: root.publishFx([{ id: "comp", params: { threshold: -14, ratio: 2, attack: 30, release: 250, makeup: 1 } }, { id: "limiter", params: { ceiling: -1 } }]) }
        MenuAction { text: qsTr("Ambience Bed"); onTriggered: root.publishFx([{ id: "highpass", params: { frequency: 120 } }, { id: "lowpass", params: { frequency: 9000 } }]) }
        MenuAction { text: qsTr("Master Limiter"); onTriggered: root.publishFx([{ id: "limiter", params: { ceiling: -1, attack: 5, release: 50 } }]) }
        C.MenuSeparator { visible: root.presets.length > 0 }
        Repeater {
            model: root.presets
            delegate: MenuAction {
                required property var modelData
                text: modelData.name
                onTriggered: root.publishFx(modelData.fx)
            }
        }
        C.MenuSeparator { visible: root.presets.length > 0 }
        C.Menu {
            title: qsTr("Delete Preset")
            enabled: root.presets.length > 0
            Repeater {
                model: root.presets
                delegate: MenuAction { required property var modelData; text: modelData.name; onTriggered: root.presetDeleteRequested(modelData.name) }
            }
        }
    }
    FormDialog {
        width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
        id: presetPrompt
        title: qsTr("Save Chain Preset")
        modal: true
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        // A preset keeps the chain as it was: nested params (an EQ's bands)
        // read from the backend stay live views, so copy all the way down.
        onAccepted: { root.presetSaveRequested(presetName.text, JSON.parse(JSON.stringify(root.copyFx()))); presetName.text = "" }
        onOpened: presetName.forceActiveFocus()
        LineEdit { id: presetName; Layout.fillWidth: true; hint: qsTr("Preset name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; tip: qsTr("Name this whole insert chain is saved and listed under"); hoverEnabled: true }
    }
}
