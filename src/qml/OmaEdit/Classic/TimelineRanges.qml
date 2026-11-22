import QtQml
import OmaEdit.Ui as Ui

// The marked range (I and O): setting and clearing it on the timeline or
// the source, and the edits that act on it. The range itself
// (timelineRangeIn / timelineRangeOut) stays on the window. The window
// holds one as `win.ranges`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // I/O mark the SOURCE range in source view (three-point editing) and
    // the timeline selection range in program view.
    function setStart() {
        if (backend.viewingSource) {
            backend.markRangeIn()
            return
        }
        if (!backend.hasVideo)
            return
        win.timelineRangeIn = backend.position
        if (win.timelineRangeOut >= 0 && win.timelineRangeOut < win.timelineRangeIn)
            win.timelineRangeOut = -1
    }
    function setEnd() {
        if (backend.viewingSource) {
            backend.markRangeOut()
            return
        }
        if (!backend.hasVideo)
            return
        win.timelineRangeOut = backend.position
        if (win.timelineRangeIn >= 0 && win.timelineRangeIn > win.timelineRangeOut)
            win.timelineRangeIn = -1
    }
    function clearAny() {
        if (backend.viewingSource) {
            backend.clearRange()
            return
        }
        win.timelineRangeIn = -1
        win.timelineRangeOut = -1
    }
    function selectClip() { // X — the clip under the playhead
        if (backend.viewingSource || !backend.hasVideo)
            return
        for (var i = 0; i < backend.timeline.length; i++) {
            var c = backend.timeline[i]
            if (backend.position >= c.start
                    && backend.position < c.start + c.frames) {
                win.timelineRangeIn = c.start
                win.timelineRangeOut = c.start + c.frames - 1
                return
            }
        }
    }
    function deleteSelectedOrRange() {
        if (win.selection.closeSelectedGap())
            return
        if (!backend.viewingSource && win.hasTimelineRange && backend.hasVideo) {
            backend.deleteRange(win.effRangeIn,
                                Math.min(win.effRangeOut, backend.duration - 1))
            win.timelineRangeIn = -1
            win.timelineRangeOut = -1
            win.selectedTimelineIndex = -1
            win.selectedConnectedIndex = -1
            return
        }
        win.clipCommands.deleteSelected()
    }
    function canTrimTo() {
        var e = win.selection.selectedClip()
        return win.workspace === "Edit" && e !== null && win.timelineRangeIn >= 0
                && win.timelineRangeOut >= win.timelineRangeIn && win.timelineRangeIn < e.start + e.frames
                && win.timelineRangeOut >= e.start && win.retiming.selectedSpeed() === 1 && e.retime === undefined
    }
    function trimSelectedTo() {
        if (!canTrimTo())
            return
        var e = win.selection.selectedClip()
        var base = win.clipMarkers.baseFrame(e)
        backend.trimClipOn(e.track, e.index,
                           base + Math.max(win.timelineRangeIn, e.start) - e.start,
                           base + Math.min(win.timelineRangeOut, e.start + e.frames - 1) - e.start)
    }
}
