pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// The virtualized grid / list of clips. The owner hands it the page's
// library indexes and the live library; it owns nothing but the view
// geometry and keyboard focus. Files dropped in from a file manager land on
// the owner as URLs.
Item {
    id: root

    property var indexes: []              // library indexes on this page, in order
    property var clips: []                // the live library, looked up per card
    property var timelineIndexes: []      // positions in `timelines` to show above them
    property var timelines: []
    property string currentTimeline: ""
    signal timelineActivated(string id)
    signal timelineOpened(int index)
    signal timelineContextRequested(int index)
    property var selection: []            // library indexes
    property int currentIndex: -1
    property var usedPaths: ({})          // path → true
    property int viewMode: 0              // 0 grid, 1 list
    property real thumbSize: 0.5
    property bool libraryEmpty: true
    property bool skimEnabled: false
    property var skimResult: ({})
    property var stackCounts: ({})
    signal skimmed(int index, int frame)
    signal rangeSelected(int index, int first, int last)
    signal stackOpened(string id)
    property real fpsFallback: 25

    signal rowActivated(int libraryIndex, int modifiers)
    signal rowOpened(int libraryIndex)
    signal rowContextRequested(int libraryIndex)
    signal importRequested()
    signal urlsDropped(var urls)
    signal selectAllRequested()

    function durationLabel(entry) {
        if (entry.offline !== undefined && entry.offline) return qsTr("offline")
        return MediaTime.lengthText(entry)
    }
    function resLabel(entry) { return MediaTime.pictureText(entry) }
    function fpsLabel(entry) { return MediaTime.rateText(entry) }
    function createdLabel(entry) {
        var s = entry.created !== undefined ? String(entry.created) : ""
        return s.replace("T", " ").substring(0, 16)
    }
    function sizeLabel(entry) { return MediaTime.sizeText(entry.size) }

    Rectangle { anchors.fill: parent; color: Theme.windowBg }

    GridView {
        id: grid
        objectName: "mediaGrid"
        anchors.fill: parent
        anchors.margins: Theme.s2
        clip: true
        readonly property bool listMode: root.viewMode === 1
        readonly property bool filmMode: root.viewMode === 2
        readonly property int minCell: Math.round(150 + root.thumbSize * 170)
        readonly property int columns: listMode || filmMode ? 1 : Math.max(1, Math.floor(width / minCell))
        cellWidth: Math.floor(width / columns)
        cellHeight: filmMode ? Theme.hMediaFilmstrip : listMode ? Theme.hRow + Theme.s3 * 2
                             : Math.round((cellWidth - Theme.s2) * 9 / 16) + 66
        model: root.indexes
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar { atRest: true } // the library grid says there is more, as a box does
        focus: true
        activeFocusOnTab: true
        keyNavigationEnabled: false

        // The timelines come first and scroll with the clips.
        header: Flow {
            width: grid.width
            // The view holds its clips still as this grows, which would leave
            // the earlier timelines scrolled away above it.
            onHeightChanged: Qt.callLater(grid.positionViewAtBeginning)
            Repeater {
                model: root.timelineIndexes
                delegate: Item {
                    id: timelineCell
                    required property int modelData
                    objectName: "mediaPageTimeline:" + modelData
                    width: grid.cellWidth
                    height: grid.filmMode ? Theme.hRow + Theme.s3 * 2 : grid.cellHeight
                    TimelineTile {
                        anchors.fill: parent
                        anchors.margins: grid.listMode || grid.filmMode ? 0 : Theme.s1
                        item: timelineCell.modelData < root.timelines.length ? root.timelines[timelineCell.modelData] : ({})
                        selected: item.id === root.currentTimeline && root.currentIndex < 0
                        listMode: grid.listMode || grid.filmMode
                        captionHeight: 66 - Theme.s1 * 2
                        onActivated: { grid.forceActiveFocus(); root.timelineActivated(item.id) }
                        onOpened: root.timelineOpened(item.index)
                        onContextRequested: root.timelineContextRequested(item.index)
                    }
                }
            }
        }

        Accessible.role: Accessible.List
        Accessible.name: qsTr("Media")

        Keys.onPressed: function (event) {
            if (root.indexes.length === 0) return
            var pos = root.indexes.indexOf(root.currentIndex)
            var next = pos
            var cols = grid.columns
            if (event.key === Qt.Key_Right) next = Math.min(root.indexes.length - 1, pos + 1)
            else if (event.key === Qt.Key_Left) next = Math.max(0, pos - 1)
            else if (event.key === Qt.Key_Down) next = Math.min(root.indexes.length - 1, pos + cols)
            else if (event.key === Qt.Key_Up) next = Math.max(0, pos - cols)
            else if (event.key === Qt.Key_Home) next = 0
            else if (event.key === Qt.Key_End) next = root.indexes.length - 1
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (pos >= 0) root.rowOpened(root.indexes[pos])
                event.accepted = true
                return
            } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                if (Commands.registry) { event.accepted = false; return }
                root.selectAllRequested()
                event.accepted = true
                return
            } else return
            if (next < 0) next = 0
            root.rowActivated(root.indexes[next], event.modifiers & Qt.ShiftModifier)
            grid.positionViewAtIndex(next, GridView.Contain)
            event.accepted = true
        }

        delegate: Item {
            id: cell
            required property int index
            required property int modelData
            readonly property int libraryIndex: modelData
            // Live: a new thumbnail or waveform shows without a rebuild.
            readonly property var entry: libraryIndex < root.clips.length ? root.clips[libraryIndex] : ({})
            readonly property bool isSelected: root.selection.indexOf(libraryIndex) >= 0
            readonly property bool isUsed: entry.path !== undefined
                                           && root.usedPaths[entry.path] === true
            width: grid.cellWidth
            height: grid.cellHeight
            objectName: "mediaItem:" + libraryIndex

            Loader {
                anchors.fill: parent
                anchors.margins: grid.listMode ? 0 : Theme.s1
                sourceComponent: grid.filmMode ? filmRow : grid.listMode ? listRow : card
            }

            Component {
                id: card
                MediaCard {
                    objectName: "sourceCard:" + cell.libraryIndex
                    entry: cell.entry
                    selected: cell.isSelected
                    current: cell.libraryIndex === root.currentIndex
                    used: cell.isUsed
                    skimEnabled: root.skimEnabled
                    skimThumb: root.skimResult.libraryId === cell.entry.libraryId ? root.skimResult.thumb || "" : ""
                    stackCount: root.stackCounts[cell.entry.stackId] || 0
                    onSkimmed: function (frame) { root.skimmed(cell.libraryIndex, frame) }
                    onStackOpened: root.stackOpened(cell.entry.stackId)
                    durationText: root.durationLabel(entry)
                    resolutionText: root.resLabel(entry)
                    fpsText: root.fpsLabel(entry)
                    onActivated: function (modifiers) { grid.forceActiveFocus(); root.rowActivated(cell.libraryIndex, modifiers) }
                    onOpened: root.rowOpened(cell.libraryIndex)
                    onContextRequested: root.rowContextRequested(cell.libraryIndex)
                }
            }
            Component {
                id: filmRow
                FilmstripRow {
                    objectName: "mediaFilmstrip:" + cell.libraryIndex
                    entry: cell.entry; selected: cell.isSelected
                    skimEnabled: root.skimEnabled
                    skimThumb: root.skimResult.libraryId === cell.entry.libraryId ? root.skimResult.thumb || "" : ""
                    onActivated: function (modifiers) { root.rowActivated(cell.libraryIndex, modifiers) }
                    onOpened: root.rowOpened(cell.libraryIndex)
                    onContextRequested: root.rowContextRequested(cell.libraryIndex)
                    onSkimmed: function (frame) { root.skimmed(cell.libraryIndex, frame) }
                    onRangeSelected: function (first, last) { root.rangeSelected(cell.libraryIndex, first, last) }
                }
            }
            Component {
                id: listRow
                MediaListRow {
                    entry: cell.entry
                    selected: cell.isSelected
                    used: cell.isUsed
                    durationText: root.durationLabel(entry)
                    resolutionText: root.resLabel(entry)
                    fpsText: root.fpsLabel(entry)
                    createdText: root.createdLabel(entry)
                    sizeText: root.sizeLabel(entry)
                    onActivated: function (modifiers) { grid.forceActiveFocus(); root.rowActivated(cell.libraryIndex, modifiers) }
                    onOpened: root.rowOpened(cell.libraryIndex)
                    onContextRequested: root.rowContextRequested(cell.libraryIndex)
                }
            }
        }
    }

    EmptyState {
        anchors.fill: parent
        anchors.topMargin: grid.headerItem ? grid.headerItem.height : 0
        visible: root.indexes.length === 0 && (root.libraryEmpty || root.timelineIndexes.length === 0)
        iconName: root.libraryEmpty ? "folder-open" : "filter"
        title: root.libraryEmpty ? qsTr("No media yet") : qsTr("No clips match")
        description: root.libraryEmpty
                     ? qsTr("Import files or drop them here from your file manager.")
                     : qsTr("Clear the search or pick another bin.")
        actionText: root.libraryEmpty ? qsTr("Import Media…") : ""
        onActionTriggered: root.importRequested()
    }

    // Drops from a file manager: every URL goes to the owner, which imports
    // files and recurses folders.
    DropArea {
        id: drop
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function (event) {
            if (event.hasUrls) {
                root.urlsDropped(event.urls)
                event.acceptProposedAction()
            }
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: drop.containsDrag
        color: "transparent"
        border.width: Theme.selectionRing
        border.color: Theme.dropValid
        Rectangle {
            anchors.centerIn: parent
            width: dropText.implicitWidth + Theme.s4 * 2
            height: Theme.hControl + Theme.s2
            radius: Theme.rControl
            color: Theme.panelRaised
            Text {
                id: dropText
                anchors.centerIn: parent
                text: qsTr("Drop to import")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textPrimary
            }
        }
    }
}
