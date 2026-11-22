import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.ModalDialog {
    id: root
    objectName: "audioTimingDialog"
    title: qsTr("Sound timing")
    width: Math.min(580, parent ? parent.width - Ui.Theme.s5 * 2 : 580)
    standardButtons: C.Dialog.NoButton
    closePolicy: pending ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property var editor: null
    property var target: ({})
    property string units: "samples"
    property int sampleRate: 48000
    property int pending: 0
    property string error: ""
    property string problem: ""
    readonly property var edits: ({start: positionField.text, in: sourceField.text, duration: durationField.text})
    readonly property var previewData: editor && target.entry
        ? editor.previewAudioTiming(target, edits, units, sampleRate) : ({})

    function setFields(fields) {
        positionField.text = fields.start || ""
        sourceField.text = fields.in || ""
        durationField.text = fields.duration || ""
    }
    function refreshProblem() {
        if (!visible || pending || !editor) return
        problem = editor.audioEditProblem(target)
        if (!problem && sampleRate !== Number(editor.projectSettings.sampleRate || 48000))
            problem = qsTr("The project sample rate changed. Reopen the sound timing controls.")
    }
    function openFor(track, index) {
        if (!editor || pending) return
        target = editor.audioEditTarget(track, index)
        sampleRate = Number(editor.projectSettings.sampleRate || 48000)
        error = ""; problem = editor.audioEditProblem(target)
        const initial = editor.previewAudioTiming(target, {}, units, sampleRate)
        setFields(initial.fields || {})
        unitPicker.currentIndex = units === "milliseconds" ? 1 : 0
        open()
    }
    function changeUnits(index) {
        const next = index === 1 ? "milliseconds" : "samples"
        if (next === units) return
        const preview = editor.previewAudioTiming(target, edits, units, sampleRate)
        if (!preview.valid) {
            error = preview.error || qsTr("Enter valid timing before changing units.")
            unitPicker.currentIndex = units === "milliseconds" ? 1 : 0
            return
        }
        const fields = editor.audioTimingFields(preview.timing, next, sampleRate)
        units = next; setFields(fields); error = ""
    }
    function submit() {
        refreshProblem()
        if (!editor || pending || problem) return
        const preview = editor.previewAudioTiming(target, edits, units, sampleRate)
        if (!preview.valid || !preview.changed) return
        error = ""; pending = editor.setAudioTiming(target, preview.timing)
    }
    onOpened: { positionField.forceActiveFocus(); positionField.selectAll() }
    onClosed: if (!pending) target = ({})
    Connections {
        target: root.editor
        function onTimelineEntriesChanged() { Qt.callLater(root.refreshProblem) }
        function onTracksChanged() { Qt.callLater(root.refreshProblem) }
        function onSequencesChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectSettingsChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectBusyChanged() { Qt.callLater(root.refreshProblem) }
        function onExportingChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectPlaybackReset() { root.pending = 0; root.close(); root.target = ({}) }
        function onAudioEditFinished(request, changed, message) {
            if (request !== root.pending) return
            root.pending = 0
            if (message) { root.error = message; root.refreshProblem() }
            else root.close()
        }
    }
    component Caption: C.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: Ui.Theme.textSecondary
        font.family: Ui.Theme.fontFamily
        font.pixelSize: Ui.Theme.fsLabel
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s3
        Caption { text: root.target.entry ? root.target.entry.name || root.target.entry.path || qsTr("Sound clip") : qsTr("Select a sound clip"); color: Ui.Theme.textPrimary }
        GridLayout {
            columns: 2
            Layout.fillWidth: true
            rowSpacing: Ui.Theme.s2; columnSpacing: Ui.Theme.s3
            enabled: !root.pending && !root.problem
            Caption { Layout.fillWidth: false; text: qsTr("Units") }
            Ui.ComboField {
                id: unitPicker; objectName: "audioTimingUnits"
                Layout.fillWidth: true
                model: [qsTr("Samples"), qsTr("Milliseconds")]
                Accessible.name: qsTr("Sound timing units")
                tip: qsTr("Change the displayed units without moving or trimming the clip")
                onActivated: function(index) { root.changeUnits(index) }
            }
            Caption { Layout.fillWidth: false; text: qsTr("Position") }
            Ui.LineEdit {
                id: positionField; objectName: "audioTimingPosition"
                Layout.fillWidth: true; font.family: Ui.Theme.monoFamily
                Accessible.name: qsTr("Sound clip position")
                tip: qsTr("Where the sound starts on the timeline")
                onAccepted: root.submit()
                onTextEdited: root.error = ""
            }
            Caption { Layout.fillWidth: false; text: qsTr("Source in") }
            Ui.LineEdit {
                id: sourceField; objectName: "audioTimingSourceIn"
                Layout.fillWidth: true; font.family: Ui.Theme.monoFamily
                Accessible.name: qsTr("Sound source in")
                tip: qsTr("Start of the selected source sound, following its playback direction")
                onAccepted: root.submit()
                onTextEdited: root.error = ""
            }
            Caption { Layout.fillWidth: false; text: qsTr("Duration") }
            Ui.LineEdit {
                id: durationField; objectName: "audioTimingDuration"
                Layout.fillWidth: true; font.family: Ui.Theme.monoFamily
                Accessible.name: qsTr("Sound clip duration")
                tip: qsTr("Length of the selected sound")
                onAccepted: root.submit()
                onTextEdited: root.error = ""
            }
        }
        Caption { text: qsTr("Sample grid: %1 Hz. Whole numbers, decimals and fractions are accepted.").arg(root.sampleRate) }
        Caption { text: qsTr("Edits change this sound clip independently. Linked picture and later clips keep their positions.") }
        Caption {
            objectName: "audioTimingValidation"
            visible: text !== ""; color: Ui.Theme.warning
            text: root.problem || root.error || (root.previewData.valid ? "" : root.previewData.error || "")
        }
        Caption { text: root.pending ? qsTr("Updating sound…") : qsTr("Apply creates one undo step. Cancel leaves the clip unchanged.") }
    }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            Ui.ToolButton { objectName: "audioTimingCancel"; text: qsTr("Cancel"); enabled: !root.pending; onClicked: root.close() }
            Ui.ToolButton { objectName: "audioTimingApply"; text: qsTr("Apply"); enabled: !root.pending && !root.problem && root.previewData.valid === true && root.previewData.changed === true; onClicked: root.submit() }
        }
    }
}
