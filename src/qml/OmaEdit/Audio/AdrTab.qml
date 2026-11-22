pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// ADR: a cue list (character, line, notes, in/out, pre/post-roll, status)
// with take recording, rating, review and promotion of the chosen take.
Rectangle {
    id: root

    property var cues: []            // [{id, character, line, notes, in, out, preRoll, postRoll, track, status, takes:[{path, rating, promoted}]}]
    property var tracks: []
    property real fps: 25
    property int position: 0
    property int rangeIn: -1
    property int rangeOut: -1
    property bool recording: false
    property bool canRecord: false
    property var timecoder: null
    // The timeline's starting timecode in frames: a cue's in and out are
    // typed and shown as timeline timecodes.
    property int startFrame: 0
    property int currentCue: -1

    signal cuesEdited(var cues)
    signal recordTakeRequested(int cueIndex)
    signal stopRequested()
    signal reviewRequested(int cueIndex, int takeIndex)
    signal promoteRequested(int cueIndex, int takeIndex)
    signal compRequested(int cueIndex, int takeIndex)
    signal seekRequested(int frame)

    color: Theme.panelBg
    readonly property bool textActive: character.activeFocus || line.activeFocus || notes.activeFocus

    function tc(f) { if (timecoder) return timecoder(f); var s = Math.floor(f / fps); var pad = function (n) { return (n < 10 ? "0" : "") + n }; return pad(Math.floor(s / 60)) + ":" + pad(s % 60) + ":" + pad(Math.round(f - s * fps)) }
    function copyCues() { return cues.map(function (c) { var o = {}; for (var k in c) o[k] = c[k]; o.takes = (c.takes || []).map(function (t) { var u = {}; for (var j in t) u[j] = t[j]; return u }); return o }) }
    function newId() { return "cue" + Date.now().toString(36) + Math.floor(Math.random() * 1000) }
    function addCue() {
        var c = copyCues()
        var a = rangeIn >= 0 ? rangeIn : position, b = rangeOut >= 0 ? rangeOut : position + Math.round(fps * 3)
        var track = -1
        for (var i = 0; i < tracks.length; ++i) if (tracks[i].kind === "audio" && tracks[i].mix !== undefined && tracks[i].mix.role === "dialogue") { track = tracks[i].index; break }
        c.push({ id: newId(), character: "", line: "", notes: "", in: a, out: b, preRoll: Math.round(fps * 2), postRoll: Math.round(fps), track: track, status: "todo", takes: [] })
        cuesEdited(c)
        currentCue = c.length - 1
    }
    function setField(key, value) { if (currentCue < 0) return; var c = copyCues(); c[currentCue][key] = value; cuesEdited(c) }
    // Picking another cue is a tap, not a focus change: keep what was typed
    // into this cue's fields before they show the next one.
    function selectCue(i) {
        if (currentCue >= 0 && cue) {
            var c = copyCues(), changed = false
            var fields = [["character", character.text], ["line", line.text], ["notes", notes.text]]
            for (var k = 0; k < fields.length; ++k)
                if ((cue[fields[k][0]] || "") !== fields[k][1]) { c[currentCue][fields[k][0]] = fields[k][1]; changed = true }
            if (changed) cuesEdited(c)
        }
        currentCue = i
    }
    function removeCue(i) { var c = copyCues(); c.splice(i, 1); cuesEdited(c); currentCue = Math.min(currentCue, c.length - 1) }
    function rateTake(ci, ti, rating) { var c = copyCues(); c[ci].takes[ti].rating = rating; cuesEdited(c) }
    function statusColor(s) { return s === "approved" ? Theme.audio : s === "recorded" ? Theme.warning : Theme.textMuted }
    readonly property var cue: currentCue >= 0 && currentCue < cues.length ? cues[currentCue] : null

    Column {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s2

        Row {
            width: parent.width
            spacing: Theme.s2
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - addCue.width - Theme.s2
                text: qsTr("ADR Cues · %1").arg(root.cues.length)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsHeading
                font.weight: Theme.wHeading
                color: Theme.textPrimary
            }
            ToolButton { id: addCue; objectName: "adrAddCue"; iconName: "plus"; text: qsTr("Cue"); showLabel: true; onClicked: root.addCue() }
        }

        // Cue list.
        Rectangle {
            width: parent.width
            // Tall enough for the empty-state words, so they stay inside the box.
            height: Math.min(parent.height * 0.42, Math.max(Theme.hRow * 3, cueList.contentHeight + 2, root.cues.length === 0 ? adrEmpty.implicitHeight + Theme.s3 * 2 : 0))
            color: Theme.windowBg
            border.color: Theme.border
            border.width: Theme.hairline
            ListView {
                id: cueList
                objectName: "adrCueList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.cues
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: cueRow
                    required property var modelData
                    required property int index
                    width: cueList.width
                    height: Theme.hRow + 6
                    color: root.currentCue === index ? Theme.controlBg : cueHover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
                    HoverHandler { id: cueHover }
                    TapHandler { onTapped: root.selectCue(cueRow.index); onDoubleTapped: root.seekRequested(cueRow.modelData.in) }
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s2
                        anchors.rightMargin: Theme.s2
                        spacing: Theme.s2
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 8; height: 8; radius: 4; color: root.statusColor(cueRow.modelData.status) }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 64; elide: Text.ElideRight; text: cueRow.modelData.character !== "" ? cueRow.modelData.character : qsTr("(who?)"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; font.weight: Theme.wHeading; color: Theme.textPrimary }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: parent.width - 8 - 64 - 60 - 40 - Theme.s2 * 5; elide: Text.ElideRight; text: cueRow.modelData.line !== "" ? cueRow.modelData.line : qsTr("(no line)"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; color: Theme.textSecondary }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 60; text: root.tc(cueRow.modelData.in); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 40; horizontalAlignment: Text.AlignRight; text: qsTr("%1 rec").arg((cueRow.modelData.takes || []).length); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    }
                }
            }
            EmptyState {
                id: adrEmpty
                objectName: "adrEmpty"
                anchors.centerIn: parent
                width: parent.width
                visible: root.cues.length === 0
                iconName: "mic"
                title: qsTr("No ADR cues")
                description: qsTr("Mark a range on the timeline and add a cue.")
            }
        }

        // The cue editor + takes.
        Flickable {
            width: parent.width
            height: parent.height - y
            visible: root.cue !== null
            contentHeight: editor.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}
            Column {
                id: editor
                width: parent.width
                spacing: Theme.s2
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    C.TextField { id: character; width: 110; placeholderText: qsTr("Character"); text: root.cue ? root.cue.character : ""; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; C.ToolTip.text: qsTr("Who speaks the line; groups the cue in the list"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; onEditingFinished: root.setField("character", text) }
                    ComboField {
                        width: parent.width - 110 - Theme.s2
                        caption: qsTr("Status")
                        tip: qsTr("How far the cue has got: to do, recorded or approved")
                        model: [qsTr("To do"), qsTr("Recorded"), qsTr("Approved")]
                        currentIndex: root.cue ? ["todo", "recorded", "approved"].indexOf(root.cue.status) : 0
                        onActivated: function (i) { root.setField("status", ["todo", "recorded", "approved"][i]) }
                    }
                }
                C.TextField { id: line; width: parent.width; placeholderText: qsTr("Line"); text: root.cue ? root.cue.line : ""; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; C.ToolTip.text: qsTr("The words to be re-recorded, shown to the talent"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; onEditingFinished: root.setField("line", text) }
                C.TextField { id: notes; width: parent.width; placeholderText: qsTr("Notes for the talent"); text: root.cue ? root.cue.notes : ""; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; C.ToolTip.text: qsTr("Direction for the recording, kept with the cue"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; onEditingFinished: root.setField("notes", text) }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    TimecodeField { label: qsTr("In"); tip: qsTr("Where the line starts; a recording you keep is laid here"); fps: root.fps; origin: root.startFrame; frame: root.cue ? root.cue.in : 0; onEdited: function (f) { root.setField("in", f) } }
                    TimecodeField { label: qsTr("Out"); tip: qsTr("Where the line ends; recording stops after the post-roll"); fps: root.fps; origin: root.startFrame; frame: root.cue ? root.cue.out : 0; onEdited: function (f) { root.setField("out", f) } }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ValueField { label: qsTr("Pre-roll"); tip: qsTr("Run-up rolled before the In point, trimmed off on promote"); fps: root.fps; from: 0; to: Math.round(root.fps * 10); property real modelValue: root.cue ? root.cue.preRoll : 0; value: modelValue; onModelValueChanged: value = modelValue; onEdited: function (v) { root.setField("preRoll", Math.round(v)) } }
                    ValueField { label: qsTr("Post-roll"); tip: qsTr("Kept rolling past the Out point, trimmed off on promote"); fps: root.fps; from: 0; to: Math.round(root.fps * 10); property real modelValue: root.cue ? root.cue.postRoll : 0; value: modelValue; onModelValueChanged: value = modelValue; onEdited: function (v) { root.setField("postRoll", Math.round(v)) } }
                    ComboField {
                        caption: qsTr("Track")
                        tip: qsTr("Audio track the recording is made on and promoted to")
                        width: 110
                        model: root.tracks.filter(function (t) { return t.kind === "audio" }).map(function (t) { return t.name })
                        currentIndex: {
                            var a = root.tracks.filter(function (t) { return t.kind === "audio" })
                            for (var i = 0; i < a.length; ++i) if (root.cue && a[i].index === root.cue.track) return i
                            return 0
                        }
                        onActivated: function (i) { var a = root.tracks.filter(function (t) { return t.kind === "audio" }); if (a[i]) root.setField("track", a[i].index) }
                    }
                }
                Row {
                    width: parent.width
                    spacing: Theme.s2
                    ToolButton {
                        objectName: "adrRecord"
                        iconName: root.recording ? "square" : "circle-dot"
                        text: root.recording ? qsTr("Stop") : qsTr("Record")
                        showLabel: true
                        enabled: root.recording || root.canRecord
                        onClicked: root.recording ? root.stopRequested() : root.recordTakeRequested(root.currentCue)
                    }
                    ToolButton { iconName: "play"; text: qsTr("Cue Up"); showLabel: true; onClicked: root.seekRequested(Math.max(0, (root.cue ? root.cue.in - root.cue.preRoll : 0))) }
                    ToolButton { iconName: "trash-2"; text: qsTr("Delete Cue"); showLabel: true; onClicked: root.removeCue(root.currentCue) }
                }
                Text { text: qsTr("Recordings"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Repeater {
                    model: root.cue ? (root.cue.takes || []).length : 0
                    delegate: Rectangle {
                        id: takeRow
                        required property int index
                        readonly property var take: root.cue.takes[index]
                        width: editor.width
                        height: Theme.hRow + 6
                        radius: Theme.rControl
                        color: take.promoted === true ? Theme.controlBg : "transparent"
                        border.color: take.promoted === true ? Theme.audio : Theme.border
                        border.width: Theme.hairline
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s2
                            spacing: Theme.s1
                            Text { anchors.verticalCenter: parent.verticalCenter; width: 50; text: qsTr("Recording %1").arg(takeRow.index + 1); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; color: Theme.textPrimary }
                            StarRating { anchors.verticalCenter: parent.verticalCenter; tip: qsTr("Rate this recording out of five to mark the keeper"); value: takeRow.take.rating !== undefined ? takeRow.take.rating : 0; onEdited: function (r) { root.rateTake(root.currentCue, takeRow.index, r) } }
                            IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "play"; Accessible.name: qsTr("Review recording"); onClicked: root.reviewRequested(root.currentCue, takeRow.index) }
                            IconButton { iconName: "scissors"; Accessible.name: qsTr("Use this recording for the In/Out selection"); enabled: root.cue && root.rangeIn>=root.cue.in && root.rangeOut<=root.cue.out && root.rangeOut>=root.rangeIn; onClicked: root.compRequested(root.currentCue,takeRow.index) }
                            ToolButton { anchors.verticalCenter: parent.verticalCenter; iconName: "check"; text: takeRow.take.promoted === true ? qsTr("On timeline") : qsTr("Promote"); showLabel: true; enabled: takeRow.take.promoted !== true; onClicked: root.promoteRequested(root.currentCue, takeRow.index) }
                        }
                    }
                }
            }
        }
    }
}
