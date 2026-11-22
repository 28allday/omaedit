import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The control block beside an audio track: role colour, badge (double-
// click renames), M / S / arm / monitor, a mini fader with its dB, and a
// context menu for role, colour, order and removal.
Rectangle {
    id: root

    property var track: ({})            // a tracks[] entry (with mix)
    property bool selected: false
    property bool current: false        // the record-armed one
    property var levels: ({})           // {in, post, out}
    property bool showFader: height >= Theme.hAudioTrack

    signal selectedRequested()
    signal flagToggled(string flag, bool on)
    signal mixEdited(string key, var value)
    signal mixStarted(string key)
    signal mixFinished(string key)
    signal renamed(string name)
    signal removeRequested()
    signal moveRequested(bool up)
    signal addBelowRequested()
    signal contextRequested()

    readonly property var mix: track.mix !== undefined ? track.mix : ({})
    readonly property string role: mix.role !== undefined && mix.role !== "" ? mix.role : "dialogue"
    readonly property color tint: mix.color !== undefined && mix.color !== "" ? Theme.labelColor(mix.color) : Theme.roleColor(role)
    readonly property real volume: mix.volume !== undefined ? mix.volume : 0
    readonly property bool armed: mix.armed === true
    readonly property bool monitor: mix.monitor === true

    color: selected ? Theme.controlBg : Theme.panelBg
    implicitWidth: Theme.wAudioTrackControl

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Track %1").arg(track.name !== undefined ? track.name : "")

    Rectangle { anchors.left: parent.left; width: 3; height: parent.height; color: root.tint }
    Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: Theme.border }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }

    TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.selectedRequested() }
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: { root.selectedRequested(); menu.popup() } }

    Row {
        id: headerRow
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2 + 3
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.top: parent.top
        anchors.topMargin: Theme.s1
        spacing: 3
        height: Theme.hControl - 2

        Rectangle { // badge
            width: 30
            height: parent.height
            radius: Theme.rControl
            color: Theme.panelRaised
            border.color: root.selected ? Theme.accent : Theme.border
            border.width: Theme.hairline
            Text {
                anchors.centerIn: parent
                visible: !nameEditor.visible
                text: root.track.name !== undefined ? root.track.name : ""
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.weight: Theme.wHeading
                color: Theme.textPrimary
                elide: Text.ElideRight
                width: parent.width - 4
                horizontalAlignment: Text.AlignHCenter
            }
            C.TextField {
                id: nameEditor
                anchors.fill: parent
                visible: false
                C.ToolTip.text: qsTr("Type a new name for this track, Enter to accept"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450
                hoverEnabled: true
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                padding: 2
                onAccepted: { root.renamed(text); visible = false }
                onActiveFocusChanged: if (!activeFocus) visible = false
                Keys.onEscapePressed: visible = false
            }
            TapHandler {
                onDoubleTapped: { nameEditor.text = root.track.name; nameEditor.visible = true; nameEditor.forceActiveFocus(); nameEditor.selectAll() }
            }
        }
        component Flag: Rectangle {
            id: flag
            property string label: ""
            property bool on: false
            property color onColor: Theme.accent
            property string tip: ""
            signal toggled()
            width: 22
            height: headerRow.height
            radius: Theme.rControl
            color: on ? onColor : flagHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
            border.color: Theme.border
            border.width: Theme.hairline
            Accessible.role: Accessible.CheckBox
            Accessible.name: tip
            Accessible.checked: on
            Text {
                anchors.centerIn: parent
                text: flag.label
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                font.weight: Theme.wHeading
                color: flag.on ? Theme.accentText : Theme.textSecondary
            }
            HoverHandler { id: flagHover }
            TapHandler { onTapped: flag.toggled() }
        }
        Flag { objectName: "trackMute"; label: "M"; on: root.track.muted === true; onColor: Theme.warning; tip: qsTr("Mute"); onToggled: root.flagToggled("muted", !on) }
        Flag { objectName: "trackSolo"; label: "S"; on: root.track.solo === true; onColor: Theme.audio; tip: qsTr("Solo"); onToggled: root.flagToggled("solo", !on) }
        Rectangle { // arm
            objectName: "trackArm"
            width: 22
            height: headerRow.height
            radius: Theme.rControl
            color: root.armed ? Theme.recordArm : armHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
            border.color: Theme.border
            border.width: Theme.hairline
            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Record arm")
            Accessible.checked: root.armed
            Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; color: root.armed ? Theme.textPrimary : Theme.recordArm }
            HoverHandler { id: armHover }
            TapHandler { onTapped: root.mixEdited("armed", !root.armed) }
        }
        Rectangle { // input monitor
            width: 22
            height: headerRow.height
            radius: Theme.rControl
            color: root.monitor ? Theme.accent : monHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
            border.color: Theme.border
            border.width: Theme.hairline
            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Input monitoring")
            Accessible.checked: root.monitor
            Icon { anchors.centerIn: parent; name: "headphones"; size: 12; color: root.monitor ? Theme.accentText : Theme.textSecondary }
            HoverHandler { id: monHover }
            TapHandler { onTapped: root.mixEdited("monitor", !root.monitor) }
        }
        Rectangle { // role short label
            width: headerRow.width - 30 - 22 * 4 - 3 * 5
            height: headerRow.height
            radius: Theme.rControl
            color: "transparent"
            visible: width > 20
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: Theme.roleShort(root.role)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel - 1
                color: root.tint
            }
        }
    }

    // Mini fader: a horizontal slot from −60 to +12 with the level underneath.
    Item {
        id: mini
        visible: root.showFader
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2 + 3
        anchors.right: db.left
        anchors.rightMargin: Theme.s1
        anchors.top: headerRow.bottom
        anchors.topMargin: Theme.s2
        height: 12
        function posFor(v) { v = Math.max(-100, Math.min(12, v)); return v >= 0 ? 0.68 + v / 12 * 0.32 : v >= -20 ? 0.32 + (v + 20) / 20 * 0.36 : (v + 100) / 80 * 0.32 }
        function dbFor(p) { p = Math.max(0, Math.min(1, p)); return p >= 0.68 ? (p - 0.68) / 0.32 * 12 : p >= 0.32 ? -20 + (p - 0.32) / 0.36 * 20 : -100 + p / 0.32 * 80 }
        Rectangle { // slot with output level inside
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.meterWell
            border.color: Theme.border
            border.width: Theme.hairline
            Rectangle {
                readonly property real lv: root.levels.out !== undefined ? Math.max(root.levels.out[0], root.levels.out[1]) : -100
                width: Math.max(0, parent.width * Math.max(0, Math.min(1, (lv + 60) / 60)))
                height: parent.height
                radius: 2
                color: lv > -6 ? Theme.meterHigh : lv > -18 ? Theme.meterMid : Theme.meterLow
            }
        }
        Rectangle { // unity
            x: parent.width * mini.posFor(0) - 0.5
            width: 1; height: parent.height; color: Theme.borderStrong
        }
        Rectangle { // cap
            x: parent.width * mini.posFor(root.volume) - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 12; radius: 2
            color: faderDrag.active ? Theme.accent : Theme.textPrimary
        }
        DragHandler {
            id: faderDrag
            target: null
            property real start: 0
            onActiveChanged: { if (active) { start = mini.posFor(root.volume); root.mixStarted("volume") } else root.mixFinished("volume") }
            onTranslationChanged: if (active) root.mixEdited("volume", Math.round(mini.dbFor(start + translation.x / mini.width) * 10) / 10)
        }
        TapHandler { onDoubleTapped: { root.mixEdited("volume", 0); root.mixFinished("volume") } }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (e) { root.mixEdited("volume", Math.max(-100, Math.min(12, root.volume + (e.angleDelta.y > 0 ? 0.5 : -0.5)))); root.mixFinished("volume") }
        }
    }
    Text {
        id: db
        objectName: "trackDb"
        visible: root.showFader
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: mini.verticalCenter
        width: 38
        horizontalAlignment: Text.AlignRight
        text: root.volume <= -100 ? "−∞" : (root.volume > 0 ? "+" : "") + root.volume.toFixed(1)
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }
    // Input level while armed (monitoring): a thin red bar under the fader.
    Rectangle {
        visible: root.armed && root.showFader && root.height >= Theme.hAudioTrack + 10
        anchors.left: mini.left
        anchors.right: mini.right
        anchors.top: mini.bottom
        anchors.topMargin: Theme.s1
        height: 3
        color: Theme.meterWell
        Rectangle {
            readonly property real lv: root.levels.input !== undefined ? root.levels.input : -100
            width: parent.width * Math.max(0, Math.min(1, (lv + 60) / 60))
            height: parent.height
            color: Theme.recordArm
        }
    }

    ContextMenu {
        id: menu
        C.Menu {
            title: qsTr("Role")
            Repeater {
                model: Theme.roleNames
                delegate: MenuAction {
                    required property string modelData
                    text: Theme.roleTitle(modelData)
                    checkable: true
                    checked: root.role === modelData
                    onTriggered: root.mixEdited("role", modelData)
                }
            }
        }
        C.Menu {
            title: qsTr("Colour")
            Repeater {
                model: Theme.labelNames
                delegate: MenuAction {
                    required property string modelData
                    text: Theme.labelTitle(modelData)
                    checkable: true
                    checked: root.mix.color === modelData
                    onTriggered: root.mixEdited("color", modelData)
                }
            }
            C.MenuSeparator {}
            MenuAction { text: qsTr("Role colour"); onTriggered: root.mixEdited("color", "") }
        }
        C.Menu {
            title: qsTr("Output")
            MenuAction { text: qsTr("Master"); checkable: true; checked: root.mix.output === undefined || root.mix.output === "" || root.mix.output === "master"; onTriggered: root.mixEdited("output", "master") }
            MenuAction { text: qsTr("Bus 1"); checkable: true; checked: root.mix.output === "bus1"; onTriggered: root.mixEdited("output", "bus1") }
        }
        MenuAction { text: qsTr("Rename…"); iconName: "pencil"; onTriggered: { nameEditor.text = root.track.name; nameEditor.visible = true; nameEditor.forceActiveFocus(); nameEditor.selectAll() } }
        C.MenuSeparator {}
        MenuAction { text: qsTr("Add Track Below"); iconName: "plus"; onTriggered: root.addBelowRequested() }
        MenuAction { text: qsTr("Move Up"); iconName: "chevron-up"; onTriggered: root.moveRequested(true) }
        MenuAction { text: qsTr("Move Down"); iconName: "chevron-down"; onTriggered: root.moveRequested(false) }
        MenuAction { text: root.track.locked === true ? qsTr("Unlock Track") : qsTr("Lock Track"); iconName: root.track.locked === true ? "unlock" : "lock"; onTriggered: root.flagToggled("locked", !(root.track.locked === true)) }
        C.MenuSeparator {}
        MenuAction { text: qsTr("Remove Track"); iconName: "trash-2"; enabled: root.track.clips === 0; onTriggered: root.removeRequested() }
    }
}
