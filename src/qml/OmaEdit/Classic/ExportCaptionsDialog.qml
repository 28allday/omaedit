import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// File ▸ Export Captions…: the format, and every caption track (a file for
// each, named by its language) or one of them; then where to save.
Ui.FormDialog {
    id: root
    objectName: "exportCaptionsDialog"
    width: Math.min(480, parent ? parent.width - Ui.Theme.s5 * 2 : 480)
    title: qsTr("Export Captions")
    standardButtons: Dialog.Ok | Dialog.Cancel

    property var formats: []
    property var tracks: []
    readonly property var trackChoices: [qsTr("Every caption track, a file for each language")]
        .concat(tracks.map(function (t) {
            return qsTr("%1 · %2 · %n caption(s)", "", t.count).arg(t.name).arg(t.language.shown)
        }))
    readonly property string format: formats.length > 0 && formatBox.currentIndex >= 0 ? formats[formatBox.currentIndex].format : "srt"

    // `chosen`: a format to start on ("scc"); the last one otherwise.
    function openDialog(chosen) {
        formats = backend.captionFormats()
        for (let i = 0; i < formats.length; ++i)
            if (formats[i].format === chosen) formatBox.currentIndex = i
        tracks = backend.captionExportTracks()
        if (tracks.length === 0) {
            // Says there are none to export.
            backend.chooseCaptionExportFile("srt", -1)
            return
        }
        trackBox.currentIndex = 0
        open()
    }
    onAccepted: backend.chooseCaptionExportFile(format, trackBox.currentIndex > 0 ? tracks[trackBox.currentIndex - 1].track : -1)
    Component.onCompleted: standardButton(Dialog.Ok).text = qsTr("Export…")

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: qsTr("Captions are written as a file of their own, to put beside a video or send to where it is shown. With every caption track, each gets a file named by its language, such as Film.fr.srt.")
        }
        RowLayout {
            spacing: 10
            Label { text: qsTr("Format"); color: Ui.Theme.textMuted; Layout.preferredWidth: 80 }
            Ui.ComboField {
                id: formatBox
                objectName: "captionExportFormat"
                Accessible.name: tip
                tip: qsTr("The kind of caption file")
                model: root.formats.map(function (f) { return f.name })
                Layout.fillWidth: true
            }
        }
        RowLayout {
            spacing: 10
            Label { text: qsTr("Captions"); color: Ui.Theme.textMuted; Layout.preferredWidth: 80 }
            Ui.ComboField {
                id: trackBox
                objectName: "captionExportTrack"
                Accessible.name: tip
                tip: qsTr("Every caption track, or the one to export")
                model: root.trackChoices
                Layout.fillWidth: true
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.format === "scc"
                  ? qsTr("SCC is the broadcast caption file: up to 4 rows of 32 letters, each caption loaded just before it shows, in timecode at the timeline's rate (drop-frame at 29.97). Longer lines are wrapped; a caption that still does not fit stops the export and is named.")
                  : root.format === "itt"
                  ? qsTr("iTT is timed text for video stores: timed in frames at the timeline's rate, with its language and the captions at the top or bottom as their track's style places them.")
                  : root.format === "vtt"
                  ? qsTr("WebVTT is for players on the web: it keeps the captions' place at the top or bottom and their alignment.")
                  : qsTr("SubRip is plain text and times, read almost everywhere.")
        }
    }
}
