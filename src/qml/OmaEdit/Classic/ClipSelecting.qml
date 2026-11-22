import QtQml
import OmaEdit.Ui as Ui

// What can be done with the clip selection: choosing one clip or many,
// linked mates, a gap, a range, a whole track; and the moves a group makes
// when it is dragged, swapped or copied. The selection itself (which clip,
// which extras, which edge) stays on the window, where the pages and the
// tests set it; this reads and writes it there. The window holds one as
// `win.selection`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win
    // The window's object that follows the selected lane clip through reordering.
    required property var connected

    function selectGap(gap) {
        clearSelection()
        win.selectedGap = gap
    }
    function closeSelectedGap() {
        if (win.selectedGap === null)
            return false
        backend.closeGap(win.selectedGap.track, win.selectedGap.start)
        win.selectedGap = null
        return true
    }
    function refindV1Primary() {
        const count = backend.timeline.length
        const changed = count !== win.v1Count
        win.v1Count = count
        if (!changed || win.selectedTimelineIndex < 0 || win.v1Identity === null)
            return
        const id = win.v1Identity
        let best = -1, bestScore = -1, bestDistance = 0
        for (let i = 0; i < count; ++i) {
            const e = backend.timeline[i]
            if (e.path !== id.path || (e.title || "") !== id.title || (e.gap === true) !== id.gap)
                continue
            const score = (e["in"] === id["in"] ? 2 : 0) + (e.out === id.out ? 1 : 0)
            // Sharing the file is not enough: a clip with neither end in
            // common is the selected one only when it is a piece of it (both
            // ends cut away). Another clip cut from the same file is another
            // clip, and the selection does not jump to it when its own goes.
            if (score === 0 && !(e["in"] >= id["in"] && e.out <= id.out))
                continue
            const distance = Math.abs(e.start - id.start)
            if (score > bestScore || (score === bestScore && distance < bestDistance)) {
                best = i; bestScore = score; bestDistance = distance
            }
        }
        // A multicam segment put on another angle is another file, and in
        // pieces where the angle has no footage: the selection stays where
        // the clip stood, on the part of the same multicam clip that begins there.
        if (best < 0 && id.camera !== undefined && id.camera !== "") {
            for (let j = 0; j < count; ++j)
                if (backend.timeline[j].start === id.start && win.multicamIdOf(backend.timeline[j]) === id.camera)
                    best = j
        }
        if (best === win.selectedTimelineIndex)
            return
        win.holdExtras = true
        win.selectedTimelineIndex = best
        if (best < 0)
            win.selectedEdge = -1
        win.holdExtras = false
    }
    function primaryMoved() {
        if (win.holdExtras || connected.restoring)
            return
        win.extraSelection = win.linkedSelection ? linkMates(primaryAddress()) : []
    }
    function primaryAddress() {
        var e = selectedEntry()
        return e === null ? null : { track: e.track, index: e.index }
    }
    function sameAddress(a, b) {
        return a !== null && b !== null && a.track === b.track && a.index === b.index
    }
    function addressKey(a) { return a.track + ":" + a.index }
    // The extras by address. Every clip block asks whether it is selected,
    // so the answer must not cost a walk of the whole set each time.
    readonly property var extraKeys: {
        var keys = {}
        for (var i = 0; i < win.extraSelection.length; i++)
            keys[addressKey(win.extraSelection[i])] = true
        return keys
    }
    function isSelectedAt(track, index) {
        var a = { track: track, index: index }
        if (sameAddress(primaryAddress(), a))
            return true
        return extraKeys[addressKey(a)] === true
    }
    // Both entry lists read once: every clip by its address, every address
    // in allAddresses() order, and the addresses that share each link id.
    // A gesture that asks about many clips (Select All, a marquee, a group
    // drag) builds one and hands it down; asking the backend's lists again
    // for each clip made those gestures grow with the cube of the edit.
    function entryLookup() {
        var story = [], lanes = {}, all = [], mates = new Map()
        var timeline = backend.timeline, connected = backend.connected
        var count = timeline.length
        for (var i = 0; i < count; i++) {
            var s = timeline[i]
            story.push(s)
            if (s.gap !== true)
                all.push({ track: 0, index: i })
        }
        count = connected.length
        for (var j = 0; j < count; j++) {
            var c = connected[j]
            var key = c.track + ":" + c.index
            if (lanes[key] === undefined)
                lanes[key] = c
            all.push({ track: c.track, index: c.index })
        }
        function entry(track, idx) {
            if (idx < 0)
                return null
            if (track === 0)
                return idx < story.length ? story[idx] : null
            var e = lanes[track + ":" + idx]
            return e === undefined ? null : e
        }
        for (var k = 0; k < all.length; k++) {
            var m = entry(all[k].track, all[k].index)
            if (m === null || m.link === undefined || m.link === "")
                continue
            var group = mates.get(m.link)
            if (group === undefined)
                mates.set(m.link, group = [])
            group.push(all[k])
        }
        return { all: all, entry: entry, mates: mates }
    }
    function selectedAddresses() { // the primary first
        var out = []
        var p = primaryAddress()
        if (p !== null)
            out.push(p)
        for (var i = 0; i < win.extraSelection.length; i++)
            out.push(win.extraSelection[i])
        return out
    }
    function selectedEntries() { return entriesOfSelection(undefined) }
    function entriesOfSelection(lookup) {
        var out = []
        var list = selectedAddresses()
        if (lookup === undefined && list.length > 1)
            lookup = entryLookup()
        for (var i = 0; i < list.length; i++) {
            var e = lookup === undefined ? trackEntry(list[i].track, list[i].index)
                                         : lookup.entry(list[i].track, list[i].index)
            if (e !== null)
                out.push(e)
        }
        return out
    }
    function connectedIndexOf(track, index) {
        for (var i = 0; i < backend.connected.length; i++) {
            var e = backend.connected[i]
            if (e.track === track && e.index === index)
                return i
        }
        return -1
    }
    // The clips sharing `addr`'s link id, as addresses (never `addr`).
    function linkMates(addr, lookup) {
        var e = addr === null ? null
              : lookup === undefined ? trackEntry(addr.track, addr.index)
              : lookup.entry(addr.track, addr.index)
        if (e === null || e.link === undefined || e.link === "")
            return []
        if (lookup === undefined)
            lookup = entryLookup()
        var out = []
        var group = lookup.mates.get(e.link)
        for (var i = 0; group !== undefined && i < group.length; i++)
            if (!sameAddress(group[i], addr))
                out.push(group[i])
        return out
    }
    // A clip picked without its linked partner (linked selection off, or
    // Alt-click): a move takes it alone and the pair stops being linked.
    function hasUnselectedMate(track, index, lookup) {
        const mates = linkMates({ track: track, index: index }, lookup)
        for (let i = 0; i < mates.length; ++i)
            if (!isSelectedAt(mates[i].track, mates[i].index))
                return true
        return false
    }
    function withMates(list, lookup) {
        if (!win.linkedSelection)
            return list
        if (lookup === undefined)
            lookup = entryLookup()
        var out = list.slice()
        var listed = {}
        for (var n = 0; n < out.length; n++)
            if (out[n] !== null)
                listed[addressKey(out[n])] = true
        for (var i = 0; i < list.length; i++) {
            var mates = linkMates(list[i], lookup)
            for (var j = 0; j < mates.length; j++) {
                var key = addressKey(mates[j])
                if (listed[key] === true)
                    continue
                listed[key] = true
                out.push(mates[j])
            }
        }
        return out
    }
    function allAddresses() { // every clip on every track, gaps skipped
        var out = []
        for (var i = 0; i < backend.timeline.length; i++)
            if (backend.timeline[i].gap !== true)
                out.push({ track: 0, index: i })
        for (var j = 0; j < backend.connected.length; j++)
            out.push({ track: backend.connected[j].track,
                       index: backend.connected[j].index })
        return out
    }
    function setPrimary(track, index) {
        win.selectedGap = null
        win.relinkPending = false // a new choice overrides a reorder still in flight
        win.reorderIdentity = null
        if (track === 0) {
            win.selectedConnectedIndex = -1
            win.selectedTimelineIndex = index
        } else {
            win.selectedTimelineIndex = -1
            win.selectedConnectedIndex = connectedIndexOf(track, index)
        }
        win.selectedEdge = -1
    }
    function clearSelection() {
        win.selectedGap = null
        win.holdExtras = true
        win.selectedTimelineIndex = -1
        win.selectedConnectedIndex = -1
        win.selectedEdge = -1
        win.holdExtras = false
        win.extraSelection = []
    }
    // A plain click: this clip alone (its linked mate rides unless `alt`).
    function selectOnly(track, index, alt) {
        win.holdExtras = true
        setPrimary(track, index)
        win.holdExtras = false
        // Explicit: an unchanged primary would leave stale extras behind.
        win.extraSelection = alt === true || !win.linkedSelection
                         ? [] : linkMates(primaryAddress())
    }
    // Whole set at once: list[0] is the primary, the rest extras.
    function selectAddresses(list) { chooseAddresses(list, undefined) }
    function chooseAddresses(list, lookup) {
        var unique = []
        var listed = {}
        for (var u = 0; u < list.length; u++) {
            var key = addressKey(list[u])
            if (listed[key] === true)
                continue
            listed[key] = true
            unique.push(list[u])
        }
        var full = withMates(unique, lookup)
        if (full.length === 0) {
            clearSelection()
            return
        }
        win.holdExtras = true
        setPrimary(full[0].track, full[0].index)
        win.extraSelection = full.slice(1)
        win.holdExtras = false
    }
    // Ctrl-click: toggle one clip in or out of the set.
    function toggleSelect(track, index) {
        var a = { track: track, index: index }
        var p = primaryAddress()
        if (p === null) {
            setPrimary(track, index)
            return
        }
        if (sameAddress(p, a)) { // demote: the first extra takes over
            var rest = withoutMates(win.extraSelection.slice(), a)
            win.holdExtras = true
            if (rest.length > 0) {
                var next = rest.shift()
                setPrimary(next.track, next.index)
                win.extraSelection = rest
            } else {
                win.selectedTimelineIndex = -1
                win.selectedConnectedIndex = -1
                win.selectedEdge = -1
                win.extraSelection = []
            }
            win.holdExtras = false
            return
        }
        var list = []
        var had = false
        for (var i = 0; i < win.extraSelection.length; i++) {
            if (sameAddress(win.extraSelection[i], a))
                had = true
            else
                list.push(win.extraSelection[i])
        }
        if (!had)
            list = withMates(list.concat([a]))
        else
            list = withoutMates(list, a) // its linked half leaves with it
        win.extraSelection = withoutPrimary(list)
    }
    // A mate of an extra can be the primary itself: never list it twice.
    function withoutPrimary(list) {
        var p = primaryAddress()
        if (p === null)
            return list
        var out = []
        for (var i = 0; i < list.length; i++)
            if (!sameAddress(list[i], p))
                out.push(list[i])
        return out
    }
    // `list` less the linked mates of `a` (the override keeps them: Alt).
    function withoutMates(list, a) {
        if (!win.linkedSelection)
            return list
        var mates = linkMates(a)
        var out = []
        for (var i = 0; i < list.length; i++) {
            var mate = false
            for (var j = 0; j < mates.length && !mate; j++)
                mate = sameAddress(list[i], mates[j])
            if (!mate)
                out.push(list[i])
        }
        return out
    }
    // True when the extras are exactly the primary's linked mates — a
    // plain click on the primary then deselects rather than collapses.
    function extrasAreMates() {
        var mates = linkMates(primaryAddress())
        if (mates.length !== win.extraSelection.length)
            return false
        for (var i = 0; i < win.extraSelection.length; i++) {
            var found = false
            for (var j = 0; j < mates.length && !found; j++)
                found = sameAddress(win.extraSelection[i], mates[j])
            if (!found)
                return false
        }
        return true
    }
    // Shift-click: every clip on the track between the primary and this one.
    function selectRangeTo(track, index) {
        var p = primaryAddress()
        if (p === null || p.track !== track) {
            setPrimary(track, index)
            return
        }
        var lo = Math.min(p.index, index), hi = Math.max(p.index, index)
        var list = win.extraSelection.slice()
        var lookup = entryLookup()
        for (var i = lo; i <= hi; i++) {
            if (i === p.index)
                continue
            var e = lookup.entry(track, i)
            if (e === null || e.gap === true || isSelectedAt(track, i))
                continue
            list.push({ track: track, index: i })
        }
        win.extraSelection = withoutPrimary(withMates(list, lookup))
    }
    function selectAll() {
        var lookup = entryLookup()
        chooseAddresses(lookup.all, lookup)
    }
    // A block click with its modifiers: the Track Select tool sweeps,
    // Ctrl toggles, Shift ranges, Alt keeps the linked mate out, and a
    // plain click on the primary deselects (the long-standing toggle).
    function clickSelect(track, index, modifiers, isPrimary) {
        var ctrl = (modifiers & Qt.ControlModifier) !== 0
        var shift = (modifiers & Qt.ShiftModifier) !== 0
        var alt = (modifiers & Qt.AltModifier) !== 0
        if (win.tool === "trackselect") {
            selectTrackForward(track, index, ctrl, alt)
            return
        }
        if (ctrl) {
            toggleSelect(track, index)
            return
        }
        if (shift) {
            selectRangeTo(track, index)
            return
        }
        if (alt) { // the override: this clip alone, whatever it is linked to
            selectOnly(track, index, true)
            return
        }
        if (isPrimary && extrasAreMates()) {
            clearSelection()
            return
        }
        selectOnly(track, index, alt)
    }
    // Right-click: this clip leads (the menus read the primary) and the
    // rest of the set stays, so Delete/Label/Nudge there hit them all.
    function promote(track, index) {
        var a = { track: track, index: index }
        if (!isSelectedAt(track, index)) {
            selectOnly(track, index, false)
            return
        }
        var p = primaryAddress()
        if (sameAddress(p, a))
            return
        var rest = [p]
        for (var i = 0; i < win.extraSelection.length; i++)
            if (!sameAddress(win.extraSelection[i], a))
                rest.push(win.extraSelection[i])
        win.holdExtras = true
        setPrimary(track, index)
        win.extraSelection = rest
        win.holdExtras = false
    }
    // The moves a group drag led by clip `lead` asks for: every selected
    // clip shifts `deltaFrames`; those of the lead's kind also change rows
    // with it (V1 is video row 0; a row past the last = one new track).
    // The row shift a group can take as ONE block: clamped so its top
    // member stays on row 1 and its bottom member goes at most one row
    // past the last (a new track) — never per member, which would fold
    // two clips onto one row and overwrite one with the other.
    function clampGroupRows(lead, rows) {
        if (rows === 0)
            return 0
        var audio = lead.audioOnly === true
        var list = audio ? win.laneKinds.audio : win.laneKinds.video
        var lo = 1e9, hi = -1e9
        var entries = selectedEntries()
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i]
            if ((e.audioOnly === true) !== audio || (audio && e.track < 1))
                continue
            var row = audio ? list.indexOf(e.track) + 1 : win.lanes.videoRowOf(e.track)
            lo = Math.min(lo, row)
            hi = Math.max(hi, row)
        }
        if (hi < lo)
            return rows
        // Audio rows count downward from V1, video rows upward from it.
        var shift = audio ? rows : -rows
        shift = Math.max((audio ? 1 : 0) - lo, Math.min(list.length + 1 - hi, shift))
        return audio ? shift : -shift
    }
    function groupMoves(lead, deltaFrames, rows) {
        var moves = []
        var lookup = entryLookup()
        var entries = entriesOfSelection(lookup)
        rows = clampGroupRows(lead, rows)
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i]
            var audio = e.audioOnly === true
            var newTrack = e.track
            if (audio === (lead.audioOnly === true) && rows !== 0) {
                if (audio) {
                    var list = win.laneKinds.audio
                    var target = list.indexOf(e.track) + 1 + rows
                    newTrack = target <= list.length ? list[target - 1]
                                                     : backend.tracks.length
                } else {
                    newTrack = win.lanes.videoTrackAtRow(win.lanes.videoRowOf(e.track) - rows)
                }
            }
            moves.push({ track: e.track, index: e.index, newTrack: newTrack,
                         newStart: Math.max(0, e.start + deltaFrames),
                         alone: hasUnselectedMate(e.track, e.index, lookup) })
        }
        return moves
    }
    // Swap the selected clip with its neighbour on the same track (left:
    // the clip before it): each takes the other's place, the space between
    // them stays, and linked clips travel with their own.
    function swapSelected(left) {
        var e = selectedEntry()
        if (e === null || e.gap === true)
            return
        var other = null
        var lookup = entryLookup()
        var all = lookup.all
        for (var i = 0; i < all.length; i++) {
            if (all[i].track !== e.track)
                continue
            var m = lookup.entry(all[i].track, all[i].index)
            if (m === null || m.gap === true || (m.track === e.track && m.index === e.index))
                continue
            if (left ? m.start + m.frames <= e.start && (other === null || m.start > other.start)
                     : m.start >= e.start + e.frames && (other === null || m.start < other.start))
                other = m
        }
        if (other === null)
            return
        var a = left ? other : e, b = left ? e : other // a before b
        var moves = []
        function shift(entry, delta) {
            var addrs = [{ track: entry.track, index: entry.index }].concat(
                win.linkedSelection ? linkMates({ track: entry.track, index: entry.index }, lookup) : [])
            for (var j = 0; j < addrs.length; j++) {
                var c = lookup.entry(addrs[j].track, addrs[j].index)
                if (c !== null)
                    moves.push({ track: c.track, index: c.index, newTrack: c.track,
                                 newStart: Math.max(0, c.start + delta) })
            }
        }
        shift(b, a.start - b.start)                         // b to a's start
        shift(a, b.start + b.frames - a.frames - a.start)   // a to end where b ended
        clearSelection()
        backend.moveClips(moves)
    }
    // Alt-drag: copies land where the moves would have taken the clips,
    // which stay put. All shift by one delta, so the set keeps its shape:
    // only the earliest copy's position and each copy's track are needed.
    function copyMoves(moves) {
        var sources = [], tracks = [], first = -1, delta = 0
        for (var i = 0; i < moves.length; i++) {
            var e = trackEntry(moves[i].track, moves[i].index)
            if (e === null || e.gap === true)
                continue
            sources.push(win.copying.snapshot(e))
            tracks.push(moves[i].newTrack)
            if (first < 0 || e.start < first) {
                first = e.start
                delta = moves[i].newStart - e.start
            }
        }
        if (sources.length === 0)
            return
        // The clips as they are drawn, read where they stand (a clip in a
        // transition is found by its own track), then sent to their tracks.
        // A pair copied together keeps its transition.
        var entries = win.copying.apart(sources)
        first = -1
        for (var j = 0; j < entries.length; j++) {
            entries[j].track = tracks[j]
            entries[j].lane = tracks[j]
            if (first < 0 || entries[j].start < first)
                first = entries[j].start // (where the earliest is drawn from)
        }
        var clock = backend.clipboardClock()
        win.clipboardPasteRequest = backend.pasteClipboardSnapshot(
            { entries: entries, fpsNum: clock.fpsNum, fpsDen: clock.fpsDen },
            backend.frameTime(Math.max(0, first + delta)), -1, false)
    }
    // Track Select tool: this clip and everything after it on its track
    // (`before` = everything up to it; `allTracks` = the same on every track).
    function selectTrackForward(track, index, allTracks, before) {
        var e = trackEntry(track, index)
        if (e === null)
            return
        var list = [{ track: track, index: index }]
        var lookup = entryLookup()
        var all = lookup.all
        for (var i = 0; i < all.length; i++) {
            var m = lookup.entry(all[i].track, all[i].index)
            if (m === null || sameAddress(all[i], list[0]))
                continue
            if (!allTracks && all[i].track !== track)
                continue
            var after = m.start >= e.start
            if (all[i].track === track)
                after = all[i].index > index
            if (before ? !after : after)
                list.push(all[i])
        }
        chooseAddresses(list, lookup)
    }
    function identityOf(a, lookup) {
        var e = lookup === undefined ? trackEntry(a.track, a.index) : lookup.entry(a.track, a.index)
        return e === null ? null
             : e.clipId ? { clipId: String(e.clipId) }
             : { track: a.track, start: e.start, path: e.path, "in": e.in, title: e.title }
    }
    // What each clip of `list` is, for finding it again after an edit.
    function identitiesOf(list) {
        var ids = []
        var lookup = list.length > 1 ? entryLookup() : undefined
        for (var i = 0; i < list.length; i++)
            ids.push(identityOf(list[i], lookup))
        return ids
    }
    function clipCountsByTrack() {
        var counts = {}
        var all = allAddresses()
        for (var i = 0; i < all.length; i++)
            counts[all[i].track] = (counts[all[i].track] || 0) + 1
        return counts
    }

    function trackEntry(track, idx) {
        if (idx < 0)
            return null
        if (track === 0)
            return idx < backend.timeline.length ? backend.timeline[idx] : null
        for (var i = 0; i < backend.connected.length; i++) {
            var e = backend.connected[i]
            if (e.track === track && e.index === idx)
                return e
        }
        return null
    }
    function trackClipCount(track) {
        if (track === 0)
            return backend.timeline.length
        var n = 0
        for (var i = 0; i < backend.connected.length; i++)
            if (backend.connected[i].track === track)
                n++
        return n
    }
    // What sits under (track, frame): {timelineIndex, connectedIndex,
    // entry} or null. Gaps on V1 count as nothing.
    // (By the block as drawn: across a transition's cut that is the clip
    // the editor sees on the frame, see win.snapping.drawn.)
    function clipAt(track, frame) {
        if (track === 0) {
            for (var i = 0; i < backend.timeline.length; i++) {
                var c = backend.timeline[i]
                var cb = win.snapping.drawn(c)
                if (frame >= cb.start && frame < cb.start + cb.frames && c.gap !== true)
                    return { timelineIndex: i, connectedIndex: -1, entry: c }
            }
            return null
        }
        const lanes = backend.connected, count = lanes.length, places = win.snapping.lanesByPlace(lanes)
        for (var j = 0; j < count; j++) {
            var e = lanes[j]
            if (e.track !== track)
                continue
            var eb = win.snapping.drawn(e, places)
            if (frame >= eb.start && frame < eb.start + eb.frames)
                return { timelineIndex: -1, connectedIndex: j, entry: e }
        }
        return null
    }
    // The selected entry map (track clip first, then storyline), or null.
    function selectedEntry() {
        if (win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length)
            return backend.connected[win.selectedConnectedIndex]
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length)
            return backend.timeline[win.selectedTimelineIndex]
        return null
    }
    // The selected clip proper: a gap has nothing of its own to act on.
    function selectedClip() {
        var e = selectedEntry()
        return e === null || e.gap === true ? null : e
    }
    // The selected clip when it has a picture to change.
    function selectedPicture() {
        var e = selectedClip()
        return e === null || e.audioOnly === true ? null : e
    }
    function selectClipAtPlayhead() {
        var e = selectedEntry()
        var first = e === null ? 0 : e.track
        var count = Math.max(1, backend.tracks.length)
        for (var i = 0; i < count; i++) {
            var hit = clipAt((first + i) % count, backend.position)
            if (hit !== null) {
                selectOnly(hit.entry.track, hit.entry.index)
                return
            }
        }
    }
    function selectNeighbourClip(step) {
        var e = selectedEntry()
        if (e === null) {
            selectClipAtPlayhead()
            return
        }
        var count = trackClipCount(e.track)
        for (var i = e.index + step; i >= 0 && i < count; i += step) {
            var next = trackEntry(e.track, i)
            if (next !== null && next.gap !== true) {
                selectOnly(e.track, i)
                return
            }
        }
    }
    // Up the stack of tracks: video rows count upwards from V1, audio rows
    // downwards from it, so "above" is the same way on the screen for both.
    function selectClipOnNextTrack(step) {
        var e = selectedClip()
        if (e === null)
            return
        if (!backend.tracks[e.track])
            return // the track list has not caught up with the entries yet
        var kind = backend.tracks[e.track].kind
        var frame = backend.position >= e.start && backend.position < e.start + e.frames
                ? backend.position : e.start
        var dir = kind === "audio" ? -step : step
        for (var t = e.track + dir; t >= 0 && t < backend.tracks.length; t += dir) {
            if (backend.tracks[t].kind !== kind)
                continue
            var hit = clipAt(t, frame)
            if (hit !== null) {
                selectOnly(hit.entry.track, hit.entry.index)
                return
            }
        }
    }
}
