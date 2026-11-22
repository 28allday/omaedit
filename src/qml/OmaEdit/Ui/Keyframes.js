.pragma library

// Keys are [{frame, value, interpolation}] in a clip's frames, in frame order
// (as EditEngine::valueAt reads them: linear, "smooth" or "hold" out of a key).

function valueAt(keys, frame, fallback) {
    if (!keys || !keys.length) return fallback
    if (frame <= keys[0].frame) return keys[0].value
    for (let i = 1; i < keys.length; ++i) {
        const a = keys[i - 1], b = keys[i]
        if (frame < b.frame) {
            if (a.interpolation === "hold") return a.value
            let u = (frame - a.frame) / (b.frame - a.frame)
            if (a.interpolation === "smooth") u = u * u * (3 - 2 * u)
            return a.value + u * (b.value - a.value)
        }
    }
    return keys[keys.length - 1].value
}

function keyedAt(keys, frame) {
    return !!keys && keys.some(function(k) { return k.frame === frame })
}

// The keys with one at `frame` holding `value`: a key already there keeps its
// easing, a new one takes the easing of the key before it.
function withKey(keys, frame, value) {
    const before = keys || []
    const earlier = before.filter(function(k) { return k.frame < frame })
    const exact = before.find(function(k) { return k.frame === frame })
    const list = before.filter(function(k) { return k.frame !== frame })
    list.push({frame: frame, value: value,
               interpolation: exact ? exact.interpolation || "linear"
                            : earlier.length ? earlier[earlier.length - 1].interpolation || "linear" : "linear"})
    list.sort(function(a, b) { return a.frame - b.frame })
    return list
}

// Where a frame falls among keys of any kind of value: the key before it, the
// one after and how far along between them (0..1, by the earlier key's easing).
function between(keys, frame) {
    if (frame <= keys[0].frame) return {a: keys[0], b: keys[0], u: 0}
    for (let i = 1; i < keys.length; ++i) {
        const a = keys[i - 1], b = keys[i]
        if (frame < b.frame) {
            if (a.interpolation === "hold") return {a: a, b: a, u: 0}
            let u = (frame - a.frame) / (b.frame - a.frame)
            if (a.interpolation === "smooth") u = u * u * (3 - 2 * u)
            return {a: a, b: b, u: u}
        }
    }
    const last = keys[keys.length - 1]
    return {a: last, b: last, u: 0}
}

// A switch or a choice: each key's setting holds until the next.
function stepAt(keys, frame, fallback) {
    return !keys || !keys.length ? fallback : between(keys, frame).a.value
}

// "#rrggbb" or "#aarrggbb" as [a, r, g, b]; null for anything else.
function channels(colour) {
    const hex = /^#([0-9a-f]{6}|[0-9a-f]{8})$/i.exec(String(colour))
    if (!hex) return null
    const digits = hex[1].length === 6 ? "ff" + hex[1] : hex[1], out = []
    for (let i = 0; i < 4; ++i) out.push(parseInt(digits.substr(i * 2, 2), 16))
    return out
}

// A colour: each channel on its own way between the two keys.
function colourAt(keys, frame, fallback) {
    if (!keys || !keys.length) return fallback
    const at = between(keys, frame), a = channels(at.a.value), b = channels(at.b.value)
    if (!a || !b || at.u === 0) return at.a.value
    let out = "#"
    for (let i = 0; i < 4; ++i) out += ("0" + Math.round(a[i] + (b[i] - a[i]) * at.u).toString(16)).slice(-2)
    return out
}

// A polygon's corners, [[x, y], …]: each on its way between the two keys.
function pointsAt(keys, frame, fallback) {
    if (!keys || !keys.length) return fallback
    const at = between(keys, frame)
    if (at.u === 0 || at.a.value.length !== at.b.value.length) return at.a.value
    return at.a.value.map(function(p, i) {
        const q = at.b.value[i]
        return [p[0] + (q[0] - p[0]) * at.u, p[1] + (q[1] - p[1]) * at.u]
    })
}

// ── An effect's mask (see VideoEffects::validateMask / validateMaskKeys) ──
var maskFields = ["x", "y", "w", "h", "rotation", "feather"]

// The mask as it stands at a frame of the clip: its keyed fields from their keys.
function maskAt(effect, frame) {
    if (!effect || !effect.mask) return null
    const mask = JSON.parse(JSON.stringify(effect.mask)), keys = effect.maskKeys || {}
    maskFields.forEach(function(f) { if (keys[f] && keys[f].length) mask[f] = valueAt(keys[f], frame, mask[f]) })
    if (mask.shape === "polygon" && keys.points && keys.points.length) {
        const points = pointsAt(keys.points, frame, mask.points)
        if (mask.points && points.length === mask.points.length) mask.points = JSON.parse(JSON.stringify(points))
    }
    return mask
}

// What to send for the mask becoming `next` at `frame`: a field with keys
// takes its new value as a key there, the rest are set as they are.
function maskUpdate(effect, next, frame) {
    const keys = effect && effect.maskKeys ? effect.maskKeys : {}, now = maskAt(effect, frame) || {}
    const mask = JSON.parse(JSON.stringify(next)), changed = {}
    let any = false
    maskFields.forEach(function(f) {
        if (!keys[f] || !keys[f].length || frame < 0) return
        if (next[f] !== undefined && next[f] !== now[f]) { changed[f] = withKey(keys[f], frame, next[f]); any = true }
        mask[f] = effect.mask[f]   // the set value stands under its keys
    })
    // A polygon's corners the same way, while it keeps as many as its keys hold
    // (a corner added or taken away ends their animation).
    if (next.shape === "polygon" && keys.points && keys.points.length && frame >= 0
            && next.points && next.points.length === keys.points[0].value.length) {
        if (JSON.stringify(next.points) !== JSON.stringify(now.points)) { changed.points = withKey(keys.points, frame, next.points); any = true }
        mask.points = effect.mask.points
    }
    const values = {mask: mask}
    if (any) values.maskKeys = changed
    return values
}
