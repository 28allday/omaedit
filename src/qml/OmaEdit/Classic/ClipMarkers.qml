import QtQml

// Markers on clips and on the timeline, as the keys and menus use them.
// The window holds one as `win.clipMarkers`.
//
// They live on the SELECTED clip (M): when the playhead is
// over it, M adds/removes a clip marker there (a timeline marker
// otherwise) and Shift+M edits the one under the playhead. Frames are
// SOURCE frames, so a marker rides trims and moves; the block draws
// them as flags, and they join snapping, Ctrl+; / Ctrl+' and the index.
QtObject {
    id: root
    // The window, for the selected clip; the dialog that edits a marker.
    required property var win
    required property var dialog

    function targetClip() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true || backend.viewingSource)
            return null
        var p = backend.position
        if (p < e.start || p >= e.start + e.frames)
            return null
        return e
    }
    // Entries' in/out are TIMEWARP frames (the engine rescales them, and
    // the markers, on a retime), so a timeline frame maps 1:1; a clip that
    // follows a dissolve shows its in pre-shrunk by the overlap, and its
    // block starts after it.
    function baseFrame(e) {
        return e.in + (e.mixIn === undefined ? 0 : e.mixIn)
    }
    function sourceFrame(e) {
        return baseFrame(e) + (backend.position - e.start)
    }
    function clipMarkerHere() { // { entry, marker } or null
        var e = targetClip()
        if (e === null || e.markers === undefined || e.markers === null)
            return null
        var sf = sourceFrame(e)
        for (var i = 0; i < e.markers.length; i++)
            if (e.markers[i].frame === sf)
                return { entry: e, marker: e.markers[i] }
        return null
    }
    function toggleHere() { // M
        backend.viewTimeline()
        var e = targetClip()
        if (e === null) {
            backend.toggleMarker()
            return
        }
        var hit = clipMarkerHere()
        if (hit !== null)
            backend.removeClipMarkerOn(e.track, e.index, hit.marker.frame)
        else
            backend.addClipMarkerOn(e.track, e.index, sourceFrame(e),
                                    "standard", "", "")
    }
    function modifyHere() { // Shift+M
        var hit = clipMarkerHere()
        if (hit !== null) {
            dialog.openForClip(hit.entry.track, hit.entry.index, hit.marker)
            return
        }
        var marker = timelineMarkerHere()
        if (marker !== null)
            dialog.openFor(marker)
    }
    // Every clip marker in view as an absolute frame: [{frame, marker, entry}].
    function frames() {
        var out = []
        var lists = [backend.timeline, backend.connected]
        for (var l = 0; l < lists.length; l++) {
            for (var i = 0; i < lists[l].length; i++) {
                var e = lists[l][i]
                if (e.markers === undefined || e.markers === null || e.gap === true)
                    continue
                var base = baseFrame(e)
                for (var m = 0; m < e.markers.length; m++) {
                    var mk = e.markers[m]
                    if (mk.frame < base || mk.frame > e.out)
                        continue // trimmed away — kept, just not shown
                    out.push({ frame: e.start + (mk.frame - base),
                               marker: mk, entry: e })
                }
            }
        }
        return out
    }
    // A block's flags: [{frac, type, text}] along its own length.
    function flags(e) {
        if (e === undefined || e === null || e.markers === undefined
                || e.markers === null || e.gap === true)
            return []
        var out = []
        var base = baseFrame(e)
        var span = Math.max(1, e.frames)
        for (var m = 0; m < e.markers.length; m++) {
            var mk = e.markers[m]
            if (mk.frame < base || mk.frame > e.out)
                continue
            out.push({ frac: (mk.frame - base) / span, type: mk.type, colour: mk.colour, text: mk.text })
        }
        return out
    }
    // Where the timeline's markers are, read when they change: the menus
    // and commands ask "is there one here" on every frame of playback, and
    // walking the whole list for each of them each frame is not needed.
    readonly property var timelineMarkerRows: {
        var rows = {}
        for (var i = 0; i < backend.markers.length; i++) {
            var frame = backend.markers[i].frame
            if (rows[frame] === undefined)
                rows[frame] = i
        }
        return rows
    }
    function timelineMarkerHere() {
        var row = timelineMarkerRows[backend.position]
        return row === undefined ? null : backend.markers[row]
    }

    // Marker colours: standard blue, chapter orange, to-do red, done green.
    function colorOf(type) { return Ui.Theme.markerColor(type) }
    function typeLabel(type) {
        if (type === "chapter")
            return qsTr("Chapter")
        if (type === "todo")
            return qsTr("To-Do")
        if (type === "done")
            return qsTr("Completed")
        return qsTr("Marker")
    }
    function addAndModify() {
        var onClip = targetClip() !== null
        if (onClip ? clipMarkerHere() !== null : timelineMarkerHere() !== null) {
            modifyHere()
            return
        }
        win.modifyAddedMarker = true
        toggleHere()
    }
    function openAdded() {
        if (!win.modifyAddedMarker
                || (targetClip() !== null ? clipMarkerHere() : timelineMarkerHere()) === null)
            return
        win.modifyAddedMarker = false
        modifyHere()
    }
    function addTyped(type) {
        backend.viewTimeline()
        var e = targetClip()
        if (e !== null) {
            var hit = clipMarkerHere()
            if (hit !== null)
                backend.setClipMarkerOn(e.track, e.index, hit.marker.frame, type, hit.marker.text, hit.marker.notes)
            else
                backend.addClipMarkerOn(e.track, e.index, sourceFrame(e), type, "", "")
            return
        }
        var marker = timelineMarkerHere()
        if (marker === null)
            backend.toggleMarker()
        backend.setMarkerInfo(backend.position, marker === null ? "" : marker.text, type,
                              marker === null ? "" : marker.notes)
    }
    function deleteHere() {
        var hit = clipMarkerHere()
        if (hit !== null)
            backend.removeClipMarkerOn(hit.entry.track, hit.entry.index, hit.marker.frame)
        else if (timelineMarkerHere() !== null)
            backend.toggleMarker()
    }
    function deleteInRange() {
        var clipMarks = frames()
        for (var i = 0; i < clipMarks.length; i++) {
            var m = clipMarks[i]
            if (m.frame >= win.timelineRangeIn && m.frame <= win.timelineRangeOut)
                backend.removeClipMarkerOn(m.entry.track, m.entry.index, m.marker.frame)
        }
        backend.removeMarkersIn(win.timelineRangeIn, win.timelineRangeOut)
    }
    // The marker stays under the playhead, so the key can be pressed again.
    function nudgeHere(delta) {
        var hit = clipMarkerHere()
        var marker = hit !== null ? hit.marker : timelineMarkerHere()
        if (marker === null)
            return
        var target = hit !== null ? backend.markerEditTarget(hit.entry.track, hit.entry.index, marker.frame)
                                  : backend.markerEditTarget(-1, -1, marker.frame)
        var to = marker.frame + delta
        if (target.marker === undefined || to < 0 || to > target.maximum)
            return
        backend.applyMarkerEdit(target, to, marker.type, marker.text, marker.notes)
        backend.seek(backend.position + delta)
    }
}
