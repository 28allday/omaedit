pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Deliver

// The Deliver page. Pure: it knows presets, jobs and the timeline's facts,
// never the engine. DeliverPage.qml binds it to Backend. The render
// settings live here as view state: a preset seeds them, every row edits
// them, Add to Queue turns them into a job. Pre-flight runs through the
// `validator` / `estimator` / `fileNamer` hooks the seam supplies.
Item {
    id: root
    property var layoutStore: null
    property alias layoutController: layout
    WorkspaceLayout { id: layout; host: root; store: root.layoutStore; workspaceKey: "Deliver" }

    // ── Inputs ──────────────────────────────────────────────────────────────
    property var presets: []
    property var jobs: []
    property bool queueActive: false
    property bool queuePaused: false
    property int activeJob: -1
    property string afterCompletion: "none"
    property var encoders: ({})
    property bool hasVideo: false
    property real fps: 0
    property int timelineWidth: 0
    property int timelineHeight: 0
    property int duration: 0
    property int position: 0
    property bool playing: false
    property real imageAspect: 16 / 9
    property int rangeIn: -1
    property int rangeOut: -1
    property var selectedSpans: []        // [{start, end, name}]
    property string projectName: ""
    property string sequenceName: ""
    property string defaultDir: ""
    property bool hasCaptions: false
    property int captionCount: 0
    property Component videoSurface: null
    // Hooks.
    property var validator: null          // function(spec, path) → [{level, text}]
    property var estimator: null          // function(spec, path) → {bytesText, freeText}
    // Both from one look at the timeline, when given: function(spec, path) →
    // {validation, estimate}. Preferred over the two above.
    property var checker: null
    property var nameTidier: null         // function(name) → the name as every render's is made tidy
    property var fileNamer: null          // function(pattern, presetName, dir, spec) → name
    property var stemNamer: null          // function(spec, path) → [{name, file}], the stems a render would make
    property var extensionOf: null        // function(spec) → ".mp4"
    property var summarizer: null         // function(spec) → text
    property var timecoder: null          // function(frame) → text

    // ── Outputs ─────────────────────────────────────────────────────────────
    signal enqueueRequested(string name, string path, var spec, string presetName, var warnings)
    signal renderAllRequested()
    signal pauseAllRequested()
    signal resumeAllRequested()
    signal clearFinishedRequested()
    signal jobAction(string action, int id)
    signal afterCompletionEdited(string action)
    signal presetSaveRequested(string name, var spec)
    signal presetUpdateRequested(string id, var spec)
    signal presetDuplicateRequested(string id)
    signal presetRenameRequested(string id, string name)
    signal presetDeleteRequested(string id)
    signal presetFavoriteRequested(string id, bool on)
    signal presetExportRequested(string id)
    signal presetImportRequested()
    signal folderRequested()
    signal rangeEdited(int rangeIn, int rangeOut)
    signal rangeCleared()
    signal seekRequested(int frame)
    signal playToggled()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    // A copy of the timeline at this size, reframed (the shape row's first choice).
    signal reshapeRequested(int width, int height)
    signal timelineExportRequested(string format)
    property var timelineFormats: []
    property bool canExportTimeline: false

    readonly property bool textActive: browser.textActive || settings.textActive || namePrompt.visible || metadataDialog.visible || advanced.visible || audioOutputs.visible

    // ── Settings state ──────────────────────────────────────────────────────
    property var audioTracks: []
    property var spec: ({ container: "mp4", vcodec: "h264", quality: 18, acodec: "aac", abitrate: 192 })
    property string presetId: ""
    property string presetName: ""
    property bool modified: false
    // The bit depth a master codec (ProRes, APV: always 10/12) replaced, put
    // back when another codec is chosen.
    property int bitDepthBeforeMaster: 0
    // Alpha lives in particular profiles; the main panel and the Advanced
    // dialog both switch to (or from) one, keeping the bit depth.
    function reconcileAlpha(s) {
        if (s.vcodec === "apv") {
            var depth = String(s.vprofile || "422-10").split("-")[1] || "10"
            if (s.alpha && String(s.vprofile || "").indexOf("4444") !== 0) s.vprofile = "4444-" + depth
            else if (!s.alpha && String(s.vprofile || "").indexOf("4444") === 0) s.vprofile = "444-" + depth
        } else if (s.vcodec === "prores" && s.alpha && ["4444", "4444xq"].indexOf(s.vprofile) < 0) {
            s.vprofile = "4444"
        }
    }
    property string fileNamePattern: "{project} - {preset}"
    property string saveDir: ""
    property string rangeMode: "full"
    property int handles: 12
    property var validation: []
    property var estimate: ({})
    property var lastPresetSpec: ({})
    // How many times the checks have run (once for each change, debounced).
    property int checksRun: 0
    // How another shape fills the frame, as chosen on each preset this
    // session: {preset id: framing}. It belongs to that preset alone.
    property var framingOf: ({})

    Settings { id: colourSettings; category: "color" }
    Settings {
        id: deliverSettings
        category: "deliver"
        // Once the default pattern every editor had, never chosen, has
        // become the tidy one (see Component.onCompleted).
        property bool patternTidied: false
        property alias saveDir: root.saveDir
        property alias fileNamePattern: root.fileNamePattern
        property alias presetId: root.presetId
        property alias handles: root.handles
    }

    readonly property string effectiveDir: saveDir !== "" ? saveDir : defaultDir
    readonly property string extension: extensionOf ? extensionOf(spec) : ".mp4"
    // Bumped whenever the disk may have changed (a job finished): {version}
    // counts files, so the name has to be looked up again.
    property int namesTick: 0
    readonly property string outputName: fileNamer && namesTick >= 0 ? fileNamer(fileNamePattern, presetName !== "" ? presetName : "Custom", effectiveDir, spec) : fileNamePattern
    readonly property string outputPath: effectiveDir !== "" && outputName !== "" ? effectiveDir + "/" + outputName + extension : ""
    readonly property var errors: validation.filter(function (r) { return r.level === "error" })
    readonly property var warnings: validation.filter(function (r) { return r.level === "warning" })
    // These settings are in the queue already, for this file: nothing to add
    // until one of them, the name or the place changes.
    readonly property bool queuedAlready: validation.some(function (r) { return r.queued === true })
    readonly property bool canQueue: hasVideo && errors.length === 0 && !queuedAlready && outputPath !== "" && (rangeMode !== "clips" && rangeMode !== "selection" || selectedSpans.length > 0)
    readonly property string queueHint: !hasVideo ? qsTr("The timeline is empty")
                                      : errors.length > 0 ? errors[0].text
                                      : queuedAlready ? qsTr("These settings are in the queue. Change a setting, the file name or the save location to add another render.")
                                      : outputPath === "" ? qsTr("Choose a save location") : ""

    function presetById(id) {
        for (var i = 0; i < presets.length; ++i) if (presets[i].id === id) return presets[i]
        return null
    }
    function copy(o) { var out = {}; for (var k in o) out[k] = o[k]; return out }
    function applyPreset(id) {
        var p = presetById(id)
        if (!p) return
        presetId = id
        presetName = p.name
        var s = copy(p.spec)
        delete s.rangeIn; delete s.rangeOut; delete s.captionCues; delete s.captionTrack; delete s.captionTracks
        // How another shape fills the frame is the preset's own: what was
        // chosen on it, or what it says.
        if (framingOf[id] !== undefined) s.framing = framingOf[id]
        spec = s
        lastPresetSpec = copy(s)
        modified = false
        settings.customResolution = false
    }
    function edit(key, value) {
        var s = copy(spec)
        if (key === "size") { s.width = value.width; s.height = value.height }
        else if (key === "qualityPreset") { s.rateMode = "quality"; s.quality = value }
        else if (key === "container") {
            s.container = value
            var audioOnly = value === "m4a" || value === "wav" || value === "mp3" || value === "flac"
            // A format that is only sound always has its sound: coming from a
            // picture sequence (which turned it off) it would otherwise be
            // left with nothing to render and its sound settings hidden.
            if (audioOnly) { s.video = false; s.audio = true }
            else if (s.video === false && (spec.container === "m4a" || spec.container === "wav" || spec.container === "mp3" || spec.container === "flac")) s.video = true
            // A numbered image sequence: pictures only, each file of the kind its format allows.
            var sequences = ["png", "tiff", "exr", "jpg"]
            if (sequences.indexOf(value) >= 0) {
                s.video = true; s.audio = false; s.subtitles = "none"; s.hw = "software"; s.fps = 0
                s.vcodec = value === "png" ? (s.vcodec === "png8" ? "png8" : "png16") : value === "tiff" ? "tiff16" : value === "exr" ? "exr" : "jpeg"
                if (value === "jpg") s.alpha = false
            } else if (sequences.indexOf(spec.container) >= 0) {
                s.vcodec = "h264"; s.audio = true // leaving a sequence: no still-picture codec stays behind
            }
            // Keep the codecs inside the container.
            if (value === "webm") { if (s.vcodec !== "vp9" && s.vcodec !== "av1") s.vcodec = "vp9"; if (s.acodec !== "opus" && s.acodec !== "vorbis") s.acodec = "opus" }
            else if (value === "mp4" && (s.vcodec === "prores" || s.vcodec === "dnxhr" || s.vcodec === "ffv1")) s.vcodec = "h264"
            if (value === "wav") s.acodec = s.acodec === "pcm24" ? "pcm24" : "pcm16"
            if (value === "mp3") s.acodec = "mp3"
            if (value === "flac") s.acodec = "flac"
            if (value === "m4a") s.acodec = "aac"
            if ((value === "mp4" || value === "mov" || value === "mkv") && s.acodec === "vorbis") s.acodec = "aac"
        }
        else if (key === "vcodec") {
            const wasMaster = s.vcodec === "prores" || s.vcodec === "apv", toMaster = value === "prores" || value === "apv"
            if (!wasMaster && toMaster) bitDepthBeforeMaster = s.bitDepth !== undefined ? s.bitDepth : 8
            else if (wasMaster && !toMaster) { s.bitDepth = bitDepthBeforeMaster > 0 ? bitDepthBeforeMaster : 8; bitDepthBeforeMaster = 0 }
            if (value !== s.vcodec) delete s.level // levels belong to a codec
            s.vcodec = value
            if (value === "prores") { s.vprofile = s.vprofile && ["proxy", "lt", "standard", "hq", "4444", "4444xq"].indexOf(s.vprofile) >= 0 ? s.vprofile : "hq"; s.bitDepth = 10 }
            else if (value === "dnxhr") { s.vprofile = s.vprofile && ["lb", "sq", "hq", "hqx", "444"].indexOf(s.vprofile) >= 0 ? s.vprofile : "hq" }
            else if (value === "apv") { s.vprofile = s.alpha ? "4444-10" : "422-10"; s.bitDepth = 10 }
            else if (s.vprofile && ["proxy", "lt", "standard", "hq", "4444", "4444xq", "lb", "sq", "hqx", "444"].indexOf(s.vprofile) >= 0) s.vprofile = ""
            if (["prores", "ffv1", "vp9", "apv", "png16", "png8", "tiff16", "exr"].indexOf(value) < 0) s.alpha = false
        }
        else if (key === "advanced") {
            const alphaWas = s.alpha
            for (var k in value) s[k] = value[k]
            if (value.alpha !== undefined && value.alpha !== alphaWas) reconcileAlpha(s)
        }
        else if (key === "alpha" && (s.vcodec === "apv" || s.vcodec === "prores")) {
            s.alpha = value
            reconcileAlpha(s)
        }
        else s[key] = value
        if (key === "framing" && presetId !== "") { var chosen = copy(framingOf); chosen[presetId] = value; framingOf = chosen }
        if (key === "container" || key === "vcodec") {
            if (s.container === "mov" && ["vp9","av1","ffv1"].indexOf(s.vcodec) >= 0) s.vcodec = "h264"
            var profiles = s.vcodec === "h264" ? ["","baseline","main","high","high10"]
                : s.vcodec === "hevc" ? ["","main","main10"]
                : s.vcodec === "prores" ? ["proxy","lt","standard","hq","4444","4444xq"]
                : s.vcodec === "dnxhr" ? ["lb","sq","hq","hqx","444"]
                : s.vcodec === "apv" ? ["422-10","422-12","444-10","444-12","4444-10","4444-12"]
                : s.vcodec === "mpeg2" ? ["","422"] : [""]
            if (profiles.indexOf(s.vprofile || "") < 0) s.vprofile = s.vcodec === "prores" || s.vcodec === "dnxhr" ? "hq" : s.vcodec === "apv" ? "422-10" : ""
            if (["h264","hevc","av1"].indexOf(s.vcodec) < 0 && !(s.vcodec === "prores" && s.hw === "vulkan")) s.hw = "software"
            if (["prores","ffv1","vp9","apv","png16","png8","tiff16","exr"].indexOf(s.vcodec) < 0) s.alpha = false
            if (s.vcodec === "mpeg2") s.bitDepth = 8
            if (s.container === "mov" && ["opus","flac","vorbis"].indexOf(s.acodec) >= 0) s.acodec = "aac"
        }
        if (key === "container" || key === "acodec") {
            if (s.acodec === "opus" || (s.acodec === "mp3" || s.acodec === "vorbis") && s.arate > 48000) s.arate = 48000
            if (s.acodec === "mp3" && s.channels > 2) s.channels = 2
        }
        // APV's profile names its depth: the settings say what it writes.
        if (s.vcodec === "apv" && s.vprofile) s.bitDepth = String(s.vprofile).endsWith("-12") ? 12 : 10
        spec = s
        modified = true
    }
    function reset() {
        if (presetId === "" || !presetById(presetId)) return
        var chosen = copy(framingOf); delete chosen[presetId]; framingOf = chosen
        applyPreset(presetId)
    }
    function jobSpec(rangeInFrame, rangeOutFrame) {
        var s = copy(spec)
        s.rangeIn = rangeInFrame
        s.rangeOut = rangeOutFrame
        s.overwritePaths = validation.filter(function(r) { return !!r.overwritePath }).map(function(r) { return r.overwritePath })
        s.toneMap = s.toneMap === "none" ? "" : s.toneMap || viewerToneMap()
        return s
    }
    // The tone map the Color page's viewer is grading through.
    function viewerToneMap() {
        colourSettings.sync()
        try {
            var shown = JSON.parse(colourSettings.value("displayJson", "{}"))
            return shown.enabled !== false && (shown.view === "ACES 1.3" || shown.view === "ACES 2.0") ? shown.view : ""
        } catch (e) { return "" }
    }
    function rangeFor(mode) {
        if (mode === "inout" && (rangeIn >= 0 || rangeOut >= 0)) return { rangeIn: rangeIn >= 0 ? rangeIn : 0, rangeOut: rangeOut >= 0 ? rangeOut : duration - 1 }
        if (mode === "selection" && selectedSpans.length > 0) {
            var lo = selectedSpans[0].start, hi = selectedSpans[0].end
            for (var i = 1; i < selectedSpans.length; ++i) { lo = Math.min(lo, selectedSpans[i].start); hi = Math.max(hi, selectedSpans[i].end) }
            return { rangeIn: lo, rangeOut: hi }
        }
        if (mode === "clips" && selectedSpans.length > 0)
            return { rangeIn: Math.max(0, selectedSpans[0].start - handles), rangeOut: Math.min(duration - 1, selectedSpans[0].end + handles) }
        return { rangeIn: -1, rangeOut: -1 }
    }
    function warningTexts() { return warnings.map(function (w) { return w.text }) }
    function requestedJobs() {
        var requests = []
        if (rangeMode === "clips") {
            for (var i = 0; i < selectedSpans.length; ++i) {
                var span = selectedSpans[i]
                var s = jobSpec(Math.max(0, span.start - handles), Math.min(duration - 1, span.end + handles))
                var name = outputName + "_clip" + String(i + 1).padStart(3, "0") + "_"
                         + (span.name !== undefined && span.name !== "" ? String(span.name).replace(/[\/\\:*?"<>|]/g, "-") : "clip")
                if (nameTidier) name = nameTidier(name)
                requests.push({ name: name, path: effectiveDir + "/" + name + extension, spec: s })
            }
            return requests
        }
        var r = rangeFor(rangeMode)
        return [{ name: outputName, path: outputPath, spec: jobSpec(r.rangeIn, r.rangeOut) }]
    }
    function addToQueue() {
        // Recheck the exact destination/settings at activation, not an older
        // debounced result (which may carry stale overwrite permissions).
        refreshValidation()
        if (!canQueue) return
        var pname = presetName !== "" ? presetName + (modified ? qsTr(" (modified)") : "") : qsTr("Custom")
        var requests = requestedJobs()
        for (var i = 0; i < requests.length; ++i) {
            var request = requests[i]
            enqueueRequested(request.name, request.path, request.spec, pname, warningTexts())
        }
        refresh()
    }
    // The checks for what Add to Queue would add, and (with the checker) the
    // size it would come to from the same look at the timeline.
    function refreshValidation() {
        var requests = requestedJobs(), checks = [], seen = new Set()
        for (var i = 0; i < requests.length; ++i) {
            var request = requests[i]
            var result = []
            if (checker) {
                var both = checker(request.spec, request.path)
                result = both.validation
                if (i === 0) estimate = both.estimate
            } else if (validator) result = validator(request.spec, request.path)
            for (var j = 0; j < result.length; ++j) {
                var key = result[j].level + ":" + result[j].text
                if (!seen.has(key)) { seen.add(key); checks.push(result[j]) }
            }
        }
        validation = checks
    }
    function runChecks() {
        namesTick += 1
        refreshValidation()
        if (!checker) {
            var r = rangeFor(rangeMode)
            estimate = estimator ? estimator(jobSpec(r.rangeIn, r.rangeOut), outputPath) : {}
        }
        checksRun += 1
    }

    // Pre-flight, debounced: it touches the file system.
    Timer {
        id: revalidate
        interval: 120
        onTriggered: root.runChecks()
    }
    function refresh() { revalidate.restart() }
    onSpecChanged: refresh()
    onOutputPathChanged: refresh()
    // A mode whose basis went away (the range cleared, the selection
    // emptied) falls back to the whole timeline.
    function checkRangeMode() {
        if (rangeMode !== "full" && !(rangeMode === "inout" ? (rangeIn >= 0 || rangeOut >= 0) : selectedSpans.length > 0))
            rangeMode = "full"
    }
    onRangeInChanged: { checkRangeMode(); refresh() }
    onRangeOutChanged: { checkRangeMode(); refresh() }
    onSelectedSpansChanged: { checkRangeMode(); refresh() }
    onHasVideoChanged: refresh()
    onHasCaptionsChanged: refresh()
    onCaptionCountChanged: refresh()
    onProjectNameChanged: refresh()
    onSequenceNameChanged: refresh()
    onTimelineWidthChanged: refresh()
    onTimelineHeightChanged: refresh()
    onFpsChanged: refresh()
    onTimecoderChanged: refresh()
    onAudioTracksChanged: refresh()
    onDurationChanged: refresh()
    onJobsChanged: refresh()
    onEncodersChanged: refresh()
    onHandlesChanged: refresh()
    Component.onCompleted: {
        // The pattern every editor had before names were made tidy, never
        // chosen, joins the project and the preset as the default now does;
        // once, so a pattern typed afterwards (even that one) is kept.
        if (!deliverSettings.patternTidied) {
            if (fileNamePattern === "{project}_{preset}") fileNamePattern = "{project} - {preset}"
            deliverSettings.patternTidied = true
        }
        if (presetId !== "" && presetById(presetId)) applyPreset(presetId)
        else if (presets.length > 0) applyPreset(presets[0].id)
        refresh()
    }
    onRangeModeChanged: { checkRangeMode(); refresh() }

    // ── Layout ──────────────────────────────────────────────────────────────
    WorkspaceSplit {
            controller: layout; splitId: "split1"
        anchors.fill: parent
        orientation: Qt.Horizontal


        WorkspaceSplit {
                controller: layout; splitId: "split2"
            visible: presetsPanel.docked || viewerPanel.docked || queuePanel.docked
            C.SplitView.fillWidth: true
            C.SplitView.minimumWidth: 550
            orientation: Qt.Vertical


            WorkspaceSplit {
                    controller: layout; splitId: "split3"
            visible: presetsPanel.docked || viewerPanel.docked
                C.SplitView.fillHeight: true
                C.SplitView.minimumHeight: 300
                orientation: Qt.Horizontal

                DockPanel {
                    id: presetsPanel
                    controller: layout; panelId: "presets"; title: qsTr("Render Presets")
                    C.SplitView.preferredWidth: 400
                    C.SplitView.minimumWidth: 260
                    openWidest: 520
                    PresetBrowser {
                        anchors.fill: parent
                        id: browser
                        dockBar: presetsPanel.barSlot
                        objectName: "presetBrowser"
                        presets: root.presets
                        currentId: root.presetId
                        currentModified: root.modified
                        describer: root.summarizer
                        timelineWidth: root.timelineWidth
                        timelineHeight: root.timelineHeight
                        onPresetSelected: function (id) { root.applyPreset(id) }
                        onNewPresetRequested: namePrompt.openFor(qsTr("New Preset"), root.presetName !== "" && root.modified ? root.presetName + qsTr(" copy") : "", "new")
                        onImportRequested: root.presetImportRequested()
                        onFavoriteToggled: function (id, on) { root.presetFavoriteRequested(id, on) }
                        onDuplicateRequested: function (id) { root.presetDuplicateRequested(id) }
                        onRenameRequested: function (id) { var p = root.presetById(id); namePrompt.openFor(qsTr("Rename Preset"), p ? p.name : "", id) }
                        onUpdateRequested: function (id) { root.presetUpdateRequested(id, root.spec); if (id === root.presetId) { root.lastPresetSpec = root.copy(root.spec); root.modified = false } }
                        onExportRequested: function (id) { root.presetExportRequested(id) }
                        onDeleteRequested: function (id) { root.presetDeleteRequested(id); if (id === root.presetId) { root.presetId = ""; root.presetName = qsTr("Custom") } }
                    }
                }
                DockPanel {
                    id: viewerPanel
                    controller: layout; panelId: "viewer"; title: qsTr("Viewer")
                    detachable: true
                    viewer: true
                    C.SplitView.fillWidth: true
                    C.SplitView.minimumWidth: 286
                    DeliveryViewer {
                        onFullscreenRequested: viewerPanel.toggleFullscreen()
                        anchors.fill: parent
                        id: viewer
                        objectName: "deliveryViewer"
                        sequenceName: root.sequenceName
                        hasMedia: root.hasVideo
                        playing: root.playing
                        position: root.position
                        duration: root.duration
                        rangeIn: root.rangeIn
                        rangeOut: root.rangeOut
                        imageAspect: root.imageAspect
                        timecoder: root.timecoder
                        fps: root.fps > 0 ? root.fps : 25
                        onPlayToggled: root.playToggled()
                        onShuttled: function (d) { root.shuttled(d) }
                        onStartRequested: root.startRequested()
                        onEndRequested: root.endRequested()
                        onSeekRequested: function (f) { root.seekRequested(f) }
                        onRangeEdited: function (a, b) { root.rangeEdited(a, b) }
                        onRangeCleared: root.rangeCleared()
    
                        Loader {
                            anchors.fill: parent
                            sourceComponent: root.videoSurface
                            active: root.videoSurface !== null && root.hasVideo
                        }
                    }
                }
            }
            DockPanel {
                id: queuePanel
                controller: layout; panelId: "queue"; title: qsTr("Render Queue")
                detail: queue.statusLine
                C.SplitView.preferredHeight: 260
                C.SplitView.minimumHeight: 120
                RenderQueuePanel {
                    anchors.fill: parent
                    id: queue
                    dockBar: queuePanel.barSlot
                    objectName: "renderQueuePanel"
                    jobs: root.jobs
                    active: root.queueActive
                    paused: root.queuePaused
                    activeJob: root.activeJob
                    canQueue: root.canQueue
                    queueHint: root.queueHint
                    afterCompletion: root.afterCompletion
                    timelineFormats: root.timelineFormats
                    canExportTimeline: root.canExportTimeline
                    onTimelineExportRequested: function (format) { root.timelineExportRequested(format) }
                    onAddRequested: root.addToQueue()
                    onRenderAllRequested: root.renderAllRequested()
                    onPauseAllRequested: root.pauseAllRequested()
                    onResumeAllRequested: root.resumeAllRequested()
                    onClearFinishedRequested: root.clearFinishedRequested()
                    onJobAction: function (action, id) { root.jobAction(action, id) }
                    onAfterCompletionEdited: function (a) { root.afterCompletionEdited(a) }
                    onLogRequested: function (id) {
                        for (var i = 0; i < root.jobs.length; ++i)
                            if (root.jobs[i].id === id) logDialog.openFor(root.jobs[i].name, root.jobs[i].log)
                    }
                }
            }
        }
        DockPanel {
            id: settingsPanel
            controller: layout; panelId: "settings"; title: qsTr("Render Settings")
            detail: settings.presetName
            C.SplitView.preferredWidth: 460
            C.SplitView.minimumWidth: 380
            openWidest: 600
            Item {
                anchors.fill: parent
                id: rightColumn
    
                RenderSettings {
                    id: settings
                    dockBar: settingsPanel.barSlot
                    objectName: "renderSettings"
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: summary.top
                    spec: root.spec
                    fileName: root.fileNamePattern
                    saveDir: root.effectiveDir
                    rangeMode: root.rangeMode
                    handles: root.handles
                    encoders: root.encoders
                    timelineWidth: root.timelineWidth
                    timelineHeight: root.timelineHeight
                    timelineFps: root.fps
                    hasRange: root.rangeIn >= 0 || root.rangeOut >= 0
                    selectionCount: root.selectedSpans.length
                    hasCaptions: root.hasCaptions
                    captionCount: root.captionCount
                    presetName: root.presetName + (root.modified ? qsTr(" (modified)") : "")
                    onSpecEdited: function (key, value) { root.edit(key, value) }
                    onFileNameEdited: function (name) { root.fileNamePattern = name }
                    onSaveDirEdited: function (dir) { root.saveDir = dir }
                    onFolderRequested: root.folderRequested()
                    onRangeModeEdited: function (mode) { root.rangeMode = mode }
                    onHandlesEdited: function (f) { root.handles = f }
                    onAudioOutputsRequested: audioOutputs.openFor(root.spec)
                    onMetadataRequested: metadataDialog.openFor(root.spec.metadata)
                    onAdvancedRequested: advanced.openFor(root.spec)
                    onResetRequested: root.reset()
                }
    
                ValidationSummary {
                    id: summary
                    objectName: "validationSummary"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    maximumHeight: Math.max(160, rightColumn.height * 0.4)
                    rows: root.validation
                    sizeText: root.estimate.bytesText !== undefined ? root.estimate.bytesText : ""
                    freeText: root.estimate.freeText !== undefined ? root.estimate.freeText : ""
                    startText: root.estimate.startText !== undefined ? root.estimate.startText : ""
                    hasMedia: root.hasVideo
                    onFramingChosen: function (framing) { root.edit("framing", framing) }
                    onReshapeRequested: function (width, height) { root.reshapeRequested(width, height) }
                }
            }
        }
    }

    NamePromptDialog {
        id: namePrompt
        taken: function (name) {
            for (var i = 0; i < root.presets.length; ++i)
                if (String(root.presets[i].name).toLowerCase() === name.toLowerCase()) return true
            return false
        }
        onAccepted2: function (name, ctx) {
            if (ctx === "new") {
                // The store echoes the list; onPresetsChanged adopts the new id by name.
                root.presetId = ""
                root.presetName = name
                root.modified = false
                root.presetSaveRequested(name, root.spec)
            }
            else root.presetRenameRequested(ctx, name)
        }
    }
    MetadataDialog {
        id: metadataDialog
        onEdited: function (meta) { root.edit("metadata", meta) }
    }
    AdvancedDialog {
        id: advanced
        timelineFps: root.fps
        onEdited: function (changes) { root.edit("advanced", changes) }
    }
    LogDialog { id: logDialog }

    // The preset a save lands on becomes the current one once the list echoes it.
    onPresetsChanged: {
        if (root.presetId !== "") {
            var current = presetById(root.presetId)
            if (current) root.presetName = current.name
            else { root.presetId = ""; root.presetName = qsTr("Custom"); root.modified = true }
            return
        }
        if (root.presetId === "" && root.presetName === "" && presets.length > 0) { applyPreset(presets[0].id); return }
        if (root.presetName === "" || root.presetId !== "") return
        for (var i = 0; i < presets.length; ++i)
            if (presets[i].name === root.presetName && !presets[i].builtin) { root.presetId = presets[i].id; root.lastPresetSpec = root.copy(root.spec); break }
    }
    function folderChosen(path) { saveDir = path }
    function setFolder(path) { saveDir = path }
    AudioOutputDialog {
        id: audioOutputs
        tracks: root.audioTracks
        stemLister: root.stemNamer === null ? null : function (draft) {
            const tick = root.namesTick // (the timeline's own changes come round with the names)
            return root.stemNamer(Object.assign({}, root.spec, draft), root.outputPath)
        }
        // The dialog hands back its whole draft: replay only what it changed,
        // or a vcodec replay would reset the codec's own choices (hardware).
        onEdited: function(changes) {
            for (var key in changes)
                if (JSON.stringify(changes[key]) !== JSON.stringify(root.spec[key])) root.edit(key, changes[key])
        }
    }

}
