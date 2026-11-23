import QtQuick
import QtQuick.Controls as C
import QtQuick.Dialogs as D
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// Right panel: everything about the selected clip. Sections stay collapsed by
// default except the ones an editor opens first.
//
// Owner-driven: `clip` is a plain object describing the selection (null for
// none) and every edit leaves as a signal. Sections that need engine work
// that does not exist yet are present but disabled, and say so.
Rectangle {
    id: root

    // { name, kind: "video"|"audio"|"title", connected, speed (signed: < 0 is
    //   reversed), volume, pan, eq, opacity, rectX, rectY, rectW, rectH,
    //   rotation, cropL, cropR, cropT, cropB, colorB, colorC, colorS, fadeIn,
    //   fadeOut, mixOut, disabled, title, in, out, length, frames, start, fps }
    property var shownClip: null
    // Shown beside the clip's length: the tempo of its mapped music ("" for none).
    property string tempoText: ""
    property int currentTab: 0
    property var videoEffectCatalogue: []
    property bool videoEffectsEditable: true
    property string keyPickUid: ""
    property string maskViewUid: ""    // the effect whose mask is shown on the viewer
    signal videoEffectEdited(string action, string uid, var values)
    signal videoEffectsWindowRequested()
    function showControls(tab) {
        currentTab = tab
        if (tab === 0) transformSection.expanded = true
        else if (tab === 1) audioLevelSection.expanded = true
        else if (tab === 2) transitionSection.expanded = true
        Qt.callLater(function() { propertiesScroll.contentItem.contentY = 0 })
    }
    readonly property bool hasSelection: shownClip !== null && shownClip !== undefined

    property int animationFrame: 0
    property bool pictureEditable: hasSelection && !isAudio
    signal animationEdited(var patch)
    signal motionPresetRequested(string preset, var options)
    signal pictureEdited(var patch)
    signal pictureReset(string group)
    signal picturePropertyReset(string key, real value)
    signal editKeysEdited(var keys)
    property var transitionStyles: []   // [{key, name, soft, colour}], the cross dissolve first
    // The clip's transition as stored, "name" or "name:soft=20,colour=ff8800" (see transitionstyles.h).
    readonly property string mixStyleStored: hasSelection && shownClip.mixStyle && shownClip.mixStyle !== "dissolve" ? shownClip.mixStyle : ""
    readonly property string mixStyleBase: mixStyleStored.split(":")[0]
    readonly property var mixStyleChoice: transitionStyles.find(function(t) { return t.key === mixStyleBase }) || ({})
    readonly property var mixStyleSettings: {
        const out = {}, rest = mixStyleStored.indexOf(":") >= 0 ? mixStyleStored.slice(mixStyleStored.indexOf(":") + 1) : ""
        rest.split(",").forEach(function(part) {
            const at = part.indexOf("=")
            if (at <= 0) return
            const key = part.slice(0, at), value = part.slice(at + 1)
            out[key] = key === "soft" ? Number(value) : key === "colour" ? "#" + value : value
        })
        return out
    }
    function mixStyleWith(change) {
        const all = Object.assign({}, mixStyleSettings, change), parts = []
        if (mixStyleChoice.soft === true && all.soft !== undefined && all.soft > 0) parts.push("soft=" + all.soft)
        if (mixStyleChoice.colour === true && all.colour !== undefined) parts.push("colour=" + String(all.colour).replace("#", ""))
        return parts.length ? mixStyleBase + ":" + parts.join(",") : mixStyleBase
    }
    signal dissolveStyleSelected(string style)
    signal dissolvePreviewRequested()
    signal animationFrameRequested(int frame)
    signal effectMoved(string key, int direction)
    function propertyKeys(key) {
        if (key === "opacity") return shownClip && shownClip.opacityKf ? shownClip.opacityKf.map(function(k) {
            return {frame: k.frame, value: k.alpha, interpolation: k.interpolation || "linear"}
        }) : []
        return shownClip && shownClip.editKeys ? shownClip.editKeys[key] || [] : []
    }
    function hasKeys(parameters) {
        return parameters.some(function(key) { return propertyKeys(key).length > 0 })
    }
    function atKeyframe(parameters) {
        return parameters.length > 0 && parameters.every(function(key) {
            return propertyKeys(key).some(function(k) { return k.frame === animationFrame })
        })
    }
    function toggleKeys(parameters) {
        if (!pictureEditable || !parameters.length) return
        const remove = atKeyframe(parameters)
        const keys = JSON.parse(JSON.stringify(shownClip.editKeys || {})), patch = {}
        let changedKeys = false
        parameters.forEach(function(key) {
            const before = propertyKeys(key)
            const current = num(key, key === "opacity" || key === "rectW" || key === "rectH"
                                || key === "colorC" || key === "colorS" ? 1 : key.indexOf("anchor") === 0 ? .5 : 0)
            let list = before.filter(function(k) { return k.frame !== animationFrame })
            if (!remove) {
                // Keep an existing member of a paired property unchanged.
                const exact = before.find(function(k) { return k.frame === animationFrame })
                const earlier = before.filter(function(k) { return k.frame < animationFrame })
                list.push(exact || {frame: animationFrame, value: current,
                                   interpolation: earlier.length ? earlier[earlier.length - 1].interpolation : "linear"})
            }
            list.sort(function(a,b) { return a.frame-b.frame })
            if (key === "opacity") {
                patch.opacityKf = list.map(function(k) { return {frame: k.frame, alpha: k.value, interpolation: k.interpolation || "linear"} })
                if (!list.length) patch.opacity = current
            } else {
                changedKeys = true
                if (list.length) keys[key] = list
                else { delete keys[key]; patch[key] = current }
            }
        })
        if (changedKeys) patch.editKeys = keys
        animationEdited(patch)
    }
    function animationValue(key, fallback) {
        const keys = propertyKeys(key)
        if (!keys.length) return fallback
        if (animationFrame <= keys[0].frame) return keys[0].value
        for (let i=1;i<keys.length;++i) {
            const a=keys[i-1], b=keys[i]
            if (animationFrame < b.frame) {
                if (a.interpolation === "hold") return a.value
                let u=(animationFrame-a.frame)/(b.frame-a.frame)
                if (a.interpolation === "smooth") u=u*u*(3-2*u)
                return a.value+u*(b.value-a.value)
            }
        }
        return keys[keys.length-1].value
    }
    function animated(group) {
        const keys=shownClip && shownClip.editKeys ? shownClip.editKeys : {}
        return Object.keys(keys).some(function(k) {return (group === "rect" ? /^(rect|anchor|rotation)/ : group === "crop" ? /^crop/ : /^color/).test(k) && keys[k].length > 0})
    }
    function removeEffect(key) {
        switch(key) {
        case "rect": case "crop": case "color": pictureReset(key); break
        case "blend": blendSelected(0); break
        case "lens": lensEdited(0,0,.5,.5); break
        case "stab": removeStabRequested(); break
        case "flip": flipEdited(false,false); break
        case "audioMap": audioMapSelected(""); break
        case "normalize": normalizeToggled(false); break
        case "opacity": animationEdited({opacity: 1, opacityKf: []}); break
        case "level": volumeEdited(0); panEdited(0); break
        case "eq": eqSelected(""); break
        case "speed": speedSelected(1); break
        }
    }
    readonly property var appliedEffects: {
        const list=[{key:"stab", title:qsTr("Stabilise"),on:hasStab},
            {key:"lens",title:qsTr("Lens"),on:hasLens},{key:"flip",title:qsTr("Flip"),on:flag("flipH")||flag("flipV")},
            {key:"color",title:qsTr("Colour"),on:hasColor||animated("color")},{key:"crop",title:qsTr("Crop"),on:hasCrop||animated("crop")},
            {key:"rect",title:qsTr("Transform"),on:hasTransform||animated("rect")},{key:"opacity",title:qsTr("Opacity"),on:hasOpacity},
            {key:"blend",title:qsTr("Blend"),on:hasBlend},{key:"level",title:qsTr("Level"),on:hasLevel},
            {key:"eq",title:qsTr("Equaliser"),on:hasEq},{key:"audioMap",title:qsTr("Channels"),on:audioMap!==""},
            {key:"normalize",title:qsTr("Normalise"),on:flag("normalize")},{key:"speed",title:qsTr("Speed"),on:hasSpeed}]
        const order=shownClip && shownClip.effectOrder ? shownClip.effectOrder : []
        return list.filter(function(e){return e.on}).sort(function(a,b){
            const ai=order.indexOf(a.key),bi=order.indexOf(b.key)
            return (ai<0?order.length+list.indexOf(a):ai)-(bi<0?order.length+list.indexOf(b):bi)
        })
    }
    signal speedDurationRequested()
    property bool canOpenSpeedDuration: hasSelection
    signal speedSelected(real speed)         // signed
    signal opacityEdited(real alpha)
    signal rectEdited(real x, real y, real w, real h)
    signal pictureGestureStarted()
    signal pictureGestureFinished()
    signal rotationEdited(real degrees)
    signal flipEdited(bool h, bool v)
    signal cropEdited(real l, real r, real t, real b)
    signal cropEdgeEdited(real feather, bool invert)   // soft edge / keep the outside
    signal anchorEdited(real anchorX, real anchorY)    // rotation pivot, fraction of the rect
    signal frameBlendSelected(int mode)                // Persisted IDs: 0 original, 1 blend, 2 flow, 3 floor, 4 nearest
    signal generatorColorSelected(string color)        // a solid's colour (#rrggbb)

    // ── Transcript tab ───────────────────────────────────────────────────
    // The clip whose words are shown: {name, libraryIndex, transcript:
    // [{start, end, text}] (ms), language, hasAudio}, or null. Works for a
    // library-only selection too, so the tab lives outside the
    // "no clip selected" gate.
    property var transcriptClip: null
    property bool transcribing: false
    property string transcribeStage: ""     // "audio" | "text"
    property int transcribePercent: 0
    property string transcriptError: ""
    property bool transcriptAvailable: true
    property string transcriptStatus: ""
    // The timeline has clips to make captions from.
    property bool captionsAvailable: false
    signal transcribeRequested()
    signal makeCaptionsRequested()
    signal transcribeCancelled()
    signal transcriptCleared()
    signal transcriptSegmentActivated(int startMs, int endMs)
    readonly property bool transcriptSearchActive: transcriptSearch.active
    readonly property var transcript: transcriptClip !== null && transcriptClip.transcript !== undefined
                                      ? transcriptClip.transcript : []
    property string transcriptQuery: ""
    readonly property var transcriptRows: {
        const needle = transcriptQuery.trim().toLowerCase()
        const out = []
        for (let i = 0; i < transcript.length; ++i) {
            const t = String(transcript[i].text)
            if (needle === "" || t.toLowerCase().indexOf(needle) >= 0)
                out.push({ index: i, start: transcript[i].start, end: transcript[i].end, text: t })
        }
        return out
    }
    function msTimecode(ms) {
        const total = Math.floor(ms / 1000)
        const pad = function (n) { return n < 10 ? "0" + n : "" + n }
        return pad(Math.floor(total / 3600)) + ":" + pad(Math.floor(total % 3600 / 60)) + ":" + pad(total % 60)
    }
    signal colorEdited(real brightness, real contrast, real saturation)
    signal titleEditRequested()
    signal titleTextEdited(string text)
    // A caption's own look (style keys, x, y), its speaker, and whether it
    // follows its caption track's style.
    signal captionLookEdited(var look)
    signal captionSpeakerEdited(string speaker)
    signal captionTrackStyleToggled(bool on)
    signal volumeEdited(real db)
    signal panEdited(real pan)
    signal eqSelected(string preset)
    signal audioMapSelected(string mode)
    signal normalizeToggled(bool on)
    readonly property var audioMaps: [
        { key: "", name: qsTr("Stereo (as recorded)") },
        { key: "mono", name: qsTr("Mono (fold to centre)") },
        { key: "swap", name: qsTr("Swap left and right") },
        { key: "left", name: qsTr("Left channel on both") },
        { key: "right", name: qsTr("Right channel on both") }]
    readonly property string audioMap: hasSelection && shownClip.audioMap ? shownClip.audioMap : ""
    signal fadeInToggled()
    signal fadeOutToggled()
    signal fadeLengthsEdited(int fadeIn, int fadeOut)
    signal dissolveToggled()
    signal dissolveLengthEdited(int frames)
    signal dissolveAlignSelected(string align)
    // Keyframe controls: a key at the playhead (with the current value) or
    // none; the curves themselves are the timeline's rubber bands.
    signal volumeKeyframeAdded()
    signal volumeKeyframesCleared()
    signal opacityKeyframeAdded()
    signal opacityKeyframesCleared()
    function keyCount(name) {
        return hasSelection && shownClip[name] !== undefined && shownClip[name] !== null ? shownClip[name].length : 0
    }
    // A keyframe row: the count, a key at the playhead, clear.
    component KeyframeRow: Item {
        property string label: ""
        property int count: 0
        signal addRequested()
        signal clearRequested()
        width: parent ? parent.width : 0
        height: Theme.hRow
        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: count === 0 ? qsTr("%1: no keyframes").arg(label)
                  : count === 1 ? qsTr("%1: 1 keyframe").arg(label)
                  : qsTr("%1: %2 keyframes").arg(label).arg(count)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s1
            IconButton {
                objectName: "keyAdd:" + label
                iconName: "diamond"
                text: qsTr("Add a %1 keyframe at the playhead").arg(label.toLowerCase())
                implicitWidth: Theme.szIcon + Theme.s1
                implicitHeight: Theme.szIcon + Theme.s1
                onClicked: addRequested()
            }
            IconButton {
                objectName: "keyClear:" + label
                iconName: "x"
                text: qsTr("Clear %1 keyframes").arg(label.toLowerCase())
                enabled: count > 0
                implicitWidth: Theme.szIcon + Theme.s1
                implicitHeight: Theme.szIcon + Theme.s1
                onClicked: clearRequested()
            }
        }
    }
    // "custom": the two sides were set by hand (a grip on either edge of the
    // box) and fit none of the others. It is shown, not chosen.
    readonly property var dissolveAligns: ["", "centre", "start", "end", "custom"]
    readonly property string dissolveAlign: hasSelection && shownClip.mixAlign ? shownClip.mixAlign : ""
    signal enabledToggled()
    // Applied ▸ eye: switch one attribute off/on without losing its value.
    signal bypassToggled(string key, bool on)
    signal blendSelected(int mode)
    // How the clip takes a frame of another size or shape; "" follows the
    // timeline. Offered only when the clip's picture does not match the frame
    // (or it already chose).
    signal scalingSelected(string scaling)
    readonly property var scalingKeys: ["", "fit", "fill", "stretch", "original"]
    readonly property var scalingNames: ({ fit: qsTr("Fit"), fill: qsTr("Fill"), stretch: qsTr("Stretch"), original: qsTr("Original size") })
    readonly property string clipScaling: hasSelection && shownClip.scaling !== undefined ? String(shownClip.scaling) : ""
    readonly property bool offSize: hasSelection && root.num("sourceWidth", 0) > 0 && root.num("sourceHeight", 0) > 0
                                    && (root.num("sourceWidth", 0) !== root.num("frameWidth", 0)
                                        || root.num("sourceHeight", 0) !== root.num("frameHeight", 0))
    signal lensEdited(real k1, real k2, real cx, real cy)
    signal stabilizeRequested(int shakiness, int smoothing)
    signal removeStabRequested()
    // Applied ▸ Paste Attributes… / Save Preset… (the window owns the
    // clipboard and the dialogs).
    property bool canPasteAttributes: false
    signal pasteAttributesRequested()
    // Render in Place / Restore Original (Applied header).
    property bool rendering: false
    property int renderPercent: 0
    signal renderRequested()
    signal restoreOriginalRequested()
    signal savePresetRequested()
    // The stabiliser's analysis of THIS clip, from the owner.
    property bool analysing: false
    property int analysePercent: 0
    readonly property var blendModes: [0, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    readonly property var blendNames: [qsTr("Normal"), qsTr("Add"), qsTr("Multiply"), qsTr("Screen"),
                                       qsTr("Overlay"), qsTr("Darken"), qsTr("Lighten"),
                                       qsTr("Colour Dodge"), qsTr("Colour Burn"), qsTr("Hard Light"),
                                       qsTr("Soft Light"), qsTr("Difference"), qsTr("Exclusion")]
    readonly property bool hasBlend: root.num("blend", 0) !== 0
    readonly property bool hasLens: root.num("lensK1", 0) !== 0 || root.num("lensK2", 0) !== 0
    readonly property bool hasStab: hasSelection && shownClip.stab !== undefined && shownClip.stab !== null
    readonly property bool stabMissing: hasStab && shownClip.stab.missing === true
    signal trimEdited(int inPoint, int outPoint)

    readonly property var speeds: [0.25, 0.5, 1, 2, 4]
    readonly property var eqPresets: [
        { key: "",             name: qsTr("Flat") },
        { key: "voice",        name: qsTr("Voice Enhance") },
        { key: "bass-boost",   name: qsTr("Bass Boost") },
        { key: "bass-reduce",  name: qsTr("Bass Reduce") },
        { key: "treble-boost", name: qsTr("Treble Boost") },
        { key: "treble-reduce",name: qsTr("Treble Reduce") },
        { key: "hum",          name: qsTr("Hum Reduction") }
    ]
    function num(key, fallback) {
        const base=root.shownClip && root.shownClip[key] !== undefined && root.shownClip[key] !== null ? Number(root.shownClip[key]) : fallback
        return animationValue(key,base)
    }
    function flag(key) {
        const v = root.shownClip ? root.shownClip[key] : undefined
        return v === true || (typeof v === "number" && v > 0)
    }
    // The owner's timecode for a timeline frame (its starting timecode
    // counted in), or null to count from zero.
    property var timelineTimecode: null
    function timecode(frames) {
        const fps = root.num("fps", 25)
        if (fps <= 0 || frames < 0)
            return "--:--:--:--"
        // Whole frames at the nominal rate (non-drop), as the timecode
        // fields count: 29.97 never shows a frame 30.
        const nominal = Math.max(1, Math.round(fps)), count = Math.round(frames)
        const total = Math.floor(count / nominal)
        const pad = function (n) { return n < 10 ? "0" + n : "" + n }
        return pad(Math.floor(total / 3600)) + ":" + pad(Math.floor(total % 3600 / 60))
               + ":" + pad(total % 60) + ":" + pad(count % nominal)
    }
    readonly property bool isTitle: hasSelection && shownClip.kind === "title"
    readonly property bool isCaption: isTitle && shownClip.caption === true
    // The selected caption's whole look (Backend::captionLook), or nothing.
    readonly property var look: isCaption && shownClip.captionLook ? shownClip.captionLook : ({})
    function lookNumber(key, fallback) { return look[key] !== undefined && look[key] !== null ? Number(look[key]) : fallback }
    function hex2(n) { const h = Math.round(Math.max(0, Math.min(255, n))).toString(16); return h.length < 2 ? "0" + h : h }
    readonly property bool isAdjust: hasSelection && shownClip.adjust === true
    readonly property string generator: hasSelection && shownClip.generator !== undefined ? String(shownClip.generator) : ""
    readonly property var solidColors: ["#000000", "#ffffff", "#808080", "#c0392b", "#e67e22", "#f1c40f",
                                        "#27ae60", "#2980b9", "#8e44ad", "#27C2FF"]
    readonly property bool isAudio: hasSelection && shownClip.kind === "audio"
    readonly property bool onTrack: hasSelection && shownClip.connected === true
    readonly property real rate: Math.abs(root.num("speed", 1))
    readonly property bool hasSegments: hasSelection && shownClip.retime !== undefined && shownClip.retime !== null
    readonly property bool reversed: root.num("speed", 1) < 0
    readonly property bool hasColor: root.num("colorB", 0) !== 0 || root.num("colorC", 1) !== 1
                                     || root.num("colorS", 1) !== 1
    readonly property bool hasCrop: root.num("cropL", 0) !== 0 || root.num("cropR", 0) !== 0
                                    || root.num("cropT", 0) !== 0 || root.num("cropB", 0) !== 0
    readonly property bool hasTransform: root.num("rectX", 0) !== 0 || root.num("rectY", 0) !== 0
                                         || root.num("rectW", 1) !== 1 || root.num("rectH", 1) !== 1
                                         || root.num("rotation", 0) !== 0
                                         || root.num("anchorX", 0.5) !== 0.5 || root.num("anchorY", 0.5) !== 0.5
    readonly property var blendingNames: [qsTr("Original sampling"), qsTr("Frame blending"), qsTr("Optical flow"), qsTr("Floor"), qsTr("Nearest frame")]
    readonly property var blendingOrder: [3, 4, 1, 2, 0]
    readonly property int frameBlend: Math.max(0, Math.min(4, root.num("frameBlend", 0)))
    readonly property bool hasEq: hasSelection && shownClip.eq !== undefined && shownClip.eq !== ""
                                  && shownClip.eq !== null
    readonly property bool hasLevel: root.num("volume", 0) !== 0 || root.num("pan", 0) !== 0
                                     || root.keyCount("volumeKf") > 0
    readonly property bool hasSpeed: hasSegments || root.num("speed", 1) !== 1
    readonly property bool hasOpacity: !isAudio && (root.num("opacity", 1) !== 1 || root.keyCount("opacityKf") > 0)
    function bypassed(key) {
        if (!shownClip || shownClip.bypass === undefined || shownClip.bypass === null)
            return false
        for (var i = 0; i < shownClip.bypass.length; i++)
            if (shownClip.bypass[i] === key)
                return true
        return false
    }

    color: Theme.panelBg

    PanelHeader {
        id: tabs
        width: parent.width
        anchors.top: parent.top
        tabs: [qsTr("Video"), qsTr("Audio"), qsTr("Effects"), qsTr("Transcript")]
        // Two-way: a click reports through activated; programmatic tab changes
        // (badges, shortcuts, Match Frame) are mirrored back into the header.
        currentIndex: root.currentTab
        onActivated: function (i) { root.currentTab = i }
        Connections { target: root; function onCurrentTabChanged() { tabs.currentIndex = root.currentTab } }
    }

    EmptyState {
        anchors.top: tabs.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !root.hasSelection && root.currentTab !== 3
        iconName: "mouse-pointer-2"
        title: qsTr("No clip selected")
        description: qsTr("Select a clip in the timeline to inspect it.")
    }

    // ── Transcript ─────────────────────────────────────────────────────────
    Item {
        id: transcriptPane
        objectName: "transcriptPane"
        anchors.top: root.hasSelection ? nameBar.bottom : tabs.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s2
        visible: root.currentTab === 3

        EmptyState {
            anchors.fill: parent
            visible: root.transcriptClip === null
            iconName: "captions"
            title: qsTr("No clip to transcribe")
            description: qsTr("Select a clip on the timeline, or in the browser.")
        }

        Column {
            id: transcriptHead
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.s2
            visible: root.transcriptClip !== null

            Row {
                spacing: Theme.s2
                Chip { label: "AI"; tint: ""; tip: qsTr("Marks a feature that runs a machine-learning model on this machine") }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.transcriptClip !== null ? root.transcriptClip.name : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    font.weight: Theme.wHeading
                    color: Theme.textPrimary
                    elide: Text.ElideMiddle
                    width: transcriptHead.width - 60
                }
            }
            Text {
                width: parent.width
                visible: root.transcript.length === 0 && !root.transcribing
                text: root.transcriptAvailable
                      ? qsTr("Transcribe runs whisper.cpp on this machine over the clip's audio. It only writes words and times onto the library clip — the media and the timeline are never changed.")
                      : root.transcriptStatus
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: root.transcriptAvailable ? Theme.textMuted : Theme.warning
            }
            // (Wraps on a narrow panel.)
            Flow {
                width: parent.width
                spacing: Theme.s2
                ToolButton {
                    objectName: "transcribeButton"
                    iconName: "sparkles"
                    text: root.transcribing ? (root.transcribeStage === "audio" ? qsTr("Extracting audio…")
                                                                                 : qsTr("Transcribing… %1%").arg(root.transcribePercent))
                        : root.transcript.length > 0 ? qsTr("Re-transcribe (AI)") : qsTr("Transcribe with Whisper (AI)")
                    showLabel: true
                    enabled: !root.transcribing && root.transcriptAvailable && root.transcriptClip !== null
                             && root.transcriptClip.hasAudio !== false
                    onClicked: root.transcribeRequested()
                }
                ToolButton {
                    visible: root.transcribing
                    iconName: "x"
                    text: qsTr("Cancel")
                    showLabel: true
                    onClicked: root.transcribeCancelled()
                }
                ToolButton {
                    visible: root.transcript.length > 0 && !root.transcribing
                    iconName: "trash-2"
                    text: qsTr("Clear")
                    showLabel: true
                    onClicked: root.transcriptCleared()
                }
                ToolButton {
                    objectName: "makeCaptionsButton"
                    iconName: "captions"
                    text: qsTr("Make Captions")
                    tip: qsTr("Turn the transcripts of the timeline's clips into a caption layer, transcribing first any clip that has none")
                    showLabel: true
                    enabled: !root.transcribing && root.captionsAvailable
                    onClicked: root.makeCaptionsRequested()
                }
            }
            Text {
                width: parent.width
                visible: root.transcriptClip !== null && root.transcriptClip.hasAudio === false
                text: qsTr("This clip has no audio.")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
            Text {
                width: parent.width
                visible: root.transcriptError !== ""
                text: root.transcriptError
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.warning
            }
            SearchField {
                id: transcriptSearch
                objectName: "transcriptSearch"
                tip: qsTr("Jump to the transcript lines containing this text")
                width: parent.width
                visible: root.transcript.length > 0
                placeholder: qsTr("Search the transcript…")
                onTextChanged: root.transcriptQuery = text
            }
            Text {
                visible: root.transcript.length > 0
                text: root.transcriptQuery.trim() !== ""
                      ? qsTr("%1 of %2 lines match — click one to range it").arg(root.transcriptRows.length).arg(root.transcript.length)
                      : qsTr("%1 lines%2 — click one to range it on the timeline")
                            .arg(root.transcript.length)
                            .arg(root.transcriptClip !== null && root.transcriptClip.language !== undefined && root.transcriptClip.language !== ""
                                 ? " · " + root.transcriptClip.language : "")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }

        ListView {
            id: transcriptList
            objectName: "transcriptList"
            anchors.top: transcriptHead.bottom
            anchors.topMargin: Theme.s2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
            visible: root.transcriptClip !== null && root.transcript.length > 0
            model: root.transcriptRows
            spacing: 1
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: line
                required property var modelData
                objectName: "transcriptLine:" + modelData.index
                width: transcriptList.width
                height: lineText.implicitHeight + Theme.s2 * 2
                radius: Theme.rControl
                color: lineHover.hovered ? Theme.hovered(Theme.panelRaised) : Theme.panelRaised
                Text {
                    id: lineTime
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: Theme.s2
                    text: root.msTimecode(line.modelData.start)
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.accent
                }
                Text {
                    id: lineText
                    anchors.left: lineTime.right
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.s2
                    text: line.modelData.text
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textPrimary
                }
                HoverHandler { id: lineHover }
                TapHandler { onTapped: root.transcriptSegmentActivated(line.modelData.start, line.modelData.end) }
            }
        }
    }

    Rectangle {
        id: nameBar
        anchors.top: tabs.bottom
        width: parent.width
        height: visible ? Theme.hRow + Theme.s2 : 0
        visible: root.hasSelection
        color: Theme.panelRaised

        Icon {
            id: kindIcon
            name: root.isTitle ? "type" : root.isAudio ? "audio-waveform" : "film"
            size: 14
            color: Theme.textMuted
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            anchors.left: kindIcon.right
            anchors.leftMargin: Theme.s2
            anchors.right: tempoLabel.visible ? tempoLabel.left : durationText.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            text: root.hasSelection ? root.shownClip.name : ""
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        // The tempo of the clip's mapped music, as the clip plays it.
        Text {
            id: tempoLabel
            objectName: "inspectorTempo"
            visible: root.hasSelection && root.tempoText !== ""
            anchors.right: durationText.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            text: root.tempoText
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Text {
            id: durationText
            anchors.right: parent.right
            anchors.rightMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            objectName: "inspectorLength"
            text: root.hasSelection ? MediaTime.durationText(root.num("frames", 0), root.num("fps", 25)) : ""
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    component PictureSliderField: SliderField {
        property string animationKey: ""
        animatable: animationKey !== ""
        keyed: root.atKeyframe([animationKey])
        hasAnimation: root.hasKeys([animationKey])
        keyObjectName: "propertyKey:" + animationKey
        onKeyToggled: root.toggleKeys([animationKey])
        liveEdit: true
        signal valueEdited(real value)
        onEdited: function(v) { if (dragging) valueEdited(v) }
        onEditingFinished: function(v) { valueEdited(v) }
        onEditingStarted: root.pictureGestureStarted()
        onEditingEnded: root.pictureGestureFinished()
    }

    // A row with a label and one value field, for the numeric entries.
    component NumberRow: PropertyRow {
        property var animationParameters: []
        property real value: 0
        property real from: -999999
        property real to: 999999
        property int decimals: 0
        property string suffix: ""
        property real fps: 0             // a length: shown and typed as time
        signal valueEdited(real value)
        animatable: animationParameters.length > 0
        keyed: root.atKeyframe(animationParameters)
        hasAnimation: root.hasKeys(animationParameters)
        keyObjectName: "propertyKey:" + animationParameters.join(",")
        onKeyToggled: root.toggleKeys(animationParameters)
        fields: [{ value: value, suffix: suffix, decimals: decimals, from: from, to: to, fps: fps }]
        onFieldEdited: function (i, v) { valueEdited(Math.max(from, Math.min(to, v))) }
    }

    // One line of the applied-effects stack: what it is, an eye that
    // bypasses it (the value stays, its effect goes), and a reset.
    component AppliedRow: Item {
        property string title: ""
        property string detail: ""
        property string key: ""            // bypass key; "" = no eye
        readonly property bool off: key !== "" && root.bypassed(key)
        signal removeRequested()
        width: parent ? parent.width : 0
        height: Theme.hRow + Theme.s1
        Text {
            anchors.left: parent.left
            anchors.right: detailText.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            text: title
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.strikeout: off
            color: off ? Theme.textMuted : Theme.textPrimary
        }
        Text {
            id: detailText
            anchors.right: moveButtons.left
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            text: detail
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            opacity: off ? Theme.disabledOpacity : 1
        }
        Row {
            id: moveButtons
            anchors.right: eyeButton.visible ? eyeButton.left : removeButton.left
            anchors.verticalCenter: parent.verticalCenter
            visible: key !== ""
            IconButton { objectName: "effectUp:" + key; iconName: "chevron-up"; text: qsTr("Move effect earlier"); implicitWidth: 22; implicitHeight:22; onClicked:root.effectMoved(key,-1) }
            IconButton { objectName: "effectDown:" + key; iconName: "chevron-down"; text: qsTr("Move effect later"); implicitWidth: 22; implicitHeight:22; onClicked:root.effectMoved(key,1) }
        }
        IconButton {
            id: eyeButton
            objectName: "bypass:" + key
            visible: key !== ""
            anchors.right: removeButton.left
            anchors.verticalCenter: parent.verticalCenter
            iconName: off ? "eye-off" : "eye"
            text: off ? qsTr("Switch %1 back on").arg(title) : qsTr("Bypass %1").arg(title)
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            onClicked: root.bypassToggled(key, !off)
        }
        IconButton {
            id: removeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            iconName: "x"
            text: qsTr("Remove %1").arg(title)
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            onClicked: removeRequested()
        }
    }

    C.ScrollView {
        anchors.top: nameBar.bottom
        id: propertiesScroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s2
        visible: root.hasSelection && root.currentTab !== 3
        clip: true
        C.ScrollBar.vertical: ScrollBar {}
        // Up and down only: any other width would also flick sideways, taking
        // the drags of the sliders inside, and the unseen sideways bar would
        // take the clicks of whatever comes to rest along the bottom edge.
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff

        Column {
            // The scroll bar has a gutter of its own: over the controls at the
            // panel's right edge it would take their clicks.
            width: root.width - Theme.s2 * 2 - Theme.s3
            spacing: 0

            // ── Video ─────────────────────────────────────────────────────
            Column {
                width: parent.width
                visible: root.currentTab === 0
                spacing: 0

                InspectorSection {
                    objectName: "generatorSection"
                    width: parent.width
                    title: root.isAdjust ? qsTr("Effect Layer") : qsTr("Generator")
                    expanded: true
                    visible: root.generator !== ""
                    Text {
                        width: parent.width
                        text: root.isAdjust
                              ? qsTr("Colour, Lens Correction, Flip and Crop set here grade everything under this clip for its length — the whole composite, tracks above included.")
                              : root.generator === "solid" ? qsTr("A solid colour, 4 s by default. Trim it like any clip.")
                              : root.generator === "bars" ? qsTr("SMPTE-style colour bars for line-up.")
                              : root.generator === "noise" ? qsTr("Random noise — a texture to composite over.")
                                                           : qsTr("A counting leader: one number per second.")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    Flow {
                        visible: root.generator === "solid"
                        width: parent.width
                        spacing: Theme.s1
                        Repeater {
                            model: root.solidColors
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "solidSwatch:" + modelData
                                width: 22
                                height: 22
                                radius: Theme.rControl
                                color: modelData
                                border.width: root.hasSelection && String(root.shownClip.generatorColor).toLowerCase() === modelData.toLowerCase()
                                              ? 2 : Theme.hairline
                                border.color: root.hasSelection && String(root.shownClip.generatorColor).toLowerCase() === modelData.toLowerCase()
                                              ? Theme.accent : Theme.border
                                TapHandler { onTapped: root.generatorColorSelected(parent.modelData) }
                            }
                        }
                    }
                }

                // A caption first: its own look, place and speaker, and whether
                // it follows its caption track's style.
                InspectorSection {
                    id: captionSection
                    objectName: "captionSection"
                    width: parent.width
                    title: qsTr("Caption")
                    expanded: true
                    visible: root.isCaption
                    modified: root.look.ownStyle === true
                    onReset: root.captionTrackStyleToggled(true)

                    // A colour typed as a hex code, set when the field is left.
                    component ColourRow: Item {
                        id: colourRow
                        property string label: ""
                        property string value: ""
                        property string tip: ""
                        property bool withAlpha: false
                        signal colourEdited(string colour)
                        width: parent ? parent.width : 260
                        implicitHeight: Theme.hControl + Theme.s1
                        Text {
                            text: colourRow.label
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                        }
                        Rectangle {
                            id: swatch
                            anchors.right: colourField.left
                            anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14; radius: 2
                            color: colourRow.value.length === 9 ? "#" + colourRow.value.substring(3) : (colourRow.value || "transparent")
                            border.width: Theme.hairline
                            border.color: Theme.border
                        }
                        LineEdit {
                            id: colourField
                            objectName: colourRow.objectName + "Field"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 96
                            tip: colourRow.tip
                            text: colourRow.value
                            validator: RegularExpressionValidator { regularExpression: colourRow.withAlpha ? /#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})/ : /#[0-9a-fA-F]{6}/ }
                            onEditingFinished: if (acceptableInput && text.toLowerCase() !== colourRow.value) colourRow.colourEdited(text.toLowerCase())
                        }
                    }

                    Toggle {
                        objectName: "inspectorCaptionUsesTrackStyle"
                        width: parent.width
                        label: qsTr("Uses the track style")
                        tip: qsTr("On: the caption follows its caption track's style and place, and changes with it. Turning it on drops the caption's own look; turning it off keeps the caption as it is when the track's style changes.")
                        checked: root.look.ownStyle !== true
                        onClicked: root.captionTrackStyleToggled(checked)
                    }
                    Item {
                        width: parent.width
                        implicitHeight: Theme.hControl + Theme.s1
                        Text {
                            text: qsTr("Speaker")
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                        }
                        LineEdit {
                            id: speakerField
                            objectName: "inspectorCaptionSpeaker"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(96, parent.width - 90)
                            tip: qsTr("Who says this caption's words; leave empty for none")
                            hint: qsTr("No speaker")
                            readonly property string stored: root.look.speaker !== undefined ? root.look.speaker : ""
                            onStoredChanged: text = stored
                            Component.onCompleted: text = stored
                            onEditingFinished: if (text.trim() !== stored) root.captionSpeakerEdited(text.trim())
                        }
                    }
                    ComboField {
                        objectName: "inspectorCaptionAlign"
                        tip: qsTr("How the caption's lines line up with each other")
                        width: parent.width
                        caption: qsTr("Alignment")
                        model: [qsTr("Left"), qsTr("Centre"), qsTr("Right")]
                        currentIndex: Math.max(0, ["left", "centre", "right"].indexOf(root.look.align || "centre"))
                        onActivated: function (i) { root.captionLookEdited({ align: ["left", "centre", "right"][i] }) }
                    }
                    PropertyRow {
                        width: parent.width
                        objectName: "inspectorCaptionPosition"
                        label: qsTr("Position")
                        animatable: false
                        fields: [{ prefix: "X", value: root.lookNumber("x", 0) * 100, suffix: "%", decimals: 1 },
                                 { prefix: "Y", value: root.lookNumber("y", 0) * 100, suffix: "%", decimals: 1 }]
                        onFieldEdited: function (i, v) {
                            root.captionLookEdited(i === 0 ? { x: Math.max(-1, Math.min(1, v / 100)) }
                                                           : { y: Math.max(-1, Math.min(1, v / 100)) })
                        }
                    }
                    PropertyRow {
                        width: parent.width
                        objectName: "inspectorCaptionSize"
                        label: qsTr("Text size")
                        animatable: false
                        fields: [{ prefix: "", value: root.lookNumber("pixelSize", 0), suffix: " px", decimals: 0 }]
                        onFieldEdited: function (i, v) { root.captionLookEdited({ pixelSize: Math.max(0, Math.min(2000, Math.round(v))) }) }
                    }
                    ColourRow {
                        objectName: "inspectorCaptionColour"
                        label: qsTr("Colour")
                        tip: qsTr("Text colour as a six-digit hex code")
                        value: root.look.color || "#ffffff"
                        onColourEdited: function (c) { root.captionLookEdited({ color: c }) }
                    }
                    Toggle {
                        objectName: "inspectorCaptionOutline"
                        width: parent.width
                        label: qsTr("Outline")
                        tip: qsTr("Draw an edge round the letters")
                        checked: root.look.outline !== false
                        onClicked: root.captionLookEdited({ outline: checked })
                    }
                    ColourRow {
                        objectName: "inspectorCaptionOutlineColour"
                        visible: root.look.outline !== false
                        label: qsTr("Outline colour")
                        tip: qsTr("Colour of the edge round the letters")
                        value: root.look.outlineColor || "#000000"
                        onColourEdited: function (c) { root.captionLookEdited({ outlineColor: c }) }
                    }
                    PropertyRow {
                        width: parent.width
                        visible: root.look.outline !== false
                        objectName: "inspectorCaptionOutlineWidth"
                        label: qsTr("Outline width")
                        animatable: false
                        fields: [{ prefix: "", value: root.lookNumber("outlineWidth", 2), suffix: " px", decimals: 1 }]
                        onFieldEdited: function (i, v) { root.captionLookEdited({ outlineWidth: Math.max(0, Math.min(20, v)) }) }
                    }
                    Toggle {
                        objectName: "inspectorCaptionShadow"
                        width: parent.width
                        label: qsTr("Shadow")
                        tip: qsTr("Draw a soft shadow under the letters")
                        checked: root.look.shadow === true
                        onClicked: root.captionLookEdited({ shadow: checked })
                    }
                    ColourRow {
                        objectName: "inspectorCaptionShadowColour"
                        visible: root.look.shadow === true
                        withAlpha: true
                        label: qsTr("Shadow colour")
                        tip: qsTr("Shadow colour: six hex digits, or eight with its opacity first")
                        value: root.look.shadowColor || "#000000"
                        onColourEdited: function (c) { root.captionLookEdited({ shadowColor: c }) }
                    }
                    PropertyRow {
                        width: parent.width
                        visible: root.look.shadow === true
                        objectName: "inspectorCaptionShadowShape"
                        label: qsTr("Shadow")
                        animatable: false
                        fields: [{ prefix: qsTr("Offset"), value: root.lookNumber("shadowOffset", 3), suffix: " px", decimals: 0 },
                                 { prefix: qsTr("Soft"), value: root.lookNumber("shadowSoftness", 4), suffix: " px", decimals: 0 }]
                        onFieldEdited: function (i, v) {
                            root.captionLookEdited(i === 0 ? { shadowOffset: Math.max(-50, Math.min(50, Math.round(v))) }
                                                           : { shadowSoftness: Math.max(0, Math.min(50, Math.round(v))) })
                        }
                    }
                    Toggle {
                        objectName: "inspectorCaptionBox"
                        width: parent.width
                        label: qsTr("Box")
                        tip: qsTr("Draw a box behind the words")
                        checked: !!root.look.bg && root.lookNumber("boxOpacity", 0) > 0
                        onClicked: root.captionLookEdited(checked ? { boxOpacity: root.lookNumber("boxOpacity", 0) > 0 ? root.lookNumber("boxOpacity", 60) : 60 } : { bg: "" })
                    }
                    ColourRow {
                        objectName: "inspectorCaptionBoxColour"
                        visible: !!root.look.bg
                        label: qsTr("Box colour")
                        tip: qsTr("Box colour as a six-digit hex code")
                        value: root.look.bg && root.look.bg.length === 9 ? "#" + root.look.bg.substring(3) : "#000000"
                        onColourEdited: function (c) { root.captionLookEdited({ bg: "#" + root.hex2(root.lookNumber("boxOpacity", 100) * 255 / 100) + c.substring(1) }) }
                    }
                    PropertyRow {
                        width: parent.width
                        visible: !!root.look.bg
                        objectName: "inspectorCaptionBoxOpacity"
                        label: qsTr("Box opacity")
                        animatable: false
                        fields: [{ prefix: "", value: root.lookNumber("boxOpacity", 0), suffix: "%", decimals: 0 }]
                        onFieldEdited: function (i, v) { root.captionLookEdited({ boxOpacity: Math.max(0, Math.min(100, Math.round(v))) }) }
                    }
                    PropertyRow {
                        width: parent.width
                        visible: !!root.look.bg
                        objectName: "inspectorCaptionBoxShape"
                        label: qsTr("Box")
                        animatable: false
                        // Padding below 0 is automatic: a quarter of the text height.
                        fields: [{ prefix: qsTr("Pad"), value: root.lookNumber("boxPadding", -1), suffix: " px", decimals: 0 },
                                 { prefix: qsTr("Corners"), value: root.lookNumber("boxRadius", 0), suffix: " px", decimals: 0 }]
                        onFieldEdited: function (i, v) {
                            root.captionLookEdited(i === 0 ? { boxPadding: v < 0 ? -1 : Math.min(200, Math.round(v)) }
                                                           : { boxRadius: Math.max(0, Math.min(200, Math.round(v))) })
                        }
                    }
                }

                InspectorSection {
                    objectName: "motionPresetsSection"
                    width: parent.width
                    title: qsTr("Motion Presets")
                    visible: !root.isAudio && !root.isAdjust
                    enabled: root.pictureEditable
                    modified: root.hasKeys(["rectX", "rectY", "rectW", "rectH"]) || (root.isTitle && root.keyCount("opacityKf") > 0)
                    onReset: {
                        const keys = JSON.parse(JSON.stringify(root.shownClip.editKeys || {}))
                        const parameters = ["rectX", "rectY", "rectW", "rectH"]
                        parameters.forEach(function(key) { delete keys[key] })
                        const patch = {editKeys: keys}
                        if (root.isTitle) patch.opacityKf = []
                        root.animationEdited(patch)
                    }
                    expanded: false
                    MotionPresets {
                        width: parent.width
                        clipData: root.shownClip
                        onApplyRequested: function(preset, options) { root.motionPresetRequested(preset, options) }
                        onFrameRequested: function(frame) { root.animationFrameRequested(frame) }
                    }
                }

                InspectorSection {
                    objectName: "editAnimationSection"
                    width: parent.width
                    title: qsTr("Animation")
                    resettable: false
                    visible: !root.isAudio
                    enabled: root.pictureEditable
                    expanded: false
                    EditAnimation {
                        width: parent.width
                        clipData: root.shownClip
                        currentFrame: root.animationFrame
                        catalogue: root.videoEffectCatalogue
                        onEffectEdited: function(uid, values) { root.videoEffectEdited("update", uid, values) }
                        onKeysEdited: function(keys) { root.editKeysEdited(keys) }
                        onOpacityKeysEdited: function(keys) { root.animationEdited({opacityKf: keys}) }
                        onFrameRequested: function(frame) { root.animationFrameRequested(frame) }
                        onPropertyReset: function(key,value) { root.picturePropertyReset(key,value) }
                    }
                }

                InspectorSection {
                    id: transformSection
                    width: parent.width
                    title: qsTr("Transform")
                    expanded: true
                    visible: !root.isAudio && !root.isAdjust
                    enabled: root.pictureEditable
                    modified: root.hasTransform || root.animated("rect") || root.hasOpacity
                    onReset: root.pictureReset("rect")

                    ComboField {
                        objectName: "scalingCombo"
                        width: parent.width
                        visible: !root.isTitle
                        caption: qsTr("Scaling")
                        tip: qsTr("How this clip fills a frame of another size or shape: the timeline's choice, the whole picture, filling the frame, stretched to its corners, or at its own size")
                        model: [qsTr("Timeline (%1)").arg(root.scalingNames[root.shownClip && root.shownClip.timelineScaling ? root.shownClip.timelineScaling : "fit"] || qsTr("Fit")),
                                qsTr("Fit"), qsTr("Fill"), qsTr("Stretch"), qsTr("Original size")]
                        currentIndex: Math.max(0, root.scalingKeys.indexOf(root.clipScaling))
                        onActivated: function (i) { root.scalingSelected(root.scalingKeys[i]) }
                    }
                    Text {
                        objectName: "scalingMatchNote"
                        width: parent.width
                        visible: !root.isTitle && !root.offSize
                        text: qsTr("This clip is already the timeline's size, so every choice looks the same until the sizes differ.")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    Text {
                        width: parent.width
                        text: qsTr("Picture in picture, as a fraction of the frame. Drag the frame in the monitor too.")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    PropertyRow {
                        objectName: "inspectorPosition"
                        width: parent.width
                        label: qsTr("Position")
                        onEditStarted: root.pictureGestureStarted()
                        onEditFinished: root.pictureGestureFinished()
                        animatable: true
                        keyObjectName: "propertyKey:position"
                        keyed: root.atKeyframe(["rectX", "rectY"])
                        hasAnimation: root.hasKeys(["rectX", "rectY"])
                        onKeyToggled: root.toggleKeys(["rectX", "rectY"])
                        fields: [{ prefix: "X", value: root.num("rectX", 0) * 100, suffix: "%" },
                                 { prefix: "Y", value: root.num("rectY", 0) * 100, suffix: "%" }]
                        onFieldEdited: function (i, v) {
                            root.rectEdited(i === 0 ? v / 100 : root.num("rectX", 0),
                                            i === 1 ? v / 100 : root.num("rectY", 0),
                                            root.num("rectW", 1), root.num("rectH", 1))
                        }
                    }
                    Toggle {
                        id: lockAspect
                        objectName: "lockAspectToggle"
                        tip: qsTr("Keep width and height equal when either size is changed")
                        width: parent.width
                        label: qsTr("Uniform scale (W = H)")
                        checked: false // the click itself toggles a checkable control
                    }
                    PropertyRow {
                        width: parent.width
                        label: qsTr("Size")
                        objectName: "inspectorScale"
                        onEditStarted: root.pictureGestureStarted()
                        onEditFinished: root.pictureGestureFinished()
                        animatable: true
                        keyObjectName: "propertyKey:size"
                        keyed: root.atKeyframe(["rectW", "rectH"])
                        hasAnimation: root.hasKeys(["rectW", "rectH"])
                        onKeyToggled: root.toggleKeys(["rectW", "rectH"])
                        fields: [{ prefix: "W", value: root.num("rectW", 1) * 100, suffix: "%" },
                                 { prefix: "H", value: root.num("rectH", 1) * 100, suffix: "%" }]
                        onFieldEdited: function (i, v) {
                            if (lockAspect.checked)
                                root.rectEdited(root.num("rectX", 0), root.num("rectY", 0), v / 100, v / 100)
                            else
                                root.rectEdited(root.num("rectX", 0), root.num("rectY", 0),
                                                i === 0 ? v / 100 : root.num("rectW", 1),
                                                i === 1 ? v / 100 : root.num("rectH", 1))
                        }
                    }
                    NumberRow {
                        objectName: "rotationRow"
                        animationParameters: ["rotation"]
                        onEditStarted: root.pictureGestureStarted()
                        onEditFinished: root.pictureGestureFinished()
                        width: parent.width
                        label: qsTr("Rotation")
                        value: root.num("rotation", 0)
                        from: -180; to: 180; decimals: 1; suffix: "°"
                        onValueEdited: function (v) { root.rotationEdited(v) }
                    }
                    PropertyRow {
                        objectName: "anchorRow"
                        onEditStarted: root.pictureGestureStarted()
                        onEditFinished: root.pictureGestureFinished()
                        width: parent.width
                        label: qsTr("Anchor")
                        animatable: true
                        keyObjectName: "propertyKey:anchor"
                        keyed: root.atKeyframe(["anchorX", "anchorY"])
                        hasAnimation: root.hasKeys(["anchorX", "anchorY"])
                        onKeyToggled: root.toggleKeys(["anchorX", "anchorY"])
                        fields: [{ prefix: "X", value: root.num("anchorX", 0.5) * 100, suffix: "%" },
                                 { prefix: "Y", value: root.num("anchorY", 0.5) * 100, suffix: "%" }]
                        onFieldEdited: function (i, v) {
                            root.anchorEdited(i === 0 ? v / 100 : root.num("anchorX", 0.5),
                                              i === 1 ? v / 100 : root.num("anchorY", 0.5))
                        }
                    }
                    Text {
                        width: parent.width
                        text: qsTr("The point the rotation turns about, across the clip: 50 / 50 is its centre.")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    PictureSliderField {
                        width: parent.width
                        tip: qsTr("How much of the tracks beneath show through this clip")
                        label: qsTr("Opacity")
                        animationKey: "opacity"
                        from: 0; to: 100; decimals: 0; suffix: "%"
                        value: root.num("opacity", 1) * 100
                        onValueEdited: function (v) { root.opacityEdited(v / 100) }
                    }
                }

                InspectorSection {
                    objectName: "flipSection"
                    width: parent.width
                    title: qsTr("Flip")
                    visible: !root.isAudio && !root.isTitle
                    modified: root.flag("flipH") || root.flag("flipV")
                    onReset: root.flipEdited(false, false)

                    Toggle {
                        objectName: "flipHToggle"
                        tip: qsTr("Mirror the picture left to right")
                        width: parent.width
                        label: qsTr("Horizontal")
                        checked: root.flag("flipH")
                        onClicked: root.flipEdited(!root.flag("flipH"), root.flag("flipV"))
                    }
                    Toggle {
                        objectName: "flipVToggle"
                        tip: qsTr("Mirror the picture top to bottom")
                        width: parent.width
                        label: qsTr("Vertical")
                        checked: root.flag("flipV")
                        onClicked: root.flipEdited(root.flag("flipH"), !root.flag("flipV"))
                    }
                    Text {
                        objectName: "freezeFrameHint"
                        visible: MediaTime.isHeldPicture(root.shownClip)
                        width: parent.width
                        text: root.shownClip && root.shownClip.freeze !== undefined
                              ? qsTr("Freeze frame — holds source frame %1").arg(root.num("freeze", 0))
                              : qsTr("Freeze frame — holds the selected picture")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                }

                InspectorSection {
                    objectName: "cropSection"
                    width: parent.width
                    title: qsTr("Crop")
                    visible: !root.isAudio
                    enabled: root.pictureEditable
                    modified: root.hasCrop || root.animated("crop")
                    onReset: root.pictureReset("crop")

                    PictureSliderField {
                        id: cropL
                        animationKey: "cropL"
                        tip: qsTr("How much of the frame is cut off the left edge")
                        width: parent.width
                        label: qsTr("Left")
                        from: 0; to: 95 - cropR.value; decimals: 0; suffix: "%"
                        value: root.num("cropL", 0) * 100
                        onValueEdited: function (v) { root.cropEdited(v / 100, cropR.value / 100, cropT.value / 100, cropB.value / 100) }
                    }
                    PictureSliderField {
                        id: cropR
                        animationKey: "cropR"
                        tip: qsTr("How much of the frame is cut off the right edge")
                        width: parent.width
                        label: qsTr("Right")
                        from: 0; to: 95 - cropL.value; decimals: 0; suffix: "%"
                        value: root.num("cropR", 0) * 100
                        onValueEdited: function (v) { root.cropEdited(cropL.value / 100, v / 100, cropT.value / 100, cropB.value / 100) }
                    }
                    PictureSliderField {
                        id: cropT
                        animationKey: "cropT"
                        tip: qsTr("How much of the frame is cut off the top edge")
                        width: parent.width
                        label: qsTr("Top")
                        from: 0; to: 95 - cropB.value; decimals: 0; suffix: "%"
                        value: root.num("cropT", 0) * 100
                        onValueEdited: function (v) { root.cropEdited(cropL.value / 100, cropR.value / 100, v / 100, cropB.value / 100) }
                    }
                    PictureSliderField {
                        id: cropB
                        animationKey: "cropB"
                        tip: qsTr("How much of the frame is cut off the bottom edge")
                        width: parent.width
                        label: qsTr("Bottom")
                        from: 0; to: 95 - cropT.value; decimals: 0; suffix: "%"
                        value: root.num("cropB", 0) * 100
                        onValueEdited: function (v) { root.cropEdited(cropL.value / 100, cropR.value / 100, cropT.value / 100, v / 100) }
                    }
                    PictureSliderField {
                        objectName: "cropFeatherField"
                        animationKey: "cropFeather"
                        tip: qsTr("Softness of the crop edge, from a hard cut to a wide blend")
                        width: parent.width
                        label: qsTr("Feather")
                        from: 0; to: 100; decimals: 0; suffix: "%"
                        enabled: root.hasCrop || root.animated("crop")
                        value: root.num("cropFeather", 0) * 100
                        onValueEdited: function (v) { root.cropEdgeEdited(v / 100, root.flag("cropInvert")) }
                    }
                    Toggle {
                        objectName: "cropInvertToggle"
                        tip: qsTr("Cut away the boxed area and keep everything around it")
                        width: parent.width
                        label: qsTr("Invert (keep the outside)")
                        enabled: root.hasCrop || root.animated("crop")
                        checked: root.flag("cropInvert")
                        onClicked: root.cropEdgeEdited(root.num("cropFeather", 0), !root.flag("cropInvert"))
                    }
                }

                InspectorSection {
                    objectName: "speedSection"
                    width: parent.width
                    title: qsTr("Speed")
                    expanded: true
                    visible: !root.isTitle && root.generator === ""
                    modified: root.hasSpeed && !root.hasSegments
                    onReset: root.speedSelected(1)

                    ToolButton {
                        objectName: "speedDurationButton"
                        text: root.hasSegments ? qsTr("Edit speed segments…") : qsTr("Speed / Duration…")
                        enabled: root.canOpenSpeedDuration
                        onClicked: root.speedDurationRequested()
                    }

                    Row {
                        visible: !root.hasSegments
                        width: parent.width
                        spacing: Theme.s2
                        Text {
                            text: qsTr("Rate")
                            width: 84
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ComboField {
                            tip: qsTr("Playback rate for the clip; its length on the timeline changes to match")
                            width: parent.width - 84 - Theme.s2 * 2
                            // A rate set elsewhere (Speed/Duration dialog, fit to fill) is not
                            // always a preset; it gets its own row instead of masquerading as 25 %.
                            readonly property bool customRate: root.speeds.indexOf(root.rate) < 0
                            model: root.speeds.map(function (s) { return Math.round(s * 100) + "%" })
                                   .concat(customRate ? [qsTr("Custom (%1%)").arg(Math.round(root.rate * 100))] : [])
                            currentIndex: customRate ? root.speeds.length : root.speeds.indexOf(root.rate)
                            onActivated: function (i) {
                                if (i >= root.speeds.length) return // the custom row only reports the current rate
                                root.speedSelected(root.reversed ? -root.speeds[i] : root.speeds[i])
                            }
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    NumberRow {
                        objectName: "durationRow"
                        visible: !root.hasSegments
                        width: parent.width
                        label: qsTr("Duration")
                        value: root.num("frames", 0)
                        from: 1; to: 999999; fps: root.num("fps", 25)
                        // A new length is a new rate over the same material.
                        onValueEdited: function (v) {
                            const frames = root.num("frames", 0)
                            if (frames <= 0 || v <= 0)
                                return
                            const next = Math.max(0.05, Math.min(20, root.rate * frames / v))
                            root.speedSelected(root.reversed ? -next : next)
                        }
                    }
                    Toggle {
                        objectName: "reverseToggle"
                        visible: !root.hasSegments
                        tip: qsTr("Play the clip backwards at the same rate")
                        width: parent.width
                        label: qsTr("Reverse")
                        checked: root.reversed
                        onClicked: root.speedSelected(root.reversed ? root.rate : -root.rate)
                    }
                    Row {
                        width: parent.width
                        spacing: Theme.s2
                        Text {
                            text: qsTr("Sampling")
                            width: 84
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ComboField {
                            objectName: "frameBlendCombo"
                            tip: qsTr("Choose pictures when the source rate or playback speed differs from the timeline")
                            width: parent.width - 84 - Theme.s2 * 2
                            model: root.blendingOrder.map(function (mode) { return root.blendingNames[mode] })
                            currentIndex: root.blendingOrder.indexOf(root.frameBlend)
                            onActivated: function (i) { root.frameBlendSelected(root.blendingOrder[i]) }
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Text {
                        visible: true
                        width: parent.width
                        text: root.frameBlend === 0 ? qsTr("Keeps this clip's saved sampling. Choose Floor or Nearest frame for an explicit picture choice.")
                              : root.frameBlend === 3 ? qsTr("Uses the picture at or before each source position in the viewer and export.")
                              : root.frameBlend === 4 ? qsTr("Uses the closest source picture in the viewer and export.")
                              : root.hasSegments ? (root.frameBlend === 2
                              ? qsTr("Follows motion in the viewer and export. Blends pictures when motion cannot be matched reliably.")
                              : qsTr("Blends adjacent pictures in the viewer and export.")) : root.hasSpeed
                              ? qsTr("Plays nearest-frame here; %1 is baked by Render in Place and on export.").arg(root.blendingNames[root.frameBlend].toLowerCase())
                              : qsTr("Applies once the clip is retimed.")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                }

                InspectorSection {
                    objectName: "clipSection"
                    width: parent.width
                    title: qsTr("Clip")
                    visible: !root.isTitle
                    modified: false

                    NumberRow {
                        objectName: "inRow"
                        width: parent.width
                        label: qsTr("In")
                        value: root.num("in", 0)
                        from: 0; to: Math.max(0, root.num("length", 1) - 1); suffix: qsTr("f")
                        onValueEdited: function (v) { root.trimEdited(v, root.num("out", 0)) }
                    }
                    NumberRow {
                        objectName: "outRow"
                        width: parent.width
                        label: qsTr("Out")
                        value: root.num("out", 0)
                        from: 0; to: Math.max(0, root.num("length", 1) - 1); suffix: qsTr("f")
                        onValueEdited: function (v) { root.trimEdited(root.num("in", 0), v) }
                    }
                    Text {
                        objectName: "inspectorClipTimes"
                        width: parent.width
                        text: qsTr("Source %1 of %2 · starts at %3")
                              .arg(root.timecode(root.num("in", 0)))
                              .arg(MediaTime.durationText(root.num("length", 0), root.num("fps", 25)))
                              .arg(typeof root.timelineTimecode === "function" && root.num("start", 0) >= 0
                                   ? root.timelineTimecode(root.num("start", 0)) : root.timecode(root.num("start", 0)))
                        wrapMode: Text.WordWrap
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                }

                // Brightness, contrast and saturation from older projects:
                // grading is the Color page's, so this shows only on a clip
                // that already carries them, to change or reset them.
                InspectorSection {
                    objectName: "legacyColourSection"
                    width: parent.width
                    title: qsTr("Colour (older projects)")
                    visible: !root.isAudio && (root.hasColor || root.animated("color"))
                    enabled: root.pictureEditable
                    modified: root.hasColor || root.animated("color")
                    onReset: root.pictureReset("color")

                    PictureSliderField {
                        id: brightness
                        animationKey: "colorB"
                        tip: qsTr("Lifts or lowers the whole picture towards white or black")
                        width: parent.width
                        label: qsTr("Brightness")
                        from: -0.5; to: 0.5; origin: 0; decimals: 2
                        value: root.num("colorB", 0)
                        onValueEdited: function (v) { root.colorEdited(v, contrast.value, saturation.value) }
                    }
                    PictureSliderField {
                        id: contrast
                        animationKey: "colorC"
                        tip: qsTr("Spread between the darkest and brightest parts of the picture")
                        width: parent.width
                        label: qsTr("Contrast")
                        from: 0.4; to: 1.6; origin: 1; decimals: 2
                        value: root.num("colorC", 1)
                        onValueEdited: function (v) { root.colorEdited(brightness.value, v, saturation.value) }
                    }
                    PictureSliderField {
                        id: saturation
                        animationKey: "colorS"
                        tip: qsTr("Colour intensity, from grey at 0 to twice as vivid at 2")
                        width: parent.width
                        label: qsTr("Saturation")
                        from: 0; to: 2; origin: 1; decimals: 2
                        value: root.num("colorS", 1)
                        onValueEdited: function (v) { root.colorEdited(brightness.value, contrast.value, v) }
                    }
                }

                InspectorSection {
                    width: parent.width
                    title: qsTr("Title")
                    expanded: true
                    visible: root.isTitle

                    // The words themselves, typed here: a caption or a title keeps
                    // its look and takes the new text when the field is left
                    // (or on Ctrl+Return); Esc puts back what it said.
                    C.TextArea {
                        id: titleText
                        objectName: "inspectorTitleText"
                        width: parent.width
                        readonly property string stored: root.hasSelection && root.shownClip.title !== undefined ? root.shownClip.title : ""
                        onStoredChanged: text = stored
                        Component.onCompleted: text = stored
                        function commit() { if (root.isTitle && text !== stored && text.trim() !== "") root.titleTextEdited(text); else text = stored }
                        onActiveFocusChanged: if (!activeFocus) commit()
                        Keys.onPressed: function(event) {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) { commit(); event.accepted = true }
                            else if (event.key === Qt.Key_Escape) { text = stored; focus = false; event.accepted = true }
                        }
                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.bold: root.hasSelection && root.shownClip.titleWeight === "bold"
                        font.italic: root.hasSelection && root.shownClip.titleItalic === true
                        padding: Theme.s2
                        topPadding: Theme.s2; bottomPadding: Theme.s2
                        leftPadding: Theme.s2; rightPadding: Theme.s2
                        topInset: 0; bottomInset: 0
                        placeholderText: ""
                        Accessible.name: qsTr("Title text")
                        background: Rectangle {
                            color: Theme.controlBg; radius: Theme.rControl
                            border.width: Theme.hairline
                            border.color: titleText.activeFocus ? Theme.accent : Theme.border
                        }
                    }
                    Text {
                        width: parent.width
                        visible: root.isTitle
                        text: root.hasSelection
                              ? [root.shownClip.titleSize || "medium",
                                 root.shownClip.titleAlign || "centre",
                                 root.shownClip.titleWeight === "bold" ? "bold" : "",
                                 root.shownClip.titleItalic === true ? "italic" : "",
                                 root.shownClip.titleBg ? "box" : "",
                                 root.shownClip.titleOutline === false ? "no outline" : ""]
                                    .filter(function (w) { return w !== "" }).join(" · ")
                              : ""
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textMuted
                    }
                    ToolButton {
                        iconName: "pencil"
                        text: qsTr("Edit title…")
                        showLabel: true
                        onClicked: root.titleEditRequested()
                    }
                }

                InspectorSection {
                    objectName: "compositeSection"
                    width: parent.width
                    title: qsTr("Composite")
                    visible: !root.isAudio
                    enabled: root.pictureEditable
                    modified: root.hasBlend || (root.isAdjust && root.hasOpacity)
                    onReset: { root.blendSelected(0); if (root.isAdjust) root.animationEdited({opacity: 1, opacityKf: []}) }

                    ComboField {
                        objectName: "blendCombo"
                        tip: qsTr("How this clip mixes with the tracks beneath it")
                        width: parent.width
                        caption: qsTr("Blend mode")
                        enabled: !root.isAudio
                        model: root.blendNames
                        currentIndex: Math.max(0, root.blendModes.indexOf(root.num("blend", 0)))
                        onActivated: function (i) { root.blendSelected(root.blendModes[i]) }
                    }
                    // Opacity lives under Transform with its keyframe diamond; an
                    // adjustment clip has no Transform, so it shows here instead.
                    PictureSliderField {
                        width: parent.width
                        visible: root.isAdjust
                        tip: qsTr("How much of the tracks beneath show through this clip")
                        label: qsTr("Opacity")
                        animationKey: "opacity"
                        keyObjectName: "compositeOpacityKey"
                        from: 0; to: 100; decimals: 0; suffix: "%"
                        value: root.num("opacity", 1) * 100
                        onValueEdited: function (v) { root.opacityEdited(v / 100) }
                    }
                    KeyframeRow {
                        visible: root.isAdjust
                        label: qsTr("Opacity")
                        count: root.keyCount("opacityKf")
                        onAddRequested: root.opacityKeyframeAdded()
                        onClearRequested: root.opacityKeyframesCleared()
                    }
                }

                InspectorSection {
                    objectName: "stabilizeSection"
                    width: parent.width
                    title: qsTr("Stabilise")
                    ai: true
                    visible: !root.isAudio && !root.isTitle
                    modified: root.hasStab
                    onReset: root.removeStabRequested()

                    SliderField {
                        id: shakinessField
                        tip: qsTr("How much camera shake the analysis should expect, 1 to 10")
                        width: parent.width
                        label: qsTr("Shakiness")
                        from: 1; to: 10; origin: 4; decimals: 0
                        // The field never writes its own value: hold what the
                        // drag chose until another clip is shown.
                        property var chosen: undefined
                        value: chosen !== undefined ? chosen : root.hasStab ? root.shownClip.stab.shakiness : 4
                        onEdited: function (v) { chosen = v }
                        Connections { target: root; function onClipChanged() { shakinessField.chosen = undefined } }
                    }
                    SliderField {
                        id: smoothingField
                        tip: qsTr("How many frames the motion is averaged over; higher is steadier")
                        width: parent.width
                        label: qsTr("Smoothing")
                        from: 0; to: 100; origin: 15; decimals: 0
                        property var chosen: undefined
                        value: chosen !== undefined ? chosen : root.hasStab ? root.shownClip.stab.smoothing : 15
                        onEdited: function (v) { chosen = v }
                        Connections { target: root; function onClipChanged() { smoothingField.chosen = undefined } }
                        // On a stabilised clip a new smoothing re-applies the
                        // same analysis at once.
                        onEditingFinished: function (v) {
                            if (root.hasStab && !root.stabMissing)
                                root.stabilizeRequested(Math.round(shakinessField.value), Math.round(v))
                        }
                    }
                    ToolButton {
                        objectName: "stabilizeButton"
                        iconName: "activity"
                        showLabel: true
                        enabled: !root.analysing
                        text: root.analysing ? qsTr("Analysing… %1%").arg(root.analysePercent)
                              : root.hasStab && !root.stabMissing ? qsTr("Analyse again")
                              : qsTr("Analyse & Stabilise")
                        onClicked: root.stabilizeRequested(Math.round(shakinessField.value),
                                                           Math.round(smoothingField.value))
                    }
                    Text {
                        width: parent.width
                        visible: root.hasStab
                        text: !root.hasStab ? ""
                              : root.stabMissing
                              ? qsTr("The analysis file is gone — analyse again to stabilise.")
                              : qsTr("Stabilised · smoothing %1").arg(root.shownClip.stab.smoothing)
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: root.stabMissing ? Theme.danger : Theme.textMuted
                    }
                }

                InspectorSection {
                    objectName: "lensSection"
                    width: parent.width
                    title: qsTr("Lens Correction")
                    visible: !root.isAudio && !root.isTitle
                    modified: root.hasLens
                    onReset: root.lensEdited(0, 0, 0.5, 0.5)

                    SliderField {
                        id: lensK1
                        tip: qsTr("Main barrel or pincushion correction for the taking lens")
                        width: parent.width
                        label: qsTr("Distortion")
                        from: -1; to: 1; origin: 0; decimals: 2
                        value: root.num("lensK1", 0)
                        onEditingFinished: function (v) { root.lensEdited(v, lensK2.value, lensCx.value, lensCy.value) }
                    }
                    SliderField {
                        id: lensK2
                        tip: qsTr("Second-order correction for what the main distortion dial leaves")
                        width: parent.width
                        label: qsTr("Fine distortion")
                        from: -1; to: 1; origin: 0; decimals: 2
                        value: root.num("lensK2", 0)
                        onEditingFinished: function (v) { root.lensEdited(lensK1.value, v, lensCx.value, lensCy.value) }
                    }
                    SliderField {
                        id: lensCx
                        tip: qsTr("Horizontal position of the lens centre the correction works from")
                        width: parent.width
                        label: qsTr("Centre X")
                        from: 0; to: 1; origin: 0.5; decimals: 2
                        value: root.num("lensCx", 0.5)
                        onEditingFinished: function (v) { root.lensEdited(lensK1.value, lensK2.value, v, lensCy.value) }
                    }
                    SliderField {
                        id: lensCy
                        tip: qsTr("Vertical position of the lens centre the correction works from")
                        width: parent.width
                        label: qsTr("Centre Y")
                        from: 0; to: 1; origin: 0.5; decimals: 2
                        value: root.num("lensCy", 0.5)
                        onEditingFinished: function (v) { root.lensEdited(lensK1.value, lensK2.value, lensCx.value, v) }
                    }
                }
            }

            // ── Audio ─────────────────────────────────────────────────────
            Column {
                width: parent.width
                visible: root.currentTab === 1
                spacing: 0

                EmptyState {
                    width: parent.width
                    height: 160
                    visible: root.isTitle
                    iconName: "volume-x"
                    title: qsTr("Titles carry no audio")
                }

                InspectorSection {
                    id: audioLevelSection
                    width: parent.width
                    title: qsTr("Level")
                    expanded: true
                    visible: !root.isTitle
                    modified: root.hasLevel
                    onReset: { root.volumeEdited(0); root.panEdited(0) }

                    SliderField {
                        width: parent.width
                        tip: qsTr("Level applied to this clip alone, in decibels")
                        label: qsTr("Volume")
                        from: -60; to: 12; origin: 0; decimals: 1; suffix: " dB"
                        value: root.num("volume", 0)
                        onEditingFinished: function (v) { root.volumeEdited(v) }
                    }
                    SliderField {
                        width: parent.width
                        tip: qsTr("Places the clip's sound between the left and right speakers")
                        label: qsTr("Pan")
                        from: -1; to: 1; origin: 0; decimals: 2
                        value: root.num("pan", 0)
                        onEditingFinished: function (v) { root.panEdited(v) }
                    }
                    KeyframeRow {
                        label: qsTr("Volume")
                        count: root.keyCount("volumeKf")
                        onAddRequested: root.volumeKeyframeAdded()
                        onClearRequested: root.volumeKeyframesCleared()
                    }
                }

                InspectorSection {
                    width: parent.width
                    title: qsTr("Equaliser")
                    expanded: true
                    visible: !root.isTitle
                    modified: root.hasEq
                    onReset: root.eqSelected("")

                    Row {
                        width: parent.width
                        spacing: Theme.s2
                        Text {
                            text: qsTr("Preset")
                            width: 84
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ComboField {
                            tip: qsTr("Tone-shaping preset applied to this clip's audio")
                            width: parent.width - 84 - Theme.s2 * 2
                            model: root.eqPresets.map(function (p) { return p.name })
                            currentIndex: {
                                const key = root.hasSelection && root.shownClip.eq ? root.shownClip.eq : ""
                                for (let i = 0; i < root.eqPresets.length; ++i)
                                    if (root.eqPresets[i].key === key) return i
                                return 0
                            }
                            onActivated: function (i) { root.eqSelected(root.eqPresets[i].key) }
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                InspectorSection {
                    objectName: "channelsSection"
                    width: parent.width
                    title: qsTr("Channels & loudness")
                    expanded: true
                    visible: !root.isTitle
                    modified: root.audioMap !== "" || root.flag("normalize")
                    onReset: { root.audioMapSelected(""); root.normalizeToggled(false) }

                    Row {
                        width: parent.width
                        spacing: Theme.s2
                        Text {
                            text: qsTr("Channels")
                            width: 84
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textSecondary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ComboField {
                            objectName: "audioMapCombo"
                            tip: qsTr("How the clip's channels reach the mix: as recorded, folded, swapped or one side")
                            width: parent.width - 84 - Theme.s2 * 2
                            model: root.audioMaps.map(function (p) { return p.name })
                            currentIndex: {
                                for (let i = 0; i < root.audioMaps.length; ++i)
                                    if (root.audioMaps[i].key === root.audioMap) return i
                                return 0
                            }
                            onActivated: function (i) { root.audioMapSelected(root.audioMaps[i].key) }
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Toggle {
                        objectName: "normalizeToggle"
                        tip: qsTr("Ride the clip towards −23 LUFS so clips match in loudness")
                        width: parent.width
                        label: qsTr("Normalise loudness (−23 LUFS)")
                        checked: root.flag("normalize")
                        onClicked: root.normalizeToggled(!root.flag("normalize"))
                    }
                }

                InspectorSection {
                    width: parent.width
                    title: qsTr("Audio fades")
                    expanded: true
                    visible: !root.isTitle

                    NumberRow {
                        width: parent.width
                        label: qsTr("Fade in")
                        value: root.num("fadeIn", 0)
                        from: 0; to: Math.max(0, root.num("frames", 1)); fps: root.num("fps", 25)
                        onValueEdited: function (v) { root.fadeLengthsEdited(v, root.num("fadeOut", 0)) }
                    }
                    NumberRow {
                        width: parent.width
                        label: qsTr("Fade out")
                        value: root.num("fadeOut", 0)
                        from: 0; to: Math.max(0, root.num("frames", 1)); fps: root.num("fps", 25)
                        onValueEdited: function (v) { root.fadeLengthsEdited(root.num("fadeIn", 0), v) }
                    }
                }
            }

            // ── Effects ───────────────────────────────────────────────────
            Column {
                width: parent.width
                visible: root.currentTab === 2
                spacing: 0

                InspectorSection {
                    objectName: "videoEffectsSection"
                    width: parent.width
                    title: qsTr("Video Effects")
                    expanded: true
                    resettable: false
                    visible: !root.isAudio
                    ToolButton {
                        objectName: "openVideoEffectsWindow"
                        iconName: "sliders-horizontal"; showLabel: true
                        text: qsTr("Effects window…")
                        tip: qsTr("Add and edit this clip's effects beside a preview of the clip alone, its matte or the timeline")
                        enabled: root.videoEffectsEditable
                        onClicked: root.videoEffectsWindowRequested()
                    }
                    VideoEffectStack {
                        width: parent.width
                        effects: root.shownClip && root.shownClip.videoEffects ? root.shownClip.videoEffects : []
                        catalogue: root.videoEffectCatalogue
                        pickingUid: root.keyPickUid
                        maskViewable: true
                        maskViewUid: root.maskViewUid
                        animationFrame: root.animationFrame
                        enabled: root.videoEffectsEditable
                        onEditRequested: function(action, uid, values) { root.videoEffectEdited(action, uid, values) }
                        onGestureStarted: root.pictureGestureStarted()
                        onGestureFinished: root.pictureGestureFinished()
                    }
                }

                InspectorSection {
                    id: transitionSection
                    width: parent.width
                    title: qsTr("Transitions")
                    expanded: true

                    Toggle {
                        objectName: "fadeInToggle"
                        tip: qsTr("Fade the clip up from nothing at its head")
                        width: parent.width
                        label: qsTr("Fade in")
                        checked: root.flag("fadeIn")
                        onClicked: root.fadeInToggled()
                    }
                    NumberRow {
                        objectName: "fadeInRow"
                        width: parent.width
                        visible: root.flag("fadeIn")
                        label: qsTr("Fade in length")
                        value: root.num("fadeIn", 0)
                        from: 1; to: Math.max(1, root.num("frames", 1)); fps: root.num("fps", 25)
                        onValueEdited: function (v) { root.fadeLengthsEdited(v, root.num("fadeOut", 0)) }
                    }
                    Toggle {
                        width: parent.width
                        tip: qsTr("Fade the clip away to nothing at its tail")
                        label: qsTr("Fade out")
                        checked: root.flag("fadeOut")
                        onClicked: root.fadeOutToggled()
                    }
                    NumberRow {
                        width: parent.width
                        visible: root.flag("fadeOut")
                        label: qsTr("Fade out length")
                        value: root.num("fadeOut", 0)
                        from: 1; to: Math.max(1, root.num("frames", 1)); fps: root.num("fps", 25)
                        onValueEdited: function (v) { root.fadeLengthsEdited(root.num("fadeIn", 0), v) }
                    }
                    Toggle {
                        width: parent.width
                        tip: qsTr("Dissolve from this clip into the next one across the cut")
                        label: qsTr("Cross dissolve into next clip")
                        checked: root.flag("mixOut")
                        onClicked: root.dissolveToggled()
                    }
                    NumberRow {
                        objectName: "dissolveRow"
                        width: parent.width
                        visible: root.flag("mixOut")
                        label: qsTr("Dissolve length")
                        value: root.num("mixOut", 0)
                        from: 1; to: Math.max(1, root.num("frames", 1) - 1); fps: root.num("fps", 25)
                        onValueEdited: function (v) { root.dissolveLengthEdited(v) }
                    }
                    ComboField {
                        objectName: "dissolveStyleCombo"
                        tip: qsTr("How this clip gives way to the next: a dissolve, a wipe, a push, a dip to colour")
                        width: parent.width
                        visible: root.flag("mixOut")
                        caption: qsTr("Style")
                        model: root.transitionStyles.map(function(t) { return t.name })
                        currentIndex: Math.max(0, root.transitionStyles.findIndex(function(t) { return t.key === root.mixStyleBase }))
                        onActivated: function (i) { root.dissolveStyleSelected(root.transitionStyles[i].key) }
                    }
                    SliderField {
                        objectName: "dissolveSoftness"
                        width: parent.width
                        visible: root.flag("mixOut") && root.mixStyleChoice.soft === true
                        label: qsTr("Edge softness")
                        tip: qsTr("How soft the wipe's travelling edge is: 0 is a hard line")
                        from: 0; to: 100; stepSize: 1; decimals: 0; suffix: "%"
                        value: root.mixStyleSettings.soft !== undefined ? root.mixStyleSettings.soft : 0
                        // Once, when the handle is let go: each restyle is an undo step.
                        onEditingFinished: function(v) { root.dissolveStyleSelected(root.mixStyleWith({soft: Math.round(v)})) }
                    }
                    ToolButton {
                        id: dipColourButton
                        objectName: "dissolveColour"
                        width: parent.width
                        visible: root.flag("mixOut") && root.mixStyleChoice.colour === true
                        iconName: "palette"; showLabel: true
                        text: qsTr("Dip colour…")
                        readonly property string colour: root.mixStyleSettings.colour !== undefined ? root.mixStyleSettings.colour : "#808080"
                        function choose(c) { root.dissolveStyleSelected(root.mixStyleWith({colour: String(c).slice(-6).toLowerCase()})) }
                        Rectangle {
                            width: Theme.szIcon; height: Theme.szIcon
                            anchors.right: parent.right; anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            color: dipColourButton.colour; radius: Theme.rControl
                            border.width: Theme.hairline; border.color: Theme.border
                        }
                        onClicked: { dipColourPicker.selectedColor = colour; dipColourPicker.open() }
                        D.ColorDialog {
                            id: dipColourPicker
                            title: qsTr("Dip colour")
                            options: D.ColorDialog.DontUseNativeDialog
                            onAccepted: dipColourButton.choose(selectedColor)
                        }
                    }
                    Row {
                        visible: root.flag("mixOut")
                        spacing: Theme.s1
                        ToolButton {
                            objectName: "dissolvePreview"
                            iconName: "play"; showLabel: true
                            text: qsTr("Preview")
                            tip: qsTr("Play through the transition, from a second before it to a second after")
                            onClicked: root.dissolvePreviewRequested()
                        }
                        ToolButton {
                            objectName: "dissolveReset"
                            iconName: "rotate-ccw"; showLabel: true
                            text: qsTr("Reset")
                            tip: qsTr("Put the transition's settings back as the style comes")
                            enabled: root.mixStyleStored.indexOf(":") >= 0
                            onClicked: root.dissolveStyleSelected(root.mixStyleBase)
                        }
                    }
                    ComboField {
                        objectName: "dissolveAlignCombo"
                        tip: qsTr("Where the dissolve sits on the cut, and whether the clips shorten")
                        width: parent.width
                        visible: root.flag("mixOut")
                        caption: qsTr("Alignment")
                        model: [qsTr("Overlap (clips shorten)"), qsTr("Centred on the cut"),
                                qsTr("Starts at the cut"), qsTr("Ends at the cut")].concat(
                                   root.dissolveAlign === "custom" ? [qsTr("Custom")] : [])
                        currentIndex: Math.max(0, root.dissolveAligns.indexOf(root.dissolveAlign))
                        onActivated: function (i) { if (root.dissolveAligns[i] !== "custom") root.dissolveAlignSelected(root.dissolveAligns[i]) }
                    }
                    Toggle {
                        width: parent.width
                        tip: qsTr("Turn the clip off so it plays as a gap without deleting it")
                        label: qsTr("Clip enabled")
                        visible: !root.onTrack
                        checked: !root.flag("disabled")
                        onClicked: root.enabledToggled()
                    }
                }

                InspectorSection {
                    id: appliedSection
                    objectName: "appliedSection"
                    width: parent.width
                    title: qsTr("Applied")
                    expanded: true
                    readonly property int count: (root.hasColor ? 1 : 0) + (root.hasCrop ? 1 : 0)
                                                 + (root.hasTransform ? 1 : 0) + (root.hasSpeed ? 1 : 0)
                                                 + (root.hasEq ? 1 : 0) + (root.hasLevel ? 1 : 0)
                                                 + (root.hasOpacity ? 1 : 0) + (root.hasBlend ? 1 : 0)
                                                 + (root.hasLens ? 1 : 0) + (root.hasStab ? 1 : 0)
                                                 + (root.shownClip && root.shownClip.videoEffects ? root.shownClip.videoEffects.length : 0)
                                                 + (root.flag("flipH") || root.flag("flipV") ? 1 : 0)
                                                 + (root.audioMap !== "" ? 1 : 0) + (root.flag("normalize") ? 1 : 0)

                    // Wraps on a narrow panel rather than cutting the second button off.
                    Flow {
                        objectName: "appliedActions"
                        width: parent.width
                        spacing: Theme.s2
                        ToolButton {
                            objectName: "pasteAttributesButton"
                            iconName: "clipboard"
                            text: qsTr("Paste Attributes…")
                            showLabel: true
                            enabled: root.canPasteAttributes
                            onClicked: root.pasteAttributesRequested()
                        }
                        ToolButton {
                            objectName: "savePresetButton"
                            iconName: "save"
                            text: qsTr("Save Preset…")
                            showLabel: true
                            enabled: appliedSection.count > 0
                            onClicked: root.savePresetRequested()
                        }
                    }
                    Row {
                        spacing: Theme.s2
                        visible: !root.isTitle && !root.flag("gap")
                        ToolButton {
                            objectName: "renderReplaceButton"
                            iconName: "film"
                            text: root.rendering ? qsTr("Rendering… %1%").arg(root.renderPercent)
                                                 : root.flag("rendered") ? qsTr("Restore Original")
                                                                         : qsTr("Render in Place")
                            showLabel: true
                            enabled: !root.rendering
                            onClicked: root.flag("rendered") ? root.restoreOriginalRequested() : root.renderRequested()
                        }
                    }
                    Repeater {
                        model: root.appliedEffects
                        delegate: AppliedRow {
                            required property var modelData
                            required property int index
                            objectName: "applied:" + modelData.key
                            title: modelData.title
                            key: modelData.key === "speed" ? "" : modelData.key
                            onRemoveRequested: root.removeEffect(modelData.key)
                        }
                    }
                    EmptyState {
                        width: parent.width
                        height: 100
                        visible: appliedSection.count === 0
                        iconName: "wand-sparkles"
                        title: qsTr("Nothing applied")
                        description: qsTr("Colour, crop, transform, retime and audio settings appear here.")
                    }
                }
            }
        }
    }
}
