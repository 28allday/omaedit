pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Color

// The Color page. Pure: the clip under the playhead (with its grade), the
// clip strip, the stills, LUTs and scope tick come in; every edit leaves
// as a signal ColorPage.qml maps to Backend. Layout follows the render:
// gallery | viewer over nodes | scopes over keyframes, then the clip strip
// and the grading tabs across the bottom; every boundary a SplitView
// handle, sizes persisted.
Item {
    id: root
    property var layoutStore: null
    property alias layoutController: layout
    WorkspaceLayout { id: layout; host: root; store: root.layoutStore; workspaceKey: "Color"; sizesVersion: 3 }

    // ── Inputs ──────────────────────────────────────────────────────────────
    property var gradeClip: ({})          // {track, index, name, start, frames, grade, gradeVersions, gradeVersion, bypass}
    property var clips: []                // backend.colorClips
    property var stills: []
    property var albums: []
    property bool maskProviderOffered: false
    property var maskProvider: ({})
    property var colourConfig: ({})
    property var maskJob: ({})
    signal maskConfigured(var settings)
    signal maskRequested(string kind,string prompt)
    signal colourSourceApplied(string scope, var fields)
    signal maskCancelled()
    property var colorGroups: []
    signal shareRequested(string action,string name,var addresses)
    property var nodePresets: []
    signal saveNodePresetRequested(string name)
    signal applyNodePresetRequested(string id)
    signal removeNodePresetRequested(string id)
    property var luts: []
    property var lutPreview: ({})
    property int scopeTick: 0
    property var trackJob: ({})
    property bool hasVideo: false
    property real fps: 25
    property int duration: 0
    property int position: 0
    property bool playing: false
    property string sequenceName: ""
    property int sequenceIndex: 0
    property string colorSpace: "Rec.709"
    property real imageAspect: 16 / 9
    property Component videoSurface: null
    property Component beforeSurface: null
    property Component referenceSurface: null
    property var timecoder: null
    property string comparison: "none"    // none | wipe | split | still (owned by the page: wipe needs the engine)
    property bool highlight: false
    property bool versionsEditable: false
    property bool hasCopiedNode: false
    property bool hasCopiedGrade: false

    // ── Outputs ─────────────────────────────────────────────────────────────
    signal gradeEdited(var grade)         // the whole grade, live (coalesced by the page)
    signal gradeCommitted(var grade)      // a drag ended — one undo step
    signal gradeGestureEnded()            // a drag ended with nothing left to send
    signal versionsEdited(var versions, int active)
    signal versionActionRequested(string action, int version, string name)
    signal copyColorRequested(string kind)
    signal pasteColorRequested(string kind)
    signal autoBalanceRequested()
    signal bypassToggled(bool on)
    signal comparisonEdited(string mode)
    signal highlightToggled(bool on)
    signal decorationWindowChanged(real first, real last, int pixels)
    signal seekRequested(int frame)
    signal playToggled()
    signal shuttled(int direction)
    signal stepRequested(int frames)
    signal grabRequested(string name, string album)
    signal stillApplied(string id, var addrs)
    signal stillRenamed(string id, string name)
    signal stillMoved(string id, string album)
    signal stillFavorited(string id, bool on)
    signal stillRemoved(string id)
    signal trackRequested(int node, int window, int frame, int direction, bool singleFrame)
    signal trackStopped(bool keep)
    signal trackMotionCopied(var points)
    signal colorAssetRequested(string action, string id)
    signal nodePresetRequested(string action)
    signal lutFolderRequested()
    signal lutFileRequested()
    signal lutRefreshRequested()
    signal lutFavoriteToggled(string path, bool on)
    signal lutPreviewRequested(string path, real intensity)
    signal lutApplyRequested(string path, real intensity)
    signal lutPreviewClosed()
    signal colorPickRequested(real fx, real fy, int modifiers)
    signal scopesActiveEdited(bool on)
    signal scopeOptionsEdited(var options)

    // ── View state ──────────────────────────────────────────────────────────
    property string viewChannel: "rgb"
    property string displayJson: '{"enabled":true,"engine":"ocio","view":"Standard","gamut":"srgb","gamma":"srgb"}'
    readonly property var displayOptions: { try { return JSON.parse(displayJson) } catch(e) { return ({ enabled: true, gamut: "srgb", gamma: "srgb" }) } }
    readonly property bool managedOutput: !!grade.management && grade.management.enabled && grade.bypass!==true && gradeClip.bypass!==true
    readonly property var referenceGrade: {var still=stills.filter(function(s){return s.id===root.referenceStillId})[0];return still?still.grade || ({}):({})}
    readonly property var referenceColorOptions: Object.assign({}, displayOptions, { from: referenceGrade.management && referenceGrade.management.enabled && !referenceGrade.bypass ? referenceGrade.management.output : "rec709", fromGamma: referenceGrade.management && referenceGrade.management.enabled && !referenceGrade.bypass ? referenceGrade.management.outputGamma : "rec709", channel: viewChannel })
    readonly property var videoColorOptions: Object.assign({}, displayOptions, { from: managedOutput ? grade.management.output : grade.management && grade.management.enabled ? grade.management.input : "rec709", fromGamma: managedOutput ? grade.management.outputGamma : grade.management && grade.management.enabled ? grade.management.inputGamma : "rec709", channel: viewChannel })
    property var editPath: []
    readonly property var graph: {
        var g = grade
        for(var i=0;i<editPath.length;++i) {
            var found=(g.nodes || []).filter(function(n){return n.id===root.editPath[i]})[0]
            if(!found)return ({nodes:[]})
            g={nodes:found.children || [],outputNode:found.childOutput || ""}
        }
        return g
    }
    readonly property string nodePath: editPath.concat([node.id || ""]).join("/")
    readonly property int topNodeIndex: editPath.length ? (grade.nodes || []).findIndex(function(n){return n.id===root.editPath[0]}) : nodeIndex
    readonly property var beforeColorOptions: Object.assign({}, displayOptions, { from: grade.management && grade.management.enabled ? grade.management.input : "rec709", fromGamma: grade.management && grade.management.enabled ? grade.management.inputGamma : "rec709", channel: viewChannel })
    property int currentWindow: 0
    property int currentNode: 0
    property string referenceStillId: ""
    property var selection: []            // clip strip Ctrl-selection [{track, index}]
    property bool picking: false
    property string pickMode: "set"
    property bool showOverlay: true
    property alias scopesDetached: scopesPanel.detached
    property string scopeLayout: "dual"
    property var scopeKinds: ["parade", "waveform", "vectorscope", "histogram"]
    property string scopeScale: "ire"
    property string scopeChannels: "rgb"
    property real scopeIntensity: 1.0
    property bool scopeGraticule: true
    property bool scopeClamp: false
    property bool scopesPaused: false
    property string scopeBitDepth: "Float"
    property int gradeTab: 0
    // Shut to begin with: under the primaries it costs them half the panel.
    // Beside them, on a wide window, it always shows.
    property bool qualifierOpen: false
    readonly property Item focusItem: Window.activeFocusItem
    readonly property bool textActive: gallery.textActive || nodeGraph.textActive || viewer.textActive || lutBrowser.visible || managementDialog.visible || routingDialog.visible || presetsDialog.visible || sharingDialog.visible || (maskDialog !== null && maskDialog.visible)
                                       || (focusItem !== null && focusItem !== undefined && focusItem.cursorPosition !== undefined)

    // Light colour work: layers of primaries, curves and a mask each, LUTs,
    // scopes and copying a grade. Heavier grading belongs in a grading
    // application, reached by File ▸ Export Timeline. The tools for it (stills,
    // versions, colour keyframes, HDR zones, the hue mesh, tracking, the AI
    // mask, shared grades) stay in the engine, so grades that use them render
    // as they did, but the page does not offer them.
    readonly property bool lean: !colourSettings.fullGrading
    onLeanChanged: if (lean && gradeTab > 2) gradeTab = 0
    Settings {
        id: colourSettings
        category: "color"
        property bool fullGrading: false
        property alias displayJson: root.displayJson
        property alias scopeLayout: root.scopeLayout
        property alias scopeScale: root.scopeScale
        property alias scopeChannels: root.scopeChannels
        property alias scopeIntensity: root.scopeIntensity
        property alias scopeGraticule: root.scopeGraticule
        property alias scopeClamp: root.scopeClamp
        // The previous bit-depth control had no effect. Start measurement
        // precision at Float instead of inheriting its saved 8-bit default.
        property alias scopePrecision: root.scopeBitDepth
        // Renamed when the qualifier joined the primaries: the old numbers meant other tabs.
        property alias gradeTool: root.gradeTab
        property alias qualifierShown: root.qualifierOpen // was qualifierOpen, when it started open
        property alias showOverlay: root.showOverlay
    }

    readonly property bool hasClip: gradeClip.track !== undefined && gradeClip.track >= 0
    // The grade being edited: the draft this page last emitted until the
    // engine answers with the clip's stored grade (a commit must send the
    // draft, never the stale engine copy; the controls must not snap back
    // between the coalesced flush and the round trip).
    property var draft: null
    readonly property var grade: draft !== null ? draft
                               : hasClip && gradeClip.grade !== undefined && gradeClip.grade.nodes !== undefined ? gradeClip.grade
                               : ({ version: 1, nodes: [], input: "auto", output: "rec709", bypass: false })
    function emitWholeGrade(g, commit) { draft=g;root.gradeEdited(g);if(commit)root.gradeCommitted(g) }
    function emitGrade(g, commit) {
        if(!versionsEditable)return
        if(!editPath.length) { emitWholeGrade(g,commit);return }
        var whole=JSON.parse(JSON.stringify(grade)), list=whole.nodes, parent
        for(var i=0;i<editPath.length;++i) { parent=list.filter(function(n){return n.id===root.editPath[i]})[0];if(!parent)return;list=parent.children }
        parent.children=g.nodes;parent.childOutput=g.outputNode || ""
        emitWholeGrade(whole,commit)
    }
    function enterCompound() { if(node.kind!=="compound")return;editPath=editPath.concat([node.id]);currentNode=0 }
    function leaveCompound() { if(!editPath.length)return;var id=editPath[editPath.length-1];editPath=editPath.slice(0,-1);currentNode=Math.max(0,nodes.findIndex(function(n){return n.id===id})) }
    // The end of a drag. The engine may already have answered its last
    // value (the draft is then gone and there is nothing to send again), but
    // the drag still ends: whoever groups its moves into one undo step must
    // be told, or the next drag joins this one.
    function commitDraft() { if (draft !== null) root.gradeCommitted(draft); else root.gradeGestureEnded() }
    function newGradeVersion() { viewer.newVersion() }
    function chooseColorAsset(action,path) { if(action==="matte" && routingDialog.visible) routingDialog.chooseMatte(path) }
    function browseLuts() { if (versionsEditable) lutBrowser.openForNode() }
    function chooseLut(path) { if (lutBrowser.visible) lutBrowser.select(path) }
    function finishLutApply(changed, message) { if (lutBrowser.visible) lutBrowser.finishApply(changed, message) }
    function stepGradeVersion(delta) { viewer.stepVersion(delta) }
    readonly property var nodes: graph.nodes || []
    readonly property int nodeIndex: Math.max(0, Math.min(currentNode, nodes.length - 1))
    readonly property var node: nodes.length > 0 ? withFrames(evaluateNode(nodes[nodeIndex])) : ({})
    readonly property var activeWindow: currentWindow > 0 && node.windows && node.windows[currentWindow - 1] ? node.windows[currentWindow - 1] : node.window || ({})
    readonly property var displayedWindow: sampleWindow(activeWindow)
    readonly property int clipFrame: hasClip ? Math.max(0, position - gradeClip.start) : 0
    readonly property string clipThumb: {
        for (var i = 0; i < clips.length; ++i)
            if (clips[i].track === gradeClip.track && clips[i].index === gradeClip.index) return clips[i].thumb !== undefined ? clips[i].thumb : ""
        return ""
    }
    function sampleWindow(raw) {
        if(!raw.track || !raw.track.length)return raw
        var w=Object.assign({},raw), points=raw.track.slice().sort(function(a,b){return a.frame-b.frame})
        for(var k of ["x","y","w","h","rotation"]) {
            var a,b
            for(var p of points){if(p[k]===undefined)continue;if(p.frame<=clipFrame)a=p;if(p.frame>=clipFrame){b=p;break}}
            if(!a)a=b;if(!b)b=a;if(!a)continue
            var t=a.frame===b.frame?0:(clipFrame-a.frame)/(b.frame-a.frame);w[k]=a[k]+(b[k]-a[k])*t
        }
        return w
    }
    function putWindow(w, index) {
        if (!hasClip) return
        var n = Object.assign({}, node), previous = index > 0 ? (n.windows || [])[index - 1] : n.window
        var updated = Object.assign({}, w)
        if (w.track && w.track.length && previous && previous.track && previous.track.length) {
            var shown = sampleWindow(previous), defaults = {x: .5, y: .5, w: .4, h: .4, rotation: 0}
            // Manual geometry edits offset the entire tracked path. Reset
            // removes the path, so it deliberately does not enter this branch.
            updated.track = previous.track.map(function(p) {
                var c = Object.assign({}, p)
                for (var key in defaults) {
                    if (c[key] === undefined || w[key] === undefined) continue
                    var before = shown[key] !== undefined ? shown[key] : defaults[key]
                    c[key] += w[key] - before
                }
                return c
            })
        }
        if (index > 0) { n.windows = (n.windows || []).slice(); n.windows[index - 1] = updated }
        else n.window = updated
        putNode(nodeIndex, n)
    }
    function parameterValue(n, key) {
        var path = key.split("."), v = ["window", "qualifier", "hdr", "lut", "opacity", "keyGain"].indexOf(path[0]) >= 0 ? n : n.primaries || {}
        for (var i = 0; i < path.length; ++i) {
            // QVariantList values exposed by Qt are array-like sequences.
            if (Array.isArray(v) || (v && typeof v !== "string" && typeof v.length === "number")) v = v["yrgb".indexOf(path[i])]
            else v = v ? v[path[i]] : undefined
        }
        return v !== undefined ? v : 0
    }
    function evaluateNode(raw) {
        if (!raw) return ({})
        if (!raw.keyframes || Object.keys(raw.keyframes).length === 0) return raw
        var n = JSON.parse(JSON.stringify(raw))
        for (var key in n.keyframes) {
            var list = n.keyframes[key].slice().sort(function(a,b) { return a.frame-b.frame })
            if (!list.length) continue
            var a = list[0], b = list[list.length-1]
            for (var j=0; j<list.length; ++j) { if (list[j].frame <= clipFrame) a=list[j]; if (list[j].frame >= clipFrame) { b=list[j]; break } }
            var t = b.frame === a.frame || a.interpolation === "hold" ? 0 : Math.max(0,Math.min(1,(clipFrame-a.frame)/(b.frame-a.frame)))
            if (a.interpolation === "smooth") t=t*t*(3-2*t)
            var val=a.value+(b.value-a.value)*t, path=key.split(".")
            if (["window", "qualifier", "hdr", "lut", "opacity", "keyGain"].indexOf(path[0]) < 0) path.unshift("primaries")
            // Untouched HDR zones are sparse; a keyed wheel still needs its
            // four channels, rather than a map with a property named "y".
            if (path[0] === "hdr" && path.length === 3) {
                if (!n.hdr) n.hdr = {}
                if (!Array.isArray(n.hdr[path[1]])) n.hdr[path[1]] = [0, 0, 0, 0]
            }
            var target=n
            for (var k=0;k<path.length-1;++k) { if (!target[path[k]]) target[path[k]]={}; target=target[path[k]] }
            target[Array.isArray(target) ? "yrgb".indexOf(path[path.length-1]) : path[path.length-1]]=val
        }
        return n
    }
    function withFrames(n) { var c = {}; for (var k in n) c[k] = n[k]; c.frames = gradeClip.frames; return c }
    function copyGrade() {
        var g = {}
        for (var k in graph) g[k] = graph[k]
        g.nodes = nodes.map(function (n) { var c = {}; for (var j in n) c[j] = n[j]; return c })
        return g
    }
    function keyAtPlayhead(list, value) {
        var result = {frame: clipFrame, value: value}, previous = null
        for (var key of list)
            if (key.frame <= clipFrame && (!previous || key.frame > previous.frame)) previous = key
        if (previous && previous.interpolation) result.interpolation = previous.interpolation
        return result
    }
    function putNode(i, n) {
        var g = copyGrade()
        var c = {}
        for (var k in n) if (k !== "frames") c[k] = n[k]
        // The slot keeps its identity: an edit never renames a node, so two
        // slots can never end up sharing an id (the engine would refuse the grade).
        if (g.nodes[i] && g.nodes[i].id !== undefined) c.id = g.nodes[i].id
        if (c.keyframes) {
            // A keyed parameter animates: an edit lands on the key under the
            // playhead, or becomes a new key there — otherwise the value would
            // snap back to the interpolated one and the edit be lost.
            c.keyframes = JSON.parse(JSON.stringify(c.keyframes))
            for (var key in c.keyframes) {
                var list = c.keyframes[key], found = false
                var value = root.parameterValue(c, key)
                for (var j=0; j<list.length; ++j)
                    if (list[j].frame === root.clipFrame) { list[j].value = value; found = true }
                if (!found && list.length > 0 && i === nodeIndex && value !== root.parameterValue(root.node, key)) {
                    list.push(root.keyAtPlayhead(list, value))
                    list.sort(function (a, b) { return a.frame - b.frame })
                }
            }
        }
        g.nodes[i] = c
        root.emitGrade(g, false)
    }
    function putNodes(list, commit) { // `commit` false: part of a drag still under way
        var g = copyGrade()
        g.nodes = list
        root.emitGrade(g, commit !== false)
    }
    // Toggle a keyframe at the playhead for one animatable parameter of the
    // current node: the key's value is the parameter's value now.
    function toggleKey(key) {
        if (!hasClip) return
        var n = {}
        for (var k in node) if (k !== "frames") n[k] = node[k]
        var kf = {}
        for (var j in (n.keyframes || {})) kf[j] = (n.keyframes[j] || []).slice()
        var list = (kf[key] || []).filter(function (e) { return e.frame !== root.clipFrame })
        if (list.length === (kf[key] || []).length) { // Preserve the value currently shown at the playhead.
            var value = root.parameterValue(n, key)
            list.push(root.keyAtPlayhead(list, value))
            list.sort(function (a, b) { return a.frame - b.frame })
        }
        if (list.length === 0) delete kf[key]; else kf[key] = list
        n.keyframes = kf
        var g = copyGrade(); g.nodes[nodeIndex] = n
        root.emitGrade(g, true)
    }
    function clearKeys(key) {
        var n = {}
        for (var k in node) if (k !== "frames") n[k] = node[k]
        var kf = {}
        for (var j in (n.keyframes || {})) if (j !== key) kf[j] = n.keyframes[j]
        n.keyframes = kf
        var g = copyGrade(); g.nodes[nodeIndex] = n
        root.emitGrade(g, true)
    }
    function neighbourClip(delta) {
        for (var i = 0; i < clips.length; ++i)
            if (clips[i].track === gradeClip.track && clips[i].index === gradeClip.index) {
                var j = i + delta
                if (j >= 0 && j < clips.length) root.seekRequested(clips[j].start)
                return
            }
        if (clips.length > 0) root.seekRequested(clips[0].start)
    }
    function nodeClone(label) {
        var base = nodes.length > 0 ? nodes[0] : null
        // A fresh node from the first node's shape, everything at rest.
        var n = { id: "n" + Date.now().toString(36) + Math.random().toString(36).slice(2,12), label: label, kind: "serial", enabled: true, maskModel: "component",
                  primaries: { lift: [0, 0, 0, 0], gamma: [0, 0, 0, 0], gain: [1, 0, 0, 0], offset: [0, 0, 0, 0], temp: 6500, tint: 0, exposure: 0, contrast: 1, pivot: 0.435, saturation: 1, hue: 0, highlights: 0, shadows: 0, whites: 0, blacks: 0, midDetail: 0 },
                  curves: { master: [[0, 0], [1, 1]], red: [[0, 0], [1, 1]], green: [[0, 0], [1, 1]], blue: [[0, 0], [1, 1]], hueHue: [[0, 0.5], [1, 0.5]], hueSat: [[0, 0.5], [1, 0.5]], hueLum: [[0, 0.5], [1, 0.5]], lumaLuma: [[0, 0], [1, 1]], satSat: [[0, 0], [1, 1]] },
                  qualifier: { enabled: false, color: "#ff0000", deltaH: 0.2, deltaS: 0.3, deltaL: 0.3, softness: 0.1, invert: false, subspace: "hsl" },
                  window: { enabled: false, shape: "circle", x: 0.5, y: 0.5, w: 0.4, h: 0.4, rotation: 0, softness: 0.1, invert: false, track: [] },
                  lut: { path: "", intensity: 1 }, keyframes: {} }
        return n
    }
    // A layer back to how a new one starts: its corrections, curves, masks,
    // LUT and keyframes go. What makes it that layer stays: its name, its
    // place and connections, and whether it is shown. One undo step.
    function resetNode(i) {
        if (!versionsEditable || !hasClip || i < 0 || i >= nodes.length) return
        var old = nodes[i], fresh = nodeClone(old.label !== undefined ? old.label : "")
        for (var key of ["id", "label", "kind", "enabled", "inputs", "keyInput", "externalMatte", "channel", "children", "childOutput"])
            if (old[key] !== undefined) fresh[key] = old[key]
        var list = nodes.slice()
        list[i] = fresh
        putNodes(list)
    }
    onCurrentNodeChanged: currentWindow = 0
    readonly property string editingClip: sequenceIndex + ":" + (gradeClip.track || 0) + ":" + (gradeClip.index || 0) + ":" + (gradeClip.path || "")
    onEditingClipChanged: { editPath=[];currentNode=0 }
    onGradeClipChanged: { draft = null; const n = (nodes || []).length; if (currentNode >= n) currentNode = Math.max(0, n - 1) }



    // The external face / object mask provider's dialog, made only while
    // that provider is offered (Backend::kMaskProviderOffered).
    Component {
        id: maskDialogComponent
        MaskProvider {
            provider:root.maskProvider;job:root.maskJob;editable:root.versionsEditable
            onConfigureRequested:function(settings){root.maskConfigured(settings)}
            onGenerateRequested:function(kind,prompt){root.maskRequested(kind,prompt)}
            onCancelRequested:root.maskCancelled()
        }
    }
    readonly property var maskDialog: maskProviderOffered ? maskDialogComponent.createObject(root) : null
    GradeSharing {
        id: sharingDialog
        groups:root.colorGroups;link:root.grade.sharing || ({});editable:root.versionsEditable
        selectedCount: Math.max(1,root.selection.length)
        onShareRequested:function(action,name){root.shareRequested(action,name,root.selection)}
    }
    NodePresets {
        id: presetsDialog
        presets: root.nodePresets;editable:root.versionsEditable;hasNode:root.nodes.length>0
        onSaveRequested:function(name){root.saveNodePresetRequested(name)}
        onApplyRequested:function(id){root.applyNodePresetRequested(id)}
        onRemoveRequested:function(id){root.removeNodePresetRequested(id)}
        onAssetRequested:function(action,id){root.colorAssetRequested(action,id)}
    }
    NodeRouting {
        id: routingDialog
        targetKey:root.editingClip+":"+root.nodePath+":"+root.gradeClip.gradeVersion
        nodes: root.nodes; current: root.nodeIndex; editable: root.versionsEditable
        onNodeApplied: function(node) { var list=root.nodes.slice();list[root.nodeIndex]=node;root.putNodes(list) }
        onLocateMatteRequested: root.colorAssetRequested("matte", "")
    }
    ColorManagement {
        id: managementDialog
        grade: root.grade; display: root.displayOptions; editable: root.versionsEditable; colourConfig: root.colourConfig
        onManagementApplied: function (management) { var g = JSON.parse(JSON.stringify(root.grade)); g.management = management; root.emitWholeGrade(g, true) }
        onDisplayEdited: function (options) { root.displayJson = JSON.stringify(options) }
        onSourceAppliedTo: function (scope, fields) { root.colourSourceApplied(scope, fields) }
    }

    LutBrowser {
        id: lutBrowser
        luts: root.luts
        lut: root.node.lut || ({})
        preview: root.lutPreview
        editable: root.versionsEditable
        targetKey: root.sequenceIndex + ":" + root.gradeClip.path + ":" + root.gradeClip.track + ":" + root.gradeClip.index
                   + ":" + root.nodePath + ":" + root.gradeClip.gradeVersion
        clipName: root.gradeClip.name || ""
        nodeName: root.node.label || qsTr("Correction layer")
        onPreviewRequested: function (path, intensity) { root.lutPreviewRequested(path, intensity) }
        onApplyRequested: function (path, intensity) { root.lutApplyRequested(path, intensity) }
        onFavoriteToggled: function (path, on) { root.lutFavoriteToggled(path, on) }
        onFolderRequested: root.lutFolderRequested()
        onFileRequested: root.lutFileRequested()
        onRescanRequested: root.lutRefreshRequested()
        onPreviewClosed: root.lutPreviewClosed()
    }

    WorkspaceSplit {
            controller: layout; splitId: "split1"
        id: rows
        anchors.fill: parent
        orientation: Qt.Vertical

        readonly property bool compact: root.height < 800
        property real bottomSplit: 0.38

        WorkspaceSplit {
                controller: layout; splitId: "split2"
            visible: galleryPanel.docked || viewerPanel.docked || nodesPanel.docked || scopesPanel.docked || keyframesPanel.docked
            id: columns
            C.SplitView.fillHeight: true
            C.SplitView.minimumHeight: rows.compact ? Math.min(340 + (Theme.densityScale - 1) * 160, rows.height * 0.60) : 360
            orientation: Qt.Horizontal

            property real gallerySplit: root.width < 1440 ? 0.18 : 0.16
            property real scopesSplit: 0.24
            DockPanel {
                id: galleryPanel
                controller: layout; panelId: "gallery"; title: qsTr("Gallery")
                offered: !root.lean
                C.SplitView.preferredWidth: columns.width * columns.gallerySplit
                C.SplitView.minimumWidth: 220
                onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) columns.gallerySplit = width / columns.width
                StillsGallery {
                    anchors.fill: parent
                    id: gallery
                    dockBar: galleryPanel.barSlot
                    objectName: "stillsGallery"
                    stills: root.stills
                    albums: root.albums
                    timecoder: root.timecoder
                    onStillActivated: function (id) { root.referenceStillId = id }
                    onApplyRequested: function (id, toAll) {
                        var addrs = toAll ? root.clips.map(function (c) { return { track: c.track, index: c.index } })
                                          : root.selection.length > 0 ? root.selection
                                          : root.hasClip ? [{ track: root.gradeClip.track, index: root.gradeClip.index }] : []
                        root.stillApplied(id, addrs)
                    }
                    onAssetRequested: function(action,id) { root.colorAssetRequested(action,id) }
                onGrabRequested: root.grabRequested("", gallery.album)
                    onRenameRequested: function (id, n) { root.stillRenamed(id, n) }
                    onAlbumRequested: function (id, a) { root.stillMoved(id, a) }
                    onFavoriteEdited: function (id, on) { root.stillFavorited(id, on) }
                    onRemoveRequested: function (id) { root.stillRemoved(id) }
                    onWipeRequested: function (id) { root.referenceStillId = id; root.comparisonEdited("still") }
                }
            }

            WorkspaceSplit {
                    controller: layout; splitId: "split3"
            visible: viewerPanel.docked || nodesPanel.docked
                id: centre
                C.SplitView.fillWidth: true
                C.SplitView.minimumWidth: 420
                // The nodes stand beside the picture, not under it: the viewer
                // keeps the whole height, which is what sets its picture's size.
                orientation: Qt.Horizontal

                property real nodesSplit: 0.26
                readonly property real minimumNodesHeight: 88 + Theme.hControl + Theme.s1 + Theme.hairline + 2 * Theme.s2
                DockPanel {
                    id: viewerPanel
                    controller: layout; panelId: "viewer"; title: qsTr("Viewer")
                    detail: root.sequenceName
                    detachable: true
                    viewer: true
                    C.SplitView.fillWidth: true
                    C.SplitView.minimumWidth: 300
                    ColorViewer {
                        anchors.fill: parent
                        id: viewer
                        dockBar: viewerPanel.barSlot
                        objectName: "colorViewer"
                        lean: root.lean
                        sequenceName: root.sequenceName
                        hasMedia: root.hasVideo
                        hasGradeClip: root.hasClip
                        gradeVersions: root.hasClip ? (root.gradeClip.gradeVersions || []) : []
                        gradeVersion: root.hasClip ? (root.gradeClip.gradeVersion || 0) : 0
                        gradeClipName: root.hasClip ? root.gradeClip.name : ""
                        gradeClipKey: root.hasClip ? root.gradeClip.track + ":" + root.gradeClip.index + ":" + root.gradeClip.path + ":" + root.gradeClip.start : ""
                        versionsEditable: root.versionsEditable && root.hasClip
                        onVersionActionRequested: function (action, version, name) { root.versionActionRequested(action, version, name) }
                        playing: root.playing
                        position: root.position
                        duration: root.duration
                        imageAspect: root.imageAspect
                        fps: root.fps
                        videoSurface: root.videoSurface
                        beforeSurface: root.beforeSurface
                        referenceSurface: root.referenceSurface
                        timecoder: root.timecoder
                        bypassAll: root.hasClip && root.gradeClip.bypass === true
                        comparison: root.comparison
                        referenceSource: root.referenceStillId !== "" ? "image://thumbs/still:" + root.referenceStillId : ""
                        colorSpace: managementDialog.sourceName(root.grade.management)
                        viewChannel: root.viewChannel
                        onViewChannelEdited: function (channel) { root.viewChannel = channel }
                        onManagementRequested: managementDialog.open()
                        window: root.displayedWindow
                        showWindow: root.versionsEditable && root.showOverlay && root.hasClip
                        highlight: root.highlight
                        picking: root.picking
                        onPlayToggled: root.playToggled()
                        onShuttled: function (d) { root.shuttled(d) }
                        onStartRequested: root.seekRequested(0)
                        onEndRequested: root.seekRequested(Math.max(0, root.duration - 1))
                        onStepRequested: function (n) { root.stepRequested(n) }
                        onGrabRequested: root.grabRequested("", gallery.album)
                        onBypassAllToggled: function (on) { root.bypassToggled(on) }
                        onComparisonEdited: function (m) { root.comparisonEdited(m) }
                        onWipeEdited: function (f) { viewer.wipe = f }
                        onWindowEdited: function (w) { root.putWindow(w, root.currentWindow) }
                        onWindowCommitted: root.commitDraft()
                        onHighlightToggled: function (on) { root.highlightToggled(on) }
                        onPreviousClipRequested: root.neighbourClip(-1)
                        onNextClipRequested: root.neighbourClip(1)
                        onColorPicked: function (fx, fy, mods) { root.colorPickRequested(fx, fy, mods) }
                    }
                }
                DockPanel {
                    id: nodesPanel
                    controller: layout; panelId: "nodes"; title: qsTr("Layers")
                    detail: nodeGraph.pathLabel
                    C.SplitView.preferredWidth: Math.max(240, Math.min(centre.width * centre.nodesSplit, 300 * Theme.densityScale))
                    C.SplitView.minimumWidth: 200
                    onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) centre.nodesSplit = width / centre.width
                    NodeGraph {
                        anchors.fill: parent
                        id: nodeGraph
                        dockBar: nodesPanel.barSlot
                        objectName: "nodeGraph"
                        lean: root.lean
                        outputNode: root.graph.outputNode || ""
                        pathLabel: root.editPath.length ? qsTr("Nested (%1)").arg(root.editPath.length) : ""
                        onLeaveRequested: root.leaveCompound()
                        onEditCompoundRequested: root.enterCompound()
                        onMaskRequested: tabs.openAiMask()
                        onSharingRequested: sharingDialog.open()
                        onSettingsRequested: routingDialog.open()
                        onOutputRequested: function(id) { var g=root.copyGrade();g.outputNode=id;root.emitGrade(g,true) }
                        onGraphEdited: function(list,output) { var g=root.copyGrade();g.nodes=list;if(output)g.outputNode=output;else delete g.outputNode;root.emitGrade(g,true) }
                        onPresetRequested: function(action) { presetsDialog.open() }
                        onCompoundRequested: {
                            var g=root.copyGrade(), n=root.nodeClone(qsTr("Nested"));n.kind="compound";n.children=g.nodes;n.childOutput=g.outputNode || "";g.nodes=[n];g.outputNode=n.id;root.currentNode=0;root.emitGrade(g,true)
                        }
                        nodes: root.nodes
                        editable: root.versionsEditable && root.hasClip
                        hasCopiedNode: root.hasCopiedNode
                        hasCopiedGrade: root.hasCopiedGrade
                        onCopyRequested: function (kind) { root.copyColorRequested(kind) }
                        onPasteRequested: function (kind) { root.pasteColorRequested(kind) }
                        current: root.nodeIndex
                        thumb: root.clipThumb
                        bypassAll: root.hasClip && root.gradeClip.bypass === true
                        onNodeSelected: function (i) { root.currentNode = i }
                        onNodesEdited: function (list) { root.putNodes(list) }
                        onNodesDragged: function (list) { root.putNodes(list, false) }
                        onDragFinished: root.commitDraft()
                        onAddRequested: function (kind) {
                            if (!root.hasClip) return
                            var n = root.nodeClone("") // named by its place in the stack until it is given a name
                            var list=root.nodes.slice(), at=root.nodes.length?root.nodeIndex:-1, selected=root.nodes[at] || ({id:"input",inputs:["input"]})
                            if(kind==="parallel" || kind==="layer") {
                                n.inputs=selected.inputs || [at>0?root.nodes[at-1].id:"input"]
                                var mixer=root.nodeClone(kind==="parallel"?qsTr("Parallel Mixer"):qsTr("Layer Mixer"));mixer.kind=kind;mixer.inputs=[selected.id,n.id]
                                list.splice(at+1,0,n,mixer)
                            } else if(kind==="splitter") {
                                var ids=[], added=[]
                                for(var c=0;c<3;++c){var split=root.nodeClone([qsTr("Red"),qsTr("Green"),qsTr("Blue")][c]);split.kind="splitter";split.channel=["red","green","blue"][c];split.inputs=[selected.id];ids.push(split.id);added.push(split)}
                                var combine=root.nodeClone(qsTr("RGB Combine"));combine.kind="combiner";combine.inputs=ids;added.push(combine)
                                list.splice.apply(list,[at+1,0].concat(added))
                            } else { n.kind=kind;if(kind==="compound")n.children=[root.nodeClone(qsTr("Correction"))];list.splice(at+1,0,n) }
                            var g=root.copyGrade();g.nodes=list
                            if(g.outputNode===selected.id)g.outputNode=list[at+(kind==="splitter"?4:(kind==="parallel"||kind==="layer")?2:1)].id
                            root.emitGrade(g,true);root.currentNode=at+1
                        }
                        onBypassAllToggled: function (on) { root.bypassToggled(on) }
                        onResetRequested: function (i) { root.resetNode(i) }
                    }
                }
            }

            WorkspaceSplit {
                    controller: layout; splitId: "split4"
            visible: scopesPanel.docked || keyframesPanel.docked
                id: rightColumn
                // Dock header, panel heading and one complete keyframe row.
                readonly property real minimumKeyframeHeight: Theme.hControl + Theme.s1 + Theme.hTab + Theme.hRow + 4 + Theme.hairline
                C.SplitView.preferredWidth: columns.width * columns.scopesSplit
                C.SplitView.minimumWidth: 240
                onWidthChanged: if (C.SplitView.view && C.SplitView.view.resizing) columns.scopesSplit = width / columns.width
                orientation: Qt.Vertical

                DockPanel {
                    id: scopesPanel
                    windowObjectName: "colorScopesWindow"
                    controller: layout; panelId: "scopes"; title: qsTr("Scopes")
                    detachable: true
                    C.SplitView.fillHeight: true
                    C.SplitView.minimumHeight: rows.compact ? Math.min(rightColumn.height * 0.68, Math.max(0, rightColumn.height - rightColumn.minimumKeyframeHeight - Theme.s1)) : 160
                    ScopeStack {
                        anchors.fill: parent
                        id: scopes
                        dockBar: scopesPanel.barSlot
                        floating: root.scopesDetached
                        objectName: "scopeStack"
                        onDetachRequested: function(detached) { scopesPanel.setDetached(detached) }
                        tick: root.scopeTick
                        layout: root.scopeLayout
                        kinds: root.scopeKinds
                        scopeScale: root.scopeScale
                        channels: root.scopeChannels
                        intensity: root.scopeIntensity
                        graticule: root.scopeGraticule
                        clamp: root.scopeClamp
                        paused: root.scopesPaused
                        bitDepth: root.scopeBitDepth
                        onLayoutEdited: function (l) { root.scopeLayout = l }
                        onKindEdited: function (slot, k) { var ks = root.scopeKinds.slice(); ks[slot] = k; root.scopeKinds = ks }
                        onScaleEdited: function (s) { root.scopeScale = s; root.pushScopeOptions() }
                        onChannelsEdited: function (c) { root.scopeChannels = c; root.pushScopeOptions() }
                        onIntensityEdited: function (v) { root.scopeIntensity = v; root.pushScopeOptions() }
                        onGraticuleToggled: function (on) { root.scopeGraticule = on; root.pushScopeOptions() }
                        onClampToggled: function (on) { root.scopeClamp = on; root.pushScopeOptions() }
                        onPausedToggled: function (on) { root.scopesPaused = on }
                        onBitDepthEdited: function (d) { root.scopeBitDepth = d; root.pushScopeOptions() }
                    }
                }
                DockPanel {
                    id: keyframesPanel
                    controller: layout; panelId: "keyframes"; title: qsTr("Keyframes")
                    offered: !root.lean
                    detail: keyframes.headTimecode
                    C.SplitView.preferredHeight: rows.compact ? Math.max(100, rightColumn.minimumKeyframeHeight) : 200
                    C.SplitView.minimumHeight: rightColumn.minimumKeyframeHeight
                    ColorKeyframes {
                        anchors.fill: parent
                        id: keyframes
                        dockBar: keyframesPanel.barSlot
                        objectName: "colorKeyframes"
                        node: root.node
                        clipFrame: root.clipFrame
                        clipStart: root.hasClip ? root.gradeClip.start : 0
                        timecoder: root.timecoder
                        onKeyToggled: function (k) { root.toggleKey(k) }
                        onKeysCleared: function (k) { root.clearKeys(k) }
                        onFrameRequested: function (f) { if (root.hasClip) root.seekRequested(root.gradeClip.start + f) }
                    }
                }
            }
        }
        DockPanel {
            id: gradingPanel
            controller: layout; panelId: "grading"; title: qsTr("Grading")
            // Its share of the page, and never less than its clips and tools
            // need to show whole while the panels above keep their own least.
            readonly property real fitHeight: Theme.hControl + Theme.s1 + Theme.hairline + strip.height + tabs.fitHeight
            C.SplitView.preferredHeight: Math.max(rows.height * rows.bottomSplit,
                                                  Math.min(fitHeight, Math.max(rows.height * 0.46, fitHeight - tabs.fitHeight + tabs.leastHeight), rows.height - columns.C.SplitView.minimumHeight - Theme.s1))
            C.SplitView.minimumHeight: rows.compact ? Math.min(306 - (Theme.densityScale - 1) * 160, rows.height * 0.48) : 260
            onHeightChanged: if (C.SplitView.view && C.SplitView.view.resizing) rows.bottomSplit = height / rows.height
            Column {
                anchors.fill: parent
                id: bottomArea
                ClipStrip {
                    id: strip
                    onDecorationWindowChanged: function(first,last,pixels) { root.decorationWindowChanged(first,last,pixels) }
                    objectName: "clipStrip"
                    height: rows.compact ? Theme.hTimeRuler + 40 : implicitHeight
                    width: parent.width
                    clips: root.clips
                    currentTrack: root.hasClip ? root.gradeClip.track : -1
                    currentIndex: root.hasClip ? root.gradeClip.index : -1
                    position: root.position
                    duration: root.duration
                    fps: root.fps
                    timecoder: root.timecoder
                    selection: root.selection
                    onClipActivated: function (t, i, start) { root.seekRequested(start) }
                    onSelectionEdited: function (s) { root.selection = s }
                }
                GradingTabs {
                    id: tabs
                    objectName: "gradingTabs"
                    lean: root.lean
                    qualifierOpen: root.qualifierOpen
                    onQualifierOpenChanged: root.qualifierOpen = qualifierOpen
                    width: parent.width
                    height: bottomArea.height - strip.height
                    enabled: root.versionsEditable
                    node: root.node
                    displayedWindow: root.displayedWindow
                    linearCurves: !!root.grade.management && root.grade.management.enabled === true
                    onWindowEdited: function(w, index) { root.putWindow(w, index) }
                    luts: root.luts
                    currentWindow: root.currentWindow
                    onCurrentWindowChanged: root.currentWindow = currentWindow
                    lutEditable: root.versionsEditable
                    onLutBrowseRequested: root.browseLuts()
                    bypass: root.hasClip && root.gradeClip.bypass === true
                    canAutoBalance: root.versionsEditable && root.gradeClip.bypass !== true && root.grade.bypass !== true
                    onAutoBalanceRequested: root.autoBalanceRequested()
                    comparison: root.comparison
                    picking: root.picking
                    highlight: root.highlight
                    showOverlay: root.showOverlay
                    trackJob: root.hasClip && root.trackJob.track === root.gradeClip.track && root.trackJob.index === root.gradeClip.index && root.trackJob.node === root.topNodeIndex ? root.trackJob : ({})
                    tab: root.gradeTab
                    onTabChanged: root.gradeTab = tab
                    onNodeEdited: function (n) { if (root.hasClip) root.putNode(root.nodeIndex, n) }
                    onCommitted: root.commitDraft()
                    onKeyToggled: function (k) { root.toggleKey(k) }
                    onLutFolderRequested: root.lutFolderRequested()
                    onBypassToggled: function (on) { root.bypassToggled(on) }
                    onComparisonEdited: function (m) { root.comparisonEdited(m) }
                    pickMode:root.pickMode
                    onPickModeEdited:function(mode){root.pickMode=mode}
                    onPickToggled: function (on) { root.picking = on }
                    onHighlightToggled: function (on) { root.highlightToggled(on) }
                    onOverlayToggled: function (on) { root.showOverlay = on }
                    onTrackRequested: function(direction,singleFrame) { if(root.hasClip) root.trackRequested(root.nodeIndex,root.currentWindow,root.clipFrame,direction,singleFrame) }
                    onTrackStopped: function(keep) { root.trackStopped(keep) }
                    onTrackMotionCopied: function(points) { root.trackMotionCopied(points) }
                    onTrackCleared: {
                        if (!root.hasClip) return
                        var n = {}; for (var k in root.node) if (k !== "frames") n[k] = root.node[k]
                        var w = Object.assign({}, root.activeWindow); w.track = []
                        if (root.currentWindow === 0) n.window = w
                        else { n.windows = n.windows.slice(); n.windows[root.currentWindow - 1] = w }
                        var g = root.copyGrade(); g.nodes[root.nodeIndex] = n
                        root.emitGrade(g, true)
                    }
                    onFrameRequested: function (f) { if (root.hasClip) root.seekRequested(root.gradeClip.start + f) }
                }
            }
        }
    }

    function pushScopeOptions() {
        root.scopeOptionsEdited({ intensity: root.scopeIntensity, channels: root.scopeChannels, clamp: root.scopeClamp, graticule: root.scopeGraticule, scale: root.scopeScale, bitDepth: root.scopeBitDepth })
    }
    // A colour sampled from the viewer lands on the qualifier.
    function pickedColor(hex, mode) { tabs.pickedColor(hex, mode) }
    readonly property bool scopesRunning: visible && scopes.visible && !scopesPaused
    onScopesRunningChanged: scopesActiveEdited(scopesRunning)
    onVisibleChanged: if (!visible) lutBrowser.close()
    Component.onCompleted: { if (lean && gradeTab > 2) gradeTab = 0; root.scopesActiveEdited(scopesRunning); pushScopeOptions() }
}
