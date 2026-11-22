pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import QtQuick.Controls as C
import OmaEdit.Ui

// The Media page. Pure: it knows the shape of the library and the source
// player, never the engine. MediaPage.qml binds it to Backend. Filtering,
// sorting, paging and selection live here because they are view state.
Item {
    id: root
    property var layoutStore: null
    property alias layoutController: layout
    WorkspaceLayout { id: layout; host: root; store: root.layoutStore; workspaceKey: "Media" }

    // ── Inputs ──────────────────────────────────────────────────────────────
    property var clips: []
    property var timelines: []                  // Backend.timelineItems
    property string currentTimeline: ""
    property var drives: []
    signal driveMountRequested(var drive)
    signal driveContextRequested(var drive)
    property var bins: []
    property var locations: []
    property var watches: []
    property var providers: []
    signal providerAddRequested()
    signal providerContextRequested(string id)
    property var metadataSchema: []
    signal customEdited(var indexes, string key, string value)
    signal watchAddRequested()
    signal watchContextRequested(string id)
    property var ruleMatcher: function (entry, rules) { return true }
    property var advancedRules: ({})
    property var viewerSettings: ({})
    signal viewerZoomRequested(int index)
    property bool externalTextActive: false
    property bool skimEnabled: false
    property bool skimAudio: false
    property bool collapseStacks: true
    property var skimResult: ({})
    property bool gangEnabled: false
    signal gangRequested(bool on)
    signal skimmed(int index, int frame, bool audio)
    signal filmRangeSelected(int index, int first, int last)
    readonly property var stackCounts: {
        var counts = {}
        clips.forEach(function (e) { if (e.stackId) counts[e.stackId] = (counts[e.stackId] || 0)+1 })
        return counts
    }
    signal binRequested(string id, bool smart, bool assign)
    signal binContextRequested(string id)
    signal locationAddRequested()
    signal locationContextRequested(int index)
    signal filtersRequested()
    property var recentProjects: []
    property string currentProject: ""
    property string projectName: ""
    property bool busy: false
    property string playbackMode: "preferred"
    signal playbackRequested(string mode)
    signal inputSettingsRequested()
    property var usedPaths: ({})
    property real fpsFallback: 25
    // The previewed clip and its player.
    property int sourceIndex: -1
    property int sourcePosition: 0
    property int sourceDuration: 0
    property string sourceTimecode: "00:00:00:00"
    property string sourceDurationText: "00:00:00:00"
    property bool sourcePlaying: false
    property real sourceAspect: 16 / 9
    property Component videoSurface: null

    // ── Selection (owned here, readable outside) ────────────────────────────
    property var selection: []
    property int currentIndex: -1
    property int anchorIndex: -1

    // ── Outputs ─────────────────────────────────────────────────────────────
    signal operationRequested(string kind)
    signal syncRequested()
    signal snapshotRequested()
    signal monitorVolumeRequested()
    signal fullscreenSourceRequested()
    signal tasksRequested()
    signal importRequested()
    signal importFolderRequested()
    signal urlsDropped(var urls)
    signal openProjectRequested()
    signal projectActivated(string path)
    signal clipActivated(int index)             // preview it
    signal clipOpened(int index)                // take it to the Edit page
    signal clipContextRequested(int index)
    signal timelineOpened(int index)
    signal timelineContextRequested(int index)
    signal ratingEdited(var indexes, int rating)
    signal favoriteEdited(var indexes, bool on)
    signal rejectedEdited(var indexes, bool on)
    signal tagEdited(var indexes, string tag, bool on)
    signal keywordsEdited(int index, var keywords)
    signal notesEdited(int index, string notes)
    signal fieldEdited(int index, string key, string value)
    signal renameRequested(int index, string name)
    signal markerAdded(int index, int frame, string type, string text)
    signal markerRemoved(int index, int frame)
    signal markerEdited(int index, int oldFrame, int frame, string type, string text, string notes)
    signal batchFieldsEdited(var indexes, var fields)
    signal subclipRequested(int index)
    signal removeRequested(var indexes)
    signal attributesRequested()
    signal revealRequested(int index)
    signal rangeInRequested(int frame)
    signal rangeOutRequested(int frame)
    signal rangeClearRequested()
    signal seekRequested(int frame)
    signal playToggled()
    signal paused()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal insertRequested(int index)
    signal overwriteRequested(int index)

    // The window stands its editing keys down while any field here is typed into.
    readonly property bool textActive: externalTextActive || moreMenu.visible || projectMenu.visible || filterBar.searchActive || logger.textActive || inspector.textActive || preview.textActive

    readonly property var primaryEntry: currentIndex >= 0 && currentIndex < clips.length ? clips[currentIndex] : ({})
    readonly property var sourceEntry: sourceIndex >= 0 && sourceIndex < clips.length ? clips[sourceIndex] : ({})

    // Narrow-width policy: the inspector folds first, then the sidebar.
    readonly property bool inspectorVisible: width >= 1380
    readonly property bool sidebarVisible: width >= 1100

    Settings {
        category: "media"
        property alias viewMode: filterBar.viewMode
        property alias thumbSize: filterBar.thumbSize
        property alias sortKey: filterBar.sortKey
        property alias sortDescending: filterBar.sortDescending
        property alias perPage: root.perPage
        property alias skimEnabled: root.skimEnabled
        property alias skimAudio: root.skimAudio
        property alias collapseStacks: root.collapseStacks
    }

    // ── Filtering ───────────────────────────────────────────────────────────
    property string filterKind: ""
    property string filterValue: ""
    property int page: 0
    property int perPage: 24

    function ext(entry) {
        var p = entry.path !== undefined ? entry.path : ""
        var m = /\.([A-Za-z0-9]+)$/.exec(p)
        return m ? m[1].toUpperCase() : ""
    }
    function isRecent(entry) {
        if (entry.imported === undefined) return false
        var t = Date.parse(String(entry.imported))
        return !isNaN(t) && (Date.now() - t) < 24 * 3600 * 1000
    }
    function seconds(entry) {
        return MediaTime.durationSeconds(entry)
    }
    function smartMatch(key, entry) {
        switch (key) {
        case "all": return true
        case "recent": return isRecent(entry)
        case "rated": return entry.rating !== undefined && entry.rating >= 4
        case "proxies": return entry.proxy !== undefined && entry.proxy
        case "uhd": return MediaTime.hasPicture(entry) && !MediaTime.isStill(entry) && entry.width !== undefined && entry.width >= 3840
        case "long": return !MediaTime.isStill(entry) && seconds(entry) > 30
        case "noaudio": return entry.hasAudio !== undefined && !entry.hasAudio
        case "unused": return !(entry.path !== undefined && usedPaths[entry.path] === true)
        case "offline": return entry.offline !== undefined && entry.offline
        }
        return true
    }
    readonly property var smartBinDefs: [
        { key: "all", label: qsTr("All Clips"), icon: "film" },
        { key: "recent", label: qsTr("Recently Added"), icon: "clock" },
        { key: "rated", label: qsTr("High Rating"), icon: "star" },
        { key: "proxies", label: qsTr("Has Proxies"), icon: "copy" },
        { key: "uhd", label: qsTr("4K UHD"), icon: "monitor" },
        { key: "long", label: qsTr("Long Clips (> 30s)"), icon: "activity" },
        { key: "noaudio", label: qsTr("No Audio"), icon: "volume-x" },
        { key: "unused", label: qsTr("Unused Clips"), icon: "circle" },
        { key: "offline", label: qsTr("Offline"), icon: "circle-alert" }
    ]
    readonly property var smartBins: smartBinDefs.map(function (d) {
        var n = 0
        for (var i = 0; i < clips.length; ++i) if (smartMatch(d.key, clips[i])) ++n
        return { key: d.key, label: d.label, icon: d.icon, count: n }
    }).filter(function (b) { return b.key !== "offline" || b.count > 0 })
    readonly property var drivesWithCounts: drives.map(function (d) {
        var n = 0
        for (var i = 0; i < clips.length && d.mounted !== false; ++i)
            if (onDrive(clips[i], d.path)) ++n
        var out = {}
        for (var k in d) out[k] = d[k]
        out.count = n
        return out
    })
    function onDrive(entry, drivePath) {
        var p = entry.path !== undefined ? entry.path : ""
        if (drivePath === "") return false // a drive not mounted holds nothing yet
        if (drivePath === "/") {
            // The root volume owns what no other mounted volume claims.
            for (var i = 0; i < drives.length; ++i)
                if (drives[i].path !== "/" && drives[i].path !== "" && p.indexOf(drives[i].path + "/") === 0) return false
            return p.indexOf("/") === 0
        }
        return p.indexOf(drivePath + "/") === 0 || p === drivePath
    }
    readonly property int favoriteCount: clips.filter(function (e) { return e.favorite !== undefined && e.favorite }).length
    readonly property int rejectedCount: clips.filter(function (e) { return e.rejected !== undefined && e.rejected }).length
    readonly property var typeOptions: {
        var seen = {}
        var out = []
        for (var i = 0; i < clips.length; ++i) {
            var e = ext(clips[i])
            if (e !== "" && !seen[e]) { seen[e] = true; out.push(e) }
        }
        return out.sort()
    }
    readonly property var tagOptions: {
        var seen = {}
        var out = []
        for (var i = 0; i < clips.length; ++i) {
            var tags = clips[i].tags !== undefined ? clips[i].tags : []
            for (var j = 0; j < tags.length; ++j)
                if (!seen[tags[j]]) { seen[tags[j]] = true; out.push(tags[j]) }
        }
        return out.sort()
    }

    function binMatch(id, entry) {
        var b = bins.find(function (candidate) { return candidate.id === id })
        if (!b) return false
        if (b.smart) return ruleMatcher(entry, b.rules || {})
        return (entry.binId || "") === id
    }
    readonly property var binsWithCounts: bins.map(function (b) {
        var out = Object.assign({}, b)
        out.count = clips.filter(function (entry) { return root.binMatch(b.id, entry) }).length
                    + timelines.filter(function (item) { return item.binId === b.id }).length
        return out
    })
    function matches(entry) {
        if (filterKind === "stack" && entry.stackId !== filterValue) return false
        if (!ruleMatcher(entry, advancedRules)) return false
        if (filterKind === "root" && entry.binId) return false
        if (filterKind === "bin" && !binMatch(filterValue, entry)) return false
        // Sidebar filter first.
        if (filterKind === "drive" && !onDrive(entry, filterValue)) return false
        if (filterKind === "network") {
            var hit = false
            for (var d = 0; d < drives.length; ++d)
                if (drives[d].network && onDrive(entry, drives[d].path)) hit = true
            if (!hit) return false
        }
        if (filterKind === "favorites" && !(entry.favorite !== undefined && entry.favorite)) return false
        if (filterKind === "rejected" && !(entry.rejected !== undefined && entry.rejected)) return false
        if (filterKind === "smart" && !smartMatch(filterValue, entry)) return false
        // Then the filter bar.
        var hasVideo = MediaTime.hasPicture(entry)
        var hasAudio = entry.hasAudio === undefined || entry.hasAudio
        var still = MediaTime.isStill(entry)
        if (filterBar.mediaFilter === 1 && !(hasVideo && !still)) return false
        if (filterBar.mediaFilter === 2 && (hasVideo || !hasAudio)) return false
        if (filterBar.mediaFilter === 3 && !still) return false
        if (filterBar.typeFilter !== "" && ext(entry) !== filterBar.typeFilter) return false
        var rating = entry.rating !== undefined ? entry.rating : 0
        switch (filterBar.ratingFilter) {
        case 1: if (rating < 5) return false; break
        case 2: if (rating < 4) return false; break
        case 3: if (rating < 3) return false; break
        case 4: if (rating > 0) return false; break
        case 5: if (!(entry.favorite !== undefined && entry.favorite)) return false; break
        case 6: if (!(entry.rejected !== undefined && entry.rejected)) return false; break
        }
        if (filterBar.tagFilter !== "" && (entry.tags === undefined || entry.tags.indexOf(filterBar.tagFilter) < 0)) return false
        var q = filterBar.searchText.trim().toLowerCase()
        if (q !== "") {
            var hay = [entry.name, entry.notes, entry.comments, entry.reel, entry.scene, entry.take, entry.shot, entry.angle,
                       (entry.transcript || []).map(function (line) { return line.text }).join(" "),
                       Object.values(entry.customMetadata || {}).join(" "), (entry.keywords || []).join(" "), (entry.tags || []).join(" ")]
                      .filter(function (s) { return s !== undefined && s !== null }).join(" ").toLowerCase()
            if (hay.indexOf(q) < 0) return false
        }
        return true
    }
    // A timeline has a place in the bins and a name, but no drive, rating,
    // tags or file type, so anything asking for those passes over it.
    function timelineMatches(item) {
        if (filterKind === "bin" ? item.binId !== filterValue : filterKind === "root" ? !!item.binId : filterKind !== "")
            return false
        if (!ruleMatcher(item, advancedRules)) return false
        if (filterBar.mediaFilter > 1 || filterBar.typeFilter !== "" || filterBar.ratingFilter !== 0 || filterBar.tagFilter !== "")
            return false
        return String(item.name).toLowerCase().indexOf(filterBar.searchText.trim().toLowerCase()) >= 0
    }
    readonly property var matchingTimelines: {
        var out = []
        for (var i = 0; i < timelines.length; ++i)
            if (timelineMatches(timelines[i])) out.push(i)
        return out
    }
    // They sit above the first page, kept stable as the clip grid is.
    property var visibleTimelines: []
    readonly property var shownTimelines: Math.min(page, pageCount - 1) === 0 ? matchingTimelines : []
    onShownTimelinesChanged: {
        if (shownTimelines.join(",") !== visibleTimelines.join(",")) visibleTimelines = shownTimelines
    }
    function selectTimeline(id) {
        selection = []
        currentIndex = -1
        anchorIndex = -1
        currentTimeline = id
    }
    readonly property var currentSearchRules: {
        var r = Object.assign({}, advancedRules)
        if (filterBar.searchText.trim()) r.text = filterBar.searchText.trim()
        if (filterBar.mediaFilter) r.kind = ["", "video", "audio", "image"][filterBar.mediaFilter]
        if (filterBar.typeFilter) r.extension = filterBar.typeFilter
        if (filterBar.tagFilter) r.tag = filterBar.tagFilter
        if (filterBar.ratingFilter >= 1 && filterBar.ratingFilter <= 3) r.ratingMin = 6-filterBar.ratingFilter
        if (filterBar.ratingFilter === 4) r.ratingMax = 0
        if (filterBar.ratingFilter === 5 || filterKind === "favorites") r.favorite = true
        if (filterBar.ratingFilter === 6 || filterKind === "rejected") r.rejected = true
        return r
    }
    function applySearchRules(rules) {
        advancedRules = rules; page = 0
        filterBar.searchText = ""; filterBar.mediaFilter = 0; filterBar.typeFilter = ""
        filterBar.ratingFilter = 0; filterBar.tagFilter = ""
    }
    function compare(a, b) {
        var ea = a.entry, eb = b.entry
        var r = 0
        switch (filterBar.sortKey) {
        case 0: r = String(ea.name || "").localeCompare(String(eb.name || ""), undefined, { numeric: true }); break
        case 1: r = String(ea.created || "").localeCompare(String(eb.created || "")); break
        case 2: r = MediaTime.durationSeconds(ea) - MediaTime.durationSeconds(eb); break
        case 3: r = (ea.rating || 0) - (eb.rating || 0); break
        case 4: r = String(ea.imported || "").localeCompare(String(eb.imported || "")); break
        case 5: r = String(ea.scene || "").localeCompare(String(eb.scene || ""), undefined, {numeric: true}); break
        case 6: r = String(ea.take || "").localeCompare(String(eb.take || ""), undefined, {numeric: true}); break
        }
        if (r === 0) r = a.index - b.index
        return filterBar.sortDescending ? -r : r
    }
    readonly property var filtered: {
        var out = []
        for (var i = 0; i < clips.length; ++i)
            if (matches(clips[i])) out.push({ index: i, entry: clips[i] })
        out.sort(compare)
        if (collapseStacks && filterKind !== "stack") {
            var seen = {}
            out = out.filter(function (row) { var id = row.entry.stackId; if (!id) return true; if (seen[id]) return false; seen[id] = true; return true })
        }
        return out
    }
    readonly property int pageCount: perPage > 0 ? Math.max(1, Math.ceil(filtered.length / perPage)) : 1
    readonly property var pageRows: perPage > 0
        ? filtered.slice(Math.min(page, pageCount - 1) * perPage, Math.min(page, pageCount - 1) * perPage + perPage)
        : filtered
    // What the grid actually models: the page's library indexes, replaced
    // only when the set or order changes. A thumbnail or waveform landing
    // re-announces the library, and that must update cards in place — a
    // rebuilt card would drop a drag and a selection ring mid-gesture.
    property var pageIndexes: []
    onPageRowsChanged: {
        var next = pageRows.map(function (r) { return r.index })
        if (next.join(",") !== pageIndexes.join(",")) pageIndexes = next
    }
    onPageCountChanged: if (page >= pageCount) page = pageCount - 1
    onFilteredChanged: {
        // A clip that filtered out of view drops out of the selection too.
        var visible = {}
        for (var i = 0; i < filtered.length; ++i) visible[filtered[i].index] = true
        var kept = selection.filter(function (i) { return visible[i] === true })
        if (kept.length !== selection.length) selection = kept
    }
    onClipsChanged: {
        var kept = selection.filter(function (i) { return i < clips.length })
        if (kept.length !== selection.length) selection = kept
        if (currentIndex >= clips.length) currentIndex = clips.length - 1
    }

    // ── Selection ───────────────────────────────────────────────────────────
    function select(index, modifiers) {
        var order = filtered.map(function (r) { return r.index })
        if (modifiers & Qt.ShiftModifier && anchorIndex >= 0) {
            var a = order.indexOf(anchorIndex), b = order.indexOf(index)
            if (a < 0) a = b
            var lo = Math.min(a, b), hi = Math.max(a, b)
            selection = order.slice(lo, hi + 1)
        } else if (modifiers & Qt.ControlModifier) {
            var next = selection.slice()
            var at = next.indexOf(index)
            if (at >= 0) next.splice(at, 1); else next.push(index)
            selection = next
            anchorIndex = index
        } else {
            selection = [index]
            anchorIndex = index
        }
        currentIndex = index
        root.clipActivated(index)
    }
    function selectAll() {
        selection = filtered.map(function (r) { return r.index })
        if (selection.length > 0 && currentIndex < 0) currentIndex = selection[0]
    }
    function targets() {
        return selection.length > 0 ? selection : (currentIndex >= 0 ? [currentIndex] : [])
    }

    // ── Layout ──────────────────────────────────────────────────────────────
    WorkspaceSplit {
            controller: layout; splitId: "split1"
        anchors.fill: parent
        orientation: Qt.Horizontal

        DockPanel {
            id: storagePanel
            controller: layout; panelId: "storage"; title: qsTr("Storage")
            autoHidden: !(root.sidebarVisible)
            openWidest: 320
            C.SplitView.preferredWidth: 220
            C.SplitView.minimumWidth: 180
            StorageSidebar {
                anchors.fill: parent
                id: sidebar
                objectName: "storageSidebar"
                drives: root.drivesWithCounts
                smartBins: root.smartBins
                bins: root.binsWithCounts
                locations: root.locations
                watches: root.watches
                providers: root.providers
                onProviderAddRequested: root.providerAddRequested()
                onProviderContextRequested: function (id) { root.providerContextRequested(id) }
                onDriveMountRequested: function (drive) { root.driveMountRequested(drive) }
                onDriveContextRequested: function (drive) { root.driveContextRequested(drive) }
                onWatchAddRequested: root.watchAddRequested()
                onWatchContextRequested: function (id) { root.watchContextRequested(id) }
                onBinAddRequested: root.binRequested("", false, false)
                onSmartBinAddRequested: root.binRequested("", true, false)
                onBinContextRequested: function (id) { root.binContextRequested(id) }
                onLocationAddRequested: root.locationAddRequested()
                onLocationContextRequested: function (index) { root.locationContextRequested(index) }
                favoriteCount: root.favoriteCount
                rejectedCount: root.rejectedCount
                recentProjects: root.recentProjects
                currentProject: root.currentProject
                projectName: root.projectName
                filterKind: root.filterKind
                filterValue: root.filterValue
                onFilterSelected: function (kind, value) { root.filterKind = kind; root.filterValue = value; root.page = 0 }
                onProjectActivated: function (path) { root.projectActivated(path) }
                onOpenProjectRequested: root.openProjectRequested()
            }
        }

        WorkspaceSplit {
                controller: layout; splitId: "split2"
            visible: browserPanel.docked || viewerPanel.docked || loggerPanel.docked || waveformPanel.docked
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 520
            orientation: Qt.Vertical

            DockPanel {
                id: browserPanel
                controller: layout; panelId: "browser"; title: qsTr("Media Browser")
                C.SplitView.fillHeight: true
                C.SplitView.minimumHeight: 240
                Item {
                    anchors.fill: parent
                    id: browserArea
    
                    MediaToolbar {
                        id: toolbar
                        anchors.top: parent.top
                        width: parent.width
                        playbackMode: root.playbackMode
                        onPlaybackRequested: function (mode) { root.playbackRequested(mode) }
                        projectName: root.projectName
                        busy: root.busy
                        onOperationRequested: function (kind) { root.operationRequested(kind) }
                        onSyncRequested: root.syncRequested()
                        onTasksRequested: root.tasksRequested()
                        onImportRequested: root.importRequested()
                        onImportFolderRequested: root.importFolderRequested()
                        onProjectMenuRequested: function (anchor) { projectMenu.popup(anchor, 0, anchor.height) }
                        onMoreRequested: function (anchor) { moreMenu.popup(anchor, 0, anchor.height) }
                    }
                    FilterBar {
                        id: filterBar
                        objectName: "filterBar"
                        anchors.top: toolbar.bottom
                        width: parent.width
                        typeOptions: root.typeOptions
                        tagOptions: root.tagOptions
                        onSearchTextChanged: root.page = 0
                        onMediaFilterChanged: root.page = 0
                        onTypeFilterChanged: root.page = 0
                        onRatingFilterChanged: root.page = 0
                        onTagFilterChanged: root.page = 0
                    }
                    MediaBrowser {
                        id: browser
                        objectName: "mediaBrowser"
                        anchors.top: filterBar.bottom
                        anchors.bottom: pager.top
                        width: parent.width
                        indexes: root.pageIndexes
                        clips: root.clips
                        timelineIndexes: root.visibleTimelines
                        timelines: root.timelines
                        currentTimeline: root.currentTimeline
                        onTimelineActivated: function (id) { root.selectTimeline(id) }
                        onTimelineOpened: function (index) { root.timelineOpened(index) }
                        onTimelineContextRequested: function (index) { root.timelineContextRequested(index) }
                        skimEnabled: root.skimEnabled
                        skimResult: root.skimResult
                        stackCounts: root.stackCounts
                        onSkimmed: function (index, frame) { root.skimmed(index, frame, root.skimAudio) }
                        onRangeSelected: function (index, first, last) { root.filmRangeSelected(index, first, last) }
                        onStackOpened: function (id) { root.filterKind = "stack"; root.filterValue = id; root.page = 0 }
                        selection: root.selection
                        currentIndex: root.currentIndex
                        usedPaths: root.usedPaths
                        viewMode: filterBar.viewMode
                        thumbSize: filterBar.thumbSize
                        libraryEmpty: root.clips.length === 0
                        fpsFallback: root.fpsFallback
                        onRowActivated: function (index, modifiers) { root.select(index, modifiers) }
                        onRowOpened: function (index) { root.clipOpened(index) }
                        onRowContextRequested: function (index) {
                            if (root.selection.indexOf(index) < 0) root.select(index, 0)
                            root.clipContextRequested(index)
                        }
                        onImportRequested: root.importRequested()
                        onUrlsDropped: function (urls) { root.urlsDropped(urls) }
                        onSelectAllRequested: root.selectAll()
                    }
                    PagerBar {
                        id: pager
                        objectName: "pagerBar"
                        anchors.bottom: parent.bottom
                        width: parent.width
                        itemCount: root.filtered.length
                        timelineCount: root.matchingTimelines.length
                        selectedCount: root.selection.length
                        page: Math.min(root.page, root.pageCount - 1)
                        perPage: root.perPage
                        onPageRequested: function (p) { root.page = p }
                        onPerPageRequested: function (n) { root.perPage = n; root.page = 0 }
                    }
                }
            }

            WorkspaceSplit {
                    controller: layout; splitId: "split3"
            visible: viewerPanel.docked || loggerPanel.docked || waveformPanel.docked
                C.SplitView.preferredHeight: 264
                C.SplitView.minimumHeight: 180
                orientation: Qt.Horizontal

                DockPanel {
                    id: viewerPanel
                    controller: layout; panelId: "viewer"; title: qsTr("Source Viewer")
                    detail: preview.clipName
                    detachable: true
                    viewer: true
                    C.SplitView.preferredWidth: 360
                    C.SplitView.minimumWidth: 260
                    SourcePreview {
                        anchors.fill: parent
                        id: preview
                        dockBar: viewerPanel.barSlot
                        objectName: "sourcePreview"
                        clipName: root.sourceEntry.name !== undefined ? root.sourceEntry.name : ""
                        hasMedia: root.sourceIndex >= 0 && (!root.sourceEntry.offline || (root.sourceEntry.proxy && root.playbackMode !== "original"))
                        offline: root.sourceEntry.offline !== undefined && root.sourceEntry.offline
                        playing: root.sourcePlaying
                        timecode: root.sourceTimecode
                        duration: root.sourceDurationText
                        imageAspect: root.sourceAspect
                        nativeWidth: root.sourceEntry.width || 1920
                        sharedView: root.viewerSettings.zoomMode !== undefined
                        zoomMode: !sharedView || root.viewerSettings.zoomMode === "fit" ? 0 : root.viewerSettings.zoomMode === "fill" ? 1 : ([25,50,100,200].indexOf(root.viewerSettings.zoomPercent) < 0 ? 6 : [25,50,100,200].indexOf(root.viewerSettings.zoomPercent) + 2)
                        onZoomRequested: function(index) { root.viewerZoomRequested(index) }
                        onSnapshotRequested: root.snapshotRequested()
                        onVolumeRequested: root.monitorVolumeRequested()
                        onFullscreenRequested: root.fullscreenSourceRequested()
                        onPlayToggled: root.playToggled()
                        onPaused: root.paused()
                        onShuttled: function (d) { root.shuttled(d) }
                        onStartRequested: root.startRequested()
                        onEndRequested: root.endRequested()
    
                        Loader {
                            anchors.fill: parent
                            active: root.videoSurface !== null && preview.hasMedia
                            sourceComponent: root.videoSurface
                        }
                    }
                }
                DockPanel {
                    id: loggerPanel
                    controller: layout; panelId: "logger"; title: qsTr("Clip Log")
                    C.SplitView.preferredWidth: 360
                    C.SplitView.minimumWidth: 300
                    ClipLogger {
                        anchors.fill: parent
                        id: logger
                        dockBar: loggerPanel.barSlot
                        objectName: "clipLogger"
                        entry: root.sourceEntry
                        hasMedia: preview.hasMedia
                        position: root.sourcePosition
                        fps: MediaTime.sourceFps(root.sourceEntry)
                        frames: root.sourceDuration
                        onInRequested: function (f) { root.rangeInRequested(f) }
                        onOutRequested: function (f) { root.rangeOutRequested(f) }
                        onRangeCleared: root.rangeClearRequested()
                        onMarkerAdded: function (f, type, text) { root.markerAdded(root.sourceIndex, f, type, text) }
                        onMarkerRemoved: function (f) { root.markerRemoved(root.sourceIndex, f) }
                        onMarkerEdited: function (oldFrame, frame, type, text, notes) { root.markerEdited(root.sourceIndex, oldFrame, frame, type, text, notes) }
                        onMarkerActivated: function (f) { root.seekRequested(f) }
                        onKeywordsEdited: function (k) { root.keywordsEdited(root.sourceIndex, k) }
                        onNotesEdited: function (t) { root.notesEdited(root.sourceIndex, t) }
                        onFavoriteToggled: function (on) { root.favoriteEdited([root.sourceIndex], on) }
                        onRejectedToggled: function (on) { root.rejectedEdited([root.sourceIndex], on) }
                        onSubclipRequested: root.subclipRequested(root.sourceIndex)
                        onInsertRequested: root.insertRequested(root.sourceIndex)
                        onOverwriteRequested: root.overwriteRequested(root.sourceIndex)
                    }
                }
                DockPanel {
                    id: waveformPanel
                    controller: layout; panelId: "waveform"; title: qsTr("Audio Waveform")
                    C.SplitView.fillWidth: true
                    C.SplitView.minimumWidth: 200
                    AudioWaveform {
                        anchors.fill: parent
                        id: waveform
                        dockBar: waveformPanel.barSlot
                        objectName: "audioWaveform"
                        path: root.sourceEntry.path !== undefined ? root.sourceEntry.path : ""
                        pendingText: root.sourceEntry.multicam !== undefined
                                     ? qsTr("Multicam clip — see its angles for waveforms") : qsTr("Reading…")
                        frames: root.sourceDuration
                        fps: logger.fps
                        position: root.sourcePosition
                        rangeIn: logger.rIn
                        rangeOut: logger.rOut
                        hasAudio: root.sourceEntry.hasAudio === undefined || root.sourceEntry.hasAudio
                        hasMedia: preview.hasMedia
                        markers: logger.markers
                        onSeekRequested: function (f) { root.seekRequested(f) }
                    }
                }
            }
        }
        DockPanel {
            id: metadataPanel
            controller: layout; panelId: "metadata"; title: qsTr("Metadata")
            autoHidden: !(root.inspectorVisible)
            openWidest: 440
            C.SplitView.preferredWidth: Math.min(openWidest, Math.round(root.width * 0.21))
            C.SplitView.minimumWidth: 280
            MetadataInspector {
                anchors.fill: parent
                id: inspector
                objectName: "metadataInspector"
                entry: root.primaryEntry
                selectedCount: root.selection.length
                schema: root.metadataSchema
                onCustomEdited: function (key, value) { root.customEdited(root.targets(), key, value) }
                fpsFallback: root.fpsFallback
                onInputSettingsRequested: root.inputSettingsRequested()
                onAttributesRequested: root.attributesRequested()
                onRatingEdited: function (n) { root.ratingEdited(root.targets(), n) }
                onTagEdited: function (tag, on) { root.tagEdited(root.targets(), tag, on) }
                onNotesEdited: function (t) { root.batchFieldsEdited(root.targets(), {notes: t}) }
                onRenameRequested: function (n) { root.renameRequested(root.currentIndex, n) }
                onFieldEdited: function (k, v) { var fields = {}; fields[k] = v; root.batchFieldsEdited(root.targets(), fields) }
            }
        }
    }

    ContextMenu {
        id: projectMenu
        MenuAction { text: qsTr("Open Project…"); shortcut: "Ctrl+O"; onTriggered: root.openProjectRequested() }
        MenuAction { text: qsTr("Import Files…"); shortcut: "Ctrl+I"; onTriggered: root.importRequested() }
        MenuAction { text: qsTr("Import Folder…"); onTriggered: root.importFolderRequested() }
    }
    ContextMenu {
        id: moreMenu
        MenuAction { text: qsTr("Hover Preview"); checkable: true; checked: root.skimEnabled; onTriggered: root.skimEnabled = !root.skimEnabled }
        MenuAction { text: qsTr("Sound With Hover Preview"); checkable: true; checked: root.skimAudio; onTriggered: root.skimAudio = !root.skimAudio }
        MenuAction { text: qsTr("Collapse Stacks"); checkable: true; checked: root.collapseStacks; onTriggered: root.collapseStacks = !root.collapseStacks }
        MenuAction { text: qsTr("Stack Numbered / Related Files"); onTriggered: root.operationRequested("autoStack") }
        MenuAction { text: qsTr("Compare Source Clips…"); enabled: root.clips.length > 0; onTriggered: root.operationRequested("compareSources") }
        MenuAction { text: qsTr("Gang Source and Timeline"); checkable: true; checked: root.gangEnabled; onTriggered: root.gangRequested(!root.gangEnabled) }
        MenuAction { text: qsTr("New Bin…"); onTriggered: root.binRequested("", false, false) }
        MenuAction { text: qsTr("Save Search…"); onTriggered: root.binRequested("", true, false) }
        MenuAction { text: qsTr("Move Selected to Bin…"); enabled: root.targets().length > 0; onTriggered: root.binRequested("", false, true) }
        MenuAction { text: qsTr("Filter Media…"); onTriggered: root.filtersRequested() }
        MenuAction { text: qsTr("Clear Filters"); onTriggered: { root.advancedRules = ({}); root.filterKind = ""; root.filterValue = ""; filterBar.searchText = ""; filterBar.mediaFilter = 0; filterBar.typeFilter = ""; filterBar.ratingFilter = 0; filterBar.tagFilter = "" } }
        MenuAction { text: qsTr("Select All"); commandId: "media.select-all"; shortcut: "Ctrl+A"; onTriggered: Commands.registry ? Commands.trigger(commandId) : root.selectAll() }
        MenuAction {
            text: qsTr("Remove Selected from Project…")
            enabled: root.targets().length > 0
            onTriggered: root.removeRequested(root.targets())
        }
        MenuAction { text: qsTr("Verify Checksums…"); onTriggered: root.operationRequested("verify") }
        MenuAction { text: qsTr("Custom Metadata Fields…"); onTriggered: root.operationRequested("schema") }
        MenuAction { text: qsTr("Import Metadata CSV…"); onTriggered: root.operationRequested("csvImport") }
        MenuAction { text: qsTr("Export Metadata CSV…"); onTriggered: root.operationRequested("csvExport") }
        MenuAction { text: qsTr("Relink Missing Media…"); onTriggered: root.operationRequested("relink") }
        MenuAction { text: qsTr("Find Duplicates…"); onTriggered: root.operationRequested("duplicates") }
    }
}
