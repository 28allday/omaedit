import QtQml
import OmaEdit.Ui as Ui

// What the Clip and Modify menus, the keys and the Inspector do to the
// selected clips, and what they ask about them: delete and lift, disable,
// label, fades, rotation, stabiliser, compound and break apart, link and
// unlink, takes, render and restore, ratings, nested sequences. The window
// holds one as `win.clipCommands`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win
    required property var layout
    required property var attributesDialog

    // Map Beats / Clear Beat Map act on the selected timeline clip's
    // recording, else on the clip selected in the browser:
    // {path, track, index} or {path, library}, null when neither has a file.
    function beatTarget() {
        const e = win.selection.selectedEntry()
        if (e !== null && e.gap !== true && e.path !== undefined && e.path !== "" && e.path.indexOf("color:") !== 0
                && !(e.title !== undefined && e.title !== "") && e.compound !== true)
            return { path: e.path, track: e.track, index: e.index }
        const i = win.selectedLibraryIndex
        if (i >= 0 && i < backend.library.length && backend.library[i].multicam !== true)
            return { path: backend.library[i].path, library: i }
        return null
    }
    // Whether Map Beats can work on that: a library clip with no sound, or
    // offline, has nothing to map (as the library's own menu has it).
    function canMapBeats() {
        const target = beatTarget()
        if (target === null) return false
        if (target.library === undefined) return true
        const entry = backend.library[target.library]
        return entry.hasAudio !== false && entry.offline !== true
    }
    function beatsMapped() {
        const target = beatTarget()
        return backend.beatMapRevision >= 0 && target !== null && backend.beatMapOf(target.path).count > 0
    }
    function mapBeats() {
        const target = beatTarget()
        if (target === null || !canMapBeats()) return
        if (target.library !== undefined) backend.mapBeats(target.library)
        else backend.mapBeatsOn(target.track, target.index)
    }
    function clearBeatMap() {
        const target = beatTarget()
        if (target === null) return
        if (target.library !== undefined) backend.clearBeatMap(target.library)
        else backend.clearBeatMapOn(target.track, target.index)
    }
    // Alt+G: compound the marked range, else adjacent selected V1 clips.
    // Keep the selection/range until the worker confirms the edit succeeded.
    function compoundSelection() {
        if (backend.viewingSource || !backend.hasVideo)
            return
        if (win.hasTimelineRange) {
            backend.makeCompound(win.effRangeIn,
                                 Math.min(win.effRangeOut, backend.duration - 1))
            return
        }
        backend.makeCompoundSelection(win.selection.selectedAddresses())
    }
    function breakApartSelection() {
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length
                && backend.timeline[win.selectedTimelineIndex].compound === true)
            backend.breakApartClip(win.selectedTimelineIndex)
    }
    // The selected clip has animation: picture keys or opacity keys.
    function keyframesOnSelection() {
        const e = win.selection.selectedEntry()
        if (!e) return false
        if (e.opacityKf && e.opacityKf.length) return true
        const keys = e.editKeys || {}
        if (Object.keys(keys).some(function (k) { return keys[k] && keys[k].length > 0 })) return true
        return (e.videoEffects || []).some(function (fx) {
            return (fx.keys && Object.keys(fx.keys).length > 0) || (fx.maskKeys && Object.keys(fx.maskKeys).length > 0)
        })
    }
    // V: disable/enable the whole selection, after the primary's state.
    function toggleDisabledSelected() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        if (win.selectionCount === 1 && e.track === 0) {
            backend.toggleClipDisabled(win.selectedTimelineIndex)
            return
        }
        backend.setClipsDisabled(win.selection.selectedAddresses(), e.disabled !== true)
    }
    // Link: the selection is one picture with no linked sound yet and
    // sound cut from that picture's own file. The addresses, or [].
    function linkTargets() {
        const all = win.selection.selectedEntries()
        const pictures = all.filter(e => e.audioOnly !== true && e.gap !== true)
        const sounds = all.filter(e => e.audioOnly === true)
        if (pictures.length !== 1 || sounds.length === 0)
            return []
        const p = pictures[0]
        if (p.videoOnly !== true || (p.title || "") !== "" || audioMateIndex(p) >= 0
                || sounds.some(s => s.path !== p.path))
            return []
        return all.filter(e => e.gap !== true).map(e => ({ track: e.track, index: e.index }))
    }
    // Unlink: the selected clips that belong to a linked pair.
    function unlinkTargets() {
        const chosen = win.selection.selectedAddresses()
        if (chosen.length === 0)
            return chosen
        const lookup = win.selection.entryLookup() // one for the whole set
        return chosen.filter(a => win.selection.linkMates(a, lookup).length > 0)
    }
    // The flat connected index of the audio clip linked to a V1 clip, −1
    // when it has none (a video-only file, a compound, a nested sequence).
    function audioMateIndex(e) {
        if (e === null || e === undefined || e.link === undefined || e.link === "")
            return -1
        for (var i = 0; i < backend.connected.length; i++) {
            var c = backend.connected[i]
            if (c.audioOnly === true && c.link === e.link)
                return i
        }
        return -1
    }
    // A nested clip names its sequence by id; the label follows renames.
    function nestName(e) {
        var name = backend.sequenceNameForId(e.nestId)
        return "⧉ " + (name !== "" ? name : qsTr("Sequence"))
    }
    function openNestedSequence(e) {
        var i = backend.sequenceIndexForId(e.nestId)
        if (i < 0 || i === backend.activeSequence)
            return
        win.selection.clearSelection()
        backend.switchSequence(i)
    }
    // Render in Place bakes the clip (attributes and all) to a cache
    // file in the background and swaps it in; Restore Original undoes that
    // at any later time (the original entry rides the rendered clip).
    function renderOrRestoreSelected() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        if (e.rendered === true)
            backend.restoreOriginal(e.track, e.index)
        else
            backend.renderAndReplace(e.track, e.index)
    }
    function removeSelectedEffects() {
        backend.viewTimeline()
        backend.pasteAttributes(win.selection.selectedAddresses(), { name: "none" }, ["effects"])
    }
    function removeAttributes() {
        var e = win.selection.selectedClip()
        if (e !== null)
            attributesDialog.openForRemoval(win.selection.selectedEntries())
    }
    // Ratings belong to the library clip: the selected clip's source, else
    // the clip picked in the browser.
    function ratingTarget() {
        var e = win.selection.selectedClip()
        if (e !== null) {
            var index = win.matching.libraryIndexForEntry(e, e.in)
            if (index >= 0)
                return index
        }
        return win.selectedLibraryIndex
    }
    function unrateTarget() {
        backend.setClipFavorite(ratingTarget(), false)
        backend.setClipRejected(ratingTarget(), false)
    }
    function reattachSelectedAudio() {
        var e = win.selection.selectedClip()
        if (e !== null)
            backend.reattachAudio(e.track, e.index)
    }
    function selectedTrackSoloed() {
        var e = win.selection.selectedClip()
        return e !== null && backend.tracks[e.track] !== undefined && backend.tracks[e.track].solo === true
    }
    function toggleSelectedTrackSolo() {
        var e = win.selection.selectedClip()
        if (e !== null)
            backend.setTrackFlag(e.track, "solo", !selectedTrackSoloed())
    }
    function selectedHasStabilization() {
        var e = win.selection.selectedClip()
        return e !== null && e.stab !== undefined && e.stab !== null
    }
    function toggleSelectedStabilization() {
        var e = win.selection.selectedPicture()
        if (e === null)
            return
        if (selectedHasStabilization())
            backend.removeStab(e.track, e.index)
        else
            backend.stabilize(e.track, e.index, 4, 10)
    }
    function autoBalanceSelected() {
        var e = win.selection.selectedPicture()
        if (e === null)
            return
        var inClip = backend.position >= e.start && backend.position < e.start + e.frames
        backend.autoBalance(e.track, e.index, inClip ? backend.position - e.start : 0)
    }
    function setSelectedFades(on) {
        var e = win.selection.selectedClip()
        if (e === null)
            return
        if ((e.fadeIn > 0) !== on)
            Ui.Commands.trigger("edit.fade-in")
        if ((e.fadeOut > 0) !== on)
            Ui.Commands.trigger("edit.fade-out")
    }
    function rotateSelected(degrees) {
        var e = win.selection.selectedPicture()
        if (e === null)
            return
        var turned = layout.pictureValue("rotation", e.rotation === undefined ? 0 : e.rotation) + degrees
        layout.editPicture({ rotation: ((turned + 540) % 360 + 360) % 360 - 180 })
    }
    function selectedIsRendered() {
        var e = win.selection.selectedClip()
        return e !== null && e.rendered === true
    }
    // Colour labels route to whichever clip is selected.
    function setSelectedLabel(name) {
        if (win.selectionCount > 1)
            backend.setLabels(win.selection.selectedAddresses(), name)
        else if (win.selectedConnectedIndex >= 0)
            backend.setConnectedLabel(win.selectedConnectedIndex, name)
        else if (win.selectedTimelineIndex >= 0)
            backend.setClipLabel(win.selectedTimelineIndex, name)
    }
    function selectedLabel() {
        var e = win.selection.selectedEntry()
        return e === null || e.label === undefined ? "" : e.label
    }
    function deleteSelected() {
        if (win.selection.closeSelectedGap())
            return
        if (win.selectionCount > 1) {
            backend.removeClips(win.selection.selectedAddresses(), true)
            win.selection.clearSelection()
            return
        }
        if (win.selectedConnectedIndex >= 0) {
            backend.removeConnectedClip(win.selectedConnectedIndex)
            win.selectedConnectedIndex = -1
        } else if (win.selectedTimelineIndex >= 0) {
            backend.removeClip(win.selectedTimelineIndex)
            win.selectedTimelineIndex = -1
        }
    }
    // Retime routes to whichever clip is selected — V1 or a track clip.
    // Clip ▸ Detect Edits: a selected clip with pictures at normal speed.
    function canDetectEdits() {
        const e = win.selection.selectedEntry()
        return !!e && !backend.detectingEdits && !e.gap && !e.title && e.audioOnly !== true
               && (e.speed === undefined || e.speed === 1) && !backend.exporting
    }
    // Edit ▸ Captions: the selected captions as flat connected indices.
    function selectedCaptionIndices() {
        const out = []
        const add = function (i) { if (i >= 0 && i < backend.connected.length && backend.connected[i].caption === true && out.indexOf(i) < 0) out.push(i) }
        add(win.selectedConnectedIndex)
        for (let n = 0; n < win.extraSelection.length; ++n)
            for (let i = 0; i < backend.connected.length; ++i)
                if (backend.connected[i].track === win.extraSelection[n].track && backend.connected[i].index === win.extraSelection[n].index) add(i)
        return out
    }
    // Shift+Del — lift. A storyline clip becomes a gap of its own length;
    // a track clip already leaves a blank behind, so lift = delete there.
    function liftSelected() {
        if (win.selection.closeSelectedGap()) // a gap has nothing to leave behind
            return
        if (win.selectionCount > 1) {
            backend.removeClips(win.selection.selectedAddresses(), false)
            win.selection.clearSelection()
            return
        }
        if (win.selectedConnectedIndex >= 0) {
            deleteSelected()
        } else if (win.selectedTimelineIndex >= 0) {
            backend.liftClip(win.selectedTimelineIndex)
            win.selectedTimelineIndex = -1
        }
    }
    // Colour correction is the one per-clip effect that leaves no other
    // trace on the block, so it earns the fx badge.
    function effectKeys(entry) {
        let keys=win.attributes.groupsOf(entry).filter(function(k){return k!=="speed" && k!=="fades"}).map(function(k){return k==="transform"?"rect":k==="channels"?"audioMap":k})
        if(entry.stab)keys.push("stab")
        if(entry.grade && entry.grade.nodes && entry.grade.nodes.length)keys.push("grade")
        Object.keys(entry.editKeys || {}).forEach(function(k){const group=/^(rect|anchor|rotation)/.test(k)?"rect":/^crop/.test(k)?"crop":"color";if(keys.indexOf(group)<0)keys.push(group)})
        const order=entry.effectOrder || []
        return keys.sort(function(a,b){const ai=order.indexOf(a),bi=order.indexOf(b);return (ai<0?999:ai)-(bi<0?999:bi)})
    }
    function entryHasFx(entry) {return effectKeys(entry).length>0}
}
