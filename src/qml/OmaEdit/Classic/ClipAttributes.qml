import QtQml
import OmaEdit.Ui as Ui

// A clip's attributes as groups that can be copied to other clips or kept
// as a preset: which groups a clip carries, Paste Attributes, and applying
// or saving a preset. The window holds one as `win.attributes`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win
    required property var pasteDialog
    required property var presetDialog

    // The attribute groups an entry can carry over (engine order).
    readonly property var groupDefs: [
        { key: "color", name: "Colour" }, { key: "crop", name: "Crop" },
        { key: "transform", name: "Transform" }, { key: "flip", name: "Flip" },
        { key: "opacity", name: "Opacity" }, { key: "level", name: "Level & Pan" },
        { key: "eq", name: "Equaliser" }, { key: "channels", name: "Channels" },
        { key: "blend", name: "Blend Mode" }, { key: "lens", name: "Lens Correction" },
        { key: "fades", name: "Fades" }, { key: "speed", name: "Speed" },
        { key: "effects", name: qsTr("Video Effects") }
    ]
    // Which groups `e` actually sets (non-default).
    function groupsOf(e) {
        if (e === null || e === undefined)
            return []
        var n = function (k, d) { return e[k] === undefined || e[k] === null ? d : Number(e[k]) }
        var list = function (k) { return e[k] !== undefined && e[k] !== null && e[k].length > 0 }
        var out = []
        if (n("colorB", 0) !== 0 || n("colorC", 1) !== 1 || n("colorS", 1) !== 1) out.push("color")
        if (n("cropL", 0) !== 0 || n("cropR", 0) !== 0 || n("cropT", 0) !== 0 || n("cropB", 0) !== 0
                || n("cropFeather", 0) !== 0 || e.cropInvert === true) out.push("crop")
        if (n("rectX", 0) !== 0 || n("rectY", 0) !== 0 || n("rectW", 1) !== 1 || n("rectH", 1) !== 1
                || n("rotation", 0) !== 0 || n("anchorX", .5) !== .5 || n("anchorY", .5) !== .5) out.push("transform")
        if (e.flipH === true || e.flipV === true) out.push("flip")
        if (n("opacity", 1) !== 1 || list("opacityKf")) out.push("opacity")
        if (n("volume", 0) !== 0 || n("pan", 0) !== 0 || list("volumeKf")) out.push("level")
        if (e.eq !== undefined && e.eq !== null && e.eq !== "") out.push("eq")
        if ((e.audioMap !== undefined && e.audioMap !== null && e.audioMap !== "") || e.normalize === true) out.push("channels")
        if (n("blend", 0) > 0) out.push("blend")
        if (n("lensK1", 0) !== 0 || n("lensK2", 0) !== 0) out.push("lens")
        if (n("fadeIn", 0) > 0 || n("fadeOut", 0) > 0) out.push("fades")
        const sampling = n("frameBlend", 0)
        if (n("speed", 1) !== 1 || e.retime !== undefined || e.speedPattern !== undefined
                || (sampling !== 0 && sampling !== 3)) out.push("speed")
        if (list("videoEffects")) out.push("effects")
        const keys = e.editKeys || {}
        const animatedGroups = {
            color: ["colorB", "colorC", "colorS"],
            crop: ["cropL", "cropR", "cropT", "cropB", "cropFeather"],
            transform: ["rectX", "rectY", "rectW", "rectH", "rotation", "anchorX", "anchorY"]
        }
        Object.keys(animatedGroups).forEach(function(group) {
            if (out.indexOf(group) < 0 && animatedGroups[group].some(function(key) { return keys[key] && keys[key].length > 0 }))
                out.push(group)
        })
        return out
    }
    readonly property bool canPaste: win.hasClipboard && win.selectionCount > 0
    // The entry Paste Attributes reads: a copied set (a linked pair, a
    // group) gives its first picture clip, else its first clip.
    function source() {
        if (!win.clipboardIsSet)
            return win.clipboard
        for (var i = 0; i < win.clipboard.length; i++)
            if (win.clipboard[i].audioOnly !== true)
                return win.clipboard[i]
        return win.clipboard.length > 0 ? win.clipboard[0] : null
    }
    // Alt+V: the dialog, from the copied clip.
    function paste() {
        if (!canPaste)
            return
        var src = source()
        if (src === null)
            return
        pasteDialog.openFor(src, src.name)
    }
    // Presets as Effects-tab tiles (cat "presets"; always lit — applying
    // without a selection is a no-op like the other tiles).
    readonly property var presetTiles: backend.presets.map(function (p) {
        return { key: "preset:" + p.name, name: p.name, cat: "presets", ok: true }
    })
    function applyPreset(name) {
        var p = backend.preset(name)
        if (p.name === undefined || win.selectionCount === 0)
            return
        backend.viewTimeline()
        backend.pasteAttributes(win.selection.selectedAddresses(), p.attributes, p.groups)
    }
    function savePresetFromSelection() {
        var e = win.selection.selectedEntry()
        if (e === null || e.gap === true)
            return
        presetDialog.openFor(e)
    }
}
