import QtQml
import OmaEdit.Ui as Ui

// From a clip on the timeline back to its source in the library: Match
// Frame, Reveal Source, and the edits that take the library's selection
// (Replace, Fit to Fill). The window holds one as `win.matching`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // Shift+R: replace the selected clip with the selected library clip's
    // material, from its range start, over the same span.
    // `fromEnd`: counted back from its range end instead.
    function replaceSelected(fromEnd) {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true || win.selectedLibraryIndex < 0)
            return
        backend.replaceClip(win.selectedLibraryIndex, e.track, e.index, fromEnd === true)
    }
    // Match: the frame of a clip to use is the playhead's when it is over the
    // clip, else the clip's middle.
    function matchFrameOf(e) {
        return backend.position >= e.start && backend.position < e.start + e.frames
               ? backend.position - e.start : Math.floor(e.frames / 2)
    }
    function isMatchReference(e) {
        return !!e && win.matchReferenceSet && e.track === backend.matchReference.track && e.index === backend.matchReference.index
    }
    // The library entry a timeline clip came from: same path; a subclip
    // whose range holds `sourceFrame` wins over the whole clip.
    function libraryIndexForEntry(e, sourceFrame) {
        if (e === null || e === undefined || e.path === undefined)
            return -1
        var whole = -1
        for (var i = 0; i < backend.library.length; i++) {
            var c = backend.library[i]
            if (c.path !== e.path || c.offline === true)
                continue
            var rIn = c.rangeIn === undefined ? -1 : c.rangeIn
            var rOut = c.rangeOut === undefined ? -1 : c.rangeOut
            if (rIn < 0 && rOut < 0) {
                if (whole < 0)
                    whole = i
            } else if (sourceFrame !== undefined && sourceFrame >= Math.max(0, rIn)
                       && (rOut < 0 || sourceFrame <= rOut)) {
                return i
            }
        }
        return whole
    }
    // The clip Match Frame / Reveal act on: the selection, else the V1
    // clip under the playhead.
    function matchTarget() {
        var e = win.selection.selectedEntry()
        if (e !== null && e.gap !== true && e.path !== undefined && e.path.indexOf("color:") !== 0)
            return e
        var p = backend.position
        for (var i = 0; i < backend.timeline.length; i++) {
            var c = backend.timeline[i]
            if (c.gap !== true && p >= c.start && p < c.start + c.frames
                    && c.path !== undefined && c.path.indexOf("color:") !== 0)
                return c
        }
        return null
    }
    // F: the source viewer on the clip's file, parked on the frame under
    // the playhead, I/O on the clip's span (source frames — the file plays
    // at 1×, so timewarp frames scale back by the rate).
    function matchFrame() {
        var e = matchTarget()
        if (e === null || (e.title !== undefined && e.title !== "") || e.compound === true)
            return
        var rate = e.speed === undefined || e.speed === 0 ? 1 : Math.abs(e.speed)
        var base = e.in + (e.mixIn === undefined ? 0 : e.mixIn)
        var p = backend.position
        var offset = p >= e.start && p < e.start + e.frames ? p - e.start : 0
        var sourceFrame = Math.round((base + offset) * rate)
        var rangeIn=Math.round(base*rate),rangeOut=Math.round((e.out+1)*rate)-1
        if(e.retime!==undefined) {
            var match=backend.speedRampSourceMatch(e,p)
            if(!match.valid){backend.announce(match.error);return}
            sourceFrame=match.frame;rangeIn=match.in;rangeOut=match.out
        }
        var idx = libraryIndexForEntry(e, sourceFrame)
        if (idx < 0) {
            backend.announce(qsTr("The source of %1 is not in the library").arg(e.name))
            return
        }
        win.selectedLibraryIndex = idx
        win.showLibraryTab()
        backend.previewClipAt(idx, sourceFrame, rangeIn, rangeOut)
    }
    // Shift+F: select the clip's library entry in the browser.
    function revealSourceClip() {
        var e = matchTarget()
        if (e === null)
            return
        var idx = libraryIndexForEntry(e, e.in)
        if (idx < 0) {
            backend.announce(qsTr("The source of %1 is not in the library").arg(e.name))
            return
        }
        win.selectedLibraryIndex = idx
        win.showLibraryTab()
        backend.announce(qsTr("Revealed %1").arg(backend.library[idx].name))
    }
    // Shift+F11: the selected library clip (its I/O, or all of it) retimed
    // to fill the timeline range — or the selected clip's span.
    function fitToFill() {
        if (win.selectedLibraryIndex < 0)
            return
        var track = backend.destTrack
        var position = -1, frames = 0, replaces = -1
        if (win.hasTimelineRange && win.timelineRangeOut >= win.timelineRangeIn) {
            position = win.timelineRangeIn
            frames = win.timelineRangeOut - win.timelineRangeIn + 1
        } else {
            var e = win.selection.selectedEntry()
            if (e === null || e.gap === true)
                return
            // The clip as it is drawn (cut to cut, where a transition lies
            // across its cut), which the new one takes the place of.
            var block = win.snapping.drawn(e)
            track = e.track
            position = block.start
            frames = block.frames
            replaces = e.index
        }
        if (frames < 1)
            return
        backend.viewTimeline()
        backend.fitToFill(win.selectedLibraryIndex, track, position, frames, replaces)
    }
}
