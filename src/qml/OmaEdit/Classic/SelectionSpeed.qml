import QtQml
import OmaEdit.Ui as Ui

// The speed of the selected clips, as the Retime menu and the keys set and
// read it. The window holds one as `win.retiming`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    function setSelectedFrameBlend(mode) {
        var e = win.selection.selectedPicture()
        if (e !== null)
            backend.setFrameBlend(e.track, e.index, mode)
    }
    // A one-click retime on the selected clip, at the playhead.
    function quickRetime(kind, rate) {
        const e = win.selection.selectedEntry()
        if (e && canRetimeSelection()) backend.quickRetime(e.track, e.index, kind, backend.position - e.start, rate)
    }
    function canRetimeSelection() {
        var entry=win.selection.selectedEntry()
        if (win.workspace!=="Edit" || !entry || entry.gap || entry.title || entry.compoundJson
            || backend.projectBusy || backend.exporting || entry.track<0 || entry.track>=backend.tracks.length
            || backend.tracks[entry.track].locked) return false
        // The default linked selection includes this clip's audio. Treat that
        // as one material target; unrelated multi-selections need separate edits.
        for (var i=0;i<win.extraSelection.length;i++) {
            var other=win.selection.trackEntry(win.extraSelection[i].track,win.extraSelection[i].index)
            if (!entry.link || !other || other.link!==entry.link) return false
        }
        return true
    }
    function setSelectedSpeed(speed) {
        if (win.selectedConnectedIndex >= 0)
            backend.setConnectedSpeed(win.selectedConnectedIndex, speed)
        else if (win.selectedTimelineIndex >= 0)
            backend.setClipSpeed(win.selectedTimelineIndex, speed)
    }
    function selectedSpeed() {
        var e = win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length
                ? backend.connected[win.selectedConnectedIndex]
                : (win.selectedTimelineIndex >= 0
                   && win.selectedTimelineIndex < backend.timeline.length
                   ? backend.timeline[win.selectedTimelineIndex] : null)
        return e === null || e.speed === undefined ? 1 : e.speed
    }
}
