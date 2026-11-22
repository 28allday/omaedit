import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// ── Timeline index: searchable clips/tags list, left of the
// timeline (Ctrl+Shift+2). Clicking a row parks the playhead
// at the item and selects it; the row under the playhead
// highlights as playback moves.
Item {
    id: indexPanel
    property alias indexSearch: indexSearch

    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.left: parent.left
    anchors.topMargin: 12
    anchors.bottomMargin: 12
    anchors.leftMargin: win.showTimelineIndex ? 12 : 0
    width: win.showTimelineIndex ? 276 : 0
    visible: win.showTimelineIndex
    clip: true

    property bool tagsTab: false
    property bool categoriesTab: false
    property string kindFilter: "all" // all / video / audio / titles

    // Storyline + connected clips merged chronologically.
    readonly property var clipRows: {
        var rows = []
        var query = indexSearch.text.toLowerCase()
        function kindOf(entry, connected) {
            if (entry.gap === true)
                return "gap"
            if (entry.title !== undefined && entry.title !== "")
                return "titles"
            if (entry.audioOnly === true)
                return "audio"
            return "video"
        }
        function push(entry, sIdx, cIdx) {
            var kind = kindOf(entry)
            if (kindFilter !== "all" && kind !== kindFilter)
                return
            if (query !== ""
                    && entry.name.toLowerCase().indexOf(query) < 0)
                return
            rows.push({ name: entry.name, start: entry.start,
                        frames: entry.frames, kind: kind,
                        storyIndex: sIdx, connIndex: cIdx })
        }
        for (var i = 0; i < backend.timeline.length; i++)
            push(backend.timeline[i], i, -1)
        for (var j = 0; j < backend.connected.length; j++)
            push(backend.connected[j], -1, j)
        rows.sort(function (a, b) { return a.start - b.start })
        return rows
    }
    readonly property var tagRows: {
        var rows = []
        var query = indexSearch.text.toLowerCase()
        for (var i = 0; i < backend.markers.length; i++) {
            var m = backend.markers[i]
            var label = m.text !== ""
                ? m.text : win.clipMarkers.typeLabel(m.type)
            if (query !== ""
                    && label.toLowerCase().indexOf(query) < 0)
                continue
            rows.push({ frame: m.frame, text: m.text, label: label,
                        type: m.type, colour: m.colour, notes: m.notes === undefined ? "" : m.notes,
                        track: -1, index: -1, srcFrame: -1 })
        }
        var clipMarks = win.clipMarkers.frames()
        for (var c = 0; c < clipMarks.length; c++) {
            var cm = clipMarks[c]
            var clipLabel = (cm.marker.text !== "" ? cm.marker.text
                             : win.clipMarkers.typeLabel(cm.marker.type))
                            + " · " + cm.entry.name
            if (query !== "" && clipLabel.toLowerCase().indexOf(query) < 0)
                continue
            rows.push({ frame: cm.frame, text: cm.marker.text, label: clipLabel,
                        type: cm.marker.type, colour: cm.marker.colour,
                        notes: cm.marker.notes === undefined ? "" : cm.marker.notes,
                        track: cm.entry.track, index: cm.entry.index,
                        srcFrame: cm.marker.frame })
        }
        rows.sort(function (a, b) { return a.frame - b.frame })
        return rows
    }

    function jumpTo(row) {
        backend.viewTimeline()
        backend.seek(row.start)
        win.selectedEdge = -1
        win.selectedTimelineIndex = row.storyIndex
        win.selectedConnectedIndex = row.connIndex
    }

    Column {
        anchors.fill: parent
        spacing: 8

        Row { // Clips | Tags pane switch
            spacing: 6
            FlatButton {
                text: qsTr("Clips")
                width: (indexPanel.width - 24) / 3
                tip: qsTr("List the clips of this sequence in playing order")
                quiet: true
                implicitHeight: 26
                highlighted: !indexPanel.tagsTab && !indexPanel.categoriesTab
                onClicked: { indexPanel.tagsTab = false; indexPanel.categoriesTab = false }
            }
            FlatButton {
                text: qsTr("Tags")
                width: (indexPanel.width - 24) / 3
                tip: qsTr("List the markers of this sequence instead of the clips")
                quiet: true
                implicitHeight: 26
                highlighted: indexPanel.tagsTab
                onClicked: { indexPanel.tagsTab = true; indexPanel.categoriesTab = false }
            }
            FlatButton {
                objectName: "indexCategoriesTab"
                text: qsTr("Categories")
                width: (indexPanel.width - 24) / 3
                tip: qsTr("List what the clips of this sequence are for, and switch each kind on or off")
                quiet: true
                implicitHeight: 26
                highlighted: indexPanel.categoriesTab
                onClicked: indexPanel.categoriesTab = true
            }
        }

        ListView {
            id: categoryList
            objectName: "indexCategoryList"
            width: parent.width
            height: indexPanel.categoriesTab ? parent.height - y - 34 : 0
            visible: indexPanel.categoriesTab
            clip: true
            model: win.categories.rows()
            delegate: Item {
                id: categoryRow
                required property var modelData
                objectName: "indexCategory:" + modelData.id
                width: categoryList.width
                height: 30
                Rectangle {
                    id: categorySwatch
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    radius: 6
                    color: win.categories.tint(categoryRow.modelData.color)
                }
                Text {
                    anchors.left: categorySwatch.right
                    anchors.right: categorySwitch.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: categoryRow.modelData.name + " (" + categoryRow.modelData.clips + ")"
                    color: win.textColor
                    font.pixelSize: 12
                }
                CheckBox {
                    id: categorySwitch
                    objectName: "indexCategorySwitch:" + categoryRow.modelData.id
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: categoryRow.modelData.clips > 0
                    checked: categoryRow.modelData.on
                    ToolTip.text: qsTr("Switches every clip in this category on or off")
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    onClicked: win.categories.setOn(categoryRow.modelData.id, checked)
                }
            }
        }
        FlatButton {
            visible: indexPanel.categoriesTab
            width: parent.width
            text: qsTr("Edit Categories…")
            tip: qsTr("Rename, colour, add or remove categories")
            quiet: true
            implicitHeight: 26
            onClicked: Ui.Commands.trigger("modify.edit-categories")
        }

        TextField {
            id: indexSearch
            width: parent.width
            visible: !indexPanel.categoriesTab
            hoverEnabled: true
            ToolTip.text: qsTr("Narrows the list to rows matching this text")
            ToolTip.visible: hovered && !activeFocus
            ToolTip.delay: 450
            placeholderText: qsTr("Search…")
            font.pixelSize: 12
        }

        ListView {
            id: indexList
            width: parent.width
            visible: !indexPanel.categoriesTab
            height: parent.height - y
                    - (indexPanel.tagsTab ? 0 : 34)
            clip: true
            model: indexPanel.tagsTab ? indexPanel.tagRows
                                      : indexPanel.clipRows
            delegate: Rectangle {
                required property var modelData
                readonly property bool isTag: indexPanel.tagsTab
                // The index playhead: the clip under the
                // timeline playhead reads highlighted.
                readonly property bool underPlayhead:
                    !isTag
                    && backend.position >= modelData.start
                    && backend.position < modelData.start
                                          + modelData.frames
                width: indexList.width
                height: 30
                radius: win.rPill(height)
                color: underPlayhead ? Qt.rgba(win.accent.r,
                                               win.accent.g,
                                               win.accent.b, 0.18)
                                     : "transparent"

                Rectangle { // kind / marker-type chip
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: isTag
                           ? Ui.Theme.markerTint(parent.modelData)
                           : parent.modelData.kind === "audio"
                             ? "#59d18c"
                             : parent.modelData.kind === "titles"
                               ? "#e6c93a"
                               : parent.modelData.kind === "gap"
                                 ? win.textDimColor
                                 : win.accent
                }

                CheckBox { // complete a to-do right here
                    id: todoBox
                    ToolTip.text: qsTr("Tick to mark this to-do marker completed")
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    visible: isTag
                             && (parent.modelData.type === "todo"
                                 || parent.modelData.type === "done")
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? 24 : 0
                    focusPolicy: Qt.NoFocus
                    checked: visible
                             && parent.modelData.type === "done"
                    onToggled: {
                        var row = parent.modelData
                        if (row.track >= 0) // a clip marker: by its clip and source frame
                            backend.setClipMarkerOn(row.track, row.index, row.srcFrame,
                                                    checked ? "done" : "todo",
                                                    row.text, row.notes)
                        else
                            backend.setMarkerInfo(row.frame, row.text,
                                                  checked ? "done" : "todo", row.notes)
                    }
                }

                Label {
                    anchors.left: todoBox.visible
                                  ? todoBox.right : parent.left
                    anchors.leftMargin: todoBox.visible ? 2 : 24
                    anchors.right: positionLabel.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: isTag ? parent.modelData.label
                                : parent.modelData.name
                    color: win.textColor
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                Label { // Position column
                    id: positionLabel
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    // Read again when the starting timecode or the rate changes.
                    text: backend.startFrame >= 0 && backend.fps >= 0
                          ? backend.timecode(isTag ? parent.modelData.frame : parent.modelData.start) : ""
                    color: win.textColor
                    opacity: 0.55
                    font.family: Ui.Theme.monoFamily
                    font.pixelSize: 10
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: todoBox.visible ? 38 : 0
                    onClicked: {
                        if (parent.isTag) {
                            backend.viewTimeline()
                            backend.seek(parent.modelData.frame)
                        } else {
                            indexPanel.jumpTo(parent.modelData)
                        }
                    }
                }
            }

            Label { // empty state
                visible: indexList.count === 0
                anchors.centerIn: parent
                text: indexPanel.tagsTab
                      ? "No markers" : "No clips"
                color: win.textColor
                opacity: 0.3
                font.pixelSize: 12
            }
        }

        Row { // kind filters along the bottom
            visible: !indexPanel.tagsTab
            spacing: 4
            Repeater {
                model: [{ key: "all", label: qsTr("All") },
                        { key: "video", label: qsTr("Video") },
                        { key: "audio", label: qsTr("Audio") },
                        { key: "titles", label: qsTr("Titles") }]
                delegate: FlatButton {
                    required property var modelData
                    text: modelData.label
                    tip: qsTr("Show only clips of this kind in the list")
                    quiet: true
                    implicitHeight: 24
                    // Material's 64px button minimum overflows
                    // the panel; the pill padding in
                    // implicitContentWidth is already enough.
                    implicitWidth: implicitContentWidth
                    font.pixelSize: 11
                    highlighted:
                        indexPanel.kindFilter === modelData.key
                    onClicked:
                        indexPanel.kindFilter = modelData.key
                }
            }
        }
    }
}
