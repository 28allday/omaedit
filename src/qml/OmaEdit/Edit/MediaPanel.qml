pragma ComponentBehavior: Bound

import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import QtQuick.Controls as C
import OmaEdit.Ui

// Left panel: where footage is found. Bin tree on the left, thumbnail grid on
// the right, both under one tab strip.
//
// Owner-driven: `clips` is the library in the shape Backend exposes (name,
// duration in frames, fps, height, thumb, rangeIn/rangeOut), `bins` is what
// the bin tree shows, and every user action leaves as a signal. The panel
// keeps only view state — the search text, the view mode.
Rectangle {
    id: root

    property int currentTab: 0
    // [{ name, duration, fps, height, thumb, rangeIn, rangeOut, tone? }]
    property var clips: []
    // The project's timelines, shown ahead of the clips in the whole library:
    // [{index, id, name, active, frames, dur, height, thumb}].
    property var timelines: []
    property string currentTimeline: ""
    // The live host supplies project-frame duration; standalone mock panels
    // have no project format and use the selected source picture count.
    property var placementFramesForIndex: function(index) { return MediaTime.rangeFrames(root.clips[index] || {}) }
    property var bins: []
    property var binMatcher: function(entry){return true}
    property int currentBin: 0
    // Index into `clips` of the selected clip; -1 for none.
    property int currentIndex: -1
    // 0 large grid, 1 grid, 2 list.
    property int viewMode: 1
    property alias searchText: search.text
    readonly property bool searchActive: search.active || catalogueSearch.active
    // The catalogue behind the other three tabs: [{key, name, cat, ok}]
    // for effects and transitions, [{key, name, icon}] for title presets.
    // Double-click applies (or opens the title dialog); drag carries the
    // tile to the timeline as an "omaedit/effect" payload.
    property var effects: []
    property var titles: []
    // key → whether the tile applies to the current selection. Kept apart
    // from the lists so a change re-dims tiles without re-creating them
    // (a re-created tile ends any drag it was carrying).
    property var applicability: ({})
    // A tile of any kind is being dragged. Set by the handlers themselves:
    // a binding on the ghosts' Drag.active never re-evaluates from here.
    property bool dragActive: false

    // A library clip taken up to be carried to the timeline: by its tile
    // here, or by the picture of the Source viewer (which says how long its
    // marked selection is). The ghost follows `handler`'s pointer at window
    // level; what a drop places is the clip's own marked range either way.
    function beginClipDrag(libraryIndex, handler, from, detail) {
        const entry = libraryIndex >= 0 && libraryIndex < root.clips.length ? root.clips[libraryIndex] : null
        if (entry === null)
            return
        root.dragActive = true
        dragGhost.libraryIndex = libraryIndex
        dragGhost.sequenceIndex = -1
        dragGhost.name = entry.name !== undefined ? entry.name : ""
        dragGhost.thumb = entry.thumb !== undefined ? entry.thumb : ""
        dragGhost.detail = detail
        dragGhost.frames = root.placementFramesForIndex(libraryIndex)
        const host = from.Window.window ? from.Window.window.contentItem : root
        dragGhost.parent = host
        dragGhost.x = Qt.binding(function () {
            return host.mapFromItem(null, handler.centroid.scenePosition).x
                   - dragGhost.width / 2
        })
        dragGhost.y = Qt.binding(function () {
            return host.mapFromItem(null, handler.centroid.scenePosition).y
                   - dragGhost.height / 2
        })
        dragGhost.Drag.active = true
    }
    // A drag that ends without its release is broken off, not dropped: its
    // handler switched off under it (a render starting, the clip gone
    // offline), or its pointer taken away, which the handler says
    // (breakClipDrag, from its canceled) just after it has let the drag go.
    property bool clipDragBroken: false
    function endClipDrag(handler) {
        if (!dragGhost.Drag.active)
            return
        if (handler !== undefined && !handler.enabled) {
            finishClipDrag(false)
            return
        }
        // Where it is let go, kept: the pointer the ghost follows is gone.
        dragGhost.x = dragGhost.x
        dragGhost.y = dragGhost.y
        clipDragBroken = false
        Qt.callLater(function () { root.finishClipDrag(!root.clipDragBroken) })
    }
    function breakClipDrag() { clipDragBroken = true }
    function finishClipDrag(drop) {
        if (!dragGhost.Drag.active)
            return
        root.dragActive = false
        if (drop)
            dragGhost.Drag.drop()
        else
            dragGhost.Drag.cancel()
        dragGhost.Drag.active = false
        dragGhost.x = 0
        dragGhost.y = 0
        dragGhost.parent = root
        dragGhost.detail = ""
    }

    signal clipActivated(int index)          // click: select + view source
    signal clipOpened(int index)             // double-click: add to timeline
    signal clipContextRequested(int index)   // right-click: select first
    signal timelineSelected(string id)       // click
    signal timelineOpened(int index)         // double-click: edit it
    signal timelineContextRequested(int index)
    signal clipFiled(int index, string binId)     // dropped on a bin
    signal timelineFiled(int index, string binId)
    signal importRequested()
    signal binContextRequested()             // right-click on the bin itself, off any tile
    signal binActivated(int index)
    signal binAddRequested()                 // the + beside Bins
    signal effectApplied(string key)
    signal transitionApplied(string key)
    signal titleRequested(string key)

    // The effects shelf: which effects the Effects tab shows — "all",
    // "favorites", "recent" or "bin:<name>" — fed by the keys the host
    // keeps. Every change is asked of the host, never done here.
    property string effectShelf: "all"
    property string effectCategory: ""
    readonly property var effectCategories: [
        {key:"", name:qsTr("All categories")}, {key:"transitions", name:qsTr("Transitions")},
        {key:"blur", name:qsTr("Blur & Sharpen")},
        {key:"stylise", name:qsTr("Stylise")}, {key:"texture", name:qsTr("Texture")},
        {key:"keying", name:qsTr("Keying")}, {key:"distort", name:qsTr("Distort")},
        {key:"transform", name:qsTr("Transform")},
        {key:"retime", name:qsTr("Speed")}, {key:"audio", name:qsTr("Audio")},
        {key:"presets", name:qsTr("Presets")}]
    property var favoriteKeys: []
    property var recentKeys: []
    property var effectBins: []               // [{name, keys}]
    signal effectFavoriteToggled(string key)
    signal recentEffectsCleared()
    signal effectBinCreated(string name, string key)   // key "" = empty bin
    signal effectBinRenamed(string name, string to)
    signal effectBinDeleted(string name)
    signal effectBinKeyAdded(string bin, string key)
    signal effectBinKeyRemoved(string bin, string key)
    readonly property string shelfBin: effectShelf.indexOf("bin:") === 0 ? effectShelf.substring(4) : ""
    function isFavorite(key) { return favoriteKeys.indexOf(key) >= 0 }
    function binKeys(name) {
        for (let i = 0; i < effectBins.length; ++i)
            if (effectBins[i].name === name)
                return effectBins[i].keys !== undefined ? effectBins[i].keys : []
        return []
    }
    function binHas(name, key) { return binKeys(name).indexOf(key) >= 0 }
    // A bin that vanished under the shelf falls back to everything.
    onEffectBinsChanged: if (shelfBin !== "" && binKeys(shelfBin).length === 0 && !hasBin(shelfBin)) effectShelf = "all"
    function hasBin(name) {
        for (let i = 0; i < effectBins.length; ++i)
            if (effectBins[i].name === name)
                return true
        return false
    }

    readonly property var catalogueRows: {
        const source = currentTab === 1 ? effects : currentTab === 2 ? titles : []
        const needle = catalogueSearch.text.trim().toLowerCase()
        // The shelf narrows the Effects tab to a key list — recents in
        // the order they were used, the rest in catalogue order.
        let keys = null
        if (currentTab === 1 && effectShelf !== "all")
            keys = effectShelf === "favorites" ? favoriteKeys
                 : effectShelf === "recent" ? recentKeys : binKeys(shelfBin)
        const out = []
        for (let i = 0; i < source.length; ++i) {
            if (currentTab === 1 && effectCategory !== "" && source[i].cat !== effectCategory) continue
            const name = source[i].name !== undefined ? String(source[i].name) : ""
            if (needle !== "" && name.toLowerCase().indexOf(needle) < 0)
                continue
            if (keys !== null && keys.indexOf(source[i].key) < 0)
                continue
            out.push(source[i])
        }
        if (keys !== null && effectShelf === "recent")
            out.sort(function (a, b) { return keys.indexOf(a.key) - keys.indexOf(b.key) })
        return out
    }
    function catIcon(cat) {
        if (cat === "colour") return "palette"
        if (cat === "retime") return "gauge"
        if (cat === "transform") return "move"
        if (cat === "audio") return "audio-waveform"
        if (cat === "keying" || cat === "stylise" || cat === "blur" || cat === "texture") return "wand-sparkles"
        if (cat === "distort") return "move"
        return "arrow-left-right"
    }

    // Filter (the funnel): kind, favourites, rejected; and the sort order.
    property string kindFilter: "all"     // all | video | audio
    property bool favouritesOnly: false
    property bool hideRejected: false
    property string sortKey: "added"      // added | name | duration | rating
    readonly property bool filterActive: kindFilter !== "all" || favouritesOnly
                                         || hideRejected || sortKey !== "added"
    // Library indexes that match the search and the filter, in the sort
    // order, so selection keeps pointing at the real library index while
    // the grid is filtered.
    readonly property var matchingIndexes: {
        const needle = search.text.trim().toLowerCase()
        const out = []
        for (let i = 0; i < clips.length; ++i) {
            const e = clips[i]
            if(!binMatcher(e))continue
            const name = e.name !== undefined ? String(e.name) : ""
            if (needle !== "" && name.toLowerCase().indexOf(needle) < 0)
                continue
            if (kindFilter === "video" && e.audioOnly === true)
                continue
            if (kindFilter === "audio" && e.audioOnly !== true)
                continue
            if (favouritesOnly && e.favorite !== true)
                continue
            if (hideRejected && e.rejected === true)
                continue
            out.push(i)
        }
        if (sortKey === "name")
            out.sort(function (a, b) {
                return String(clips[a].name || "").localeCompare(String(clips[b].name || ""))
            })
        else if (sortKey === "duration")
            out.sort(function (a, b) { return MediaTime.durationSeconds(clips[b]) - MediaTime.durationSeconds(clips[a]) })
        else if (sortKey === "rating")
            out.sort(function (a, b) { return (clips[b].rating || 0) - (clips[a].rating || 0) })
        return out
    }
    // What the grid actually models: replaced only when the set or order
    // changes. A thumbnail or waveform landing re-announces the library,
    // and that must update tiles in place — a rebuilt tile takes its
    // DragHandler with it and drops a tile drag mid-gesture (the Media
    // page keeps its grid stable the same way).
    property var visibleIndexes: []
    onMatchingIndexesChanged: {
        if (matchingIndexes.join(",") !== visibleIndexes.join(","))
            visibleIndexes = matchingIndexes
    }

    // Positions in `timelines`, kept stable as the clip grid's are. The library
    // shows them all and a bin those filed in it; they are neither audio nor rated.
    readonly property var matchingTimelines: {
        const needle = search.text.trim().toLowerCase()
        const bin = bins[currentBin]
        const out = []
        if (kindFilter === "audio" || favouritesOnly)
            return out
        for (let i = 0; i < timelines.length; ++i)
            if ((!bin || !bin.id || timelines[i].binId === bin.id)
                && (needle === "" || String(timelines[i].name).toLowerCase().indexOf(needle) >= 0))
                out.push(i)
        return out
    }
    property var visibleTimelines: []
    onMatchingTimelinesChanged: {
        if (matchingTimelines.join(",") !== visibleTimelines.join(","))
            visibleTimelines = matchingTimelines
    }

    function durationLabel(entry) {
        if (entry.dur !== undefined) return entry.dur
        // The same reading of a length as everywhere else (1.20 s, 1:23.40).
        return MediaTime.lengthText(entry)
    }
    function resLabel(entry) {
        if (entry.res !== undefined) return entry.res
        return MediaTime.pictureText(entry)
    }

    color: Theme.panelBg

    PanelHeader {
        id: tabs
        width: parent.width
        anchors.top: parent.top
        tabs: [qsTr("Project"), qsTr("Effects"), qsTr("Titles")]
        // Two-way: a click reports through activated; programmatic tab changes
        // (badges, shortcuts, Match Frame) are mirrored back into the header.
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        Connections { target: root; function onCurrentTabChanged() { tabs.currentIndex = root.currentTab } }
    }

    // ── Effects / Transitions / Titles: a tile grid over the catalogue ─────
    Item {
        id: cataloguePane
        anchors.top: tabs.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.currentTab !== 0

        SearchField {
            id: catalogueSearch
            tip: qsTr("Filter the catalogue below by name as you type")
            anchors.top: parent.top
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            placeholder: root.currentTab === 1 ? qsTr("Search effects…")
                       : root.currentTab === 2 ? qsTr("Search titles…")
                       : qsTr("Search titles…")
        }

        // The shelf: All · ★ Favourites · Recent · each custom bin · +
        Flow {
            id: shelfRow
            objectName: "effectShelf"
            anchors.top: catalogueSearch.bottom
            anchors.topMargin: root.currentTab === 1 ? Theme.s2 : 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            spacing: Theme.s1
            visible: root.currentTab === 1
            height: visible ? implicitHeight : 0

            ComboField {
                objectName: "effectCategory"
                tip: qsTr("Show effects from one category")
                width: Math.min(180, shelfRow.width)
                caption: ""
                model: root.effectCategories.map(function(c) { return c.name })
                currentIndex: root.effectCategories.findIndex(function(c) { return c.key === root.effectCategory })
                onActivated: function(i) { root.effectCategory = root.effectCategories[i].key }
                Accessible.name: qsTr("Effect category")
            }

            Chip {
                objectName: "shelf:all"
                tip: qsTr("Show every effect in the catalogue")
                label: qsTr("All")
                selected: root.effectShelf === "all"
                onClicked: root.effectShelf = "all"
            }
            Chip {
                objectName: "shelf:favorites"
                tip: qsTr("Show only the effects you have starred")
                label: "★ " + qsTr("Favourites") + (root.favoriteKeys.length > 0 ? " " + root.favoriteKeys.length : "")
                selected: root.effectShelf === "favorites"
                onClicked: root.effectShelf = "favorites"
            }
            Chip {
                objectName: "shelf:recent"
                tip: qsTr("Show the effects you applied most recently")
                label: qsTr("Recent") + (root.recentKeys.length > 0 ? " " + root.recentKeys.length : "")
                selected: root.effectShelf === "recent"
                onClicked: root.effectShelf = "recent"
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: recentMenu.popup()
                }
                ContextMenu {
                    id: recentMenu
                    MenuAction { text: qsTr("Clear recent effects"); enabled: root.recentKeys.length > 0; onTriggered: root.recentEffectsCleared() }
                }
            }
            Repeater {
                model: root.effectBins
                delegate: Chip {
                    id: binChip
                    required property var modelData
                    objectName: "shelf:bin:" + modelData.name
                    tip: qsTr("Show only the effects filed in this bin")
                    label: modelData.name + (modelData.keys !== undefined && modelData.keys.length > 0 ? " " + modelData.keys.length : "")
                    selected: root.effectShelf === "bin:" + modelData.name
                    onClicked: root.effectShelf = "bin:" + modelData.name
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: binMenu.popup()
                    }
                    ContextMenu {
                        id: binMenu
                        MenuAction { text: qsTr("Rename bin…"); iconName: "pencil"; onTriggered: binPrompt.openFor("rename", binChip.modelData.name, "") }
                        MenuAction { text: qsTr("Delete bin “%1”").arg(binChip.modelData.name); iconName: "trash-2"; onTriggered: root.effectBinDeleted(binChip.modelData.name) }
                    }
                }
            }
            IconButton {
                objectName: "newEffectBinButton"
                iconName: "folder-plus"
                text: qsTr("New effect bin")
                implicitWidth: 18
                implicitHeight: 18
                onClicked: binPrompt.openFor("new", "", "")
            }
        }

        EmptyState {
            anchors.fill: tileGrid
            visible: root.catalogueRows.length === 0
            iconName: root.currentTab === 1 && root.effectShelf !== "all" && catalogueSearch.text === ""
                      ? (root.effectShelf === "favorites" ? "star" : root.effectShelf === "recent" ? "clock" : "folder") : "search"
            title: root.currentTab === 1 && root.effectShelf === "favorites" && catalogueSearch.text === "" ? qsTr("No favourites yet")
                 : root.currentTab === 1 && root.effectShelf === "recent" && catalogueSearch.text === "" ? qsTr("Nothing used yet")
                 : root.currentTab === 1 && root.shelfBin !== "" && catalogueSearch.text === "" ? qsTr("Empty bin")
                 : qsTr("No matches")
            description: root.currentTab === 1 && root.effectShelf === "favorites" && catalogueSearch.text === ""
                         ? qsTr("Click the star on an effect to keep it here.")
                         : root.currentTab === 1 && root.effectShelf === "recent" && catalogueSearch.text === ""
                           ? qsTr("Effects you apply appear here, latest first.")
                           : root.currentTab === 1 && root.shelfBin !== "" && catalogueSearch.text === ""
                             ? qsTr("Right-click an effect ▸ Add to “%1”.").arg(root.shelfBin)
                             : qsTr("Nothing here contains “%1”.").arg(catalogueSearch.text)
        }

        // The bin name prompt (new / rename).
        C.Popup {
            id: binPrompt
            objectName: "effectBinPrompt"
            property string mode: "new"
            property string binName: ""
            property string withKey: ""
            function openFor(m, name, key) {
                mode = m
                binName = name
                withKey = key
                binNameField.text = m === "rename" ? name : ""
                open()
                binNameField.forceActiveFocus()
                binNameField.selectAll()
            }
            function commit() {
                const name = binNameField.text.trim()
                if (name === "")
                    return
                if (mode === "rename")
                    root.effectBinRenamed(binName, name)
                else
                    root.effectBinCreated(name, withKey)
                close()
            }
            parent: cataloguePane
            x: Theme.s2
            y: shelfRow.y + shelfRow.height + Theme.s2
            width: cataloguePane.width - Theme.s2 * 2
            padding: Theme.s2
            modal: true
            focus: true
            closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutside
            background: Rectangle {
                radius: Theme.rControl
                color: Theme.panelRaised
                border.width: Theme.hairline
                border.color: Theme.accent
            }
            contentItem: Column {
                spacing: Theme.s2
                Text {
                    text: binPrompt.mode === "rename" ? qsTr("Rename bin") : qsTr("New effect bin")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textSecondary
                }
                C.TextField {
                    id: binNameField
                    objectName: "effectBinNameField"
                    hoverEnabled: true
                    C.ToolTip.text: qsTr("Name for this effect bin; press Enter to confirm"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450
                    width: parent.width
                    placeholderText: qsTr("Bin name")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsControl
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textMuted
                    background: Rectangle {
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: Theme.hairline
                        border.color: binNameField.activeFocus ? Theme.accent : Theme.border
                    }
                    onAccepted: binPrompt.commit()
                }
                Row {
                    spacing: Theme.s2
                    anchors.right: parent.right
                    ToolButton { text: qsTr("Cancel"); showLabel: true; onClicked: binPrompt.close() }
                    ToolButton {
                        objectName: "effectBinOkButton"
                        text: binPrompt.mode === "rename" ? qsTr("Rename") : qsTr("Create")
                        showLabel: true
                        enabled: binNameField.text.trim() !== ""
                        onClicked: binPrompt.commit()
                    }
                }
            }
        }

        GridView {
            id: tileGrid
            anchors.top: shelfRow.bottom
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: hint.top
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            clip: true
            focus: false
            readonly property int columns: Math.max(1, Math.floor(width / 128))
            cellWidth: Math.floor(width / columns)
            cellHeight: 66
            model: root.catalogueRows
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: tile
                required property int index
                required property var modelData
                readonly property string kind: root.currentTab === 2 ? "title"
                                             : modelData.cat === "transitions" ? "transition" : "effect"
                // A transition is dragged to a cut, so what is selected now does not matter.
                readonly property bool ok: kind === "transition" ? true
                                           : root.applicability[modelData.key] !== undefined
                                           ? root.applicability[modelData.key] === true
                                           : modelData.ok === undefined || modelData.ok === true
                objectName: "catalogueTile:" + modelData.key
                width: GridView.view.cellWidth - Theme.s2
                height: GridView.view.cellHeight - Theme.s2

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.rControl
                    color: tileHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
                    border.width: Theme.hairline
                    border.color: Theme.border
                    opacity: tile.ok ? 1 : Theme.disabledOpacity
                }
                Icon {
                    id: tileIcon
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.s2
                    anchors.top: parent.top
                    anchors.topMargin: Theme.s2
                    name: tile.modelData.icon !== undefined ? tile.modelData.icon
                        : root.catIcon(tile.modelData.cat)
                    size: Theme.szIcon
                    color: tile.kind === "title" ? Theme.clipTitle
                         : tile.kind === "transition" ? Theme.accent : Theme.textSecondary
                    opacity: tile.ok ? 1 : Theme.disabledOpacity
                }
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.s2
                    text: tile.modelData.name
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: tile.ok ? Theme.textPrimary : Theme.textMuted
                }

                HoverHandler { id: tileHover }
                Tooltip {
                    visible: tileHover.hovered && !starHover.hovered
                    text: tile.kind === "title"
                          ? qsTr("Double-click to add a title, or drag it onto the timeline")
                          : tile.ok ? qsTr("Double-click to apply to the selected clip, or drag it onto a clip")
                                    : qsTr("Select a clip this applies to, or drag it onto one")
                }
                // Favourite star: lit when kept, shown on hover otherwise.
                Item {
                    id: star
                    objectName: "tileStar"
                    visible: tile.kind === "effect" && (root.isFavorite(tile.modelData.key) || tileHover.hovered)
                    anchors.right: parent.right
                    anchors.top: parent.top
                    width: Theme.szIcon + Theme.s2
                    height: Theme.szIcon + Theme.s2
                    readonly property bool lit: root.isFavorite(tile.modelData.key)
                    Icon {
                        anchors.centerIn: parent
                        name: "star"
                        size: Theme.szIcon - 2
                        color: star.lit ? Theme.accent : Theme.textMuted
                        opacity: star.lit ? 1 : (starHover.hovered ? 1 : 0.6)
                    }
                    HoverHandler { id: starHover }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.effectFavoriteToggled(tile.modelData.key)
                    }
                    Tooltip {
                        visible: starHover.hovered
                        text: star.lit ? qsTr("Remove from favourites") : qsTr("Add to favourites")
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    enabled: tile.kind === "effect"
                    onTapped: tileMenu.popup()
                }
                ContextMenu {
                    id: tileMenu
                    MenuAction {
                        text: root.isFavorite(tile.modelData.key) ? qsTr("Remove from Favourites") : qsTr("Add to Favourites")
                        iconName: "star"
                        onTriggered: root.effectFavoriteToggled(tile.modelData.key)
                    }
                    MenuAction {
                        visible: root.shelfBin !== ""
                        height: visible ? implicitHeight : 0
                        text: qsTr("Remove from “%1”").arg(root.shelfBin)
                        iconName: "minus"
                        onTriggered: root.effectBinKeyRemoved(root.shelfBin, tile.modelData.key)
                    }
                    Instantiator {
                        model: root.effectBins
                        delegate: MenuAction {
                            required property var modelData
                            text: root.binHas(modelData.name, tile.modelData.key)
                                  ? qsTr("In “%1”").arg(modelData.name) : qsTr("Add to “%1”").arg(modelData.name)
                            iconName: "folder"
                            enabled: !root.binHas(modelData.name, tile.modelData.key)
                            onTriggered: root.effectBinKeyAdded(modelData.name, tile.modelData.key)
                        }
                        onObjectAdded: function (index, object) { tileMenu.insertItem(2 + index, object) }
                        onObjectRemoved: function (index, object) { tileMenu.removeItem(object) }
                    }
                    MenuAction {
                        text: qsTr("New bin with this…")
                        iconName: "folder-plus"
                        onTriggered: binPrompt.openFor("new", "", tile.modelData.key)
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        if (tile.kind === "title")
                            root.titleRequested(tile.modelData.key)
                        else if (tile.kind === "transition")
                            root.transitionApplied(tile.modelData.key)
                        else
                            root.effectApplied(tile.modelData.key)
                    }
                }
                DragHandler {
                    id: tileDrag
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: {
                        // Not dragActive: the new-track rows take clips only, and
                        // showing them would shift the timeline under this drag.
                        if (active) {
                            tileGhost.kind = tile.kind
                            tileGhost.key = tile.modelData.key
                            tileGhost.name = tile.modelData.name
                            const host = tile.Window.window ? tile.Window.window.contentItem : root
                            tileGhost.parent = host
                            tileGhost.x = Qt.binding(function () {
                                return host.mapFromItem(null, tileDrag.centroid.scenePosition).x
                                       - tileGhost.width / 2
                            })
                            tileGhost.y = Qt.binding(function () {
                                return host.mapFromItem(null, tileDrag.centroid.scenePosition).y
                                       - tileGhost.height / 2
                            })
                            tileGhost.Drag.active = true
                        } else {
                            tileGhost.Drag.drop()
                            tileGhost.Drag.active = false
                            tileGhost.x = 0
                            tileGhost.y = 0
                            tileGhost.parent = root
                        }
                    }
                }
            }
        }

        Text {
            id: hint
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.s2
            text: root.currentTab === 2 ? qsTr("Double-click adds a title; drag one onto the timeline")
                                        : qsTr("Drag onto a clip, or a transition onto a cut; double-click applies to the selection")
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    // ── Bin tree ────────────────────────────────────────────────────────────
    Item {
        id: binsPane
        anchors.top: tabs.bottom
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        // The bin tree yields first when the panel is narrow; the grid is
        // what the editor is actually looking at.
        width: Math.min(168, Math.round(root.width * 0.42))
        visible: root.currentTab === 0

        SearchField {
            id: search
            tip: qsTr("Filter the project's clips by file or clip name")
            anchors.top: parent.top
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            placeholder: qsTr("Search media…")
        }

        Text {
            id: binsLabel
            anchors.top: search.bottom
            anchors.topMargin: Theme.s3
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            text: qsTr("Bins")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }

        IconButton {
            iconName: "plus"
            objectName: "mediaNewBin"
            text: qsTr("New bin…")
            onClicked: root.binAddRequested()
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            anchors.verticalCenter: binsLabel.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
        }

        Column {
            anchors.top: binsLabel.bottom
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            Repeater {
                model: root.bins

                delegate: Rectangle {
                    id: binRow
                    required property int index
                    required property var modelData

                    objectName: "mediaBinRow:" + index
                    width: parent.width
                    height: Theme.hRow + Theme.s1
                    readonly property bool selected: index === root.currentBin
                    color: selected || binDrop.containsDrag ? Theme.controlBg
                         : binHover.hovered ? Theme.hovered(Theme.panelBg)
                         : "transparent"

                    HoverHandler { id: binHover }
                    TapHandler { onTapped: root.binActivated(binRow.index) }
                    // A smart bin holds what its rules find, so nothing is filed in it.
                    DropArea {
                        id: binDrop
                        anchors.fill: parent
                        enabled: binRow.modelData.smart !== true
                        keys: ["omaedit/clip"]
                        onDropped: function (drop) {
                            if (drop.source.sequenceIndex >= 0)
                                root.timelineFiled(drop.source.sequenceIndex, binRow.modelData.id)
                            else
                                root.clipFiled(drop.source.libraryIndex, binRow.modelData.id)
                            drop.accept()
                        }
                    }

                    Icon {
                        id: binIcon
                        name: "folder"
                        size: 14
                        color: parent.selected ? Theme.accent : Theme.textMuted
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s4
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        anchors.left: binIcon.right
                        anchors.leftMargin: Theme.s2
                        anchors.right: count.left
                        anchors.rightMargin: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        text: binRow.modelData.name
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: parent.selected ? Theme.textPrimary : Theme.textSecondary
                    }

                    Text {
                        id: count
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        text: binRow.modelData.count
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: parent.selected ? Theme.accent : Theme.textMuted
                    }
                }
            }
        }
    }

    Rectangle {
        id: seam
        anchors.left: binsPane.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        width: Theme.hairline
        color: Theme.border
        visible: root.currentTab === 0
    }

    // ── Thumbnail grid ──────────────────────────────────────────────────────
    Item {
        anchors.left: seam.right
        anchors.right: parent.right
        anchors.top: tabs.bottom
        anchors.bottom: parent.bottom
        visible: root.currentTab === 0

        Row {
            id: gridTools
            anchors.top: parent.top
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            spacing: Theme.s2

            IconButton {
                iconName: "download"
                text: qsTr("Import media")
                shortcut: "Ctrl+I"
                onClicked: root.importRequested()
            }
            IconButton {
                id: filterButton
                objectName: "mediaFilterButton"
                iconName: "filter"
                text: qsTr("Filter and sort")
                checkable: true
                checked: root.filterActive
                // A click toggles `checked`; the menu, not the click, decides.
                onClicked: { checked = Qt.binding(() => root.filterActive); filterMenu.popup(filterButton, 0, filterButton.height) }
                ContextMenu {
                    id: filterMenu
                    MenuAction { text: qsTr("All media"); checkable: true; checked: root.kindFilter === "all"; onTriggered: { root.kindFilter = "all"; checked = Qt.binding(() => root.kindFilter === "all") } }
                    MenuAction { text: qsTr("Video only"); checkable: true; checked: root.kindFilter === "video"; onTriggered: { root.kindFilter = "video"; checked = Qt.binding(() => root.kindFilter === "video") } }
                    MenuAction { text: qsTr("Audio only"); checkable: true; checked: root.kindFilter === "audio"; onTriggered: { root.kindFilter = "audio"; checked = Qt.binding(() => root.kindFilter === "audio") } }
                    MenuAction { text: qsTr("Favourites only"); checkable: true; checked: root.favouritesOnly; onTriggered: { root.favouritesOnly = !root.favouritesOnly; checked = Qt.binding(() => root.favouritesOnly) } }
                    MenuAction { text: qsTr("Hide rejected"); checkable: true; checked: root.hideRejected; onTriggered: { root.hideRejected = !root.hideRejected; checked = Qt.binding(() => root.hideRejected) } }
                    MenuAction { text: qsTr("Sort: as added"); checkable: true; checked: root.sortKey === "added"; onTriggered: { root.sortKey = "added"; checked = Qt.binding(() => root.sortKey === "added") } }
                    MenuAction { text: qsTr("Sort: name"); checkable: true; checked: root.sortKey === "name"; onTriggered: { root.sortKey = "name"; checked = Qt.binding(() => root.sortKey === "name") } }
                    MenuAction { text: qsTr("Sort: longest first"); checkable: true; checked: root.sortKey === "duration"; onTriggered: { root.sortKey = "duration"; checked = Qt.binding(() => root.sortKey === "duration") } }
                    MenuAction { text: qsTr("Sort: rating"); checkable: true; checked: root.sortKey === "rating"; onTriggered: { root.sortKey = "rating"; checked = Qt.binding(() => root.sortKey === "rating") } }
                }
            }
        }

        Row {
            anchors.verticalCenter: gridTools.verticalCenter
            anchors.left: gridTools.right
            anchors.leftMargin: Theme.s2
            spacing: Theme.s1
            visible: root.bins.length > 0

            Icon {
                name: "chevron-down"
                size: Theme.szIcon
                color: Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: root.bins.length > root.currentBin && root.currentBin >= 0
                      ? root.bins[root.currentBin].name : ""
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textPrimary
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        EmptyState {
            anchors.fill: grid
            anchors.topMargin: grid.headerItem ? grid.headerItem.height : 0
            // Over the grid: the empty grid lies on top of what is declared
            // before it and took the button's press for a flick of its own.
            z: 1
            visible: root.clips.length === 0
            iconName: "film"
            title: qsTr("No media yet")
            description: qsTr("Import video to start editing.")
            actionText: qsTr("Import Media")
            onActionTriggered: root.importRequested()
        }

        EmptyState {
            anchors.fill: grid
            visible: root.clips.length > 0 && root.visibleIndexes.length === 0
                     && root.visibleTimelines.length === 0
            iconName: "search"
            title: qsTr("No matches")
            description: search.text !== "" ? qsTr("No clip name contains “%1”.").arg(search.text)
                                            : qsTr("Nothing passes the filter — loosen it with the funnel.")
        }

        // A right-click on the bin itself (not on a clip or a timeline in
        // it) asks for the bin's own menu.
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function (eventPoint) {
                const p = grid.mapFromItem(parent, eventPoint.position.x, eventPoint.position.y)
                if (p.x < 0 || p.y < 0 || p.x >= grid.width || p.y >= grid.height)
                    return
                if (grid.itemAt(p.x + grid.contentX, p.y + grid.contentY))
                    return
                const head = grid.headerItem
                if (head) {
                    const h = head.mapFromItem(grid, p.x, p.y)
                    if (head.childAt(h.x, h.y))
                        return
                }
                root.binContextRequested()
            }
        }

        GridView {
            id: grid
            objectName: "mediaGrid"
            anchors.top: gridTools.bottom
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footerBar.top
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            clip: true
            focus: false
            // Column count follows the width rather than being fixed at two:
            // a hard two-up collapses into unreadable slivers as soon as the
            // panel is dragged near its minimum.
            readonly property bool listMode: root.viewMode === 2
            readonly property int minCell: root.viewMode === 0 ? 210 : 150
            readonly property int columns: listMode ? 1 : Math.max(1, Math.floor(width / minCell))
            cellWidth: Math.floor(width / columns)
            cellHeight: listMode ? Theme.hRow * 2 + Theme.s2
                                 : Math.round(cellWidth * 0.5625) + 40
            model: root.visibleIndexes
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar { atRest: true } // the library grid says there is more, as a box does

            // The project's timelines come first and scroll with the clips,
            // at the same size. Double-click opens one; drag it onto a track
            // to use it as a clip.
            header: Flow {
                width: grid.width
                // The view holds its clips still as this grows, which would
                // leave the earlier timelines scrolled away above it.
                onHeightChanged: Qt.callLater(grid.positionViewAtBeginning)
                Repeater {
                    model: root.visibleTimelines
                    delegate: Item {
                        id: timelineCell
                        required property int modelData
                        readonly property var item: modelData < root.timelines.length
                                                    ? root.timelines[modelData] : ({})
                        objectName: "mediaTimeline:" + modelData
                        width: grid.cellWidth
                        height: grid.cellHeight
                        TimelineTile {
                            width: parent.width - Theme.s2
                            height: parent.height - Theme.s2
                            item: timelineCell.item
                            selected: item.id === root.currentTimeline && root.currentIndex < 0
                            listMode: grid.listMode
                            // The open timeline cannot go inside itself.
                            draggable: item.active !== true
                            onActivated: root.timelineSelected(item.id)
                            onOpened: root.timelineOpened(item.index)
                            onContextRequested: root.timelineContextRequested(item.index)
                            onDragChanged: function (active, handler) {
                                root.dragActive = active
                                if (active) {
                                    dragGhost.libraryIndex = -1
                                    dragGhost.sequenceIndex = item.index
                                    dragGhost.name = item.name
                                    dragGhost.thumb = item.thumb
                                    dragGhost.frames = item.frames
                                    const host = timelineCell.Window.window ? timelineCell.Window.window.contentItem : root
                                    dragGhost.parent = host
                                    dragGhost.x = Qt.binding(function () {
                                        return host.mapFromItem(null, handler.centroid.scenePosition).x
                                               - dragGhost.width / 2
                                    })
                                    dragGhost.y = Qt.binding(function () {
                                        return host.mapFromItem(null, handler.centroid.scenePosition).y
                                               - dragGhost.height / 2
                                    })
                                    dragGhost.Drag.active = true
                                } else {
                                    dragGhost.Drag.drop()
                                    dragGhost.Drag.active = false
                                    dragGhost.x = 0
                                    dragGhost.y = 0
                                    dragGhost.parent = root
                                }
                            }
                        }
                    }
                }
            }

            delegate: Item {
                id: card
                required property int index
                required property int modelData
                readonly property int libraryIndex: modelData
                // Live: a new thumbnail shows without a rebuild.
                readonly property var entry: libraryIndex < root.clips.length
                                             ? root.clips[libraryIndex] : ({})
                readonly property bool listMode: GridView.view.listMode

                objectName: "mediaCard:" + libraryIndex
                width: GridView.view.cellWidth - Theme.s2
                height: GridView.view.cellHeight - Theme.s2
                readonly property bool selected: card.libraryIndex === root.currentIndex
                // Native range count is useful for logging; a drop preview
                // asks the live host for its duration at the project rate.
                readonly property int rangeFrames: MediaTime.rangeFrames(entry)

                Rectangle {
                    anchors.fill: parent
                    color: card.selected ? Theme.controlBg
                         : cardHover.hovered ? Theme.hovered(Theme.panelBg)
                         : "transparent"
                    radius: Theme.rControl
                    border.width: card.selected ? Theme.selectionRing : 0
                    border.color: Theme.accent
                }

                HoverHandler { id: cardHover }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: root.clipActivated(card.libraryIndex)
                    onDoubleTapped: root.clipOpened(card.libraryIndex)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.clipContextRequested(card.libraryIndex)
                }
                // Dragging a tile carries the clip to the timeline: the ghost
                // follows the pointer at window level (so it rides over every
                // panel) while the tile stays put.
                DragHandler {
                    id: cardDrag
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: {
                        if (active)
                            root.beginClipDrag(card.libraryIndex, cardDrag, card, "")
                        else
                            root.endClipDrag(cardDrag)
                    }
                    onCanceled: root.breakClipDrag()
                }

                // Poster frame; a tonal plate until (or unless) one exists.
                Rectangle {
                    id: thumb
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: Theme.s1
                    width: card.listMode ? Math.round(height * 16 / 9) : parent.width - Theme.s1 * 2
                    height: card.listMode ? parent.height - Theme.s1 * 2 : parent.height - 40
                    radius: Theme.rControl
                    clip: true
                    readonly property real tone: card.entry.tone !== undefined ? card.entry.tone : 0.4
                    gradient: Gradient {
                        GradientStop {
                            position: 0.0
                            color: Qt.hsla(0.55, 0.18, 0.16 + thumb.tone * 0.16, 1)
                        }
                        GradientStop {
                            position: 1.0
                            color: Qt.hsla(0.55, 0.22, 0.09 + thumb.tone * 0.10, 1)
                        }
                    }

                    Image {
                        anchors.fill: parent
                        source: card.entry.thumb !== undefined ? card.entry.thumb : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }

                    Icon {
                        anchors.centerIn: parent
                        visible: card.entry.thumb === undefined || card.entry.thumb === ""
                        name: "film"
                        size: 18
                        color: Qt.rgba(1, 1, 1, 0.18)
                    }
                    // A multicam clip wears its angle count.
                    Rectangle {
                        visible: card.entry.multicam === true
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.s1
                        width: mcBadge.implicitWidth + Theme.s2
                        height: 14
                        radius: 2
                        color: Theme.accent
                        Text {
                            id: mcBadge
                            anchors.centerIn: parent
                            text: card.entry.angles !== undefined
                                  ? qsTr("MULTICAM ×%1").arg(card.entry.angles.length) : qsTr("MULTICAM")
                            font.family: Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Theme.wHeading
                            color: Theme.accentText
                        }
                    }

                    // Marked I/O range across the source duration.
                    Rectangle {
                        readonly property int rIn: card.entry.rangeIn !== undefined ? card.entry.rangeIn : -1
                        readonly property int rOut: card.entry.rangeOut !== undefined ? card.entry.rangeOut : -1
                        readonly property int frames: Math.max(1, MediaTime.sourceFrames(card.entry))
                        visible: rIn >= 0 || rOut >= 0
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 3
                        color: Qt.rgba(0, 0, 0, 0.5)

                        Rectangle {
                            x: (parent.rIn >= 0 ? parent.rIn : 0) / parent.frames * parent.width
                            width: Math.max(2, (((parent.rOut >= 0 ? parent.rOut : parent.frames - 1) + 1)
                                                - (parent.rIn >= 0 ? parent.rIn : 0))
                                               / parent.frames * parent.width)
                            height: parent.height
                            color: Theme.accent
                        }
                    }
                }

                Text {
                    id: clipName
                    anchors.top: card.listMode ? undefined : thumb.bottom
                    anchors.topMargin: Theme.s1
                    anchors.left: card.listMode ? thumb.right : thumb.left
                    anchors.leftMargin: card.listMode ? Theme.s2 : 0
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.s2
                    anchors.verticalCenter: card.listMode ? parent.verticalCenter : undefined
                    anchors.verticalCenterOffset: card.listMode ? -Theme.s2 : 0
                    text: card.entry.name !== undefined ? card.entry.name : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: card.selected ? Theme.textPrimary : Theme.textSecondary
                    elide: Text.ElideMiddle
                }

                Row {
                    anchors.top: clipName.bottom
                    anchors.left: clipName.left
                    spacing: Theme.s2

                    Text {
                        text: root.durationLabel(card.entry)
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    Text {
                        visible:card.entry.proxy === true
                        text:qsTr("PROXY")
                        font.family:Theme.monoFamily;font.pixelSize:Theme.fsLabel;color:Theme.accent
                    }
                    Text { // the file is gone — the Media page relinks
                        visible: card.entry.offline === true
                        text: qsTr("OFFLINE")
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        font.weight: Theme.wHeading
                        color: Theme.danger
                    }
                    Text {
                        text: root.resLabel(card.entry)
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                }
            }
        }

        Rectangle {
            id: footerBar
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.hRow + Theme.s2
            color: Theme.panelBg

            SegmentedControl {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                tip: qsTr("How clips are laid out: large thumbnails, small thumbnails or a list")
                icons: ["grid-2x2", "layout-grid", "list"]
                names: [qsTr("Large grid"), qsTr("Grid"), qsTr("List")]
                currentIndex: root.viewMode
                onActivated: function (i) { root.viewMode = i }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                readonly property int shown: root.visibleIndexes.length + root.visibleTimelines.length
                readonly property int total: root.clips.length + root.timelines.length
                text: shown !== total ? qsTr("%1 of %2").arg(shown).arg(total)
                      : total === 1 ? qsTr("1 item")
                      : qsTr("%1 items").arg(total)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    // The catalogue tile ghost: kind (effect | transition | title) + key,
    // under the "omaedit/effect" payload key.
    Item {
        id: tileGhost
        property string kind: ""
        property string key: ""
        property string name: ""
        width: Theme.s5 * 5
        height: Theme.s5 * 2
        visible: Drag.active
        z: 1000
        Drag.keys: ["omaedit/effect"]
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2

        Rectangle {
            anchors.fill: parent
            radius: Theme.rControl
            color: Theme.controlBg
            border.width: Theme.hairline
            border.color: tileGhost.kind === "title" ? Theme.clipTitle : Theme.accent
            opacity: 0.92
            Icon {
                id: ghostIcon
                anchors.left: parent.left
                anchors.leftMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                name: tileGhost.kind === "title" ? "type"
                    : tileGhost.kind === "transition" ? "arrow-left-right" : "sparkles"
                color: tileGhost.kind === "title" ? Theme.clipTitle : Theme.accent
            }
            Text {
                anchors.left: ghostIcon.right
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.s2
                text: tileGhost.name
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textPrimary
            }
        }
    }

    // The drag ghost. Carries the library index and the length a drop
    // places, so a DropArea can act on `drag.source` without asking the
    // panel anything. Key "omaedit/clip" names the payload.
    Item {
        id: dragGhost
        objectName: "mediaDragPreview"
        property int libraryIndex: -1
        // A timeline being carried as a clip; -1 for a library clip.
        property int sequenceIndex: -1
        property string name: ""
        property string thumb: ""
        // A second line under the name: the length of a selection carried
        // from the Source viewer.
        property string detail: ""
        property int frames: 0
        width: Theme.s5 * 5
        height: Theme.s5 * 3
        visible: Drag.active
        z: 1000
        Drag.keys: ["omaedit/clip"]
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2

        Rectangle {
            anchors.fill: parent
            radius: Theme.rControl
            color: Theme.controlBg
            border.width: Theme.hairline
            border.color: Theme.accent
            // Over a track the clip shows where it lands; this must not hide it.
            opacity: dragGhost.Drag.target ? 0.3 : 0.92
            clip: true
            Image {
                anchors.fill: parent
                anchors.margins: Theme.hairline
                source: dragGhost.thumb
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Icon {
                visible: dragGhost.thumb === ""
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -Theme.s2
                name: "film"
                color: Theme.accent
            }
            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.s1
                anchors.bottomMargin: dragGhost.detail !== "" ? Theme.s1 + ghostDetail.implicitHeight : Theme.s1
                text: dragGhost.name
                elide: Text.ElideMiddle
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textPrimary
            }
            Text {
                id: ghostDetail
                objectName: "mediaDragDetail"
                visible: dragGhost.detail !== ""
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.s1
                text: dragGhost.detail
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.accent
            }
        }
    }
}
