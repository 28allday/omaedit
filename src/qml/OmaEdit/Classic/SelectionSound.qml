import QtQml
import OmaEdit.Ui as Ui

// The sound of the selected clips: level, pan and EQ as the Clip menu, the
// keys and the Inspector set and read them, and the badge a clip shows for
// them. The window holds one as `win.sound`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // Volume routes to whichever clip is selected — lane or storyline.
    readonly property bool targetExists:
        win.selectedConnectedIndex >= 0 || win.selectedTimelineIndex >= 0
    function adjustVolume(deltaDb) {
        // A keyframed clip shifts its whole curve — a static set would
        // silently wipe the keyframes.
        function shiftKf(entry) {
            var out = []
            for (var i = 0; i < entry.volumeKf.length; i++)
                out.push({ frame: entry.volumeKf[i].frame,
                           db: Math.max(-60, Math.min(12,
                               entry.volumeKf[i].db + deltaDb)) })
            return out
        }
        if (win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length) {
            var lane = backend.connected[win.selectedConnectedIndex]
            if (lane.volumeKf !== undefined && lane.volumeKf.length > 0) {
                backend.setConnectedVolumeKf(win.selectedConnectedIndex,
                                             shiftKf(lane))
                return
            }
        }
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length
                && win.selectedConnectedIndex < 0) {
            var clip = backend.timeline[win.selectedTimelineIndex]
            if (clip.volumeKf !== undefined && clip.volumeKf.length > 0) {
                backend.setClipVolumeKf(win.selectedTimelineIndex, shiftKf(clip))
                return
            }
        }
        if (win.selectedConnectedIndex >= 0)
            backend.adjustConnectedVolume(win.selectedConnectedIndex, deltaDb)
        else if (win.selectedTimelineIndex >= 0)
            backend.adjustVolume(win.selectedTimelineIndex, deltaDb)
    }
    function setVolume(db) {
        if (win.selectedConnectedIndex >= 0)
            backend.setConnectedVolume(win.selectedConnectedIndex, db)
        else if (win.selectedTimelineIndex >= 0)
            backend.setClipVolume(win.selectedTimelineIndex, db)
    }
    function volume() {
        if (win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length)
            return backend.connected[win.selectedConnectedIndex].volume
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length)
            return backend.timeline[win.selectedTimelineIndex].volume
        return 0
    }
    // Pan routes the same way as volume.
    function setPan(pan) {
        if (win.selectedConnectedIndex >= 0)
            backend.setConnectedPan(win.selectedConnectedIndex, pan)
        else if (win.selectedTimelineIndex >= 0)
            backend.setClipPan(win.selectedTimelineIndex, pan)
    }
    function pan() {
        if (win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length)
            return backend.connected[win.selectedConnectedIndex].pan
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length)
            return backend.timeline[win.selectedTimelineIndex].pan
        return 0
    }
    // EQ routes the same way as volume and pan.
    function setEq(preset) {
        if (win.selectedConnectedIndex >= 0)
            backend.setConnectedEq(win.selectedConnectedIndex, preset)
        else if (win.selectedTimelineIndex >= 0)
            backend.setClipEq(win.selectedTimelineIndex, preset)
    }
    function eq() {
        if (win.selectedConnectedIndex >= 0
                && win.selectedConnectedIndex < backend.connected.length)
            return backend.connected[win.selectedConnectedIndex].eq || ""
        if (win.selectedTimelineIndex >= 0
                && win.selectedTimelineIndex < backend.timeline.length)
            return backend.timeline[win.selectedTimelineIndex].eq || ""
        return ""
    }
    // "L60" / "R30" for a badge; "" when centred.
    function panLabel(pan) {
        if (pan === undefined || pan === 0)
            return ""
        return (pan < 0 ? "L" : "R") + Math.round(Math.abs(pan) * 100)
    }
    // The audio badge on a clip block: "-6 dB · L40 · EQ", or "" when the
    // clip is at unity, centred and flat.
    function badge(entry) {
        var parts = []
        var volume = entry.volume !== undefined ? entry.volume : 0
        if (volume !== 0)
            parts.push(volume <= -60 ? "silent"
                       : (volume > 0 ? "+" : "") + Math.round(volume) + " dB")
        var pan = panLabel(entry.pan)
        if (pan !== "")
            parts.push(pan)
        if (entry.eq !== undefined && entry.eq !== "")
            parts.push("EQ")
        return parts.join(" · ")
    }
}
