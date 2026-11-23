import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// The caption timing tools that take a number (Edit ▸ Captions ▸ Timing and
// the caption list's menu): Snap Edges to Cuts within so many frames, Close
// Small Gaps under so many frames, Shift Captions by a number of frames
// (less than nothing: earlier). On the selected captions, or every one when
// none is selected. One undo step.
Ui.FormDialog {
    id: root
    objectName: "captionTimingDialog"
    width: Math.min(440, parent ? parent.width - Ui.Theme.s5 * 2 : 440)
    standardButtons: Dialog.Ok | Dialog.Cancel
    title: mode === "snap" ? qsTr("Snap Caption Edges to Cuts")
         : mode === "gaps" ? qsTr("Close Small Gaps Between Captions")
                           : qsTr("Shift Captions")

    property string mode: "snap"
    property var indices: []
    // Remembered while the editor is open: the last number used for each.
    property int snapFrames: 6
    property int gapFrames: 12
    property int shiftFrames: 0

    function openFor(kind, chosen) {
        mode = kind
        // The selected captions, else every one.
        const selected = win.clipCommands.selectedCaptionIndices()
        indices = chosen !== undefined && chosen !== null ? chosen : selected
        framesField.value = mode === "snap" ? snapFrames : mode === "gaps" ? gapFrames : shiftFrames
        open()
    }
    onAccepted: {
        landTyped()
        const n = Math.round(framesField.value)
        if (mode === "snap") { snapFrames = n; backend.snapCaptionsToCuts(indices, n) }
        else if (mode === "gaps") { gapFrames = n; backend.closeCaptionGaps(indices, n) }
        else { shiftFrames = n; backend.shiftCaptions(indices, n) }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.mode === "snap"
                  ? qsTr("A caption's start or end this close to a cut in the picture moves onto the cut.")
                  : root.mode === "gaps"
                    ? qsTr("A caption followed by a gap shorter than this lasts until the next caption on its track.")
                    : qsTr("Moves the captions later by this many frames, or earlier by a number below nothing.")
        }
        RowLayout {
            spacing: 10
            Label { text: root.mode === "snap" ? qsTr("Within") : root.mode === "gaps" ? qsTr("Gaps under") : qsTr("Shift by"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: framesField
                objectName: "captionTimingFrames"
                label: qsTr("Frames")
                Accessible.name: label
                tip: qsTr("A number of frames")
                decimals: 0
                from: root.mode === "shift" ? -100000 : 1
                to: root.mode === "shift" ? 100000 : 1000
                editable: true
            }
            Label {
                text: qsTr("frames (%1 s)").arg(backend.fps > 0 ? (framesField.value / backend.fps).toFixed(2) : "0")
                color: Ui.Theme.textMuted
            }
            Item { Layout.fillWidth: true }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.indices.length > 0 ? qsTr("On the %n selected caption(s). One undo step.", "", root.indices.length)
                                          : qsTr("On every caption of the timeline. One undo step.")
        }
    }
}
