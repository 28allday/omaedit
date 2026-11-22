pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui
Item {
    id: root
    required property var engine
    readonly property bool textActive: volumeDialog.visible
    readonly property var entry: engine.previewLibraryIndex >= 0 ? engine.library[engine.previewLibraryIndex] || ({}) : ({})
    property real unmutedVolume: 1
    property string pendingAction: ""
    property int pendingValue: 0
    function applyTransport(action, value) {
        if (action === "play") engine.togglePlay()
        else if (action === "shuttle") engine.shuttle(value)
        else if (action === "seek") engine.seek(value)
    }
    function transport(action, value) {
        if (engine.previewLibraryIndex < 0) return
        if (!engine.viewingSource) {
            pendingAction = action; pendingValue = value || 0
            engine.previewClipAt(engine.previewLibraryIndex, engine.sourcePosition)
        } else applyTransport(action, value || 0)
    }
    Connections {
        target: root.engine
        function onSourceChanged() {
            if (root.engine.previewLibraryIndex < 0) root.pendingAction = ""
            if (root.engine.viewingSource && root.pendingAction) Qt.callLater(function () {
                var action = root.pendingAction; var value = root.pendingValue; root.pendingAction = ""
                if (root.engine.viewingSource) root.applyTransport(action, value)
            })
        }
    }
    function snapshot() { engine.pause(); engine.mediaTasks.browse("source-snapshot", engine.sourceLinear ? "exr" : "png") }
    function volume() { volumeDialog.open() }
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose, paths) { if (purpose === "source-snapshot" && paths.length) root.engine.saveSourceFrame(paths[0]) }
    }
    ModalDialog {
        id: volumeDialog
        objectName: "mediaMonitorVolumeDialog"
        title: qsTr("Monitor Volume")
        standardButtons: C.Dialog.Close
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            SliderField { objectName: "sourceMonitorVolume"; tip: qsTr("Listening level for source and timeline playback; exports are unchanged"); Layout.fillWidth: true; label: qsTr("Volume"); value: root.engine.monitorVolume * 100; from: 0; to: 100; decimals: 0; suffix: "%"; onEdited: function (value) { root.engine.monitorVolume = value / 100 } }
            Toggle { tip: qsTr("Silence monitoring without losing the volume setting"); label: qsTr("Mute"); checked: root.engine.monitorVolume === 0; onClicked: { if (checked) { root.unmutedVolume = root.engine.monitorVolume || 1; root.engine.monitorVolume = 0 } else root.engine.monitorVolume = root.unmutedVolume } }
            Text { Layout.fillWidth: true; text: qsTr("Controls listening volume for source and timeline playback. Exported audio levels stay as mixed."); color: Theme.textSecondary; wrapMode: Text.Wrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
    }
}
