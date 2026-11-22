import QtQml
import OmaEdit.Ui as Ui

// Clip categories (dialogue, music, effects, video, titles, and the
// project's own): which one a clip belongs to, its colour, and switching a
// whole category on or off. The window holds one as `win.categories`.
QtObject {
    id: root
    // The window: the selection and the entries on the tracks.
    required property var win

    readonly property var list: backend.mediaCatalog.categories
    function idOf(e) {
        var id = e.category === undefined ? "" : e.category
        var kind = e.audioOnly === true ? "audio" : "video"
        for (var i = 0; i < list.length; i++)
            if (list[i].id === id && list[i].kind === kind)
                return id
        if (e.audioOnly === true)
            return "dialogue"
        return e.title !== undefined && e.title !== "" ? "titles" : "video"
    }
    // Label colours are made to sit under a clip's name; lifted, they read as
    // a thin strip and a small dot.
    function tint(name) {
        return Qt.lighter(Ui.Theme.labelColor(name), 1.7)
    }
    function colorOf(e) {
        var id = idOf(e)
        for (var i = 0; i < list.length; i++)
            if (list[i].id === id)
                return tint(list[i].color)
        return "transparent"
    }
    // Every clip in the timeline; a picture and its sound count apart.
    function clips(id) {
        var out = []
        var lists = [backend.timeline, backend.connected]
        for (var l = 0; l < lists.length; l++)
            for (var i = 0; i < lists[l].length; i++) {
                var e = lists[l][i]
                if (e.gap !== true && idOf(e) === id)
                    out.push(e)
            }
        return out
    }
    // [{id, name, kind, color, clips, on}]: on while any of its clips is on.
    function rows() {
        return list.map(function (c) {
            var found = clips(c.id)
            return { id: c.id, name: c.name, kind: c.kind, color: c.color, clips: found.length,
                     on: found.length === 0 || found.some(function (e) { return e.disabled !== true }) }
        })
    }
    function setOn(id, on) {
        var addrs = clips(id).map(function (e) { return { track: e.track, index: e.index } })
        if (addrs.length > 0)
            backend.setClipsDisabled(addrs, !on)
    }
    // A category goes on the selected clips it can describe: a sound category
    // on sound, a picture category on pictures, linked mates included.
    function selectedAddresses(kind) {
        var chosen = win.selection.selectedAddresses()
        if (chosen.length === 0)
            return chosen
        var lookup = win.selection.entryLookup() // one for the whole set
        return win.selection.withMates(chosen, lookup).filter(function (a) {
            var e = lookup.entry(a.track, a.index)
            return e !== null && e.gap !== true && (e.audioOnly === true) === (kind === "audio")
        })
    }
    function selected(kind) {
        var addrs = selectedAddresses(kind)
        return addrs.length === 0 ? "" : idOf(win.selection.trackEntry(addrs[0].track, addrs[0].index))
    }
    function assign(id) {
        var row = list.filter(function (c) { return c.id === id })[0]
        if (row === undefined)
            return
        var addrs = selectedAddresses(row.kind)
        if (addrs.length === 0) {
            backend.announce(row.kind === "audio" ? qsTr("Select a clip with sound for this category")
                                                  : qsTr("Select a clip with a picture for this category"))
            return
        }
        backend.setClipCategory(addrs, id)
    }
}
