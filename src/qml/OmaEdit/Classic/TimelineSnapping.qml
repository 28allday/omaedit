import QtQml
import OmaEdit.Ui as Ui

// Where a dragged edge or clip snaps: the playhead, every cut and every
// marker in view. The window holds one as `win.snapping`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // Snapping (guide p. ~208): triggers are clip boundaries, the playhead
    // and markers, within a 10px screen tolerance at the current zoom.
    // What the edit itself gives a drag to snap to and to meet: every cut
    // and key, every marker, and where each clip lies. Read once and kept
    // until the edit or its markers change: a drag asks on every move of the
    // pointer, and reading the whole edit again each time made it stutter
    // more the longer the edit grew.
    property var held: null
    // A transition made of the clips' spare frames sits across its cut:
    // this many of its frames follow the cut. The entry before the cut runs
    // on to the transition's end and the entry after it starts there, so
    // the cut itself is this many frames before both (as MixShape has it
    // for the blocks drawn).
    function mixTail(entry) {
        if (!entry || !(entry.mixOut > 0) || !entry.mixAlign) return 0
        return Math.max(0, Math.min(entry.mixOut, entry.mixExtA || 0))
    }
    // Where a clip's block is drawn, {start, frames}: the two blocks on a
    // transition's cut meet at the cut, so the one before stops short of
    // its entry's end and the one after starts ahead of its entry's start.
    // Whatever finds the clip under a point looks here, so a click, a
    // rubber band or a hover lands on the clip drawn there.
    // Whether the transition out of `entry` lies across its cut, made of the
    // two clips' spare frames alone (one that shares frames of the pair, an
    // overlap, has no cut of its own). Such a cut rolls and trims with its
    // transition kept.
    function acrossCut(entry) {
        return !!entry && entry.mixOut > 0 && !!entry.mixAlign
               && Math.max(0, entry.mixExtA || 0) + Math.max(0, entry.mixExtB || 0) >= entry.mixOut
    }
    // `lanes`, from lanesByPlace(), where the caller goes through the lanes'
    // clips: the clip before a transition is looked up there, where finding
    // it alone walks the lanes, once for each transition met.
    function drawn(entry, lanes) {
        const before = !(entry.mixIn > 0) ? null
                     : lanes !== undefined && entry.track !== 0 ? lanes[entry.track + ":" + (entry.index - 1)]
                     : win.selection.trackEntry(entry.track, entry.index - 1)
        const lead = mixTail(before)
        return { start: entry.start - lead, frames: entry.frames + lead - mixTail(entry) }
    }
    // The lanes' clips by "track:index".
    function lanesByPlace(list) {
        const out = {}, count = list.length
        for (let i = 0; i < count; ++i) { const e = list[i]; out[e.track + ":" + e.index] = e }
        return out
    }
    function forget() { held = null } // the window says when either has changed
    function edit() {
        if (held !== null)
            return held
        const targets = [], spans = []
        const read = function(list, story) {
            const count = list.length
            for (let i=0;i<count;++i) {
                const e=list[i];targets.push(e.start,e.start+e.frames)
                // The cut a transition sits across, which is where the two
                // blocks meet: before the place the entries do.
                const tail=mixTail(e);if(tail>0)targets.push(e.start+e.frames-tail)
                spans.push({ track: story ? 0 : e.track, index: story ? i : e.index, start: e.start, frames: e.frames })
                const curves=[e.volumeKf || [],e.opacityKf || []]
                Object.keys(e.editKeys || {}).forEach(function(k){curves.push(e.editKeys[k])})
                ;(e.videoEffects || []).forEach(function(fx){Object.keys(fx.keys || {}).forEach(function(k){curves.push(fx.keys[k])});Object.keys(fx.maskKeys || {}).forEach(function(k){curves.push(fx.maskKeys[k])})})
                curves.forEach(function(keys){keys.forEach(function(k){if(k.frame>=0 && k.frame<e.frames)targets.push(e.start+k.frame)})})
            }
        }
        read(backend.timeline, true)
        read(backend.connected, false)
        for (var m = 0; m < backend.markers.length; m++)
            targets.push(backend.markers[m].frame)
        var clipMarks = win.clipMarkers.frames()
        for (var c = 0; c < clipMarks.length; c++)
            targets.push(clipMarks[c].frame)
        // The beats of mapped music, wherever its clips now lie, while
        // their lines are shown (View ▸ Show Beat Map).
        var beats = win.showBeatMap ? backend.beatFrames() : []
        for (var b = 0; b < beats.length; b++)
            targets.push(beats[b])
        held = { targets: targets, spans: spans }
        return held
    }
    function allTargets() {
        return [0, backend.duration, backend.position].concat(edit().targets)
    }
    // Sequence + clip marker frames, for Ctrl+; / Ctrl+'.
    function markerFrames() {
        var frames = []
        for (var m = 0; m < backend.markers.length; m++)
            frames.push(backend.markers[m].frame)
        var clipMarks = win.clipMarkers.frames()
        for (var c = 0; c < clipMarks.length; c++)
            frames.push(clipMarks[c].frame)
        return frames
    }
    // Snap one edge; excludeA/excludeB drop the dragged clip's own
    // boundaries so a small drag doesn't just spring back.
    function edge(frame, pxPerFrame, excludeA, excludeB) {
        if (!win.snapEnabled || pxPerFrame <= 0)
            return frame
        var tolerance = 10 / pxPerFrame
        var targets = allTargets()
        var best = frame
        var bestDist = tolerance
        for (var i = 0; i < targets.length; i++) {
            var t = targets[i]
            if (t === excludeA || t === excludeB)
                continue
            var d = Math.abs(t - frame)
            if (d < bestDist) {
                bestDist = d
                best = t
            }
        }
        return Math.round(best)
    }
    // A clip on its way along `track`, wanting to start at `start` (frames,
    // not yet whole): {frame, snapped}. Butting against a clip already on that
    // track catches it from `reach` pixels off and then holds it for `hold`
    // pixels of pushing on, so the two are felt to meet before one slides over
    // the other; failing that, either edge takes the nearest cut, marker or
    // the playhead. `self` is the clip when it is already on the timeline
    // ({track, index, start, frames}): its own edges and the clips travelling
    // with it catch nothing.
    readonly property real reach: 10
    readonly property real hold: 28
    function drag(start, frames, pxPerFrame, track, self) {
        const free = Math.max(0, start)
        if (!win.snapEnabled || pxPerFrame <= 0)
            return {frame: Math.round(free), snapped: false}
        let best = 0, bestPx = -1
        const meet = function(at, into) { // `into` > 0: pushed that far over the neighbour
            const d = Math.abs(into) * pxPerFrame
            if (at < 0 || (self && self.track === track && at === self.start)) return
            if ((into >= 0 ? d <= root.hold : d <= root.reach) && (bestPx < 0 || d < bestPx)) { best = at; bestPx = d }
        }
        const neighbour = function(t, index, e) {
            if (t !== track) return
            if (self && ((t === self.track && index === self.index)
                         || (win.selectionCount > 1 && win.selection.isSelectedAt(t, index)))) return
            meet(e.start + e.frames, e.start + e.frames - free) // my start against its end
            meet(e.start - frames, free - (e.start - frames))   // my end against its start
        }
        const spans = edit().spans
        for (let n = 0; n < spans.length; ++n)
            neighbour(spans[n].track, spans[n].index, spans[n])
        if (bestPx >= 0)
            return {frame: Math.round(best), snapped: true}
        const targets = allTargets()
        let bestDist = root.reach / pxPerFrame, found = false
        best = free
        for (let i = 0; i < targets.length; i++) {
            const t = targets[i]
            if (self && (t === self.start || t === self.start + self.frames)) continue
            if (Math.abs(t - free) < bestDist) { bestDist = Math.abs(t - free); best = t; found = true }
            if (Math.abs(t - (free + frames)) < bestDist) { bestDist = Math.abs(t - (free + frames)); best = t - frames; found = true }
        }
        return {frame: Math.max(0, Math.round(best)), snapped: found}
    }
}
