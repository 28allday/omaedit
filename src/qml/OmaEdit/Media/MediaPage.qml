import QtQuick
import QtQuick.Controls as C
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Media

// The seam: the Media workspace bound to the engine. The workspace knows
// nothing of `backend` or `win`; everything it asks for is answered here.
MediaWorkspace {
    id: page
    function layoutTransport(action,value) {
        if(action==="pause")backend.pause()
        else if(action==="start")sourceTools.transport("seek",0)
        else if(action==="end")sourceTools.transport("seek",Math.max(0,backend.sourceDuration-1))
        else if(action==="step")sourceTools.transport("seek",Math.max(0,backend.sourcePosition+value))
        else sourceTools.transport(action,value)
    }

    clips: backend.library
    timelines: backend.timelineItems
    drives: backend.volumes.drives
    // A drive not mounted mounts on a click; once it has, the browser
    // shows what of the library is on it, as for any mounted drive.
    property string mountingObject: ""
    function mountDrive(drive) { page.mountingObject = drive.object; backend.volumes.mount(drive.object) }
    onDriveMountRequested: function (drive) { page.mountDrive(drive) }
    onDriveContextRequested: function (drive) { driveMenu.drive = drive; driveMenu.popup() }
    Connections {
        target: backend.volumes
        function onDiskActionFinished(object, action, ok, message, mountPoint) {
            if (action === "mount" && object === page.mountingObject) {
                page.mountingObject = ""
                if (ok) { page.filterKind = "drive"; page.filterValue = mountPoint; page.page = 0 }
            } else if (ok && action !== "mount" && page.filterKind === "drive" && page.filterValue === driveMenu.drive.path) {
                page.filterKind = ""; page.filterValue = ""; page.page = 0
            }
        }
    }
    Ui.ContextMenu {
        id: driveMenu
        objectName: "driveMenu"
        property var drive: ({})
        Ui.MenuAction {
            objectName: "driveMenu:mount"
            text: qsTr("Mount")
            showInMenu: driveMenu.drive.mounted === false
            enabled: driveMenu.drive.canMount === true
            onTriggered: page.mountDrive(driveMenu.drive)
        }
        Ui.MenuAction {
            objectName: "driveMenu:import"
            text: qsTr("Import from This Drive…")
            showInMenu: driveMenu.drive.mounted === true
            onTriggered: operations.open("import", [driveMenu.drive.path])
        }
        Ui.MenuAction {
            objectName: "driveMenu:unmount"
            text: qsTr("Unmount")
            showInMenu: driveMenu.drive.canUnmount === true
            onTriggered: backend.volumes.unmount(driveMenu.drive.object)
        }
        Ui.MenuAction {
            objectName: "driveMenu:eject"
            text: qsTr("Eject")
            showInMenu: driveMenu.drive.canEject === true
            onTriggered: backend.volumes.eject(driveMenu.drive.object)
        }
    }
    bins: collections.tree
    locations: backend.mediaCatalog.locations
    watches: backend.mediaCatalog.watches
    providers: backend.mediaCatalog.providers
    onProviderAddRequested: collections.addProvider()
    onProviderContextRequested: function (id) { collections.providerContext(id) }
    onWatchAddRequested: collections.addWatch()
    onWatchContextRequested: function (id) { collections.watchContext(id) }
    ruleMatcher: function (entry, rules) { return backend.mediaCatalog.matches(entry, rules) }
    externalTextActive: collections.textActive || mediaContextMenu.visible || driveMenu.visible || subclipName.visible || operations.textActive || relinking.textActive || proxies.textActive || metadataTools.textActive || comparison.textActive || stackName.visible || sourceTools.textActive
    onBinRequested: function (id, smart, assign) { collections.openBin(id, smart, assign) }
    onBinContextRequested: function (id) { collections.context(id) }
    onLocationAddRequested: collections.addLocation()
    onLocationContextRequested: function (index) { collections.locationContext(index) }
    onFiltersRequested: collections.openFilters()
    CollectionControls {
        id: collections
        catalog: backend.mediaCatalog
        onProviderManifestRequested: backend.mediaTasks.browse("provider-manifest", "file")
        selection: page.targets()
        activeRules: page.currentSearchRules
        onMoveRequested: function (indexes, binId) { backend.moveMediaToBin(indexes, binId) }
        onFilterRequested: function (kind, value) { page.filterKind = kind; page.filterValue = value; page.page = 0 }
        onRulesApplied: function (rules) { page.applySearchRules(rules) }
    }
    recentProjects: backend.recentProjects
    currentProject: backend.projectPath
    projectName: backend.projectName
    busy: backend.busy
    fpsFallback: backend.fps > 0 ? backend.fps : 25
    usedPaths: {
        var used = {}
        // Only while this page is shown: it stays loaded once opened, and
        // every edit on another page would walk the whole timeline here.
        if (!page.visible)
            return used
        var lists = [backend.timeline, backend.connected]
        for (var l = 0; l < lists.length; ++l)
            for (var i = 0; i < lists[l].length; ++i)
                if (lists[l][i].path !== undefined) used[lists[l][i].path] = true
        return used
    }

    sourceIndex: backend.previewLibraryIndex
    sourcePosition: backend.sourcePosition
    sourceDuration: backend.sourceDuration
    sourceTimecode: backend.previewLibraryIndex >= 0 ? backend.sourceTimecode(backend.sourcePosition) : "--:--:--:--"
    sourceDurationText: backend.previewLibraryIndex >= 0 ? backend.sourceTimecode(Math.max(0, backend.sourceDuration - 1)) : "--:--:--:--"
    sourcePlaying: backend.playing && backend.viewingSource
    sourceAspect: sourceEntry.width !== undefined && sourceEntry.width > 0 && sourceEntry.height > 0
                  ? sourceEntry.width / sourceEntry.height : 16 / 9
    viewerSettings: backend.viewerSettings
    onViewerZoomRequested: function(index) {
        backend.setViewerOption("zoomMode", index === 0 ? "fit" : index === 1 ? "fill" : "custom")
        if (index >= 2) backend.setViewerOption("zoomPercent", [25,50,100,200][index - 2])
    }
    videoSurface: Component {
        VideoView {
            overlayPalette: Ui.Theme.viewerOverlayPalette;
            source: backend
            channel: VideoView.Source
        }
    }

    Connections {
        target: backend.mediaTasks
        function onPathsChosen(purpose, paths) { if (purpose === "provider-manifest" && paths.length) backend.startMediaTask("providerManifest", [paths[0]], {}) }
    }
    // The Edit page and this one share the primary selection.
    currentIndex: win.selectedLibraryIndex
    onCurrentIndexChanged: if (currentIndex !== win.selectedLibraryIndex) win.selectedLibraryIndex = currentIndex

    skimResult: backend.mediaPreviews.skim || ({})
    gangEnabled: backend.mediaGang
    onGangRequested: function (on) { backend.mediaGang = on }
    onSkimmed: function (index, frame, audio) { backend.previewMedia(index, frame, audio, "skim") }
    onFilmRangeSelected: function (index, first, last) { backend.previewClipAt(index, first, first, last) }
    SourceTools { id: sourceTools; engine: backend }
    onSnapshotRequested: { page.layoutController.exitFullscreen(); Qt.callLater(function () { win.requestActivate(); sourceTools.snapshot() }) }
    onMonitorVolumeRequested: { page.layoutController.exitFullscreen(); Qt.callLater(function () { win.requestActivate(); sourceTools.volume() }) }
    onFullscreenSourceRequested: { if(backend.previewLibraryIndex>=0 && !backend.viewingSource)backend.previewClipAt(backend.previewLibraryIndex,backend.sourcePosition); page.layoutController.toggleViewerFullscreen() }
    CompareSources { id: comparison; engine: backend }
    MediaNameDialog {
        id: stackName
        title: qsTr("Name Stack")
        property var ids: []
        onAccepted: { var indexes = []; backend.library.forEach(function (e, i) { if (stackName.ids.indexOf(e.libraryId) >= 0) indexes.push(i) }); backend.stackMedia(indexes, value) }
    }
    MetadataControls { id: metadataTools; engine: backend; selection: page.targets() }
    metadataSchema: backend.mediaCatalog.schema
    onCustomEdited: function (indexes, key, value) { var fields = {}; fields[key] = value; backend.setCustomMediaFields(indexes, fields) }
    RelinkControls { id: relinking; engine: backend }
    ProxyControls { id: proxies; engine: backend }
    ClipAttributes { id: attributes; objectName: "clipAttributes"; engine: backend }
    onAttributesRequested: attributes.open(page.targets())
    playbackMode: backend.mediaPlayback
    onPlaybackRequested: function (mode) { backend.mediaPlayback = mode }
    onInputSettingsRequested: proxies.input(page.targets())
    MediaOperations { id: operations; engine: backend; binId: page.filterKind === "bin" ? page.filterValue : "" }
    onOperationRequested: function (kind) {
        if (kind === "autoStack") { backend.autoStackMedia(); return }
        if (kind === "compareSources") { comparison.open(page.targets()); return }
        if (kind === "schema") { metadataTools.openSchema(); return }
        if (kind === "csvImport") { metadataTools.importCsv(); return }
        if (kind === "csvExport") { metadataTools.exportCsv(); return }
        if (kind === "relink") { relinking.open([]); return }
        var indexes = page.targets()
        if (!indexes.length && (kind === "verify" || kind === "duplicates")) indexes = clips.map(function (entry, index) { return index })
        operations.open(kind, indexes.map(function (index) { return page.clips[index].path }).filter(function (path) { return path.indexOf("multicam:") !== 0 }))
    }
    onSyncRequested: win.openSyncDialog(page.targets())
    onTasksRequested: operations.showTasks()
    function importMedia() { operations.open("import", []); backend.mediaTasks.browse("media-input", "files") }
    onImportRequested: Ui.Commands.trigger("media.import")
    onImportFolderRequested: { operations.open("import", []); backend.mediaTasks.browse("media-input") }
    onUrlsDropped: function (urls) { operations.open("import", urls.map(function (url) { return decodeURIComponent(String(url).replace(/^file:\/\//, "")) })) }
    onOpenProjectRequested: Ui.Commands.trigger("project.open")
    onProjectActivated: function (path) { win.project.guardedOpen(path) }
    onClipActivated: function (index) {
        win.selectedLibraryIndex = index
        if (!backend.library[index].offline || (backend.library[index].proxy && backend.mediaPlayback !== "original"))
            backend.previewClip(index)
    }
    onClipOpened: function (index) {
        win.selectedLibraryIndex = index
        win.workspace = "Edit"
    }
    onTimelineOpened: function (index) {
        if (backend.projectBusy || backend.exporting)
            return
        if (index !== backend.activeSequence) {
            win.selection.clearSelection()
            backend.switchSequence(index)
        }
        win.workspace = "Edit"
    }
    onTimelineContextRequested: function (index) {
        if (!backend.projectBusy && !backend.exporting)
            classicLayout.openTimelineMenu(index)
    }
    onClipContextRequested: function (index) {
        win.selectedLibraryIndex = index
        mediaContextMenu.popup()
    }
    onRatingEdited: function (indexes, rating) { for (var i = 0; i < indexes.length; ++i) backend.setClipRating(indexes[i], rating) }
    onFavoriteEdited: function (indexes, on) { for (var i = 0; i < indexes.length; ++i) backend.setClipFavorite(indexes[i], on) }
    onRejectedEdited: function (indexes, on) { for (var i = 0; i < indexes.length; ++i) backend.setClipRejected(indexes[i], on) }
    onTagEdited: function (indexes, tag, on) {
        for (var i = 0; i < indexes.length; ++i) {
            var entry = backend.library[indexes[i]]
            var tags = entry && entry.tags !== undefined ? Array.prototype.slice.call(entry.tags) : []
            var at = tags.indexOf(tag)
            if (on && at < 0) tags.push(tag)
            else if (!on && at >= 0) tags.splice(at, 1)
            else continue
            backend.setClipTags(indexes[i], tags)
        }
    }
    onKeywordsEdited: function (index, keywords) { backend.setClipKeywords(index, keywords) }
    onNotesEdited: function (index, notes) { backend.setClipNotes(index, notes) }
    onFieldEdited: function (index, key, value) { backend.setClipField(index, key, value) }
    onRenameRequested: function (index, name) { backend.renameLibraryClip(index, name) }
    onMarkerAdded: function (index, frame, type, text) { backend.addClipMarker(index, frame, type, text) }
    onMarkerRemoved: function (index, frame) { backend.removeClipMarker(index, frame) }
    onBatchFieldsEdited: function (indexes, fields) { backend.setMediaFields(indexes, fields) }
    onMarkerEdited: function (index, oldFrame, frame, type, text, notes) { backend.editMediaMarker(index, oldFrame, frame, type, text, notes) }
    onSubclipRequested: function (index) {
        if (index < 0 || index >= clips.length || clips[index].offline) return
        subclipName.sourceId = clips[index].libraryId
        subclipName.value = clips[index].name + qsTr(" subclip")
        subclipName.open()
    }
    MediaNameDialog {
        id: subclipName
        objectName: "mediaSubclipNameDialog"
        title: qsTr("Create Subclip")
        property string sourceId: ""
        onAccepted: {
            var index = backend.library.findIndex(function (entry) { return entry.libraryId === subclipName.sourceId })
            if (index >= 0) backend.createSubclip(index, value)
        }
    }
    onRemoveRequested: function (indexes) {
        var anyUsed = false
        for (var i = 0; i < indexes.length; ++i) if (backend.clipIsUsed(indexes[i])) anyUsed = true
        // By what the clips are, not where they are: the library can change under the open question.
        removeDialog.pending = indexes.map(function (i) { return backend.library[i] ? backend.library[i].libraryId : undefined })
        removeDialog.openFor(indexes.length, anyUsed, indexes.map(function (i) { return backend.library[i] ? backend.library[i].name : "" }))
    }
    onRevealRequested: function (index) { backend.revealClip(index) }
    onRangeInRequested: function (frame) { backend.setRange(frame, sourceEntry.rangeOut !== undefined ? sourceEntry.rangeOut : -1) }
    onRangeOutRequested: function (frame) { backend.setRange(sourceEntry.rangeIn !== undefined ? sourceEntry.rangeIn : -1, frame) }
    onRangeClearRequested: backend.clearRange()
    onSeekRequested: function (frame) { sourceTools.transport("seek", frame) }
    onPlayToggled: Ui.Commands.trigger("transport.play")
    onPaused: Ui.Commands.trigger("transport.stop")
    onShuttled: function(d) { Ui.Commands.trigger(d < 0 ? "transport.reverse" : "transport.forward") }
    onStartRequested: Ui.Commands.trigger("transport.start")
    onEndRequested: Ui.Commands.trigger("transport.end")
    onInsertRequested: function (index) { win.selectedLibraryIndex = index; Ui.Commands.trigger("edit.insert") }
    onOverwriteRequested: function (index) { win.selectedLibraryIndex = index; Ui.Commands.trigger("edit.overwrite") }

    RemoveClipDialog {
        id: removeDialog
        objectName: "removeClipDialog"
        property var pending: []
        onConfirmed: {
            // Highest first, so earlier indexes stay valid.
            var found = []
            for (var p = 0; p < pending.length; ++p) {
                var at = pending[p] === undefined ? -1 : backend.library.findIndex(function (entry) { return entry.libraryId === pending[p] })
                if (at >= 0 && found.indexOf(at) < 0) found.push(at)
            }
            var sorted = found.sort(function (a, b) { return b - a })
            for (var i = 0; i < sorted.length; ++i) backend.removeFromLibrary(sorted[i])
            page.selection = []
        }
    }

    Ui.ContextMenu {
        id: mediaContextMenu
        objectName: "mediaContextMenu"
        Ui.MenuAction { text: qsTr("Preview"); onTriggered: page.clipActivated(win.selectedLibraryIndex) }
        Ui.MenuAction { text: qsTr("Open in Edit"); onTriggered: page.clipOpened(win.selectedLibraryIndex) }
        C.MenuSeparator {}
        Ui.ContextMenu {
            title: qsTr("Add to Timeline")
            Ui.MenuAction { text: qsTr("Append to Timeline"); shortcut: "E"; onTriggered: Ui.Commands.trigger("edit.append") }
            Ui.MenuAction { text: qsTr("Insert at Playhead"); shortcut: "W"; onTriggered: Ui.Commands.trigger("edit.insert") }
            Ui.MenuAction { text: qsTr("Overwrite at Playhead"); shortcut: "D"; onTriggered: Ui.Commands.trigger("edit.overwrite") }
        }
        Ui.ContextMenu {
            title: qsTr("Organise")
            Ui.MenuAction { text: qsTr("Move to Bin…"); onTriggered: collections.openBin("", false, true) }
            Ui.MenuAction { text: qsTr("Create Stack…"); enabled: page.targets().length > 1; onTriggered: { stackName.ids = page.targets().map(function (i) { return page.clips[i].libraryId }); stackName.value = qsTr("Related media"); stackName.open() } }
            Ui.MenuAction { text: qsTr("Open Stack"); showInMenu: !!page.primaryEntry.stackId; onTriggered: { page.filterKind = "stack"; page.filterValue = page.primaryEntry.stackId } }
            Ui.MenuAction { text: qsTr("Unstack"); showInMenu: !!page.primaryEntry.stackId; onTriggered: backend.unstackMedia(page.targets()) }
            Ui.MenuAction { text: qsTr("Compare Source Clips…"); onTriggered: comparison.open(page.targets()) }
        }
        Ui.ContextMenu {
            title: qsTr("Create")
            Ui.MenuAction { text: qsTr("Duplicate Reference"); onTriggered: backend.duplicateMedia(win.selectedLibraryIndex) }
            Ui.MenuAction { text: qsTr("Create Subclip"); onTriggered: page.subclipRequested(win.selectedLibraryIndex) }
            Ui.MenuAction { text: qsTr("New Multicam Clip…"); enabled: backend.library.length >= 2; onTriggered: win.openMulticamDialog(page.targets()) }
        }
        C.MenuSeparator {}
        Ui.ContextMenu {
            title: qsTr("Proxies")
            Ui.MenuAction { text: qsTr("Create Proxies…"); onTriggered: page.operationRequested("proxy") }
            Ui.MenuAction { text: qsTr("Attach / Relink Proxy…"); onTriggered: proxies.attach(win.selectedLibraryIndex) }
            Ui.MenuAction { text: qsTr("Detach Proxies"); enabled: !!page.primaryEntry.proxyPath; onTriggered: backend.detachMediaProxy(page.targets()) }
            Ui.MenuAction { text: qsTr("Reveal Proxy Folder"); enabled: !!page.primaryEntry.proxyPath; onTriggered: backend.revealMediaPath(page.primaryEntry.proxyPath) }
            Ui.MenuAction { text: page.primaryEntry.preferProxy === false ? qsTr("Prefer This Proxy") : qsTr("Prefer This Original"); enabled: !!page.primaryEntry.proxyPath; onTriggered: backend.preferMediaProxy(page.targets(), page.primaryEntry.preferProxy === false) }
        }
        Ui.MenuAction { text: qsTr("Input Settings…"); onTriggered: proxies.input(page.targets()) }
        Ui.MenuAction { text: qsTr("Clip Attributes…"); enabled: !page.primaryEntry.multicam; onTriggered: attributes.open(page.targets()) }
        // A multicam clip's sound source, for when it is placed from now on.
        // (As the Edit page's library menu has it: "Follows the picture",
        // then every source lined up when the clip was made that has sound.)
        Ui.ContextMenu {
            id: mediaSoundFrom
            objectName: "mediaSoundFromMenu"
            title: qsTr("Sound From")
            readonly property var rows: mediaContextMenu.visible && win.selectedLibraryIndex >= 0 && backend.library.length > win.selectedLibraryIndex
                                        ? backend.libraryMulticamSoundSources(win.selectedLibraryIndex) : []
            showInParent: rows.length > 0
            Instantiator {
                model: mediaSoundFrom.rows.length
                delegate: Ui.MenuAction {
                    required property int index
                    text: mediaSoundFrom.rows[index].text
                    shortcut: mediaSoundFrom.rows[index].current ? "●" : ""
                    onTriggered: backend.setLibraryMulticamSound(win.selectedLibraryIndex, mediaSoundFrom.rows[index].source)
                }
                onObjectAdded: function (i, object) { mediaSoundFrom.insertItem(i, object) }
                onObjectRemoved: function (i, object) { mediaSoundFrom.removeItem(object) }
            }
        }
        Ui.MenuAction { text: qsTr("Reframe 360° Video…"); onTriggered: page.operationRequested("reframe360") }
        C.MenuSeparator {}
        Ui.MenuAction { text: qsTr("Reveal in File Manager"); onTriggered: backend.revealClip(win.selectedLibraryIndex) }
        Ui.MenuAction { text: qsTr("Relink Media…"); onTriggered: relinking.open(page.targets()) }
        C.MenuSeparator {}
        Ui.MenuAction { text: qsTr("Remove from Project…"); onTriggered: page.removeRequested(page.targets()) }
    }
}
