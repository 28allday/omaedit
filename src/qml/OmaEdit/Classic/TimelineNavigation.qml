import QtQml
import OmaEdit.Ui as Ui

// Getting about the timeline: zooming, and moving the playhead to the
// markers and edit points either side of it. The window holds one as
// `win.navigation`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    function zoomIn() {
        if (backend.duration < 1)
            return
        var effective = win.pxPerFrame > 0 ? win.pxPerFrame
                                       : win.timelineScroll.width / backend.duration
        // A short edit can be held at its fitted scale above the ceiling:
        // zooming in from there stays put, it never zooms out.
        win.pxPerFrame = Math.min(Math.max(20, effective), effective * 1.5)
        Qt.callLater(centerPlayhead)
    }
    function zoomOut() {
        if (win.pxPerFrame <= 0 || backend.duration < 1)
            return
        var fit = win.timelineScroll.width / backend.duration
        var next = win.pxPerFrame / 1.5
        if (next <= fit) {
            win.pxPerFrame = 0 // back to fit — stop zooming out into a sliver
        } else {
            win.pxPerFrame = next
            Qt.callLater(centerPlayhead)
        }
    }
    // Zoom by `factor` keeping `frame` under the pointer (the zoom tool and
    // the toolbar slider both go through here).
    function zoomAt(frame, factor) {
        if (backend.duration < 1)
            return
        var fit = win.timelineScroll.width / backend.duration
        var effective = win.pxPerFrame > 0 ? win.pxPerFrame : fit
        var viewX = frame * effective - win.timelineScroll.contentX
        var next = effective * factor
        if (next <= fit) {
            win.pxPerFrame = 0
            return
        }
        win.pxPerFrame = Math.min(Math.max(20, effective), next) // as zoomIn: never out by zooming in
        Qt.callLater(function () {
            win.timelineScroll.contentX = Math.max(0, Math.min(
                frame * win.pxPerFrame - viewX,
                Math.max(0, win.timelineScroll.contentWidth - win.timelineScroll.width)))
        })
    }
    // Zoom as a 0…1 slider value: 0 = fit, 1 = the 20 px/frame ceiling,
    // geometric in between so every step feels the same.
    readonly property real zoomLevel: {
        if (win.pxPerFrame <= 0 || backend.duration < 1)
            return 0
        var fit = win.timelineScroll.width / backend.duration
        if (fit >= 20)
            return 0
        return Math.max(0, Math.min(1,
            Math.log(win.pxPerFrame / fit) / Math.log(20 / fit)))
    }
    function setZoomLevel(level) {
        if (backend.duration < 1 || level <= 0) {
            win.pxPerFrame = 0
            return
        }
        var fit = win.timelineScroll.width / backend.duration
        if (fit >= 20)
            return
        win.pxPerFrame = fit * Math.pow(20 / fit, Math.min(1, level))
        Qt.callLater(centerPlayhead)
    }
    function centerPlayhead() {
        if (backend.duration > 1)
            win.timelineScroll.keepInView(backend.position / Math.max(1, win.timelineScroll.shownFrames)
                                      * win.timelineScroll.contentWidth)
    }
    function previousMarker() {
        var best = -1
        var frames = win.snapping.markerFrames()
        for (var i = 0; i < frames.length; i++) {
            var f = frames[i]
            if (f < backend.position && f > best)
                best = f
        }
        return best
    }
    function nextMarker() {
        var best = -1
        var frames = win.snapping.markerFrames()
        for (var i = 0; i < frames.length; i++) {
            var f = frames[i]
            if (f > backend.position && (best < 0 || f < best))
                best = f
        }
        return best
    }
    // The mapped beat before or after the playhead (-1 for none), for the
    // Previous Beat and Next Beat commands; seekMarker goes there.
    function beatFrom(direction) {
        var best = -1
        var frames = backend.beatMapRevision === 0 ? [] : backend.beatFrames()
        for (var i = 0; i < frames.length; i++) {
            var f = frames[i]
            if (direction < 0 ? f < backend.position && f > best : f > backend.position && (best < 0 || f < best))
                best = f
        }
        return best
    }
    function seekMarker(frame) {
        if (frame < 0)
            return
        backend.viewTimeline()
        backend.pause()
        backend.seek(frame)
    }
    // Edit-point navigation (↑/↓): clip starts, plus the very end. Where a
    // transition sits across a cut the edit point is the cut, not the
    // transition's end, where the clip's entry starts.
    function cutBefore(i) {
        var e = backend.timeline[i]
        return e.start - (i > 0 && e.mixIn > 0 ? win.snapping.mixTail(backend.timeline[i - 1]) : 0)
    }
    function previousEditPoint() {
        var best = 0
        for (var i = 0; i < backend.timeline.length; i++) {
            var start = cutBefore(i)
            if (start < backend.position && start > best)
                best = start
        }
        return best
    }
    function nextEditPoint() {
        var best = backend.duration - 1
        for (var i = 0; i < backend.timeline.length; i++) {
            var start = cutBefore(i)
            if (start > backend.position && start < best)
                best = start
        }
        return best
    }
}
