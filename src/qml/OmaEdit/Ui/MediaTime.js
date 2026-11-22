.pragma library

// Quantize pointer coordinates once. Exact clip anchors remain rational in the
// edit engine; this conversion is only the user's chosen grid displacement.
function sampleTicks(pixels, pixelsPerFrame, fps, sampleRate) {
    return Math.round(pixels / pixelsPerFrame / fps * sampleRate)
}
function samplePosition(pixels, pixelsPerFrame, fps, sampleRate, duration) {
    return String(Math.max(0, Math.min(Math.ceil(duration / fps * sampleRate) - 1,
                                      sampleTicks(pixels, pixelsPerFrame, fps, sampleRate))))
}
function seconds(time) { return Number(time.ticks) / Number(time.rate) }

function isHeldPicture(entry) {
    if (!entry) return false
    if (entry.freeze !== undefined) return true
    const segments = entry.retime && entry.retime.segments
    if (!segments || segments.length !== 1) return false
    const segment = segments[0]
    return segment.speed === 0 && (segment.endSpeed === undefined || segment.endSpeed === 0)
}

// What a library clip is, for the labels the Media page shows. Projects saved
// before clips carried `still` are read by the file's extension.
// Only a sound file (sound, no picture stream) has no picture; a generated
// colour carries neither codec and is still a picture.
function hasPicture(entry) {
    return !!entry && (entry.hasVideo !== false || entry.hasAudio === false)
}
function isStill(entry) {
    if (!entry || !hasPicture(entry)) return false
    if (entry.still !== undefined) return entry.still === true
    return /\.(png|jpe?g|tiff?|exr|bmp|psd)$/i.test(String(entry.path || entry.name || ""))
}
// A frame rate as people write it: 25, 29.97, 23.976. Stills and sound files
// have none to show.
function rateText(entry) {
    const fps = Number(entry && entry.fps)
    if (!hasPicture(entry) || isStill(entry) || !(fps > 0)) return ""
    return String(Number(fps.toFixed(3)))
}
// A picture's size: 1080p and 4K for landscape video, the full size for a
// still or an upright picture (1080×1920), nothing for a sound file.
function pictureText(entry) {
    const w = Number(entry && entry.width), h = Number(entry && entry.height)
    if (!hasPicture(entry) || !(w > 0) || !(h > 0)) return ""
    if (isStill(entry) || h > w) return w + "\u00d7" + h
    if (w >= 7680) return "8K"
    if (w >= 3840) return "4K"
    return h + "p"
}
// A file's size in the units the Deliver page uses (1 kB = 1000 bytes).
function sizeText(bytes) {
    const b = Number(bytes)
    return b > 0 ? Qt.locale().formattedDataSize(b, 2, 3 /* Locale.DataSizeSIFormat */) : ""
}

// Legacy interpretation metadata may expand duration while also changing fps.
// Marks, thumbnails and preview seeks always address recorded source pictures.
function sourceFrames(entry) {
    return Math.max(0, Number(entry.sourceDuration !== undefined ? entry.sourceDuration : entry.duration) || 0)
}
function sourceFps(entry) {
    const rate = Number(entry.sourceFps !== undefined ? entry.sourceFps : entry.fps)
    return rate > 0 ? rate : 25
}
function effectiveFps(entry) {
    const rate = Number(entry.forceFps)
    return rate > 0 ? rate : sourceFps(entry)
}
function durationSeconds(entry) {
    return clipFrames(entry) / effectiveFps(entry)
}
function rangeFrames(entry) {
    const frames = sourceFrames(entry)
    const first = entry.rangeIn >= 0 ? Math.min(frames, entry.rangeIn) : 0
    const end = entry.rangeOut >= 0 ? Math.min(frames, entry.rangeOut + 1) : frames
    return Math.max(0, end - first)
}

// A library clip's own length in recorded pictures: a subclip is its range,
// any other clip the whole file (its marked range is only a selection).
function clipFrames(entry) {
    return entry && entry.subclip === true ? rangeFrames(entry) : sourceFrames(entry)
}
// The length a clip plays for, as the cards and the inspector read it.
function lengthText(entry) {
    return durationText(clipFrames(entry), effectiveFps(entry))
}

// A length of time as people read it — 6.12 s, 1:23.40, 1:02:03.40 — for a
// count of frames at `fps`. Lengths are shown as time everywhere; frames stay
// the unit underneath, so an edited length always lands on a whole frame.
function clockText(frames, fps) {
    const rate = Number(fps) > 0 ? Number(fps) : 25
    const hundredths = Math.round(Math.max(0, Number(frames) || 0) / rate * 100)
    const total = Math.floor(hundredths / 100), part = hundredths % 100
    const fraction = "." + (part < 10 ? "0" : "") + part
    if (total < 60) return total + fraction
    const h = Math.floor(total / 3600), m = Math.floor(total / 60) % 60, s = total % 60
    const ss = (s < 10 ? "0" : "") + s
    return h > 0 ? h + ":" + (m < 10 ? "0" : "") + m + ":" + ss + fraction : m + ":" + ss + fraction
}
function durationText(frames, fps) {
    const text = clockText(frames, fps)
    return text.indexOf(":") < 0 ? text + " s" : text
}
// A typed length in whole frames, or -1 when the text is not a length. Takes
// seconds ("2", "2.5", "2.5 s"), minutes and seconds ("1:30"), hours too
// ("1:02:03.5"), a full timecode ("00:00:06:03") or a frame count ("12f").
function parseDuration(text, fps) {
    const rate = Number(fps) > 0 ? Number(fps) : 25
    let t = String(text === undefined ? "" : text).trim().toLowerCase().replace(/,/g, ".").replace(/\s+/g, "")
    if (t === "") return -1
    if (/^\d+f(rames?)?$/.test(t)) return parseInt(t, 10)
    t = t.replace(/s(ec(onds?)?)?$/, "")
    const parts = t.split(":")
    if (parts.length > 4 || parts.some(function (p) { return !/^\d*\.?\d*$/.test(p) || p === "" || p === "." })) return -1
    // Every field after the first counts up to 60 (frames up to the rate).
    for (let i = 1; i < parts.length; ++i)
        if (Number(parts[i]) >= (i === 3 ? Math.round(rate) : 60)) return -1
    if (parts.length === 4) {
        if (parts.some(function (p) { return p.indexOf(".") >= 0 })) return -1
        return Math.round(((+parts[0] * 60 + +parts[1]) * 60 + +parts[2]) * Math.round(rate)) + +parts[3]
    }
    let seconds = 0
    for (let i = 0; i < parts.length; ++i) seconds = seconds * 60 + Number(parts[i])
    return isFinite(seconds) ? Math.round(seconds * rate) : -1
}
