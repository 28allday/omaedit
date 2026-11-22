pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui
Item {
    id: root
    required property var engine
    property var selectedIds: []
    property bool rawSource: false
    readonly property bool textActive: inputDialog.visible
    function indexes() {
        var out = []
        engine.library.forEach(function (e, i) { if (root.selectedIds.indexOf(e.libraryId) >= 0) out.push(i) })
        return out
    }
    function attach(index) {
        if (index >= 0 && index < engine.library.length) engine.mediaTasks.browse("attach-proxy:" + engine.library[index].libraryId, "file")
    }
    function input(indexes) {
        if (!indexes.length) return
        selectedIds = indexes.map(function (i) { return root.engine.library[i].libraryId })
        var entry = engine.library[indexes[0]]
        rawSource = entry.videoCodec === "prores_raw"
        rawExposure.value = Number(entry.rawExposure || 0)
        rawRed.value = Number(entry.rawRed || 1)
        rawBlue.value = Number(entry.rawBlue || 1)
        lut.text = entry.inputLut || ""
        space.currentIndex = Math.max(0, [0, 601, 709, 2020].indexOf(Number(entry.inputSpace || 0)))
        inputDialog.open()
    }
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose, paths) {
            if (!paths.length) return
            if (purpose.indexOf("attach-proxy:") === 0) {
                var index = root.engine.library.findIndex(function (entry) { return entry.libraryId === purpose.substring(13) })
                if (index >= 0) root.engine.attachMediaProxy(index, paths[0])
            } else if (purpose === "input-lut") lut.text = paths[0]
        }
    }
    ModalDialog {
        id: inputDialog
        objectName: "mediaInputSettingsDialog"
        title: qsTr("Input Settings · %1 Clip(s)").arg(root.selectedIds.length)
        onAccepted: {
            root.engine.setMediaInput(root.indexes(), lut.text.trim(), [0, 601, 709, 2020][space.currentIndex])
            if (root.rawSource) root.engine.setMediaRaw(root.indexes(), rawExposure.value, rawRed.value, rawBlue.value)
        }
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            Text { Layout.fillWidth: true; text: qsTr("These source settings apply to its references, source preview and timeline. Exports retain the input correction."); wrapMode: Text.Wrap; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
            Text { visible: root.rawSource; Layout.fillWidth: true; text: qsTr("ProRes RAW · camera colour and As Shot white balance. Adjustments apply before grading. Enable OpenColorIO to preserve scene-linear highlights."); wrapMode: Text.Wrap; color: Theme.textSecondary; font.pixelSize: Theme.fsBase }
            SliderField { id: rawExposure; tip: qsTr("Adjust scene exposure before grading; one stop doubles or halves the light"); objectName: "rawExposure"; visible: root.rawSource; Layout.fillWidth: true; label: qsTr("RAW exposure (stops)"); from: -6; to: 6; stepSize: 0.1; value: 0; onEdited: function(v) { value=v } }
            SliderField { id: rawRed; tip: qsTr("Multiply the camera’s As Shot red white balance; 1 keeps the recorded balance"); objectName: "rawRed"; visible: root.rawSource; Layout.fillWidth: true; label: qsTr("White balance · red"); from: 0.25; to: 4; stepSize: 0.01; value: 1; onEdited: function(v) { value=v } }
            SliderField { id: rawBlue; tip: qsTr("Multiply the camera’s As Shot blue white balance; 1 keeps the recorded balance"); objectName: "rawBlue"; visible: root.rawSource; Layout.fillWidth: true; label: qsTr("White balance · blue"); from: 0.25; to: 4; stepSize: 0.01; value: 1; onEdited: function(v) { value=v } }
            ToolButton { visible: root.rawSource; text: qsTr("As Shot"); onClicked: { rawExposure.value=0; rawRed.value=1; rawBlue.value=1 } }
            RowLayout {
                LineEdit { id: lut; tip: qsTr("A .cube LUT applied to this source in preview, timeline and export"); Layout.fillWidth: true; hint: qsTr("Input LUT (.cube), empty for none") }
                ToolButton { text: qsTr("Browse…"); onClicked: root.engine.mediaTasks.browse("input-lut", "file") }
                ToolButton { text: qsTr("Clear"); onClicked: lut.text = "" }
            }
            ComboField { id: space; tip: qsTr("How Y\u2032CbCr video is interpreted when the file says nothing"); Layout.fillWidth: true; caption: qsTr("Input matrix"); model: [qsTr("Use source metadata"), "Rec. 601", "Rec. 709", "Rec. 2020"] }
            Text { Layout.fillWidth: true; text: qsTr("The matrix override changes how Y′CbCr video is interpreted. Use a LUT for a camera log-to-display conversion."); wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
    }
}
