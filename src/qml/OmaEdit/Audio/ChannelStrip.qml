import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// One mixer strip: name plate, input (or output for the master), inserts,
// sends, pan, M / S / arm / automation, the long fader beside its meter,
// the peak readout and the plate again at the foot. Buses and the master
// use the same strip with fewer rows.
Rectangle {
    id: root

    property var strip: ({})           // {index, name, role, mix, kind: "track"|"bus"|"master", muted, solo, locked}
    property var levels: ({})          // {in, post, out}
    property var catalogue: []
    property var inputs: []            // [{id, name}]
    property var outputs: []
    property var buses: []             // [{id, name}]
    property bool compact: false
    property bool selected: false
    property bool showAutomationMode: true

    signal selectedRequested()
    signal mixEdited(string key, var value)
    signal mixFinished(string key)
    signal mixStarted(string key)
    signal flagToggled(string flag, bool on)
    signal insertOpened(int fxIndex)
    signal insertBypassToggled(int fxIndex, bool bypass)
    signal insertAddRequested()

    readonly property var mix: strip.mix !== undefined ? strip.mix : ({})
    readonly property string kind: strip.kind !== undefined ? strip.kind : "track"
    readonly property string role: kind === "master" ? "master" : kind === "bus" ? "bus" : (mix.role !== undefined && mix.role !== "" ? mix.role : "dialogue")
    readonly property color tint: mix.color !== undefined && mix.color !== "" ? Theme.labelColor(mix.color) : Theme.roleColor(role)
    readonly property real volume: levels.volume !== undefined && mix.automation !== "off" ? levels.volume : mix.volume !== undefined ? mix.volume : 0
    readonly property real pan: levels.pan !== undefined && mix.automation !== "off" ? levels.pan : mix.pan !== undefined ? mix.pan : 0
    readonly property var fx: mix.fx !== undefined ? mix.fx : []
    readonly property var sends: mix.sends !== undefined ? mix.sends : []
    readonly property real outL: levels.out !== undefined ? levels.out[0] : -100
    readonly property real outR: levels.out !== undefined ? levels.out[1] : -100

    implicitWidth: compact ? Theme.wMixerStripCompact : Theme.wMixerStrip
    color: selected ? Theme.controlBg : Theme.panelBg
    border.color: selected ? Theme.accent : Theme.border
    border.width: Theme.hairline

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Channel strip %1").arg(strip.name !== undefined ? strip.name : "")

    TapHandler { onTapped: root.selectedRequested() }

    component Plate: Rectangle {
        property bool foot: false
        width: root.width
        height: Theme.hControl + 2
        color: root.tint
        Text {
            anchors.centerIn: parent
            width: parent.width - 4
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.kind === "master" ? qsTr("MASTER") : root.kind === "bus" ? String(root.strip.name).toUpperCase()
                : root.compact ? String(root.strip.name) : Theme.roleShort(root.role)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
    }
    component Caption: Text {
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel - 1
        color: Theme.textMuted
    }
    component MiniCombo: Rectangle {
        property string value: ""
        property var options: []       // [{id, name}]
        property string key: ""
        activeFocusOnTab: true
        Accessible.role: Accessible.ComboBox
        Accessible.name: key === "sendBus" ? qsTr("Send destination") : key === "input" ? qsTr("Input") : qsTr("Output")
        Accessible.description: value
        Keys.onSpacePressed: comboMenu.popup()
        Keys.onReturnPressed: comboMenu.popup()
        Keys.onEnterPressed: comboMenu.popup()
        width: root.width - Theme.s2
        height: Theme.hControl - 4
        radius: Theme.rControl
        color: comboHover.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
        border.color: Theme.border
        border.width: Theme.hairline
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 4
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: {
                for (var i = 0; i < parent.options.length; ++i) if (parent.options[i].id === parent.value) return parent.options[i].name
                return parent.value !== "" ? parent.value : (parent.options.length > 0 ? parent.options[0].name : "—")
            }
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel - 1
            color: Theme.textPrimary
        }
        Icon { anchors.right: parent.right; anchors.rightMargin: 2; anchors.verticalCenter: parent.verticalCenter; name: "chevron-down"; size: 10 }
        HoverHandler { id: comboHover }
        TapHandler { onTapped: comboMenu.popup() }
        ContextMenu {
            id: comboMenu
            objectName: "stripComboMenu"
            // The popup's `parent` is the combo it is declared in.
            Repeater {
                model: comboMenu.parent.options
                delegate: MenuAction {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: modelData.id === comboMenu.parent.value
                    onTriggered: root.mixEdited(comboMenu.parent.key, modelData.id)
                }
            }
            C.MenuSeparator { visible: comboMenu.parent.key === "sendBus" }
            MenuAction {
                visible: comboMenu.parent.key === "sendBus"
                text: qsTr("Pre-fader")
                checkable: true
                checked: root.sends.length > 0 && root.sends[0].pre === true
                onTriggered: root.mixEdited("sendPre", !(root.sends.length > 0 && root.sends[0].pre === true))
            }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.hairline
        spacing: 0

        Plate {}

        // Routing.
        Column {
            visible: !root.compact
            width: root.width
            padding: Theme.s1
            spacing: 2
            Caption { text: root.kind === "master" ? qsTr("Output") : qsTr("Input") }
            MiniCombo {
                objectName: "stripInput"
                key: root.kind === "master" ? "output" : "input"
                value: root.kind === "master" ? (root.mix.output !== undefined ? root.mix.output : "") : (root.mix.input !== undefined ? root.mix.input : "")
                options: root.kind === "master" ? root.outputs : root.kind === "bus" ? [{ id: "", name: qsTr("Tracks") }] : [{ id: "", name: qsTr("None") }].concat(root.inputs)
            }
        }

        // Inserts remain available in Effects when the mixer is short.
        Column {
            visible: !root.compact && root.height >= 400
            width: root.width
            padding: Theme.s1
            spacing: 2
            Caption { text: qsTr("Insert") }
            InsertList {
                width: root.width - Theme.s2
                fx: root.fx
                catalogue: root.catalogue
                maxRows: 3
                onOpened: function (i) { root.insertOpened(i) }
                onBypassToggled: function (i, b) { root.insertBypassToggled(i, b) }
                onAddRequested: root.insertAddRequested()
            }
        }

        // Sends (tracks only).
        Column {
            visible: !root.compact && root.height >= 400 && root.kind === "track"
            width: root.width
            padding: Theme.s1
            spacing: 2
            Caption { text: qsTr("Sends") }
            Row {
                spacing: 2
                MiniCombo {
                    width: root.width - Theme.s2 - Theme.szKnobSmall - 4
                    key: "sendBus"
                    value: root.sends.length > 0 && root.sends[0].bus !== undefined ? root.sends[0].bus : ""
                    options: [{ id: "", name: qsTr("None") }].concat(root.buses)
                }
                Knob {
                    objectName: "stripSend"
                    size: Theme.szKnobSmall
                    from: -60; to: 6; origin: -60
                    step: 0.5
                    bipolar: false
                    decimals: 0
                    value: root.sends.length > 0 && root.sends[0].level !== undefined ? root.sends[0].level : -60
                    enabled: root.sends.length > 0 && root.sends[0].bus !== undefined && root.sends[0].bus !== ""
                    onEdited: function (v) { root.mixEdited("sendLevel", v) }
                    onEditingStarted: root.mixStarted("sends")
                    onEditingFinished: root.mixFinished("sends")
                }
            }
        }

        // Pan.
        Item {
            width: root.width
            height: Theme.szKnob + Theme.fsLabel * 2 + Theme.s2
            Caption { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("Pan"); visible: !root.compact }
            Knob {
                objectName: "stripPan"
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                size: root.compact ? Theme.szKnobSmall : Theme.szKnob
                from: -1; to: 1; origin: 0
                pan: true
                value: root.pan
                onEdited: function (v) { root.mixEdited("pan", v) }
                onEditingFinished: root.mixFinished("pan")
                onEditingStarted: root.mixStarted("pan")
            }
        }

        // M S ● A
        Grid {
            anchors.horizontalCenter: parent.horizontalCenter
            columns: root.compact ? 1 : 2
            spacing: 2
            component Btn: Rectangle {
                property string label: ""
                property bool on: false
                property color onColor: Theme.accent
                property string tip: ""
                signal toggled()
                activeFocusOnTab: true
                Keys.onSpacePressed: toggled()
                Keys.onReturnPressed: toggled()
                Accessible.onPressAction: toggled()
                C.ToolTip.text: tip
                C.ToolTip.visible: btnHover.hovered
                C.ToolTip.delay: 450
                width: root.compact ? root.width - 6 : (root.width - 6) / 2
                height: Theme.hControl - 6
                radius: Theme.rControl
                color: on ? onColor : btnHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
                border.color: Theme.border
                border.width: Theme.hairline
                Accessible.role: Accessible.CheckBox
                Accessible.name: tip
                Accessible.checked: on
                Text { anchors.centerIn: parent; text: parent.label; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading; color: parent.on ? Theme.accentText : Theme.textSecondary }
                HoverHandler { id: btnHover }
                TapHandler { onTapped: parent.toggled() }
            }
            Btn { objectName: "stripMute"; label: "M"; on: root.strip.muted === true; onColor: Theme.warning; tip: qsTr("Mute"); onToggled: root.flagToggled("muted", !on) }
            Btn { label: "S"; on: root.strip.solo === true; onColor: Theme.audio; tip: qsTr("Solo"); visible: root.kind !== "master"; onToggled: root.flagToggled("solo", !on) }
            Btn { label: "●"; on: root.mix.armed === true; onColor: Theme.recordArm; tip: qsTr("Record arm"); visible: root.kind === "track"; onToggled: root.mixEdited("armed", !on) }
            Btn {
                label: root.mix.automation !== undefined && root.mix.automation !== "" ? String(root.mix.automation).charAt(0).toUpperCase() : "R"
                on: root.mix.automation !== undefined && root.mix.automation !== "" && root.mix.automation !== "read"
                tip: qsTr("Automation mode (read / touch / latch / write)")
                visible: root.kind === "track" && root.showAutomationMode
                onToggled: {
                    var modes = ["read", "touch", "latch", "write"]
                    var cur = root.mix.automation !== undefined && root.mix.automation !== "" ? modes.indexOf(root.mix.automation) : 0
                    root.mixEdited("automation", modes[(Math.max(0, cur) + 1) % modes.length])
                }
            }
        }
        Item { width: 1; height: Theme.s1 }

        // Fader + meter.
        Item {
            id: faderArea
            width: root.width
            height: parent.height - y - foot.height - peak.height - Theme.s1
            Fader {
                id: fader
                objectName: "stripFader"
                anchors.left: parent.left
                anchors.leftMargin: root.compact ? 0 : Theme.s1
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                compact: root.compact
                showScale: !root.compact
                width: root.compact ? Theme.wFaderCap + Theme.s1 : Theme.wFaderCap + Theme.s5
                value: root.volume
                tint: root.tint
                onEdited: function (v) { root.mixEdited("volume", v) }
                onEditingFinished: root.mixFinished("volume")
                onEditingStarted: root.mixStarted("volume")
            }
            VMeter {
                objectName: "stripMeter"
                anchors.left: fader.right
                anchors.leftMargin: root.compact ? 2 : Theme.s1
                anchors.top: parent.top
                anchors.topMargin: Theme.hFaderCap / 2 - 7
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.hFaderCap / 2 + Theme.fsLabel + Theme.s2
                compact: root.compact
                rmsLeft: root.levels.rms ? root.levels.rms[0] : -100
                rmsRight: root.levels.rms ? root.levels.rms[1] : -100
                levelLeft: root.outL
                levelRight: root.outR
                showScale: false
            }
        }
        Text {
            id: peak
            objectName: "stripPeak"
            width: root.width
            horizontalAlignment: Text.AlignHCenter
            text: {
                var p = Math.max(root.outL, root.outR)
                return p <= -99 ? "−∞" : p.toFixed(1)
            }
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Math.max(root.outL, root.outR) > -0.5 ? Theme.meterHigh : Theme.textSecondary
        }
        Plate { id: foot; foot: true }
    }
}
