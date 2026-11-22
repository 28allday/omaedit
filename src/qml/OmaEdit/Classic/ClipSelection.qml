import QtQuick

// Shared selection rules for timeline buttons and clip context menus.
QtObject {
    id: root
    readonly property var entry: win.selection.selectedEntry()
    readonly property bool onTrack: win.selectedConnectedIndex >= 0
    readonly property bool isGap: entry === null || entry.gap === true
    readonly property bool isTitle: !isGap && !!entry.title
    readonly property bool isAudio: !isGap && entry.audioOnly === true
    readonly property bool hasAudio: !isGap && !isTitle && entry.adjust !== true
        && ((!entry.videoOnly && entry.silent !== true) || win.clipCommands.audioMateIndex(entry) >= 0)
    // A picture and its linked sound are one editing target. An unrelated
    // multi-selection only offers operations that really act on the whole set.
    readonly property bool singleClip: entry !== null && win.selection.selectedEntries().every(function(e) {
        return (e.track === root.entry.track && e.index === root.entry.index)
            || (!!root.entry.link && e.link === root.entry.link)
    })
    readonly property bool editable: entry !== null && win.selection.selectedEntries().every(function(e) {
        return !!backend.tracks[e.track] && !backend.tracks[e.track].locked
    })
}
