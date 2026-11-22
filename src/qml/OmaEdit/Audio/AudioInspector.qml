pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Edit as Edit

// The right-hand inspector for the selected strip: Voice Cleanup (the DSP
// repair chain), Equalizer with its graph, Dynamics (compressor +
// gain-reduction meter), De-Esser, Noise Reduction with the spectrum, and
// Loudness. Each section toggles the insert of that kind on the strip's
// chain and edits its parameters; reset puts the catalogue defaults back.
Rectangle {
    id: root

    property string stripName: ""
    property var mix: ({})              // the selected strip's mix map
    property var catalogue: []
    property var levels: ({})           // {in, post, out}
    property var spectrum: []
    property var loudness: ({})
    property bool hasStrip: false
    property bool editable: true
    property bool isMaster: false
    property var audioServices: ({})
    property bool before: false         // A/B: every voice-cleanup insert bypassed for a listen

    signal mixEdited(string key, var value)
    signal beforeToggled(bool on)
    signal editingStarted()
    signal editingFinished()
    property bool gestureActive: false
    function beginGesture() { if (!gestureActive) { gestureActive = true; editingStarted() } }
    function endGesture() { if (gestureActive) { gestureActive = false; editingFinished() } }
    function publishFx(value) {
        if (!gestureActive) editingStarted()
        mixEdited("fx", value)
        if (!gestureActive) editingFinished()
    }
    component EffectSlider: SliderField {
        labelWidth: 64
        liveEdit: true
        onEditingStarted: root.beginGesture()
        onEditingEnded: root.endGesture()
    }
    component EqValue: ValueField {
        property real modelValue: 0
        value: modelValue
        onModelValueChanged: value = modelValue
        onEditStarted: root.beginGesture()
        onEditFinished: root.endGesture()
    }

    color: Theme.panelBg
    readonly property var fx: mix.fx !== undefined ? mix.fx : []
    readonly property bool textActive: false

    function copyFx() { return fx.map(function (e) { var c = {}; for (var k in e) c[k] = e[k]; if (e.params) { c.params = {}; for (var p in e.params) c.params[p] = e.params[p] } return c }) }
    function indexOf(id) { for (var i = 0; i < fx.length; ++i) if (fx[i].id === id && fx[i].group !== "voice") return i; return -1 }
    function entry(id) { var i = indexOf(id); return i >= 0 ? fx[i] : null }
    function catalogueEntry(id) { for (var i = 0; i < catalogue.length; ++i) if (catalogue[i].id === id) return catalogue[i]; return null }
    function param(id, key) {
        var e = entry(id)
        if (e && e.params !== undefined && e.params[key] !== undefined) return e.params[key]
        var c = catalogueEntry(id)
        return c && c.params[key] !== undefined ? c.params[key] : 0
    }
    function enabledOf(id) { var e = entry(id); return e !== null && e.bypass !== true }
    function setEnabled(id, on) {
        var f = copyFx()
        var i = indexOf(id)
        if (on) {
            if (i >= 0) f[i].bypass = false
            else f.push({ id: id, bypass: false, params: {} })
        } else if (i >= 0) {
            f[i].bypass = true
        }
        publishFx(f)
    }
    function setParam(id, key, value) {
        var f = copyFx()
        var i = indexOf(id)
        if (i < 0) { f.push({ id: id, bypass: false, params: {} }); i = f.length - 1 }
        if (!f[i].params) f[i].params = {}
        f[i].params[key] = value
        publishFx(f)
    }
    function resetSection(id) {
        var f = copyFx()
        var i = indexOf(id)
        if (i >= 0) { f[i].params = {}; publishFx(f) }
    }
    function bands() {
        var e = entry("eq")
        if (e && e.params !== undefined && e.params.bands !== undefined) return e.params.bands
        var c = catalogueEntry("eq")
        return c ? c.params.bands : []
    }
    function setBand(index, key, value) {
        var list = bands().map(function (x) { return { f: x.f, g: x.g, q: x.q } })
        if (!list[index]) return
        list[index][key] = value
        setParam("eq", "bands", list)
    }
    // Voice cleanup: a group of inserts tagged group:"voice".
    readonly property bool voiceOn: { for (var i = 0; i < fx.length; ++i) if (fx[i].group === "voice" && fx[i].bypass !== true) return true; return false }
    readonly property bool voicePresent: { for (var i = 0; i < fx.length; ++i) if (fx[i].group === "voice") return true; return false }
    function setVoice(on) {
        var f = copyFx()
        if (!voicePresent && on) f = JSON.parse(JSON.stringify(audioServices.cleanupChain || [])).concat(f)
        else f.forEach(function(e) {
            if (e.group !== "voice") return
            if (on) { e.bypass = e.voiceWasBypassed === true; delete e.voiceWasBypassed }
            else { if (e.voiceWasBypassed === undefined) e.voiceWasBypassed = e.wasBypassed !== undefined ? e.wasBypassed : e.bypass === true; e.bypass = true }
            delete e.wasBypassed
        })
        publishFx(f)
    }
    function loudText(k, u) { var v = loudness[k]; return v === undefined || v <= -99 ? "—" : v.toFixed(1) + " " + u }
    readonly property real grDb: levels.gainReduction !== undefined ? Math.max(0, levels.gainReduction) : 0

    Item {
        id: header
        width: parent.width
        height: Theme.hTab
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            text: root.hasStrip ? root.stripName : qsTr("Inspector")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }

    EmptyState {
        anchors.centerIn: parent
        visible: !root.hasStrip
        iconName: "sliders-horizontal"
        title: qsTr("Nothing selected")
        description: qsTr("Select a track, a strip or the master.")
    }

    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.hasStrip
        contentHeight: sections.height + Theme.s3
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: sections
            enabled: root.editable
            width: parent.width

            // Voice Cleanup
            Rectangle {
                width: parent.width
                height: Theme.hControl + Theme.s3
                color: "transparent"
                visible: !root.isMaster
                Rectangle {
                    objectName: "voiceCleanup"
                    anchors.fill: parent
                    anchors.margins: Theme.s2
                    radius: Theme.rControl
                    color: root.voiceOn ? Qt.rgba(Theme.aiAccent.r, Theme.aiAccent.g, Theme.aiAccent.b, 0.22) : Theme.controlBg
                    border.color: Theme.aiAccent
                    border.width: Theme.hairline
                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s1
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.s2
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 28; height: 16; radius: Theme.rControl
                            color: Theme.aiAccent
                            Text { anchors.centerIn: parent; text: "DSP"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; font.weight: Theme.wHeading; color: Theme.textPrimary }
                        }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Voice Cleanup"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; color: Theme.textPrimary }
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s1
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.s1
                        ToolButton {
                            visible: root.voicePresent
                            text: root.before ? qsTr("Before") : qsTr("After")
                            showLabel: true
                            checked: root.before
                            onClicked: root.beforeToggled(!root.before)
                        }
                        Toggle { objectName: "voiceToggle"; tip: qsTr("Switch the dialogue cleanup chain on for this strip"); checked: root.voiceOn; onToggled: root.setVoice(checked) }
                    }
                    TapHandler { onTapped: root.setVoice(!root.voiceOn) }
                }
            }

            // Equalizer
            Edit.InspectorSection {
                objectName: "eqSection"
                width: parent.width
                title: qsTr("Equaliser")
                expanded: true
                modified: root.entry("eq") !== null
                onReset: root.resetSection("eq")
                Toggle { objectName: "eqToggle"; anchors.right: parent.right; tip: qsTr("Switch the five-band EQ in or out of the insert chain"); checked: root.enabledOf("eq"); onToggled: root.setEnabled("eq", checked) }
                EqGraph {
                    id: eqGraph
                    objectName: "eqGraph"
                    width: parent.width
                    bands: root.bands()
                    opacity: root.enabledOf("eq") ? 1 : Theme.disabledOpacity
                    property int band: 2
                    current: band
                    onEditingStarted: root.beginGesture()
                    onEditingFinished: root.endGesture()
                    onBandSelected: function (i) { band = i }
                    onBandEdited: function (i, f, g) {
                        var list = root.bands().map(function (x) { return { f: x.f, g: x.g, q: x.q } })
                        list[i].f = f; list[i].g = g
                        root.setParam("eq", "bands", list)
                    }
                }
                Grid {
                    id: eqGrid
                    width: parent.width
                    columns: 6
                    columnSpacing: 2
                    rowSpacing: 2
                    readonly property real cell: (width - 26 - 10) / 5
                    component UnitLabel: Text { width: 26; height: Theme.hControl; verticalAlignment: Text.AlignVCenter; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
                    UnitLabel { text: "" }
                    Repeater {
                        model: ["LF", "LMF", "MF", "HMF", "HF"]
                        delegate: Text { required property string modelData; width: eqGrid.cell; horizontalAlignment: Text.AlignHCenter; text: modelData; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
                    }
                    UnitLabel { text: "dB" }
                    Repeater {
                        model: 5
                        delegate: EqValue { required property int index; objectName: "eqGain:" + index; width: eqGrid.cell; tip: qsTr("Boost or cut this band, -18 to +18 dB"); modelValue: root.bands()[index] ? root.bands()[index].g : 0; from: -18; to: 18; step: 0.1; decimals: 1; onEdited: function (v) { root.setBand(index, "g", v) } }
                    }
                    UnitLabel { text: "Hz" }
                    Repeater {
                        model: 5
                        delegate: EqValue {
                            required property int index
                            objectName: "eqFrequency:" + index
                            modelValue: root.bands()[index] ? root.bands()[index].f : 20
                            width: eqGrid.cell
                            tip: qsTr("Centre frequency in Hz, from 20 to 20000")
                            decimals: 0
                            from: 20
                            to: 20000
                            step: 10
                            onEdited: function (v) { root.setBand(index, "f", v) }
                        }
                    }
                    UnitLabel { text: "Q" }
                    Repeater {
                        model: 5
                        delegate: EqValue { required property int index; objectName: "eqQ:" + index; width: eqGrid.cell; tip: qsTr("Width of this band; a higher Q touches fewer frequencies"); modelValue: root.bands()[index] ? root.bands()[index].q : 1; from: 0.1; to: 10; step: 0.1; decimals: 1; onEdited: function (v) { root.setBand(index, "q", v) } }
                    }
                }
            }

            // Dynamics
            Edit.InspectorSection {
                objectName: "dynamicsSection"
                width: parent.width
                title: qsTr("Dynamics")
                expanded: true
                modified: root.entry("comp") !== null
                onReset: root.resetSection("comp")
                Toggle { objectName: "compToggle"; anchors.right: parent.right; label: qsTr("Comp"); tip: qsTr("Switch the compressor on to even out the level"); checked: root.enabledOf("comp"); onToggled: root.setEnabled("comp", checked) }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    Column {
                        width: parent.width - gr.width - Theme.s2
                        spacing: 2
                        opacity: root.enabledOf("comp") ? 1 : Theme.disabledOpacity
                        EffectSlider { width: parent.width; label: qsTr("Threshold"); tip: qsTr("Level the signal has to pass before it is compressed"); from: -60; to: 0; stepSize: 0.5; suffix: " dB"; value: root.param("comp", "threshold"); onEdited: function (v) { root.setParam("comp", "threshold", v) } }
                        EffectSlider { width: parent.width; label: qsTr("Ratio"); tip: qsTr("How hard the signal is squeezed once past the threshold"); from: 1; to: 20; stepSize: 0.1; suffix: ":1"; value: root.param("comp", "ratio"); onEdited: function (v) { root.setParam("comp", "ratio", v) } }
                        EffectSlider { width: parent.width; label: qsTr("Attack"); tip: qsTr("How fast the compressor clamps down on a loud sound"); from: 0.1; to: 500; stepSize: 0.1; suffix: " ms"; value: root.param("comp", "attack"); onEdited: function (v) { root.setParam("comp", "attack", v) } }
                        EffectSlider { width: parent.width; label: qsTr("Release"); tip: qsTr("How long the compressor takes to let go afterwards"); from: 5; to: 2000; stepSize: 1; decimals: 0; suffix: " ms"; value: root.param("comp", "release"); onEdited: function (v) { root.setParam("comp", "release", v) } }
                        EffectSlider { width: parent.width; label: qsTr("Makeup"); tip: qsTr("Gain added after compression to bring the level back up"); from: 0; to: 24; stepSize: 0.5; suffix: " dB"; value: root.param("comp", "makeup"); onEdited: function (v) { root.setParam("comp", "makeup", v) } }
                    }
                    Column { // GR meter
                        id: gr
                        width: 22
                        spacing: 2
                        Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "GR"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
                        Rectangle {
                            objectName: "grMeter"
                            width: 10
                            anchors.horizontalCenter: parent.horizontalCenter
                            height: 96
                            color: Theme.meterWell
                            border.color: Theme.border
                            border.width: Theme.hairline
                            Rectangle {
                                anchors.top: parent.top
                                width: parent.width
                                height: parent.height * Math.min(1, root.grDb / 24)
                                color: Theme.warning
                            }
                        }
                        Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "-" + root.grDb.toFixed(0); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textSecondary }
                    }
                }
                Toggle { label: qsTr("Gate"); tip: qsTr("Switch the gate on to silence anything below its threshold"); checked: root.enabledOf("gate"); onToggled: root.setEnabled("gate", checked) }
                EffectSlider { visible: root.enabledOf("gate"); width: parent.width; label: qsTr("Gate thr."); tip: qsTr("Level below which the gate closes and mutes the signal"); from: -80; to: 0; stepSize: 1; decimals: 0; suffix: " dB"; value: root.param("gate", "threshold"); onEdited: function (v) { root.setParam("gate", "threshold", v) } }
                Toggle { label: qsTr("Limiter"); tip: qsTr("Switch the limiter on to hold peaks under the ceiling"); checked: root.enabledOf("limiter"); onToggled: root.setEnabled("limiter", checked) }
                EffectSlider { visible: root.enabledOf("limiter"); width: parent.width; label: qsTr("Ceiling"); tip: qsTr("Highest sample peak the limiter will let through; check true peaks in Loudness"); from: -12; to: 0; stepSize: 0.1; suffix: " dBFS"; value: root.param("limiter", "ceiling"); onEdited: function (v) { root.setParam("limiter", "ceiling", v) } }
            }

            // De-Esser
            Edit.InspectorSection {
                objectName: "deesserSection"
                width: parent.width
                title: qsTr("De-Esser")
                expanded: !root.isMaster
                visible: !root.isMaster
                modified: root.entry("deesser") !== null
                onReset: root.resetSection("deesser")
                Toggle { anchors.right: parent.right; tip: qsTr("Switch the de-esser on to tame harsh sibilance"); checked: root.enabledOf("deesser"); onToggled: root.setEnabled("deesser", checked) }
                Column {
                    width: parent.width
                    spacing: 2
                    opacity: root.enabledOf("deesser") ? 1 : Theme.disabledOpacity
                    EffectSlider { width: parent.width; label: qsTr("Tuning"); tip: qsTr("Sibilance filter tuning; higher values retain more lower frequencies"); from: 0; to: 100; stepSize: 1; decimals: 0; suffix: " %"; value: root.param("deesser", "frequency") / 240; onEdited: function (v) { root.setParam("deesser", "frequency", v * 240) } }
                    EffectSlider { width: parent.width; label: qsTr("Intensity"); tip: qsTr("How strongly sibilance is pulled down when found"); from: 0; to: 100; stepSize: 1; decimals: 0; suffix: " %"; value: root.param("deesser", "intensity") * 100; onEdited: function (v) { root.setParam("deesser", "intensity", v / 100) } }
                    EffectSlider { width: parent.width; label: qsTr("Max cut"); tip: qsTr("Cap on how much the de-esser is allowed to cut"); from: 0; to: 48; stepSize: 1; decimals: 0; suffix: " dB"; value: (1 - root.param("deesser", "max")) * 48; onEdited: function (v) { root.setParam("deesser", "max", 1 - v / 48) } }
                }
            }

            // Noise Reduction
            Edit.InspectorSection {
                objectName: "nrSection"
                width: parent.width
                title: qsTr("Noise Reduction")
                expanded: !root.isMaster
                visible: !root.isMaster
                modified: root.entry("nr") !== null
                onReset: root.resetSection("nr")
                Toggle { anchors.right: parent.right; tip: qsTr("Switch noise reduction on to pull down the noise floor"); checked: root.enabledOf("nr"); onToggled: root.setEnabled("nr", checked) }
                Column {
                    width: parent.width
                    spacing: 2
                    opacity: root.enabledOf("nr") ? 1 : Theme.disabledOpacity
                    EffectSlider { width: parent.width; label: qsTr("Reduction"); tip: qsTr("How far the noise under the floor is attenuated"); from: 0; to: 40; stepSize: 0.5; suffix: " dB"; origin: 0; value: root.param("nr", "reduction"); onEdited: function (v) { root.setParam("nr", "reduction", v) } }
                    EffectSlider { width: parent.width; label: qsTr("Sensitivity"); tip: qsTr("Level treated as the noise floor, drawn on the spectrum"); from: -80; to: -20; stepSize: 1; decimals: 0; suffix: " dB"; value: root.param("nr", "floor"); onEdited: function (v) { root.setParam("nr", "floor", v) } }
                }
                SpectrumView {
                    width: parent.width
                    bins: root.spectrum
                    floorDb: root.enabledOf("nr") ? root.param("nr", "floor") : -100
                }
            }

            // Loudness (master readings, always the programme)
            Edit.InspectorSection {
                objectName: "loudnessSection"
                width: parent.width
                title: qsTr("Loudness")
                expanded: true
                resettable: false
                component LoudRow: Row {
                    property string label: ""
                    property string value: ""
                    width: parent.width
                    height: Theme.hRow
                    Text { width: parent.width - 80; anchors.verticalCenter: parent.verticalCenter; text: parent.label; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                    Rectangle {
                        width: 80; height: Theme.hControl - 4; radius: Theme.rControl; anchors.verticalCenter: parent.verticalCenter
                        color: Theme.controlBg; border.color: Theme.border; border.width: Theme.hairline
                        Text { anchors.centerIn: parent; text: parent.parent.value; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary }
                    }
                }
                LoudRow { label: qsTr("Integrated"); value: root.loudText("integrated", "LUFS") }
                LoudRow { label: qsTr("Short Term"); value: root.loudText("shortTerm", "LUFS") }
                LoudRow { label: qsTr("True Peak"); value: root.loudText("truePeak", "dBTP") }
                LoudRow { label: qsTr("LRA"); value: root.loudText("range", "LU") }
            }
        }
    }
}
