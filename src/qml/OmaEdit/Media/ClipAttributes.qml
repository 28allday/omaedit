pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// Media ▸ Clip Attributes…: how a source is read. Frame rate interprets the
// recorded frames at another rate (a 25 fps file read at 12.5 plays at half
// speed, every frame twice — the length follows); the start timecode
// overrides the container's for multicam sync and the inspector. Pixel
// aspect and field order follow the file.
Item {
    id: root
    required property var engine
    property var selectedIds: []
    property string message: ""
    readonly property bool textActive: dialog.visible
    readonly property var rates: [23.976, 24, 25, 29.97, 30, 48, 50, 59.94, 60, 100, 120]
    function indexes() {
        var out = []
        engine.library.forEach(function (e, i) { if (root.selectedIds.indexOf(e.libraryId) >= 0) out.push(i) })
        return out
    }
    function fpsLabel(fps) { return (Math.abs(fps - Math.round(fps)) < 0.001 ? Math.round(fps) : fps.toFixed(3)) + " fps" }
    function open(indexes) {
        if (typeof indexes !== "object") indexes = [Number(indexes)] // the grab hook passes one index as text
        if (!indexes.length) return
        selectedIds = indexes.map(function (i) { return root.engine.library[i].libraryId })
        var entry = engine.library[indexes[0]]
        dialog.entry = entry
        var recorded = Number(entry.sourceFps || entry.fps || 0)
        var forced = Number(entry.forceFps || 0)
        dialog.sourceFps = recorded
        dialog.forceFps = forced
        var listed = forced > 0 ? root.rates.findIndex(function (r) { return Math.abs(r - forced) < 0.0005 }) : -1
        rateChoice.currentIndex = forced <= 0 ? 0 : listed >= 0 ? listed + 1 : root.rates.length + 1
        customRate.value = forced > 0 ? forced : recorded
        dialog.sourceTimecode = entry.timecodeFrames !== undefined ? Number(entry.timecodeFrames) : -1
        dialog.startTimecode = entry.startTimecodeFrames !== undefined ? Number(entry.startTimecodeFrames) : dialog.sourceTimecode
        useFileTimecode.checked = entry.startTimecodeFrames === undefined
        dialog.startRate = Math.round(forced > 0 ? forced : recorded)
        keepFootage.checked = false
        message = ""
        startField.clearRefused()
        dialog.open()
    }
    ModalDialog {
        id: dialog
        objectName: "clipAttributesDialog"
        property var entry: ({})
        property real sourceFps: 0
        property real forceFps: 0
        property real sourceTimecode: -1
        property real startTimecode: 0
        // Whole frames a second startTimecode is counted at, so the same
        // timecode stands when another frame rate is chosen here.
        property int startRate: 0
        property int pendingRequest: 0
        readonly property bool applying: pendingRequest > 0
        readonly property real chosenFps: rateChoice.currentIndex === 0 ? 0
                                        : rateChoice.currentIndex <= root.rates.length ? root.rates[rateChoice.currentIndex - 1]
                                        : customRate.value
        readonly property real effectiveFps: chosenFps > 0 ? chosenFps : sourceFps
        readonly property int recordedFrames: Number(entry.sourceDuration !== undefined ? entry.sourceDuration : (entry.duration || 0))
        readonly property int effectiveFrames: sourceFps > 0 && chosenFps > 0 && Math.abs(chosenFps - sourceFps) > 0.0005
                                               ? Math.max(1, Math.round(recordedFrames * sourceFps / chosenFps)) : recordedFrames
        title: root.selectedIds.length === 1 ? qsTr("Clip Attributes · %1").arg(entry.name || "") : qsTr("Clip Attributes · %1 Clips").arg(root.selectedIds.length)
        standardButtons: C.Dialog.NoButton
        closePolicy: applying ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
        function submit() {
            if (applying) return
            // A start typed and not confirmed counts; one that could not be
            // read keeps the box open and says so.
            if (!useFileTimecode.checked && startField.commitHolds()) { root.message = startField.refusedText; return }
            root.message = ""
            pendingRequest = root.engine.applyClipAttributes(root.indexes(), {
                forceFps: Math.abs(chosenFps - sourceFps) < 0.0005 ? 0 : chosenFps,
                startTimecodeFrames: useFileTimecode.checked ? -1 : root.engine.startFrameAtRate(Math.max(0, Math.round(startTimecode)), startRate, effectiveFps)
            }, keepFootage.checked)
        }
        footer: DialogButtons {
            ToolButton {
                objectName: "clipAttributes:cancel"
                text: qsTr("Cancel"); enabled: !dialog.applying
                C.DialogButtonBox.buttonRole: C.DialogButtonBox.RejectRole
            }
            ToolButton {
                objectName: "clipAttributes:apply"
                text: dialog.applying ? qsTr("Applying…") : qsTr("Apply")
                enabled: !dialog.applying
                C.DialogButtonBox.buttonRole: C.DialogButtonBox.ActionRole
                onClicked: dialog.submit()
            }
        }
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            enabled: !dialog.applying
            Text { text: qsTr("Video"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading }
            RowLayout {
                spacing: Theme.s2
                ComboField {
                    id: rateChoice
                    objectName: "clipAttributes:rate"
                    tip: qsTr("Rate the recorded frames are read at, changing speed and length")
                    Layout.preferredWidth: 220
                    caption: qsTr("Frame rate")
                    model: [qsTr("As recorded (%1)").arg(root.fpsLabel(dialog.sourceFps))].concat(root.rates.map(function (r) { return root.fpsLabel(r) })).concat([qsTr("Custom…")])
                }
                ValueField {
                    id: customRate
                    objectName: "clipAttributes:customRate"
                    tip: qsTr("Frame rate to read the file at when it is not in the list")
                    visible: rateChoice.currentIndex === root.rates.length + 1
                    label: qsTr("Custom frame rate"); from: 1; to: 240; decimals: 3; step: 0.1; suffix: "fps"
                    Layout.preferredWidth: 110
                }
            }
            Text {
                objectName: "clipAttributes:consequence"
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: dialog.chosenFps > 0 && Math.abs(dialog.chosenFps - dialog.sourceFps) > 0.0005
                      ? qsTr("The recorded frames play at %1: %2 of recording lasts %3 in the project (%4× speed, every frame shown %5).")
                            .arg(root.fpsLabel(dialog.chosenFps)).arg(MediaTime.durationText(dialog.recordedFrames, dialog.sourceFps))
                            .arg(MediaTime.durationText(dialog.effectiveFrames, dialog.chosenFps))
                            .arg((dialog.chosenFps / Math.max(0.001, dialog.sourceFps)).toFixed(3))
                            .arg(dialog.chosenFps < dialog.sourceFps ? qsTr("longer") : qsTr("for less time"))
                      : qsTr("Frames play as the file records them (%1, %2).").arg(root.fpsLabel(dialog.sourceFps)).arg(MediaTime.durationText(dialog.recordedFrames, dialog.sourceFps))
            }
            Toggle {
                id: keepFootage
                objectName: "clipAttributes:keepFootage"
                label: qsTr("Keep selected footage")
                tip: qsTr("Keep the recorded footage used by existing edits and adjust their playback durations")
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                text: keepFootage.checked
                      ? qsTr("Edit durations change to retain the selected footage. Later clips move with ripple editing. Applies across project timelines; undo restores the change together.")
                      : qsTr("Existing edits keep their trim-frame numbers and duration, so their selected footage may change. Applies across project timelines; undo restores the change together.")
            }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                   text: qsTr("Pixel aspect and field order follow the file. Colour matrix and input LUT are under Input Settings…; reel, scene and number are in the inspector.") }
            Text { text: qsTr("Timecode"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading; Layout.topMargin: Theme.s1 }
            RowLayout {
                spacing: Theme.s2
                Toggle {
                    id: useFileTimecode
                    objectName: "clipAttributes:useFileTimecode"
                    tip: qsTr("Off lets you set a start timecode instead of the file's own")
                    label: dialog.sourceTimecode >= 0 ? qsTr("Use the file's start timecode") : qsTr("Use the file's start timecode (none recorded)")
                    onToggled: if (!checked && dialog.sourceTimecode >= 0 && dialog.startTimecode < 0) dialog.startTimecode = dialog.sourceTimecode
                }
            }
            RowLayout {
                spacing: Theme.s2
                enabled: !useFileTimecode.checked
                Text { text: qsTr("Start timecode"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
                TimecodeField {
                    id: startField
                    objectName: "clipAttributes:startTimecode"
                    start: true
                    refusalInForm: true
                    tip: qsTr("Timecode of the first frame, used for timecode multicam sync")
                    label: qsTr("Start timecode")
                    fps: dialog.effectiveFps > 0 ? dialog.effectiveFps : 25
                    frame: root.engine.startFrameAtRate(Math.max(0, dialog.startTimecode), dialog.startRate, fps)
                    onEdited: function (f) { dialog.startTimecode = f; dialog.startRate = Math.round(fps) }
                }
            }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                   text: qsTr("Used by New Multicam Clip (timecode sync) and shown in the inspector; the timeline keeps its own timecode.") }
            Text {
                objectName: "clipAttributes:message"
                visible: root.message !== ""; Layout.fillWidth: true; wrapMode: Text.Wrap
                color: Theme.dangerText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; text: root.message
            }
        }
    }
    Connections {
        target: root.engine
        function onClipAttributesFinished(request, error) {
            if (request !== dialog.pendingRequest) return
            dialog.pendingRequest = 0
            root.message = error
            if (error === "") dialog.close()
        }
    }
}
