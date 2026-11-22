pragma ComponentBehavior: Bound
import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import QtQuick.Controls as C
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui
Item {
    id: root
    objectName: "sourceComparison"
    required property var engine
    property string secondId: ""
    property int secondFrame: 0
    property int offset: 0
    readonly property bool textActive: dialog.visible
    readonly property var entries: engine.library.filter(function (entry) { return !entry.multicam })
    readonly property int secondIndex: engine.library.findIndex(function (entry) { return entry.libraryId === root.secondId })
    readonly property var secondEntry: secondIndex >= 0 ? engine.library[secondIndex] : ({})
    function requestFrame() { if (dialog.visible && secondIndex >= 0) engine.previewMedia(secondIndex, secondFrame, false, "compare") }
    function follow() {
        if (!linked.checked || engine.previewLibraryIndex < 0) return
        var first = engine.library[engine.previewLibraryIndex]
        if (!first) return
        secondFrame = Math.max(0, Math.min(Math.max(0, MediaTime.sourceFrames(secondEntry)-1), Math.round(engine.sourcePosition*MediaTime.sourceFps(secondEntry)/MediaTime.sourceFps(first)) + offset))
        requestFrame()
    }
    function open(indexes) {
        indexes = indexes.filter(function (i) { return root.engine.library[i] && !root.engine.library[i].multicam })
        var first = indexes.length ? indexes[0] : engine.library.findIndex(function (e) { return !e.multicam })
        var second = indexes.length > 1 ? indexes[1] : engine.library.findIndex(function (e, i) { return !e.multicam && i !== first })
        if (second < 0) second = first
        if (first < 0 || first >= engine.library.length) return
        secondId = engine.library[second].libraryId; secondFrame = 0; offset = 0
        engine.previewClip(first); dialog.open(); requestFrame()
    }
    Connections { target: root.engine; function onSourcePositionChanged() { root.follow() } }
    ModalDialog {
        id: dialog
        objectName: "mediaCompareDialog"
        width: Math.min(Theme.wMediaCompare, parent ? parent.width-Theme.s5*2 : Theme.wMediaCompare)
        title: qsTr("Compare Source Clips")
        standardButtons: C.Dialog.Close
        onClosed: root.engine.pause()
        ColumnLayout {
            width: parent.width; spacing: Theme.s2
            RowLayout {
                ComboField {
                    tip: qsTr("Clip shown on the left, which also provides the audio")
                    Layout.fillWidth: true; caption: "A"
                    model: root.entries.map(function (e) { return e.name })
                    currentIndex: root.entries.findIndex(function (entry) { return root.engine.library[root.engine.previewLibraryIndex] && entry.libraryId === root.engine.library[root.engine.previewLibraryIndex].libraryId })
                    onActivated: function (index) { var id = root.entries[index].libraryId; root.engine.previewClip(root.engine.library.findIndex(function (e) { return e.libraryId === id })) }
                }
                ComboField {
                    tip: qsTr("Clip shown on the right, as a still frame")
                    Layout.fillWidth: true; caption: "B"
                    model: root.entries.map(function (e) { return e.name })
                    currentIndex: root.entries.findIndex(function (entry) { return entry.libraryId === root.secondId })
                    onActivated: function (index) { root.secondId = root.entries[index].libraryId; root.follow(); root.requestFrame() }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(Theme.hMediaTasks, dialog.width * 0.28)
                VideoView { Layout.fillWidth: true; Layout.fillHeight: true; source: root.engine; channel: VideoView.Source }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true; color: "black"
                    Image { anchors.fill: parent; source: (root.engine.mediaPreviews.compare || {}).libraryId === root.secondId ? root.engine.mediaPreviews.compare.thumb : ""; fillMode: Image.PreserveAspectFit; cache: false }
                }
            }
            RowLayout {
                IconButton { iconName: root.engine.playing ? "pause" : "play"; text: qsTr("Play / pause A"); onClicked: root.engine.togglePlay() }
                TimecodeField { tip: qsTr("Position played in clip A; type to jump there"); frame: root.engine.sourcePosition; fps: MediaTime.sourceFps(root.engine.library[root.engine.previewLibraryIndex] || {}); label: qsTr("A position"); onEdited: function (f) { root.engine.seek(f) } }
                Toggle { id: linked; tip: qsTr("Move B with A, keeping the offset the two have now"); checked: true; label: qsTr("Link positions"); onClicked: { root.offset = root.secondFrame - Math.round(root.engine.sourcePosition*MediaTime.sourceFps(root.secondEntry)/MediaTime.sourceFps(root.engine.library[root.engine.previewLibraryIndex] || {})); root.follow() } }
                Item { Layout.fillWidth: true }
                TimecodeField { tip: qsTr("Position shown for clip B; typing here unlinks them"); frame: root.secondFrame; fps: MediaTime.sourceFps(root.secondEntry); label: qsTr("B position"); onEdited: function (f) { root.secondFrame = Math.max(0, Math.min(Math.max(0, MediaTime.sourceFrames(root.secondEntry)-1), f)); linked.checked = false; root.requestFrame() } }
            }
            Text { Layout.fillWidth: true; text: qsTr("Positions use recorded source timecode. Linking preserves the offset and accounts for each recorded frame rate. Audio comes from A."); wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
        }
    }
}
