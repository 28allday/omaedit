pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Audio

// The Audio page. Pure: tracks, clips, levels, the library and the meters
// come in; every edit leaves as a signal AudioPage.qml maps to Backend.
// Layout follows the render: library | viewer | mixer tabs | inspector
// over the audio timeline, every boundary a SplitView handle, sizes
// persisted.
Item {
    id: root
    property var layoutStore: null
    property alias layoutController: layout
    WorkspaceLayout { id: layout; host: root; store: root.layoutStore; workspaceKey: "Audio" }

    // ── Inputs ──────────────────────────────────────────────────────────────
    property var tracks: []
    property var gestureTarget: null
    property var gesturePreview: null
    property var snapPoints: null
    property var beatLines: []
    // The clips' waveforms stand from the foot (the owner's View choice).
    property bool rectifiedWaveforms: false
    property bool gesturePending: false
    property string gestureError: ""
    signal gestureCommitted(var target, string action, var changes, int sampleRate)
    signal timeSeekRequested(var position, bool audition)
    property var backgroundRender: ({})
    property var clips: []
    property var trackLevels: []
    property var routingLevels: []
    property var master: ({})
    property var monitorSettings: ({})
    property real masterLeft: -100
    property real masterRight: -100
    property var loudness: ({})
    property var loudnessHistory: []
    property var spectrum: []
    property var analysis: ({})
    // The sound library panel is not offered: sound is what the editor
    // imports into the project. The setting audio/soundLibrary brings it back.
    property bool soundLibraryOffered: false
    property var sounds: []
    property var soundFolders: []
    property var collections: []
    property bool scanning: false
    property int pending: 0
    property string scanStatus: ""
    property var audioServices: ({})
    property var mixEffects: []
    property var adrCues: []
    property var inputs: []
    property var outputs: []
    property bool recording: false
    property int recordTrack: -1
    property real recordLevel: -100
    property bool recorderAvailable: false
    property string inputDevice: ""
    property bool hasVideo: false
    property real fps: 25
    property int sampleRate: 48000
    property int duration: 0
    property int position: 0
    property real cursorFrames: position
    property string cursorText: ""
    signal cursorTextEdited(string text)
    signal sampleStepRequested(int samples)
    signal sampleSeekRequested(string ticks, bool audition)
    property bool playing: false
    property real auditionVolume: 1
    signal auditionVolumeEdited(real value)
    property bool auditioning: false
    property bool auditionPlaying: false
    property string auditionPath: ""
    property int rangeIn: -1
    property int rangeOut: -1
    property var markers: []
    property bool loop: false
    property string projectName: ""
    property string sequenceName: ""
    property real imageAspect: 16 / 9
    property Component videoSurface: null
    property var timecoder: null
    property int startFrame: 0
    property var fxPresets: []
    property int selectedClip: -1
    property int embeddedAudioCount: 0 // video clips carrying their own sound on V1

    // ── Outputs ─────────────────────────────────────────────────────────────
    signal trackMixEdited(int track, string key, var value)
    signal mixFinished(int track, string key)
    signal mixStarted(int track, string key)
    signal trackFlagEdited(int track, string flag, bool on)
    signal trackRenamed(int track, string name)
    signal addTrackRequested(string role)
    signal removeTrackRequested(int track)
    signal moveTrackRequested(int track, bool up)
    signal masterEdited(string key, var value)
    signal duplicateClipRequested()
    signal clipTimingRequested(int index)
    signal clipSpeedRequested(real speed)
    signal clipFadeCurveRequested(string curve)
    signal clipSelected(int index)
    signal clipMoveRequested(int index, int newTrack, int newStart)
    signal clipTrimRequested(int index, int inFrame, int outFrame, int newStart)
    signal clipRemoveRequested(int index, bool ripple)
    signal splitRequested(int track, int frame)
    signal clipFadesEdited(int index, int fadeIn, int fadeOut)
    signal clipVolumeEdited(int index, real db)
    signal clipVolumeKfEdited(int index, var kf)
    signal decorationWindowChanged(real first, real last, int pixels)
    signal seekRequested(int frame)
    signal scrubRequested(int frame)
    signal playToggled()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal stepRequested(int frames)
    signal loopToggled(bool on)
    signal rangeEdited(int rangeIn, int rangeOut)
    signal rangeCleared()
    signal markerRequested(int frame)
    signal auditionRequested(string path)
    signal auditionStopped()
    signal soundFavorited(string path, bool on)
    signal soundCategorized(string path, string category)
    signal soundTagsEdited(string path, var tags)
    signal soundFolderAddRequested()
    signal soundFolderRemoved(string path)
    signal soundRescanRequested()
    signal soundScanCancelRequested()
    signal collectionAdded(string name, string query, string category)
    signal collectionRemoved(string name)
    signal soundDropped(string path, int track, int frame)
    signal soundPlacedAtPlayhead(string path)
    signal recordToggled()
    signal recordTakeRequested(int cueIndex)
    signal adrCuesEdited(var cues)
    signal adrReviewRequested(int cueIndex, int takeIndex)
    signal adrPromoteRequested(int cueIndex, int takeIndex)
    signal adrCompRequested(int cueIndex, int takeIndex)
    signal inputDeviceEdited(string id)
    signal loudnessReset()
    signal analyzeRequested(bool selection)
    signal cancelAnalysisRequested()
    signal normalizeRequested(real target, real peak)
    signal fxPresetSaved(string name, var fx)
    signal fxPresetDeleted(string name)
    signal muteMonitoringToggled(bool on)
    signal monitorEdited(bool muted, bool dim, bool mono)
    signal snapshotRequested()

    // ── View state ──────────────────────────────────────────────────────────
    property int selectedTrack: -1         // engine index; -2 master; -10-n bus n
    property string tool: "select"
    // Whether drags snap: the window's choice, the same as the Edit page's.
    // The toolbar's magnet asks for it to change (snappingToggled).
    property bool snapping: true
    signal snappingToggled(bool on)
    property string units: "frames"
    property real zoom: 0
    property string wheelAction: "scroll"
    property bool wheelInvert: false
    property real wheelSensitivity: 1.0
    property int rowHeight: Theme.hAudioTrack
    property bool showAutomation: true
    property bool compactMixer: false
    property int mixerTab: 0
    property string loudnessTarget: "ebu"
    readonly property bool before: (selectedMix.fx || []).some(function(e) { return e.group === "voice" && e.wasBypassed !== undefined })
    readonly property var buses: master.buses !== undefined ? master.buses : [{ id: "bus1", name: qsTr("Bus 1"), mix: {} }]
    readonly property var busMix: busSelected && buses[-10 - selectedTrack] ? buses[-10 - selectedTrack].mix || {} : ({})
    property var busBypassed: ({})
    // Any focused text editor on the page (a track rename, an inspector
    // value field, the ADR fields…) stands the window's shortcuts down.
    readonly property Item focusItem: Window.activeFocusItem
    readonly property bool textActive: mixTools.visible || library.textActive || effects.textActive || adr.textActive
                                       || (focusItem !== null && focusItem !== undefined && focusItem.cursorPosition !== undefined)

    Settings {
        category: "audio"
        property alias tool: root.tool
        property alias units: root.units
        property alias zoom: root.zoom
        property alias rowHeight: root.rowHeight
        property alias soundLibrary: root.soundLibraryOffered
        property alias showAutomation: root.showAutomation
        property alias compactMixer: root.compactMixer
        property alias mixerTab: root.mixerTab
        property alias loudnessTarget: root.loudnessTarget
    }

    function trackByIndex(i) { for (var k = 0; k < tracks.length; ++k) if (tracks[k].index === i) return tracks[k]; return null }
    readonly property var selectedTrackEntry: trackByIndex(selectedTrack)
    readonly property bool masterSelected: selectedTrack === -2
    readonly property bool busSelected: selectedTrack <= -10
    readonly property var sourceMix: masterSelected ? master : busSelected ? busMix : (selectedTrackEntry !== null && selectedTrackEntry.mix !== undefined ? selectedTrackEntry.mix : ({}))
    // Keep successive UI edits together until the worker echoes the latest chain.
    property var fxDraft: null
    readonly property var selectedMix: fxDraft === null ? sourceMix : Object.assign({}, sourceMix, {fx:fxDraft})
    function canonical(value) {
        if (Array.isArray(value)) return value.map(canonical)
        if (value !== null && typeof value === "object") { var result={}; Object.keys(value).sort().forEach(function(k){result[k]=canonical(value[k])}); return result }
        return value
    }
    function fxKey(value) {
        // QVariantList uses a QML sequence wrapper, not a JavaScript Array.
        return JSON.stringify(canonical(JSON.parse(JSON.stringify(value))))
    }
    onSourceMixChanged: if (fxDraft !== null && fxKey(sourceMix.fx || []) === fxKey(fxDraft)) fxDraft = null
    onSelectedTrackChanged: fxDraft = null

    // The selected track is a place in the engine's list, and the list changes
    // shape under it: a track is removed, moved, added, or brought back by
    // undo. The selection then goes where that track went, so the next fader
    // or effect edit reaches the track the editor chose; when the tracks
    // cannot be told apart, or the list is another one altogether (another
    // sequence), nothing stays selected rather than the wrong track.
    // `trackOp` is what this page itself has just asked for, as
    // { remove: index } or { swap: [a, b] }: exact, where the list alone
    // could not tell two alike tracks apart.
    property var trackOp: null
    property var trackMarks: []
    Timer { id: trackOpExpiry; interval: 2000; onTriggered: root.trackOp = null } // refused: no answer came
    onTrackOpChanged: if (trackOp !== null) trackOpExpiry.restart(); else trackOpExpiry.stop()
    function trackMark(t) { // what a track is, apart from where it is and what lies on it
        return t.kind + "|" + [t.locked, t.disabled, t.muted, t.solo, t.autoSelect].join() + "|" + JSON.stringify(t.mix || {})
    }
    function forgetOwnHeights() { // a new common height: every track back on it
        for (var i = 0; i < tracks.length; ++i)
            if (tracks[i].mix && tracks[i].mix.audioHeight)
                trackMixEdited(tracks[i].index, "audioHeight", undefined)
    }
    // Where the track at `at` in the old list `was` is in the new list `now`; -1 when it has gone or cannot be told.
    function trackMovedTo(at, was, now, op) {
        var i
        if (op && op.swap && was.length === now.length)
            return at === op.swap[0] ? op.swap[1] : at === op.swap[1] ? op.swap[0] : at
        if (op && op.remove !== undefined && now.length === was.length - 1)
            return at === op.remove ? -1 : at > op.remove ? at - 1 : at
        if (now.length === was.length) {
            var changed = []
            for (i = 0; i < now.length; ++i) if (now[i] !== was[i]) changed.push(i)
            // Two tracks that changed places (a move made on another page, or its undo).
            if (changed.length === 2 && now[changed[0]] === was[changed[1]] && now[changed[1]] === was[changed[0]])
                return at === changed[0] ? changed[1] : at === changed[1] ? changed[0] : at
            return at // the same tracks: a fader, a flag, a clip
        }
        var removed = now.length === was.length - 1, added = now.length === was.length + 1
        if (!removed && !added)
            return -1
        // One track went or came: every place it can have been at.
        var longer = removed ? was : now, shorter = removed ? now : was, places = []
        for (var p = 0; p < longer.length; ++p) {
            var fits = true
            for (i = 0; i < shorter.length && fits; ++i) fits = shorter[i] === longer[i < p ? i : i + 1]
            if (fits) places.push(p)
        }
        if (places.length === 0)
            return -1
        var first = places[0], last = places[places.length - 1]
        if (removed)
            return at < first ? at : at > last ? at - 1 : -1
        return at < first ? at : at >= last ? at + 1 : -1
    }
    onTracksChanged: {
        var now = [], was = trackMarks, op = trackOp
        for (var i = 0; i < tracks.length; ++i) now.push(trackMark(tracks[i]))
        trackMarks = now
        trackOp = null
        if (selectedTrack >= 0 && selectedTrack < was.length) {
            var to = trackMovedTo(selectedTrack, was, now, op)
            if (to !== selectedTrack)
                selectedTrack = to
        }
    }
    function startEffectEdit() { mixStarted(busSelected ? -2 : selectedTrack, busSelected ? "buses" : "fx") }
    function finishEffectEdit() { mixFinished(busSelected ? -2 : selectedTrack, busSelected ? "buses" : "fx") }
    readonly property string selectedName: masterSelected ? qsTr("Master") : busSelected && buses[-10 - selectedTrack] ? buses[-10 - selectedTrack].name : (selectedTrackEntry !== null ? selectedTrackEntry.name : "")
    readonly property bool hasSelection: masterSelected || busSelected || selectedTrackEntry !== null
    readonly property bool canEditSelection: hasSelection && (selectedTrackEntry === null || selectedTrackEntry.locked !== true)
    function editSelectedMix(key, value) {
        if (!canEditSelection) return
        if (key === "fx" && hasSelection) fxDraft = value
        if (masterSelected) masterEdited(key, value)
        else if (busSelected && buses[-10 - selectedTrack]) editBus(buses[-10 - selectedTrack].id, key, value)
        else if (selectedTrackEntry !== null) trackMixEdited(selectedTrack, key, value)
    }
    function editBus(id, key, value) {
        var next = buses.map(function (b) { var copy = Object.assign({}, b); copy.mix = Object.assign({}, b.mix || {}); if (b.id === id) copy.mix[key] = value; return copy })
        masterEdited("buses", next)
    }
    function setBefore(on) { // A/B: bypass every voice insert on the selected strip without losing its state
        if (!hasSelection) return
        var fx = (selectedMix.fx !== undefined ? selectedMix.fx : []).map(function (e) { var c = {}; for (var k in e) c[k] = e[k]; return c })
        for (var i = 0; i < fx.length; ++i) if (fx[i].group === "voice") {
            if (on && fx[i].wasBypassed === undefined) { fx[i].wasBypassed = fx[i].bypass === true; fx[i].bypass = true }
            else if (!on && fx[i].wasBypassed !== undefined) { fx[i].bypass = fx[i].wasBypassed === true; delete fx[i].wasBypassed }
        }
        startEffectEdit(); editSelectedMix("fx", fx); finishEffectEdit()
    }
    function selectClip(index) { selectedClip = index; clipSelected(index) }

    WorkspaceSplit {
            controller: layout; splitId: "split1"
        id: rows
        anchors.fill: parent
        orientation: Qt.Vertical

        property real timelineSplit: 0.45

        WorkspaceSplit {
                controller: layout; splitId: "split2"
            visible: libraryPanel.docked || viewerPanel.docked || mixerPanel.docked || inspectorPanel.docked
            id: columns
            C.SplitView.fillHeight: true
            C.SplitView.minimumHeight: 240
            orientation: Qt.Horizontal

            property real librarySplit: 0.26
            property real viewerSplit: 0.34
            property real mixerSplit: 0.26
            DockPanel {
                id: libraryPanel
                controller: layout; panelId: "library"; title: qsTr("Sound Library")
                offered: root.soundLibraryOffered
                C.SplitView.preferredWidth: columns.width * columns.librarySplit
                C.SplitView.minimumWidth: 190
                onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) columns.librarySplit = width / columns.width
                SoundLibrary {
                    anchors.fill: parent
                    id: library
                    objectName: "soundLibrary"
                    sounds: root.sounds
                    folders: root.soundFolders
                    collections: root.collections
                    scanning: root.scanning
                    pending: root.pending
                    scanStatus: root.scanStatus
                    auditionVolume: root.auditionVolume
                    onAuditionVolumeEdited: function(v){root.auditionVolumeEdited(v)}
                    auditionPath: root.auditionPath
                    auditionPlaying: root.auditionPlaying
                    loop: root.loop
                    auditionLeft: root.masterLeft
                    auditionRight: root.masterRight
                    fps: root.fps
                    timecoder: root.timecoder
                    onAuditionRequested: function (p) { root.auditionRequested(p) }
                    onAuditionStopped: root.auditionStopped()
                    onLoopToggled: function (on) { root.loopToggled(on) }
                    onFavoriteEdited: function (p, on) { root.soundFavorited(p, on) }
                    onTagsEdited: function (path,tags) { root.soundTagsEdited(path,tags) }
                    onCategoryEdited: function (p, c) { root.soundCategorized(p, c) }
                    onFolderAddRequested: root.soundFolderAddRequested()
                    onFolderRemoved: function (p) { root.soundFolderRemoved(p) }
                    onRescanRequested: root.soundRescanRequested()
                    onScanCancelRequested: root.soundScanCancelRequested()
                    onCollectionAdded: function (n, q, c) { root.collectionAdded(n, q, c) }
                    onCollectionRemoved: function (n) { root.collectionRemoved(n) }
                    onSoundOpened: function (p) { root.soundPlacedAtPlayhead(p) }
                }
            }
            DockPanel {
                id: viewerPanel
                controller: layout; panelId: "viewer"; title: qsTr("Viewer")
                detachable: true
                viewer: true
                C.SplitView.preferredWidth: columns.width * columns.viewerSplit
                C.SplitView.minimumWidth: 240
                onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) columns.viewerSplit = width / columns.width
                AudioViewer {
                    anchors.fill: parent
                    id: viewer
                    objectName: "audioViewer"
                    sequenceName: root.sequenceName
                    hasMedia: root.hasVideo
                    playing: root.playing || root.auditionPlaying
                    auditioning: root.auditioning
                    position: root.position
                    duration: root.duration
                    imageAspect: root.imageAspect
                    fps: root.fps
                    levelLeft: root.masterLeft
                    levelRight: root.masterRight
                    loop: root.loop
                    videoSurface: root.videoSurface
                    timecoder: root.timecoder
                    onPlayToggled: root.playToggled()
                    onShuttled: function (d) { root.shuttled(d) }
                    onStartRequested: root.startRequested()
                    onEndRequested: root.endRequested()
                    onStepRequested: function (f) { root.stepRequested(f) }
                    onLoopToggled: function (on) { root.loopToggled(on) }
                    monitorSettings: root.monitorSettings
                    onMonitorEdited: function (muted, dim, mono) { root.monitorEdited(muted, dim, mono) }
                    onSnapshotRequested: root.snapshotRequested()
                }
            }
            DockPanel {
                id: mixerPanel
                controller: layout; panelId: "mixer"; title: qsTr("Mixer")
                detachable: true
                C.SplitView.preferredWidth: columns.width * columns.mixerSplit
                C.SplitView.minimumWidth: 240
                onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) columns.mixerSplit = width / columns.width
                Item {
                    anchors.fill: parent
                    id: mixerColumn
                    MixerPanel {
                        id: mixer
                        objectName: "mixerPanel"
                        anchors.fill: parent
                        tracks: root.tracks
                        trackLevels: root.trackLevels
                        routingLevels: root.routingLevels
                        master: root.master
                        masterLeft: root.masterLeft
                        masterRight: root.masterRight
                        masterMuted: root.master.muted === true
                        buses: root.buses
                        catalogue: root.mixEffects
                        inputs: root.inputs
                        outputs: root.outputs
                        selectedTrack: root.selectedTrack
                        compact: root.compactMixer
                        currentTab: root.mixerTab
                        onCompactChanged: root.compactMixer = compact
                        onCurrentTabChanged: root.mixerTab = currentTab
                        onTrackSelected: function (t) { root.selectedTrack = t }
                        onMixFinished: function (t, key) { root.mixFinished(t <= -10 ? -2 : t, t <= -10 ? "buses" : key) }
                        onMixStarted: function (t, key) { root.mixStarted(t <= -10 ? -2 : t, t <= -10 ? "buses" : key) }
                        onTrackMixEdited: function (t, k, v) {
                            if (k === "input") { root.inputDeviceEdited(v); root.trackMixEdited(t, k, v); return }
                            if (k === "sendBus" || k === "sendLevel" || k === "sendPre") {
                                var tr = root.trackByIndex(t)
                                var sends = tr && tr.mix !== undefined && tr.mix.sends !== undefined ? tr.mix.sends.map(function (s) { var c = {}; for (var kk in s) c[kk] = s[kk]; return c }) : []
                                if (sends.length === 0) sends.push({ bus: "", level: -12, pre: false })
                                if (k === "sendBus") sends[0].bus = v; else if (k === "sendLevel") sends[0].level = v; else sends[0].pre = v
                                root.trackMixEdited(t, "sends", sends)
                                return
                            }
                            root.trackMixEdited(t, k, v)
                        }
                        onTrackFlagEdited: function (t, f, on) { root.trackFlagEdited(t, f, on) }
                        onMasterEdited: function (k, v) { root.masterEdited(k, v) }
                        onMasterMuteToggled: function (on) { root.masterEdited("muted", on) }
                        onBusEdited: function (id, k, v) { root.editBus(id, k, v) }
                        onInsertOpened: function (t, i) { root.selectedTrack = t; root.mixerTab = 1; effects.currentIndex = i }
                        onInsertAddRequested: function (t) { root.selectedTrack = t; root.mixerTab = 1 }
                    }
                    EffectsTab {
                        id: effects
                        objectName: "effectsTab"
                        anchors.fill: parent
                        anchors.topMargin: mixer.tabsItem.height
                        visible: root.mixerTab === 1
                        stripName: root.selectedName
                        fx: root.selectedMix.fx !== undefined ? root.selectedMix.fx : []
                        catalogue: root.mixEffects
                        audioServices: root.audioServices
                        presets: root.fxPresets
                        enabled2: root.canEditSelection
                        onEditingStarted: root.startEffectEdit()
                        onEditingFinished: root.finishEffectEdit()
                        onFxEdited: function (fx) { root.editSelectedMix("fx", fx) }
                        onPresetSaveRequested: function (n, fx) { root.fxPresetSaved(n, fx) }
                        onPresetDeleteRequested: function (n) { root.fxPresetDeleted(n) }
                    }
                    AdrTab {
                        id: adr
                        objectName: "adrTab"
                        anchors.fill: parent
                        anchors.topMargin: mixer.tabsItem.height
                        visible: root.mixerTab === 2
                        cues: root.adrCues
                        tracks: root.tracks
                        fps: root.fps
                        position: root.position
                        rangeIn: root.rangeIn
                        rangeOut: root.rangeOut
                        recording: root.recording
                        canRecord: root.recorderAvailable && root.hasVideo
                        timecoder: root.timecoder
                        startFrame: root.startFrame
                        onCuesEdited: function (c) { root.adrCuesEdited(c) }
                        onRecordTakeRequested: function (i) { root.recordTakeRequested(i) }
                        onStopRequested: root.recordToggled()
                        onReviewRequested: function (c, t) { root.adrReviewRequested(c, t) }
                        onCompRequested: function(c,t) { root.adrCompRequested(c,t) }
                        onPromoteRequested: function (c, t) { root.adrPromoteRequested(c, t) }
            onSeekRequested: function (f) { root.seekRequested(f) }
                    }
                    LoudnessTab {
                        id: loudnessTab
                        objectName: "loudnessTab"
                        anchors.fill: parent
                        anchors.topMargin: mixer.tabsItem.height
                        visible: root.mixerTab === 3
                        live: root.loudness
                        history: root.loudnessHistory
                        analysis: root.analysis
                        targetId: root.loudnessTarget
                        playing: root.playing
                        onTargetEdited: function (id) { root.loudnessTarget = id }
                        onResetRequested: root.loudnessReset()
                        hasRange: root.rangeIn >= 0 && root.rangeOut >= root.rangeIn
                        onAnalyzeRequested: function (selection) { root.analyzeRequested(selection) }
                        onCancelRequested: root.cancelAnalysisRequested()
                        onNormalizeRequested: function (t, p) { root.normalizeRequested(t, p) }
                    }
                }
            }
            DockPanel {
                id: inspectorPanel
                controller: layout; panelId: "inspector"; title: qsTr("Audio Inspector")
                C.SplitView.fillWidth: true
                C.SplitView.minimumWidth: Math.max(248, Theme.wInspectorAudio)
                AudioInspector {
                    anchors.fill: parent
                    id: inspector
                    objectName: "audioInspector"
                    stripName: root.selectedName
                    mix: root.selectedMix
                    catalogue: root.mixEffects
                    levels: root.selectedTrack >= 0 && root.trackLevels[root.selectedTrack] !== undefined ? root.trackLevels[root.selectedTrack] : root.routingLevels.find(function(m){return m.id === (root.masterSelected ? "master" : root.busSelected && root.buses[-10-root.selectedTrack] ? root.buses[-10-root.selectedTrack].id : "")}) || ({})
                    spectrum: root.spectrum
                    loudness: root.loudness
                    hasStrip: root.hasSelection
                    editable: root.canEditSelection
                    audioServices: root.audioServices
                    isMaster: root.masterSelected
                    before: root.before
                    onEditingStarted: root.startEffectEdit()
                    onEditingFinished: root.finishEffectEdit()
                    onMixEdited: function (k, v) { root.editSelectedMix(k, v) }
                    onBeforeToggled: function (on) { root.setBefore(on) }
                }
            }
        }
        DockPanel {
            id: timelinePanel
            controller: layout; panelId: "timeline"; title: qsTr("Audio Timeline")
            C.SplitView.preferredHeight: rows.height * rows.timelineSplit
            C.SplitView.minimumHeight: 160
            onHeightChanged: if (C.SplitView.view && C.SplitView.view.resizing) rows.timelineSplit = height / rows.height
            Item {
                anchors.fill: parent
                id: timelineHost
                AudioToolbar {
                    id: toolbar
                    objectName: "audioToolbar"
                    width: parent.width
                    tool: root.tool
                    snapping: root.snapping
                    units: root.units
                    cursorText: root.cursorText
                    onCursorTextEdited: function(text) { root.cursorTextEdited(text) }
                    onSampleStepRequested: function(samples) { root.sampleStepRequested(samples) }
                    recording: root.recording
                    canRecord: root.recorderAvailable && root.hasVideo && root.recordTrackCandidate >= 0
                    loop: root.loop
                    fps: root.fps
                    sampleRate: root.sampleRate
                    zoom: timeline.fpx
                    canZoom: root.duration > 0
                    onMixToolsRequested: mixTools.open()
                    automationVisible: root.showAutomation
                    onToolSelected: function (t) { root.tool = t }
                    onSnappingToggled: function (on) { root.snappingToggled(on) }
                    onUnitsEdited: function (u) { root.units = u }
                    onAddTrackRequested: root.addTrackRequested(root.selectedTrackEntry !== null && root.selectedTrackEntry.mix !== undefined && root.selectedTrackEntry.mix.role !== undefined ? root.selectedTrackEntry.mix.role : "dialogue")
                    onRecordToggled: root.recordToggled()
                    onLoopToggled: function (on) { root.loopToggled(on) }
                    onZoomEdited: function (z) { root.zoom = Math.max(0.005, Math.min(40000, z)) }
                    onFitRequested: root.zoom = 0
                    onTallerRequested: { root.rowHeight = Math.min(Theme.hAudioTrackTall * 2, root.rowHeight + 12); root.forgetOwnHeights() }
                    onShorterRequested: { root.rowHeight = Math.max(Theme.hAudioGroupHeader + 8, root.rowHeight - 12); root.forgetOwnHeights() }
                    onAutomationToggled: function (on) { root.showAutomation = on }
                    onMarkerRequested: root.markerRequested(root.position)
                }
                AudioTimeline {
                    id: timeline
                    objectName: "audioTimeline"
                    wheelAction: root.wheelAction
                    wheelInvert: root.wheelInvert
                    wheelSensitivity: root.wheelSensitivity
                    embeddedAudioCount: root.embeddedAudioCount
                    timecoder: root.timecoder
                    anchors.top: toolbar.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    tracks: root.tracks
                    gestureTarget: root.gestureTarget
                    gesturePreview: root.gesturePreview
                    snapPoints: root.snapPoints
                    beatLines: root.beatLines
                    rectifiedWaveforms: root.rectifiedWaveforms
                    gesturePending: root.gesturePending
                    gestureError: root.gestureError
                    onGestureCommitted: function(target, action, changes, rate) { root.gestureCommitted(target, action, changes, rate) }
                    onTimeSeekRequested: function(position, audition) { root.timeSeekRequested(position, audition) }
                    renderRanges: root.backgroundRender.ranges || []
                    renderedRanges: root.backgroundRender.readyRanges || []
                    clips: root.clips
                    units: root.units
                    fps: root.fps
                    duration: root.duration
                    position: root.position
                    cursorFrames: root.cursorFrames
                    sampleRate: root.sampleRate
                    markers: root.markers
                    rangeIn: root.rangeIn
                    rangeOut: root.rangeOut
                    trackLevels: root.trackLevels
                    master: root.master
                    masterLeft: root.masterLeft
                    masterRight: root.masterRight
                    masterMuted: root.master.muted === true
                    selectedTrack: root.selectedTrack
                    selectedClip: root.selectedClip
                    tool: root.tool
                    snapping: root.snapping
                    zoom: root.zoom
                    rowHeight: root.rowHeight
                    showAutomation: root.showAutomation
                    recordTrack: root.recordTrack
                    recording: root.recording
                    recordLevel: root.recordLevel
                    playing: root.playing
                    onDecorationWindowChanged: function(first, last, pixels) { root.decorationWindowChanged(first, last, pixels) }
                    onTrackSelected: function (t) { root.selectedTrack = t }
                    onClipSelected: function (i) { root.selectClip(i) }
                    onTrackFlagEdited: function (t, f, on) { root.trackFlagEdited(t, f, on) }
                    onMixStarted: function(t, k) { root.mixStarted(t, k) }
                    onMixFinished: function(t, k) { root.mixFinished(t, k) }
                    onTrackMixEdited: function (t, k, v) { root.trackMixEdited(t, k, v) }
                    onTrackRenamed: function (t, n) { root.trackRenamed(t, n) }
                    onAddTrackRequested: function (role) { root.addTrackRequested(role) }
                    onRemoveTrackRequested: function (t) { root.removeTrackRequested(t) }
                    onMoveTrackRequested: function (t, up) { root.moveTrackRequested(t, up) }
                    onMasterVolumeEdited: function (db) { root.masterEdited("volume", db); root.mixFinished(-2, "volume") }
                    onMasterMuteToggled: function (on) { root.masterEdited("muted", on) }
                    onClipMoveRequested: function (i, t, s) { root.clipMoveRequested(i, t, s) }
                    onClipTrimRequested: function (i, a, b, s) { root.clipTrimRequested(i, a, b, s) }
                    onClipRemoveRequested: function (i, r) { root.clipRemoveRequested(i, r) }
                    onSplitRequested: function (t, f) { root.splitRequested(t, f) }
                    onClipFadesEdited: function (i, a, b) { root.clipFadesEdited(i, a, b) }
                    onClipVolumeEdited: function (i, db) { root.clipVolumeEdited(i, db) }
                    onClipVolumeKfEdited: function (i, kf) { root.clipVolumeKfEdited(i, kf) }
                    onClipFxRequested: function (i) { root.selectClip(i); root.mixerTab = 1 }
                    onSeekRequested: function (f) { root.seekRequested(f) }
                    onScrubRequested: function (f) { root.scrubRequested(f) }
                    onSampleSeekRequested: function(ticks, audition) { root.sampleSeekRequested(ticks, audition) }
                    onRangeEdited: function (a, b) { root.rangeEdited(a, b) }
                    onRangeCleared: root.rangeCleared()
                    onZoomEdited: function (z) { root.zoom = Math.max(0.005, Math.min(40000, z)) }
                    onSoundDropped: function (p, t, f) { root.soundDropped(p, t, f) }
                    onMarkerRequested: function (f) { root.markerRequested(f) }
                    onContextRequested: function (t, f) { laneMenu.track = t; laneMenu.frame = f; laneMenu.popup() }
                }
            }
        }
    }

    // The armed track (first armed audio track), for the toolbar's record.
    readonly property int recordTrackCandidate: {
        for (var i = 0; i < tracks.length; ++i)
            if (tracks[i].kind === "audio" && tracks[i].mix !== undefined && tracks[i].mix.armed === true) return tracks[i].index
        return -1
    }

    ContextMenu {
        id: laneMenu
        property int track: -1
        property int frame: 0
        MenuAction { text: qsTr("Add Marker Here"); iconName: "flag"; onTriggered: root.markerRequested(laneMenu.frame) }
        MenuAction { text: qsTr("Split at Playhead"); iconName: "scissors"; enabled: laneMenu.track >= 0; onTriggered: root.splitRequested(laneMenu.track, root.position) }
        MenuAction { text: qsTr("Delete Clip"); iconName: "trash-2"; enabled: root.selectedClip >= 0; onTriggered: root.clipRemoveRequested(root.selectedClip, false) }
        MenuAction { text: qsTr("Ripple Delete Clip"); enabled: root.selectedClip >= 0; onTriggered: root.clipRemoveRequested(root.selectedClip, true) }
        C.MenuSeparator {}
        C.Menu {
            title: qsTr("Add Track")
            Repeater {
                model: Theme.roleNames
                delegate: MenuAction { required property string modelData; text: Theme.roleTitle(modelData); onTriggered: root.addTrackRequested(modelData) }
            }
        }
    }
    MixTools {
        id: mixTools
        stripName: root.selectedName
        mix: root.selectedMix
        buses: root.buses
        duration: root.duration
        position: root.position
        fps: root.fps
        isMaster: root.masterSelected
        liveAutomation: root.selectedTrack >= 0
        canEdit: root.hasSelection && (root.selectedTrackEntry === null || root.selectedTrackEntry.locked !== true)
        selectedClip: root.selectedClip >= 0 && root.selectedClip < root.clips.length ? root.clips[root.selectedClip] : null
        onMixEdited: function (key,value) { root.editSelectedMix(key,value) }
        onBusesEdited: function (buses) { root.masterEdited("buses",buses) }
        onDuplicateRequested: root.duplicateClipRequested()
        onTimingRequested: { mixTools.close(); root.clipTimingRequested(root.selectedClip) }
        onSpeedRequested: function (speed) { root.clipSpeedRequested(speed) }
        onFadesRequested: function (a,b) { root.clipFadesEdited(root.selectedClip,a,b) }
        onFadeCurveRequested: function (curve) { root.clipFadeCurveRequested(curve) }
    }

}
