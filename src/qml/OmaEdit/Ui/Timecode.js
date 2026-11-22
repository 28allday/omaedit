.pragma library

// The one reading of a typed timecode, used by every field that takes one.
//
// Digits are read from the right in pairs, frames last: 10000000 is
// 10:00:00:00, 1000 is ten seconds, 5 is five frames. `:` and `;` separate
// pairs, and so does a full stop or comma that has a digit after it
// (9:59:50:00, 09.59.50.00); a single digit after a separator is a pair with
// a nought before it (1.2 is one second two frames). A full stop or comma at
// the end of the entry, or with another after it, is a pair of noughts
// (10... is 10:00:00:00, 230. is 00:02:30:00).
//
// Timecode has no twenty-fourth hour: frames count to the whole rate (30 for
// 29.97, non-drop), seconds and minutes to 60, hours to 23, and a count of
// frames shown as a timecode starts again at 00:00:00:00 after a day.

function wholeRate(fps) {
    return Math.max(1, Math.round(Number(fps) > 0 ? Number(fps) : 25))
}
function dayFrames(fps) { return 86400 * wholeRate(fps) }
// A count of frames as the frame of the day its timecode names.
function wrap(frame, fps) {
    const day = dayFrames(fps)
    return ((Math.round(Number(frame) || 0) % day) + day) % day
}
function format(frame, fps) {
    const rate = wholeRate(fps), whole = wrap(frame, fps)
    const s = Math.floor(whole / rate)
    const pad = function (n) { return (n < 10 ? "0" : "") + n }
    return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(whole % rate)
}

// The digits an entry stands for, in order, or "" when it is not an entry.
function digits(text) {
    const t = String(text === undefined || text === null ? "" : text).trim()
    if (!/^\d[\d:;.,]*$/.test(t)) return ""
    let out = "", run = "", noughts = "", first = true
    const close = function () {
        // After a separator one digit is a pair; three or five are nothing.
        if (!first && run.length % 2 === 1) {
            if (run.length > 1) return false
            run = "0" + run
        }
        out += run + noughts
        run = ""; noughts = ""; first = false
        return true
    }
    for (let i = 0; i < t.length; ++i) {
        const c = t[i], next = i + 1 < t.length ? t[i + 1] : ""
        if (c >= "0" && c <= "9") {
            run += c
        } else if ((c === "." || c === ",") && (next === "" || next === "." || next === ",")) {
            noughts += "00"
        } else {
            if (!(next >= "0" && next <= "9") || !close()) return ""
        }
    }
    if (!close() || out.length > 8) return ""
    return out
}

// The frame of the day a typed timecode names at `fps`, or -1 when it names
// none. Pairs not typed on the left are nought, or with `base` (a count of
// frames) the hours, minutes and seconds of its timecode.
function read(text, fps, base) {
    let d = digits(text)
    if (d === "") return -1
    if (d.length % 2 === 1) d = "0" + d
    const rate = wholeRate(fps)
    const kept = base === undefined || base === null || base < 0 ? 0 : Math.floor(wrap(base, fps) / rate)
    const parts = [Math.floor(kept / 3600), Math.floor(kept / 60) % 60, kept % 60, 0]
    for (let i = 0; i < d.length / 2; ++i)
        parts[4 - d.length / 2 + i] = Number(d.substr(i * 2, 2))
    if (parts[0] > 23 || parts[1] >= 60 || parts[2] >= 60 || parts[3] >= rate) return -1
    return (parts[0] * 3600 + parts[1] * 60 + parts[2]) * rate + parts[3]
}

// An amount typed after + or -: one or two digits are frames, however many
// (30 at 25 fps is a second and five frames); a longer entry is read as a
// timecode. -1 when it is neither.
function readAmount(text, fps) {
    const t = String(text === undefined || text === null ? "" : text).trim()
    if (/^\d{1,2}$/.test(t)) return Number(t)
    return read(t, fps)
}

// What was typed where a position is asked for: { kind: "to", frame } for a
// timecode, { kind: "by", frames } for + or - and an amount, { kind: "" }
// for what is neither.
function readEntry(text, fps, base) {
    const t = String(text === undefined || text === null ? "" : text).trim()
    const sign = t.charAt(0)
    if (sign === "+" || sign === "-" || sign === "−") {
        const amount = readAmount(t.substr(1), fps)
        return amount < 0 ? { kind: "" } : { kind: "by", frames: sign === "+" ? amount : -amount }
    }
    const frame = read(t, fps, base)
    return frame < 0 ? { kind: "" } : { kind: "to", frame: frame }
}

// Where the timecode `frame` (a frame of the day) lies on something whose
// first frame's timecode is `origin`, counting forward from it through
// midnight. With `last` (its last frame) a timecode outside goes to the
// nearer end, and `outside` says which: -1 before the start, 1 past the end.
// Without, a timecode more than half a day on is taken as before the start.
function place(frame, origin, fps, last) {
    const day = dayFrames(fps)
    const on = (((frame - wrap(origin, fps)) % day) + day) % day
    if (last === undefined || last === null || last < 0)
        return on > day / 2 ? { frame: 0, outside: -1 } : { frame: on, outside: 0 }
    if (on <= last) return { frame: on, outside: 0 }
    // Counted from nought there is nothing before the start to be nearer to.
    return wrap(origin, fps) > 0 && day - on < on - last ? { frame: 0, outside: -1 } : { frame: last, outside: 1 }
}
