import QtQml
import OmaEdit.Ui as Ui

// Edits at a clip's edges from the keys and menus: rolling a cut, trimming
// or extending to the playhead, nudging a clip or an edge, sliding, joining,
// and which cut a dissolve command means. The window holds one as
// `win.trimming`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // Ctrl+J: join the selected clip with the next one on its track (an
    // IN edge joins with the previous one).
    function joinSelected() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        var idx = win.selectedConnectedIndex < 0 && win.selectedEdge === 0 ? e.index - 1 : e.index
        if (idx < 0)
            return
        backend.joinClip(e.track, idx)
        win.selectedEdge = -1
    }
    // How far a slide can go: [min, max] frame deltas bounded by both
    // neighbours' handles; null when the clip has no touching neighbours.
    function slideBounds(track, index) {
        var list = track === 0 ? backend.timeline : backend.connected
        var a = null, m = null, b = null
        for (var i = 0; i < list.length; i++) {
            var e = list[i]
            if (track !== 0 && e.track !== track)
                continue
            if (e.index === index - 1) a = e
            else if (e.index === index) m = e
            else if (e.index === index + 1) b = e
        }
        if (a === null || m === null || b === null)
            return null
        if (m.start !== a.start + a.frames || b.start !== m.start + m.frames)
            return null
        return [Math.max(a.in - a.out, -b.in),
                Math.min(a.length - 1 - a.out, b.out - b.in)]
    }
    // Roll the selected edge, the selected clip's following cut, or the open
    // Precision Editor cut. Bounds include linked sound and lane neighbours.
    readonly property var rollEditTarget: buildRollTarget()
    function rollTarget() { return rollEditTarget }
    function buildRollTarget() {
        if (backend.viewingSource || backend.exporting || !backend.hasVideo) return null
        var selected = win.selection.selectedEntry()
        var track = win.precisionBoundary >= 0 ? win.precisionTrack : (selected ? selected.track : -1)
        var index = win.precisionBoundary >= 0 ? win.precisionBoundary : (selected ? selected.index : -1)
        if (track < 0 || index < 0) return null
        if (win.precisionBoundary < 0) {
            if (win.selectedEdge === 0 || (win.selectedEdge < 0 && win.selection.trackEntry(track, index + 1) === null)) index--
        }
        var a = win.selection.trackEntry(track, index), b = win.selection.trackEntry(track, index + 1)
        if (a === null || b === null || b.start !== a.start + a.frames) return null
        function belongs(e) {
            return win.selection.sameAddress(e, a) || win.selection.sameAddress(e, b)
                || (!!a.link && e.link === a.link) || (!!b.link && e.link === b.link)
        }
        if (win.precisionBoundary < 0 && !win.selection.selectedEntries().every(belongs)) return null
        var limits = rollLimits(track, index)
        return limits === null ? null
             : { track: track, index: index, frame: limits.frame, minimum: limits.minimum, maximum: limits.maximum }
    }
    // Whether a clip is a multicam clip with a sound source of its own: its
    // sound is that source under the picture wherever a cut lies, so it has
    // no spare frames of its own for a roll to run out of.
    function hasOwnSound(e) {
        if (typeof e.mcam !== "string" || e.mcam.indexOf("\"sound\"") < 0) return false
        try {
            var camera = JSON.parse(e.mcam)
            return camera.version === 2 && !!camera.sound && !!camera.sound.path
        } catch (error) {
            return false
        }
    }
    // How far the cut after clip `index` of `track` can be rolled, as the
    // engine will have it: {frame, minimum, maximum}, the cut's frame and the
    // fewest and most frames it can move; null where the two clips do not
    // butt together or the cut cannot be rolled at all (a locked track, a
    // gap, a transition on another edge). The keys, the Trim Editor and a
    // drag of the cut by the mouse all stop at these.
    function rollLimits(track, index) {
        var a = win.selection.trackEntry(track, index), b = win.selection.trackEntry(track, index + 1)
        if (a === null || b === null || b.start !== a.start + a.frames) return null
        function belongs(e) {
            return win.selection.sameAddress(e, a) || win.selection.sameAddress(e, b)
                || (!!a.link && e.link === a.link) || (!!b.link && e.link === b.link)
        }
        var all = backend.timeline.concat(backend.connected)
        var edges = [{ entry: a, outgoing: true }, { entry: b, outgoing: false }]
        var ownA = hasOwnSound(a), ownB = hasOwnSound(b)
        for (var i = 0; i < all.length; i++) {
            var e = all[i]
            if (!belongs(e)) continue
            if (!backend.tracks[e.track] || backend.tracks[e.track].locked) return null
            if (win.selection.sameAddress(e, a) || win.selection.sameAddress(e, b) || !e.audioOnly) continue
            var followsA = !a.audioOnly && !!a.link && e.link === a.link && !ownA
            var followsB = !b.audioOnly && !!b.link && e.link === b.link && !ownB
            if (followsA && followsB) return null
            if (followsA || followsB) edges.push({ entry: e, outgoing: followsA })
        }
        var minimum = -2147483647, maximum = 2147483647
        function edited(e) { return edges.some(function(edge) { return win.selection.sameAddress(edge.entry, e) }) }
        // A transition on this cut rolls with it, as far as both clips still
        // cover their halves of it: each keeps a frame of its own outside
        // the transition (its entry counts the transition out of it).
        var across = b.mixIn > 0 && win.snapping.acrossCut(a)
        for (var j = 0; j < edges.length; j++) {
            var part = edges[j], clip = part.entry
            if (clip.gap || (!across && (clip.mixIn > 0 || clip.mixOut > 0)) || !backend.tracks[clip.track] || backend.tracks[clip.track].mix.editKind === "caption") return null
            var own = clip.frames - (clip.mixOut > 0 ? clip.mixOut : 0)
            if (part.outgoing) {
                minimum = Math.max(minimum, -(own - 1))
                maximum = Math.min(maximum, clip.length - clip.out - 1)
            } else {
                minimum = Math.max(minimum, -clip.in, -clip.start)
                maximum = Math.min(maximum, own - 1)
            }
            if (clip.track === 0) continue
            for (var k = 0; k < all.length; k++) {
                var other = all[k]
                if (other.track !== clip.track || edited(other)) continue
                if (part.outgoing && other.start >= clip.start + clip.frames)
                    maximum = Math.min(maximum, other.start - clip.start - clip.frames)
                if (!part.outgoing && other.start + other.frames <= clip.start)
                    minimum = Math.max(minimum, other.start + other.frames - clip.start)
            }
        }
        // (The cut: where the two blocks meet, which a transition's entries run past.)
        return { frame: win.snapping.drawn(b).start, minimum: minimum, maximum: maximum }
    }
    function canRollEdit(delta) {
        var target = rollTarget()
        if (target === null) return false
        var step = delta === undefined ? backend.position - target.frame : delta
        return step !== 0 && step >= target.minimum && step <= target.maximum
    }
    function rollSelectedEdit(delta) {
        var target = rollTarget()
        if (target === null) return
        var step = delta === undefined ? backend.position - target.frame : delta
        if (!canRollEdit(step)) return
        if (win.precisionBoundary < 0) {
            var e = win.selection.selectedEntry()
            win.selectedEdge = e.index === target.index ? 1 : 0
        }
        backend.rollEditOn(target.track, target.index, step)
    }
    function canTrimToPlayhead() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true || backend.viewingSource || backend.exporting)
            return false
        var entries = win.selection.selectedEntries()
        return entries.every(function(item) {
            return backend.tracks[item.track] && !backend.tracks[item.track].locked
                && (win.selection.sameAddress(item, e) || (!!e.link && item.link === e.link))
        }) && win.selection.linkMates(e).every(function(mate) {
            return backend.tracks[mate.track] && !backend.tracks[mate.track].locked
        })
    }
    function trimSelectedToPlayhead(end) {
        if (!canTrimToPlayhead()) return
        var e = win.selection.selectedEntry()
        backend.trimToPlayhead(e.track, e.index, end)
    }
    // Shift+X chooses an edge; the explicit commands choose start or end.
    // All three share worker-side validation and the current ripple mode.
    function extendEditToPlayhead() {
        if (!canTrimToPlayhead()) return
        var e = win.selection.selectedEntry()
        var p = backend.position
        var edge = win.selectedEdge >= 0
                   ? win.selectedEdge : (p < e.start + e.frames / 2 ? 0 : 1)
        backend.trimToPlayhead(e.track, e.index, edge === 1)
    }
    // Alt+←/→: move the selected clip by frames (Shift adds a 10-frame step).
    function nudgeSelected(delta) {
        if (win.selectionCount > 1) {
            backend.nudgeClips(win.selection.selectedAddresses(), delta)
            return
        }
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        backend.nudgeClip(e.track, e.index, delta)
    }
    // Ctrl+T targets the boundary under the selected edge: an IN edge rides
    // the boundary with the PREVIOUS clip (transitions apply to the
    // selected edit point), anything else the clip's own out boundary.
    function dissolveTarget() {
        if (win.selectedTimelineIndex < 0)
            return -1
        if (win.selectedEdge === 0)
            return win.selectedTimelineIndex - 1 // -1 before the first clip: refuse
        return win.selectedTimelineIndex
    }
    function nudgeEdge(delta) {
        if (win.selectedTimelineIndex < 0 || win.selectedEdge < 0
                || win.selectedTimelineIndex >= backend.timeline.length)
            return
        var c = backend.timeline[win.selectedTimelineIndex]
        if (win.selectedEdge === 0)
            backend.trimClip(win.selectedTimelineIndex, c.in + delta, c.out)
        else
            backend.trimClip(win.selectedTimelineIndex, c.in, c.out + delta)
    }
}
