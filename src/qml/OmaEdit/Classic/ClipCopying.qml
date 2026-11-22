import QtQml
import OmaEdit.Ui as Ui

// Cut, copy, paste and duplicate of clips. What was copied (the clipboard)
// stays on the window; this fills it and places from it. The window holds
// one as `win.copying`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // A detached copy of entry data. Lists read from a backend property stay
    // live views of it (Qt 6.12), so a key-by-key copy of an entry would
    // follow later edits of the clip; the clipboard keeps what was copied.
    function snapshot(v) {
        if (v === null || typeof v !== "object" || v instanceof Date)
            return v
        var i, out
        if (v instanceof Array) {
            out = []
            for (i = 0; i < v.length; i++)
                out.push(snapshot(v[i]))
            return out
        }
        out = {}
        for (i in v)
            out[i] = snapshot(v[i])
        return out
    }
    // Copies as the clipboard holds them: each clip as it is drawn, taken
    // out of the transitions it is in (the frames one borrows past a cut are
    // not the clip's). A real array of detached entries: what a backend
    // call hands back is a list, not an array.
    function apart(list) {
        var clips = backend.clipsApart(list)
        var out = []
        for (var i = 0; i < clips.length; i++)
            out.push(snapshot(clips[i]))
        return out
    }
    // Ctrl+C: remember the selected clip. A gap has nothing to copy.
    function copySelected() {
        if (win.selectionCount > 1) { // the set, layout and all
            var list = []
            var entries = win.selection.selectedEntries()
            for (var i = 0; i < entries.length; i++) {
                if (entries[i].gap === true)
                    continue
                list.push(snapshot(entries[i]))
            }
            if (list.length === 0)
                return
            win.clipboard = apart(list)
            win.clipboardClock = backend.clipboardClock()
            backend.announce(qsTr("Copied %1 clips").arg(list.length))
            return
        }
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        win.clipboard = apart([wholeCopyOf(e)])[0]
        win.clipboardClock = backend.clipboardClock()
        backend.announce(qsTr("Copied %1").arg(e.name))
    }
    // A lone clip as the clipboard or Ctrl+D carries it. The copy pairs
    // with nothing (no link), but a V1 clip's sound lives on its linked
    // audio clip: the copy goes in whole, and the engine splits it again,
    // linked, with the same sound settings.
    function wholeCopyOf(e) {
        var copy = snapshot(e)
        delete copy.link
        if (e.track === 0 && e.videoOnly === true) {
            var mate = win.clipCommands.audioMateIndex(e)
            if (mate >= 0) {
                var a = backend.connected[mate]
                copy.videoOnly = false
                var keys = ["volume", "pan", "eq", "audioMap", "normalize", "volumeKf"]
                for (var j = 0; j < keys.length; j++)
                    if (a[keys[j]] !== undefined)
                        copy[keys[j]] = snapshot(a[keys[j]])
            }
        }
        return copy
    }
    // Ctrl+V: overwrite at the playhead on the destination track (a copied
    // track clip goes back to its own kind of track when the destination
    // can't hold it — titles and audio never land on V1).
    function pasteClipboard() {
        pasteClipboardWithMode(false)
    }
    function pasteInsertClipboard() {
        pasteClipboardWithMode(true)
    }
    function pasteClipboardWithMode(insert) {
        if (win.clipboard === null)
            return
        var track = win.clipboardIsSet ? -1 : backend.destTrack
        var needsLane = win.clipboard.audioOnly === true
                        || (win.clipboard.title !== undefined && win.clipboard.title !== "")
        if (!win.clipboardIsSet && needsLane && track === 0) {
            var sourceTrack = win.clipboard.track === undefined ? -1 : win.clipboard.track
            var kind = win.clipboard.audioOnly === true ? "audio" : "video"
            track = sourceTrack >= 1 && sourceTrack < backend.tracks.length
                    && backend.tracks[sourceTrack].kind === kind
                    ? sourceTrack : Math.max(1, backend.tracks.length)
        }
        win.clipboardPasteRequest = backend.pasteClipboardSnapshot({
            entries: win.clipboardIsSet ? win.clipboard : [win.clipboard],
            fpsNum: win.clipboardClock.fpsNum, fpsDen: win.clipboardClock.fpsDen
        }, backend.audioPosition, track, insert)
    }
    // Ctrl+D: insert the selected group at its exact end without changing
    // the clipboard. The backend keeps rational timing out of JS arithmetic.
    function duplicateSelected() {
        var entries = []
        if (win.selectionCount > 1) {
            var selected = win.selection.selectedEntries()
            for (var i = 0; i < selected.length; ++i)
                if (selected[i].gap !== true) entries.push(selected[i])
        } else {
            var e = win.selection.selectedEntry()
            if (e !== null && e.gap !== true) entries.push(wholeCopyOf(e))
        }
        if (entries.length)
            win.clipboardPasteRequest = backend.duplicateClips(entries)
    }
    function cutSelected() {
        if (win.selection.selectedClip() === null)
            return
        copySelected()
        win.clipCommands.deleteSelected()
    }
}
