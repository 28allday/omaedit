import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui
import "../Ui/MediaTime.js" as MediaTime

Ui.ModalDialog {
    id: root
    objectName: "speedDurationDialog"
    title: qsTr("Speed / Duration")
    width: Math.min(640,parent ? parent.width-Ui.Theme.s5*2 : 640)
    standardButtons: C.Dialog.NoButton
    closePolicy: pending ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property var target: ({})
    property bool durationMode: false
    property int pending: 0
    property string error: ""
    property string problem: ""
    readonly property var entry: target.entry || ({})
    readonly property real rate: target.fps > 0 ? target.fps : 25
    readonly property real parsedValue: parseValue(valueField.text)
    readonly property var previewData: backend.previewRetime(target,parsedValue,durationMode,reverse.checked)
    function percent(speed) { return String(Number((Math.abs(speed)*100).toPrecision(8))) }
    function lengthText(frames) { return MediaTime.durationText(frames,rate) }
    function parseValue(text) {
        var value=text.trim()
        if (!durationMode) return /^(\d+(\.\d*)?|\.\d+)$/.test(value) ? Number(value) : -1
        return MediaTime.parseDuration(value,rate)
    }
    function changeMode(index) {
        var previous=previewData
        durationMode=index===1
        valueField.text=durationMode ? MediaTime.clockText(previous.valid ? previous.frames : entry.frames || 1,rate)
                                    : String(Math.abs(previous.valid ? previous.speed : entry.speed || 1)*100)
    }
    function openFor(track,index) {
        target=backend.retimeTarget(track,index)
        if(target.entry && target.entry.retime){speedRampDialog.openFor(track,index);return}
        pending=0; error=""; problem=backend.retimeProblem(target)
        durationMode=false; reverse.checked=(entry.speed || 1)<0
        valueField.text=String(Math.abs(entry.speed || 1)*100)
        open()
    }
    function openForSelection() {
        var selected=win.selection.selectedEntry()
        if (selected && win.retiming.canRetimeSelection()) openFor(selected.track,selected.index)
    }
    function refreshProblem() { if (visible && !pending) problem=backend.retimeProblem(target) }
    function submit() {
        refreshProblem()
        if (pending || problem || !previewData.valid) return
        error=""; pending=backend.applyRetime(target,previewData.speed)
    }
    onOpened: { valueField.forceActiveFocus(); valueField.selectAll() }
    Connections {
        target: backend
        function onTimelineEntriesChanged() { Qt.callLater(root.refreshProblem) }
        function onTracksChanged() { Qt.callLater(root.refreshProblem) }
        function onSequencesChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectBusyChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectPlaybackReset() { root.close(); root.target=({}) }
        function onRetimeFinished(request,changed,message) {
            if (request!==root.pending) return
            root.pending=0
            if (message) root.error=message
            else root.close()
        }
    }
    component Caption: C.Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        color: Ui.Theme.textSecondary; font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s3
        Caption { objectName: "retimeTargetLabel"; text: (root.entry.name || qsTr("Clip"))+" · "+(root.target.sequenceName || qsTr("Sequence")); color: Ui.Theme.textPrimary }
        Caption { text: qsTr("Current: %1 · %2%").arg(root.lengthText(root.entry.frames || 0)).arg(root.percent(root.entry.speed || 1)) }
        GridLayout {
            columns: 2; Layout.fillWidth: true
            rowSpacing: Ui.Theme.s2; columnSpacing: Ui.Theme.s3
            enabled: !root.pending && !root.problem
            Caption { Layout.fillWidth: false; text: qsTr("Set") }
            Ui.ComboField {
                objectName: "retimeMode"; Layout.fillWidth: true
                tip: qsTr("Whether you type a speed percentage or a new duration")
                Accessible.name: qsTr("Edit speed or duration")
                model: [qsTr("Speed (%)"),qsTr("Duration")]
                currentIndex: root.durationMode ? 1 : 0
                onActivated: function (i) { root.changeMode(i) }
            }
            Caption { Layout.fillWidth: false; text: root.durationMode ? qsTr("Duration") : qsTr("Speed (%)") }
            Ui.LineEdit {
                id: valueField; objectName: "retimeValue"
                Layout.fillWidth: true; font.family: Ui.Theme.monoFamily
                tip: qsTr("New speed or duration; the result is previewed below")
                hint: root.durationMode ? qsTr("Seconds, such as 2.5 or 1:30") : qsTr("Speed percentage")
                onAccepted: root.submit()
            }
            Caption { Layout.fillWidth: false; text: qsTr("Direction") }
            C.CheckBox { id: reverse; objectName: "retimeReverse"; text: qsTr("Reverse"); Accessible.name: qsTr("Reverse playback direction"); C.ToolTip.text: qsTr("Play the clip backwards at the chosen speed"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450 }
        }
        Caption {
            objectName: "retimePreview"
            visible: root.previewData.valid
            text: qsTr("Result: %1 · %2% · %3")
                .arg(root.lengthText(root.previewData.frames || 0))
                .arg(root.percent(root.previewData.speed || 1))
                .arg(reverse.checked ? qsTr("Reverse") : qsTr("Forward"))
            color: Ui.Theme.textPrimary
        }
        Caption {
            visible: root.previewData.valid && root.previewData.delta!==0
            text: ((root.previewData.delta || 0) > 0 ? qsTr("The clip gets %1 longer.") : qsTr("The clip gets %1 shorter.")).arg(root.lengthText(Math.abs(root.previewData.delta || 0)))
                  + " " + qsTr("Later clips on this track move with it; sync-locked, unlocked tracks follow the current ripple rules.")
        }
        Caption { text: qsTr("Audio pitch is preserved. Clip markers and animation keyframes follow the retime. The existing frame-blending mode is retained.") }
        Ui.ToolButton {
            objectName:"openSpeedSegments";text:qsTr("Speed segments…")
            enabled:!root.pending && !root.problem
            tip:qsTr("Create speed ramps, blade speed, replay, rewind or hold sections")
            onClicked:{root.close();speedRampDialog.openFor(root.target.track,root.target.index)}
        }
        Caption { objectName: "retimeValidation"; visible: text!==""; color: Ui.Theme.warning; text: root.problem || root.error || (root.previewData.valid ? "" : root.previewData.error || qsTr("Enter a valid value.")) }
        Caption { text: root.pending ? qsTr("Updating clip…") : qsTr("Changes can be undone in one step. Cancel leaves the clip unchanged.") }
    }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            Ui.ToolButton { objectName: "retimeCancel"; text: qsTr("Cancel"); enabled: !root.pending; onClicked: root.close() }
            Ui.ToolButton { objectName: "retimeApply"; text: qsTr("Apply"); enabled: !root.pending && !root.problem && root.previewData.valid; onClicked: root.submit() }
        }
    }
}
