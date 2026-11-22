import QtQuick
import QtQuick.Layouts
import omaedit
import OmaEdit.Edit as Edit
import OmaEdit.Ui as Ui

// Browser (media library): the Edit-page media panel bound to the engine's
// library. This file is the seam — the panel knows nothing of the backend.
Edit.MediaPanel {
    id: media
    objectName: "mediaPanel"

    // The window stands the editing shortcuts down while this has the keys:
    // MediaPanel's own searchActive (a redeclaration here bound to itself).
    // (`dragActive` — a tile drag in flight — is the panel's own property;
    // the timeline opens its new-track rows while it is on.)

    // The brief's 28%, never below what the bin tree plus one column needs.
    Layout.preferredWidth: Math.max(300, Math.round(parent.width * 0.28))
    Layout.fillHeight: true

    clips: backend.library
    timelines: backend.timelineItems
    placementFramesForIndex: function(index) { return backend.libraryPlacementRange(index).frames || 0 }
    bins: {
        const catalogRevision=backend.mediaCatalog.bins
        return [{id:"",name:qsTr("Library"),count:backend.library.length+backend.timelineItems.length}].concat(backend.mediaCatalog.tree().map(function(bin){
        const b=Object.assign({},bin)
        b.name="  ".repeat(b.depth || 0)+b.name
        b.count=backend.library.filter(function(e){return b.smart ? backend.mediaCatalog.matches(e,b.rules || {}) : e.binId===b.id}).length
                +backend.timelineItems.filter(function(t){return t.binId===b.id}).length
        return b
    }))
    }
    currentBin: 0
    onBinActivated:function(index){currentBin=index}
    binMatcher:function(entry){
        const b=bins[currentBin];if(!b || !b.id)return true
        return b.smart ? backend.mediaCatalog.matches(entry,b.rules || {}) : entry.binId===b.id
    }
    currentIndex: win.selectedLibraryIndex
    // The Effects and Titles tabs draw on the window's
    // catalogue; applicability follows the selection.
    effects: win.catalogue.list.concat(win.attributes.presetTiles)
    applicability: win.catalogue.applicability
    titles: win.catalogue.titlePresets.concat(win.catalogue.generatorTiles)
    // The effects shelf: favourites, recents and custom bins live on the
    // engine side (QSettings); the panel only shows and asks.
    favoriteKeys: backend.effectShelf.favorites
    recentKeys: backend.effectShelf.recents
    effectBins: backend.effectShelf.bins
    onEffectFavoriteToggled: function (key) { backend.effectShelf.toggleFavorite(key) }
    onRecentEffectsCleared: backend.effectShelf.clearRecents()
    onEffectBinCreated: function (name, key) {
        if (backend.effectShelf.addBin(name) && key !== "")
            backend.effectShelf.addToBin(name, key)
    }
    onEffectBinRenamed: function (name, to) { backend.effectShelf.renameBin(name, to) }
    onEffectBinDeleted: function (name) { backend.effectShelf.deleteBin(name) }
    onEffectBinKeyAdded: function (bin, key) { backend.effectShelf.addToBin(bin, key) }
    onEffectBinKeyRemoved: function (bin, key) { backend.effectShelf.removeFromBin(bin, key) }

    Connections { // Match Frame / Reveal flip the browser to Project
        target: win
        function onShowLibraryTab() { media.currentTab = 0 }
    }
    onEffectApplied: function (key) { win.catalogue.apply(key) }
    onTransitionApplied: function (key) { win.catalogue.apply(key) }
    onTitleRequested: function (key) {
        if (key.indexOf("gen:") === 0) {
            backend.viewTimeline()
            backend.addGenerator(key.substring(4))
            return
        }
        var p = win.catalogue.titlePreset(key)
        titleDialog.openFor(-1, "", p.place, p.size, "", "")
    }

    onTimelineSelected: function (id) {
        currentTimeline = id
        win.selectedLibraryIndex = -1
    }
    onTimelineOpened: function (index) {
        if (index === backend.activeSequence || backend.projectBusy || backend.exporting)
            return
        win.selection.clearSelection()
        backend.switchSequence(index)
    }
    onTimelineContextRequested: function (index) {
        if (!backend.projectBusy && !backend.exporting)
            sequenceBar.openMenu(index)
    }

    onClipFiled: function (index, binId) { backend.moveMediaToBin([index], binId) }
    onTimelineFiled: function (index, binId) { backend.moveSequenceToBin(index, binId) }

    onClipActivated: function (index) {
        win.selectedLibraryIndex = index
        backend.previewClip(index) // source view
    }
    onClipOpened: function (index) {
        win.selectedLibraryIndex = index
        backend.appendClipAt(index)
    }
    onClipContextRequested: function (index) {
        win.selectedLibraryIndex = index
        libraryContextMenu.popup()
    }
    onBinContextRequested: binContextMenu.popup()
    onImportRequested: backend.importDialog()
    // New Bin… itself: the Media page's box, where bins are made and filled.
    onBinAddRequested: Ui.Commands.trigger("file.new-bin")

    // Seam against the viewer well.
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: win.seamColor
        z: 2
    }
}
