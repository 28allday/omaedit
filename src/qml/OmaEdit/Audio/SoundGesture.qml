import QtQuick
import "../Ui/MediaTime.js" as MediaTime

// One press owns one immutable target. Movement only previews; release commits
// once. A cancelled grab must never apply an edit.
QtObject {
    id: root
    property var entry: ({})
    property string units: "frames"
    property string tool: "select"
    property real fps: 25
    property real fpx: 1
    property int sampleRate: 48000
    property int duration: 0
    property bool snapping: true
    property bool pending: false
    property bool available: true
    property var targetFor: null
    property var previewFor: null
    property var pointsFor: null
    signal committed(var target, string action, var changes, int sampleRate)
    property bool usesFine: false
    property bool active: false
    property bool releasing: false
    property bool waiting: false
    property int serial: 0
    property var capture: null
    property var reference: null
    property var draft: null
    property var lastValid: null
    property var changes: ({})
    property string problem: ""
    property real lastPixels: 0
    property int lastDestination: -1
    property int displayTrack: -1
    readonly property var displayEntry: (active || releasing || waiting) && lastValid ? lastValid.entry : null
    function clear() {
        ++serial; active = false; releasing = false; waiting = false
        capture = null; reference = null; draft = null; lastValid = null
    }
    function cancel() { clear() }
    function viewMatches() {
        return capture && capture.fpx === fpx && capture.fps === fps
            && capture.rate === sampleRate && capture.units === units
    }
    function checkView() {
        if (!capture || waiting || viewMatches()) return
        cancel(); problem = qsTr("The view changed. Start the sound edit again.")
    }
    onFpxChanged: checkView()
    onFpsChanged: checkView()
    onSampleRateChanged: checkView()
    onUnitsChanged: checkView()
    onAvailableChanged: if (!available) cancel()
    onPendingChanged: if (!pending && waiting) clear()
    onEntryChanged: if (active) update(lastPixels, lastDestination)
    function arm(action, globalPixels) {
        clear(); problem = ""
        usesFine = units === "samples" || units === "milliseconds" || entry.audioTiming !== undefined
        if (!usesFine || pending || !available) return
        const target = targetFor ? targetFor(entry.track, entry.index) : ({})
        const clock = target.context || ({fpsNum: 25, fpsDen: 1})
        const fineGrid = units === "samples" || units === "milliseconds"
        const position = fineGrid
            ? {ticks: MediaTime.samplePosition(globalPixels, fpx, fps, sampleRate, duration), rate: sampleRate}
            : {ticks: String(Math.max(0, Math.floor(globalPixels / fpx)) * clock.fpsDen), rate: clock.fpsNum}
        capture = {target: target, action: action, track: entry.track, rate: sampleRate,
                   fps: fps, fpx: fpx, units: units, clock: clock, fineGrid: fineGrid,
                   ripple: tool === "trim" && (action === "trimIn" || action === "trimOut"),
                   position: position, points: pointsFor ? pointsFor(target) : []}
        displayTrack = entry.track
        reference = previewFor ? previewFor(target, "move", {delta: {ticks: "0", rate: sampleRate}}, sampleRate) : null
        if (reference && reference.valid) lastValid = reference
        else problem = reference ? reference.error : qsTr("Sound editing is unavailable.")
    }
    function activate() {
        if (!capture || !available || !viewMatches()) return false
        active = true; lastPixels = 0; lastDestination = capture.track
        update(0, capture.track)
        return true
    }
    function nearest(frame) {
        if (!snapping || !capture) return null
        let best = null, distance = 8.000001
        for (let i = 0; i < capture.points.length; ++i) {
            const point = capture.points[i], d = Math.abs(point.frame - frame) * capture.fpx
            if (d < distance) { best = {time: point.time, distance: d}; distance = d }
        }
        return best
    }
    function update(pixels, destination) {
        if (!active || !capture) return
        if (!viewMatches()) { checkView(); return }
        lastPixels = pixels; lastDestination = destination
        const c = capture
        const delta = c.fineGrid
            ? {ticks: String(MediaTime.sampleTicks(pixels, c.fpx, c.fps, c.rate)), rate: c.rate}
            : {ticks: String(Math.round(pixels / c.fpx) * c.clock.fpsDen), rate: c.clock.fpsNum}
        let next = {delta: delta}
        if (c.ripple) next.ripple = true
        if (c.action === "move" && destination !== c.track) next.destinationTrack = destination
        if (reference && reference.valid && c.action !== "slip") {
            const start = MediaTime.seconds(reference.timing.start) * c.fps
            const end = MediaTime.seconds(reference.end) * c.fps
            const shift = MediaTime.seconds(delta) * c.fps
            let point = null, edge = "start"
            if (c.action !== "trimOut") point = nearest(start + shift)
            if (c.action !== "trimIn") {
                const tail = nearest(end + shift)
                if (tail && (!point || tail.distance < point.distance)) { point = tail; edge = "end" }
            }
            if (point) { next.snap = point.time; next.edge = edge }
        }
        changes = next
        draft = previewFor ? previewFor(c.target, c.action, next, c.rate) : null
        problem = draft && draft.valid ? "" : draft ? draft.error : qsTr("Sound editing is unavailable.")
        if (draft && draft.valid) {
            lastValid = draft
            displayTrack = destination === c.track ? entry.track : destination
        }
    }
    function finish() {
        if (!active || !capture) return
        releasing = true; active = false
        const ticket = serial
        // Cancellation can arrive after active=false, in the same event.
        Qt.callLater(function() {
            if (ticket !== serial || !releasing || !capture || !available) return
            if (!viewMatches()) { checkView(); return }
            releasing = false; waiting = true
            committed(capture.target, capture.action, changes, capture.rate)
        })
    }
    function split() {
        if (!capture || !available || !viewMatches()) return
        let position = capture.position
        const point = nearest(MediaTime.seconds(position) * capture.fps)
        if (point) position = point.time
        waiting = true
        committed(capture.target, "split", {position: position}, capture.rate)
    }
}
