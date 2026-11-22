import QtQml
import OmaEdit.Ui as Ui

// Playing a stretch of the timeline: around an edit point, the selection,
// from the beginning; and dynamic trim, which trims the selected edge to
// wherever playback stops. The window holds one as `win.playing`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    function toggleDynamicTrim() {
        win.dynamicTrim = !win.dynamicTrim
        if (win.dynamicTrim && win.selectedEdge >= 0 && win.trimEdgeFrame >= 0)
            parkAtTrimEdge()
        backend.announce(win.dynamicTrim ? qsTr("Dynamic trim: play, and the selected edge follows where you stop")
                                     : qsTr("Dynamic trim off"))
    }
    function parkAtTrimEdge() { // the frame a stop here would leave unchanged
        var c = win.selection.selectedEntry()
        if (c === null) return
        backend.viewTimeline()
        backend.seek(win.selectedEdge === 0 ? c.start : c.start + c.frames - 1)
    }
    function playAround() {
        if (!backend.hasVideo)
            return
        var around = win.trimEdgeFrame >= 0 ? win.trimEdgeFrame : backend.position
        var roll = Math.round(backend.fps * 2)
        playBetween(Math.max(0, around - roll), Math.min(backend.duration - 1, around + roll))
    }
    function playBetween(first, last) {
        backend.viewTimeline()
        // pause() reports the stop at once: that stop is Play Around's own,
        // not one that ends a dynamic trim.
        win.startingPlayAround = true
        backend.pause()
        win.startingPlayAround = false
        win.playAroundEnd = last
        win.playingAround = true
        backend.seek(first)
        backend.play()
    }
    // The range, else the selected clip.
    function playSelectionSpan() {
        if (win.timelineRangeIn >= 0 && win.timelineRangeOut >= win.timelineRangeIn)
            return { first: win.timelineRangeIn, last: win.timelineRangeOut }
        var e = win.selection.selectedClip()
        return e === null ? null : { first: e.start, last: e.start + e.frames - 1 }
    }
    function playSelection() {
        var span = playSelectionSpan()
        if (span !== null)
            playBetween(span.first, span.last)
    }
    function playFromBeginning() {
        backend.viewTimeline()
        backend.seek(0)
        backend.play()
    }
}
