import QtQml
import OmaEdit.Ui as Ui

// What the media panel's Effects, Transitions and Titles tabs offer, which of
// it applies to the current selection, and applying one. The window holds
// one as `win.catalogue`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win

    // One list feeds the media panel's Effects / Transitions / Titles tabs; `ok` is applicability against
    // the current selection (the same conditions as the menus).
    // The effect catalogue is a constant list — the tiles built from it must
    // never be re-created while one is being dragged. What CAN change (which
    // tiles apply to the current selection) lives in the applicability map
    // beside it, so a selection or library change re-dims tiles in place.
    readonly property var list: [
        { key: "speed-0.5", name: "Slow 50%", cat: "retime" },
        { key: "speed-0.25", name: "Slow 25%", cat: "retime" },
        { key: "speed-2", name: "Fast 2×", cat: "retime" },
        { key: "speed-4", name: "Fast 4×", cat: "retime" },
        { key: "speed-1", name: "Normal 100%", cat: "retime" },
        { key: "pip-full", name: "PiP Full Frame", cat: "transform" },
        { key: "pip-tl", name: "PiP Top Left", cat: "transform" },
        { key: "pip-tr", name: "PiP Top Right", cat: "transform" },
        { key: "pip-bl", name: "PiP Bottom Left", cat: "transform" },
        { key: "pip-br", name: "PiP Bottom Right", cat: "transform" },
        { key: "pip-centre", name: "PiP Centre Small", cat: "transform" },
        { key: "eq-voice", name: "EQ Voice Enhance", cat: "audio" },
        { key: "eq-bass-boost", name: "EQ Bass Boost", cat: "audio" },
        { key: "eq-bass-reduce", name: "EQ Bass Reduce", cat: "audio" },
        { key: "eq-treble-boost", name: "EQ Treble Boost", cat: "audio" },
        { key: "eq-treble-reduce", name: "EQ Treble Reduce", cat: "audio" },
        { key: "eq-hum", name: "EQ Hum Reduction", cat: "audio" },
        { key: "eq-flat", name: "EQ Flat (off)", cat: "audio" },
        { key: "pan--1", name: "Pan Full Left", cat: "audio" },
        { key: "pan--0.5", name: "Pan Half Left", cat: "audio" },
        { key: "pan-0", name: "Pan Centre", cat: "audio" },
        { key: "pan-0.5", name: "Pan Half Right", cat: "audio" },
        { key: "pan-1", name: "Pan Full Right", cat: "audio" },
        { key: "vol-silence", name: "Silence Clip", cat: "audio" },
        { key: "vol-reset", name: "Reset Volume", cat: "audio" },
        { key: "dissolve", name: qsTr("Cross Dissolve"), cat: "transitions" }]
        .concat(backend.transitionStyles.filter(function(t) { return t.key !== "" }).map(function(t) {
            return { key: "style-" + t.key, name: t.name, cat: "transitions" } }))
        .concat([{ key: "fade-in", name: qsTr("Fade In"), cat: "transitions" },
        { key: "fade-out", name: qsTr("Fade Out"), cat: "transitions" }]).concat(backend.videoEffectCatalogue)
    readonly property var applicability: {
        var stSel = win.selectedTimelineIndex >= 0
        var laneSel = win.selectedConnectedIndex >= 0
        var anySel = stSel || laneSel
        var audioOk = win.sound.targetExists
        var target = win.trimming.dissolveTarget()
        var hasDissolve = target >= 0 && target < backend.timeline.length
                          && backend.timeline[target].mixOut > 0
        var dissolveOk = target >= 0
                         && (hasDissolve || target < backend.timeline.length - 1)
        var applicable = {
            "speed-0.5": anySel,
            "speed-0.25": anySel,
            "speed-2": anySel,
            "speed-4": anySel,
            "speed-1": anySel,
            "pip-full": laneSel,
            "pip-tl": laneSel,
            "pip-tr": laneSel,
            "pip-bl": laneSel,
            "pip-br": laneSel,
            "pip-centre": laneSel,
            "eq-voice": audioOk,
            "eq-bass-boost": audioOk,
            "eq-bass-reduce": audioOk,
            "eq-treble-boost": audioOk,
            "eq-treble-reduce": audioOk,
            "eq-hum": audioOk,
            "eq-flat": audioOk,
            "pan--1": audioOk,
            "pan--0.5": audioOk,
            "pan-0": audioOk,
            "pan-0.5": audioOk,
            "pan-1": audioOk,
            "vol-silence": audioOk,
            "vol-reset": audioOk,
            "dissolve": dissolveOk || laneSel,
            "fade-in": anySel,
            "fade-out": anySel
        }
        backend.transitionStyles.forEach(function(t) { if (t.key !== "") applicable["style-" + t.key] = hasDissolve || laneSel })
        var entry = win.selection.selectedEntry()
        var videoOk = entry !== null && entry.audioOnly !== true && entry.gap !== true
                      && !backend.exporting && !!backend.tracks[entry.track] && !backend.tracks[entry.track].locked
        backend.videoEffectCatalogue.forEach(function(d) { applicable[d.key] = videoOk && d.available === true })
        return applicable
    }
    function defs() {
        var ok = applicability
        return list.map(function (d) {
            return { key: d.key, name: d.name, cat: d.cat, ok: ok[d.key] === true }
        })
    }
    function apply(key) {
        var st = win.selectedTimelineIndex
        var lane = win.selectedConnectedIndex
        // Recents: what was actually applicable when it was used.
        if (applicability[key] === true || key.indexOf("preset:") === 0)
            backend.effectShelf.noteUsed(key)
        if (key.indexOf("video:") === 0) {
            const entry = win.selection.selectedEntry()
            if (entry && applicability[key] === true) {
                backend.editVideoEffect(entry.track, entry.index, "add", "", {id: key.substring(6)})
                win.showClipInspector(2)
            }
        } else if (key.indexOf("preset:") === 0)
            win.attributes.applyPreset(key.substring(7))
        else if (key.indexOf("speed-") === 0)
            win.retiming.setSelectedSpeed(Number(key.substring(6)))
        else if (key === "pip-full")
            backend.setConnectedRect(lane, 0, 0, 1, 1)
        else if (key === "pip-tl")
            backend.setConnectedRect(lane, 0.03, 0.03, 0.42, 0.42)
        else if (key === "pip-tr")
            backend.setConnectedRect(lane, 0.55, 0.03, 0.42, 0.42)
        else if (key === "pip-bl")
            backend.setConnectedRect(lane, 0.03, 0.55, 0.42, 0.42)
        else if (key === "pip-br")
            backend.setConnectedRect(lane, 0.55, 0.55, 0.42, 0.42)
        else if (key === "pip-centre")
            backend.setConnectedRect(lane, 0.29, 0.29, 0.42, 0.42)
        else if (key === "eq-flat")
            win.sound.setEq("")
        else if (key.indexOf("eq-") === 0)
            win.sound.setEq(key.substring(3))
        else if (key.indexOf("pan-") === 0)
            win.sound.setPan(Number(key.substring(4)))
        else if (key === "vol-silence")
            win.sound.setVolume(-60)
        else if (key === "vol-reset")
            win.sound.setVolume(0)
        else if (key === "dissolve" || key.indexOf("style-") === 0) {
            // On a track clip: into the next clip on its track. On V1: the
            // selected edit point (an IN edge = the boundary before).
            var style = key === "dissolve" ? "" : key.substring(6)
            var e = win.selection.selectedEntry()
            if (e === null)
                return
            if (lane >= 0)
                backend.applyTransitionAt(e.track, e.index, style)
            else
                backend.applyTransitionAt(0, win.trimming.dissolveTarget(), style)
        } else if (key === "fade-in" || key === "fade-out") {
            if (lane >= 0)
                backend.toggleConnectedFade(lane, key === "fade-out")
            else
                backend.toggleFade(st, key === "fade-out")
        }
    }
    // Title presets (the Titles tab): a place + size the dialog starts
    // from, or a title dropped straight onto the timeline.
    readonly property var titlePresets: [
        { key: "centre", name: qsTr("Centre Title"), place: "centre", size: "medium", icon: "type" },
        { key: "lower", name: qsTr("Lower Third"), place: "lower", size: "small", icon: "captions" },
        { key: "big", name: qsTr("Big Centre"), place: "centre", size: "large", icon: "maximize" },
        { key: "small", name: qsTr("Small Centre"), place: "centre", size: "small", icon: "minimize" }]
    // Generators (the Titles tab's second half): placed straight on a lane,
    // 4 s at the playhead or where they are dropped.
    readonly property var generatorTiles: [
        { key: "gen:solid", name: qsTr("Solid Colour"), icon: "square" },
        { key: "gen:bars", name: qsTr("Colour Bars"), icon: "layout-grid" },
        { key: "gen:noise", name: qsTr("Noise"), icon: "activity" },
        { key: "gen:count", name: qsTr("Counting Leader"), icon: "clock" },
        { key: "gen:adjust", name: qsTr("Effect Layer"), icon: "sliders-horizontal" }]
    // A placed title's styling beyond the basics, for the dialog.
    function titleExtras(e) {
        if (e === null || e === undefined)
            return null
        const extras = { align: e.titleAlign, weight: e.titleWeight, italic: e.titleItalic,
                 bg: e.titleBg, outline: e.titleOutline, tracking:e.titleTracking, lineSpacing:e.titleLineSpacing, pixelSize:e.titlePixelSize, margin:e.titleMargin, width:e.titleWidth }
        // The fuller look (shadow, outline colour and width, box padding and
        // corners) rides along, so new words keep it.
        const look = e.titleLook || {}
        Object.keys(look).forEach(function(k) { extras[k] = look[k] })
        return extras
    }
    // A title's whole look as updateTitle takes it, for new words in the
    // same title. What the title never set is left out: sent as nothing, a
    // setting such as the line spacing would be read as its lowest value.
    function titleStyle(e) {
        const style = titleExtras(e) || {}
        style.place = e.titlePlace; style.size = e.titleSize; style.font = e.titleFont; style.color = e.titleColor
        Object.keys(style).forEach(function(k) { if (style[k] === undefined || style[k] === null) delete style[k] })
        return style
    }
    function titlePreset(key) {
        for (var i = 0; i < titlePresets.length; i++)
            if (titlePresets[i].key === key)
                return titlePresets[i]
        return titlePresets[0]
    }
}
