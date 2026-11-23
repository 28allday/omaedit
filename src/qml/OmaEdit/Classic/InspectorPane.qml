import QtQuick
import QtQuick.Layouts
import omaedit
import OmaEdit.Edit as Edit

// The Edit-page Inspector bound to the window's selection: a storyline clip
// (selectedTimelineIndex) or a track clip (selectedConnectedIndex). The seam
// between Edit.InspectorPanel and the engine's per-clip invokables.
Edit.InspectorPanel {
    id: inspector
    objectName: "inspector"
    timelineTimecode: backend.startFrame >= 0 && backend.fps >= 0 ? function (frame) { return backend.timecode(frame) } : null

    readonly property bool trackSelected: win.selectedConnectedIndex >= 0
                                    && win.selectedConnectedIndex < backend.connected.length
    readonly property var entry: trackSelected ? backend.connected[win.selectedConnectedIndex]
                                 : win.selectedTimelineIndex >= 0
                                   && win.selectedTimelineIndex < backend.timeline.length
                                   ? backend.timeline[win.selectedTimelineIndex] : null

    // A V1 clip's sound lives on its linked audio clip: the Audio tab
    // reads and edits THAT clip when the video is selected.
    readonly property int audioMate: !trackSelected && entry !== null && entry.videoOnly === true
                                     ? win.clipCommands.audioMateIndex(entry) : -1
    readonly property var audioEntry: audioSrc()
    // Read in place (not through the cached property): the clip map can
    // re-evaluate before the property catches up with a new entry.
    function audioSrc() {
        var a = audioMate >= 0 && audioMate < backend.connected.length ? backend.connected[audioMate] : null
        return a !== null && a !== undefined ? a : entry
    }
    Layout.preferredWidth: Math.max(280, Math.round(parent.width * 0.22))
    Layout.maximumWidth: 440
    Layout.fillHeight: true

    // The mapped tempo as this clip plays it: at half speed, half the tempo.
    // A speed map has no one tempo, so it shows the recording's own.
    tempoText: {
        if (entry === null || entry.path === undefined || backend.beatMapRevision === 0) return ""
        const bpm = backend.beatMapOf(entry.path).bpm
        if (!(bpm > 0)) return ""
        const rate = entry.retime !== undefined || entry.speed === undefined || entry.speed === 0 ? 1 : Math.abs(entry.speed)
        return qsTr("%1 BPM").arg((bpm * rate).toFixed(1))
    }
    shownClip: entry === null ? null : {
        name: entry.title !== undefined && entry.title !== "" ? entry.title : entry.name,
        kind: entry.title !== undefined && entry.title !== "" ? "title"
            : entry.audioOnly === true ? "audio" : "video",
        adjust: entry.adjust === true, generator: entry.generator !== undefined ? entry.generator : "",
        generatorColor: entry.generatorColor !== undefined ? entry.generatorColor : "",
        connected: trackSelected,
        speed: entry.speed, retime: entry.retime, volume: audioSrc().volume, pan: audioSrc().pan, eq: audioSrc().eq,
        opacity: entry.opacity,
        rectX: entry.rectX, rectY: entry.rectY, rectW: entry.rectW, rectH: entry.rectH,
        colorB: entry.colorB, colorC: entry.colorC, colorS: entry.colorS,
        fadeIn: entry.fadeIn, fadeOut: entry.fadeOut, mixOut: entry.mixOut, mixAlign: entry.mixAlign, mixStyle: entry.mixStyle,
        disabled: entry.disabled, title: entry.title, caption: entry.caption === true, captionLook: captionLook,
        titleSize: entry.titleSize, titleAlign: entry.titleAlign, titleWeight: entry.titleWeight,
        titleItalic: entry.titleItalic, titleBg: entry.titleBg, titleOutline: entry.titleOutline,
        rotation: entry.rotation,
        flipH: entry.flipH, flipV: entry.flipV, freeze: entry.freeze,
        audioMap: audioSrc().audioMap, normalize: audioSrc().normalize,
        cropL: entry.cropL, cropR: entry.cropR, cropT: entry.cropT, cropB: entry.cropB,
        cropFeather: entry.cropFeather, cropInvert: entry.cropInvert,
        anchorX: entry.anchorX, anchorY: entry.anchorY, frameBlend: entry.frameBlend,
        "in": entry.in, out: entry.out, length: entry.length, frames: entry.frames,
        start: entry.start, fps: backend.fps,
        bypass: entry.bypass, editKeys: entry.editKeys, effectOrder: entry.effectOrder, videoEffects: entry.videoEffects,
        volumeKf: audioSrc().volumeKf, opacityKf: entry.opacityKf,
        blend: entry.blend, lensK1: entry.lensK1, lensK2: entry.lensK2,
        lensCx: entry.lensCx, lensCy: entry.lensCy, stab: entry.stab,
        scaling: entry.scaling, timelineScaling: backend.projectSettings.scaling || "fit",
        sourceWidth: media ? media.width || 0 : 0, sourceHeight: media ? media.height || 0 : 0,
        frameWidth: backend.videoWidth, frameHeight: backend.videoHeight
    }
    // The selected caption's whole look, read again whenever the clips or the
    // tracks (its track's style) change.
    readonly property var captionLook: trackSelected && entry !== null && entry.caption === true && backend.tracks.length > 0
                                       ? backend.captionLook(win.selectedConnectedIndex) : null
    // The clip's media in the library: its picture size decides whether
    // Scaling is offered.
    readonly property var media: {
        if (!entry || entry.audioOnly === true || entry.gap === true || entry.title) return null
        const i = win.matching.libraryIndexForEntry(entry)
        return i >= 0 ? backend.library[i] : null
    }
    analysing: entry !== null && backend.stabJob.track !== undefined
               && backend.stabJob.track === entry.track && backend.stabJob.index === entry.index
    analysePercent: analysing ? backend.stabJob.percent : 0
    animationFrame: entry ? Math.max(0,Math.min(entry.frames-1,backend.position-entry.start)) : 0
    pictureEditable: entry !== null && entry.audioOnly !== true && entry.gap !== true
                     && !backend.exporting && !!backend.tracks[entry.track] && !backend.tracks[entry.track].locked
    function picturePatch(patch) {
        if(!pictureEditable)return
        const keys=JSON.parse(JSON.stringify(entry.editKeys || {}))
        const out={}
        Object.keys(patch).forEach(function(key) {
            if(keys[key] && keys[key].length) {
                // Group controls submit their companion values too. Do not
                // add keys to unchanged companions, or replace easing when
                // adjusting the value of an existing keyframe.
                if (Math.abs(animationValue(key, 0) - patch[key]) < 0.000000001) return
                const previous = keys[key].filter(function(k) { return k.frame <= animationFrame })
                const mode = previous.length ? previous[previous.length-1].interpolation : "linear"
                const list=keys[key].filter(function(k){return k.frame!==animationFrame})
                list.push({frame:animationFrame,value:patch[key],interpolation:mode});list.sort(function(a,b){return a.frame-b.frame});keys[key]=list
            } else out[key]=patch[key]
        })
        out.editKeys=keys
        backend.setEditProperties(entry.track,entry.index,out)
    }
    onPictureEdited: function(patch) { picturePatch(patch) }
    onPictureGestureStarted: backend.beginEditGesture()
    onPictureGestureFinished: backend.endEditGesture()
    onAnimationEdited: function(patch) { if(pictureEditable) backend.setEditProperties(entry.track,entry.index,patch) }
    onMotionPresetRequested: function(preset, options) {
        if (pictureEditable) backend.applyMotionPreset(entry.track, entry.index, preset, options)
    }
    onEditKeysEdited: function(keys) { if(pictureEditable)backend.setEditProperties(entry.track,entry.index,{editKeys:keys}) }
    onAnimationFrameRequested: function(frame) { if(entry){backend.viewTimeline();backend.seek(entry.start+frame)} }
    onPicturePropertyReset:function(key,value){
        if(!pictureEditable)return
        if (key === "opacity") { backend.setEditProperties(entry.track,entry.index,{opacity:value,opacityKf:[]}); return }
        const keys=JSON.parse(JSON.stringify(entry.editKeys || {}));delete keys[key]
        const patch={editKeys:keys};patch[key]=value;backend.setEditProperties(entry.track,entry.index,patch)
    }
    onPictureReset: function(group) {
        if(!entry)return
        const defaults={rect:{rectX:0,rectY:0,rectW:1,rectH:1,rotation:0,anchorX:.5,anchorY:.5,opacity:1,opacityKf:[]},crop:{cropL:0,cropR:0,cropT:0,cropB:0,cropFeather:0,cropInvert:false},color:{colorB:0,colorC:1,colorS:1}}
        const patch=defaults[group],keys=JSON.parse(JSON.stringify(entry.editKeys || {}))
        Object.keys(patch).forEach(function(k){delete keys[k]});patch.editKeys=keys
        backend.setEditProperties(entry.track,entry.index,patch)
    }
    onEffectMoved: function(key,direction) {
        if(!entry)return
        const order=appliedEffects.map(function(e){return e.key}).filter(function(k){return k!=="speed"})
        const at=order.indexOf(key),to=at+direction
        if(at<0||to<0||to>=order.length)return
        order.splice(at,1);order.splice(to,0,key)
        backend.setEditProperties(entry.track,entry.index,{effectOrder:order})
    }
    onDissolveAlignSelected: function (align) { if (entry) backend.setDissolveAlign(entry.track, entry.index, align) }
    transitionStyles: backend.transitionStyles
    onDissolveStyleSelected: function (style) { if (entry) backend.applyTransitionAt(entry.track, entry.index, style) }
    // The cut the transition is on, with a second of each clip around it.
    onDissolvePreviewRequested: {
        if (!entry) return
        const cut = entry.start + entry.frames, roll = Math.round(backend.fps) + (entry.mixOut || 0)
        win.playing.playBetween(Math.max(0, cut - roll), Math.min(backend.duration - 1, cut + roll))
    }
    // Keyframes are clip-relative; a new one takes the playhead's frame
    // (clamped into the clip) and the value the clip has now.
    function keyframeFrame() {
        return Math.max(0, Math.min(entry.frames - 1, backend.position - entry.start))
    }
    // The value the keyframed curve has at `frame` (held flat outside
    // the keys), or `fallback` when there are none.
    function valueAtKey(list, valueName, frame, fallback) {
        var have = list !== undefined && list !== null ? list : []
        if (have.length === 0)
            return fallback
        var before = null, after = null
        for (var i = 0; i < have.length; i++) {
            if (have[i].frame <= frame)
                before = have[i]
            else if (after === null)
                after = have[i]
        }
        if (before === null)
            return after[valueName]
        if (after === null)
            return before[valueName]
        if (after.frame === before.frame)
            return before[valueName]
        if (valueName === "alpha" && before.interpolation === "hold") return before[valueName]
        var u = (frame - before.frame) / (after.frame - before.frame)
        if (valueName === "alpha" && before.interpolation === "smooth") u = u * u * (3 - 2 * u)
        return before[valueName] + (after[valueName] - before[valueName]) * u
    }
    function withKey(list, key, valueName, value) {
        var out = []
        var placed = false
        var frame = keyframeFrame()
        var have = list !== undefined && list !== null ? list : []
        for (var i = 0; i < have.length; i++) {
            if (have[i].frame === frame) {
                placed = true
                var k = JSON.parse(JSON.stringify(have[i]))
                k.frame = frame
                k[valueName] = value
                out.push(k)
            } else {
                out.push(have[i])
            }
        }
        if (!placed) {
            var n = {}
            n.frame = frame
            n[valueName] = value
            if (valueName === "alpha") {
                var earlier = have.filter(function(k) { return k.frame < frame })
                n.interpolation = earlier.length ? earlier[earlier.length-1].interpolation || "linear" : "linear"
            }
            out.push(n)
        }
        out.sort(function (a, b) { return a.frame - b.frame })
        return out
    }
    onVolumeKeyframeAdded: {
        if (!entry)
            return
        var kf = withKey(audioEntry.volumeKf, "frame", "db",
                         valueAtKey(audioEntry.volumeKf, "db", keyframeFrame(),
                                    audioEntry.volume !== undefined ? audioEntry.volume : 0))
        if (audioMate >= 0)
            backend.setConnectedVolumeKf(audioMate, kf)
        else if (trackSelected)
            backend.setConnectedVolumeKf(win.selectedConnectedIndex, kf)
        else
            backend.setClipVolumeKf(win.selectedTimelineIndex, kf)
    }
    onVolumeKeyframesCleared: {
        if (!entry)
            return
        if (audioMate >= 0)
            backend.setConnectedVolumeKf(audioMate, [])
        else if (trackSelected)
            backend.setConnectedVolumeKf(win.selectedConnectedIndex, [])
        else
            backend.setClipVolumeKf(win.selectedTimelineIndex, [])
    }
    onOpacityKeyframeAdded: {
        if (!entry)return
        if(!trackSelected){backend.setEditProperties(entry.track,entry.index,{opacityKf:withKey(entry.opacityKf,"frame","alpha",valueAtKey(entry.opacityKf,"alpha",keyframeFrame(),entry.opacity!==undefined?entry.opacity:1))});return}
        backend.setConnectedOpacityKf(win.selectedConnectedIndex,
                                      withKey(entry.opacityKf, "frame", "alpha",
                                              valueAtKey(entry.opacityKf, "alpha", keyframeFrame(),
                                                         entry.opacity !== undefined ? entry.opacity : 1)))
    }
    onOpacityKeyframesCleared: if(pictureEditable) backend.setEditProperties(entry.track,entry.index,
        {opacity: animationValue("opacity", entry.opacity !== undefined ? entry.opacity : 1), opacityKf: []})
    onBlendSelected: function (mode) { if (entry) backend.setBlend(entry.track, entry.index, mode) }
    onScalingSelected: function (scaling) { if (entry) backend.setClipScaling(entry.track, entry.index, scaling) }
    canPasteAttributes: win.attributes.canPaste
    rendering: entry !== null && backend.renderJob.track !== undefined
               && backend.renderJob.track === entry.track && backend.renderJob.index === entry.index
    renderPercent: rendering ? backend.renderJob.percent : 0
    onRenderRequested: win.clipCommands.renderOrRestoreSelected()
    onRestoreOriginalRequested: win.clipCommands.renderOrRestoreSelected()
    onPasteAttributesRequested: win.attributes.paste()
    onSavePresetRequested: win.attributes.savePresetFromSelection()
    onLensEdited: function (k1, k2, cx, cy) { if (entry) backend.setLens(entry.track, entry.index, k1, k2, cx, cy) }
    videoEffectCatalogue: backend.videoEffectCatalogue
    keyPickUid: win.keyPick ? win.keyPick.uid : ""
    maskViewUid: win.maskEditUid
    videoEffectsEditable: entry !== null && !backend.exporting && !!backend.tracks[entry.track] && !backend.tracks[entry.track].locked
    onVideoEffectsWindowRequested: effectsWindow.open()
    Edit.VideoEffectsWindow {
        id: effectsWindow
        entry: inspector.entry
        catalogue: backend.videoEffectCatalogue
        onEdited: function(action, uid, values) { inspector.videoEffectEdited(action, uid, values) }
        onGestureStarted: inspector.pictureGestureStarted()
        onGestureFinished: inspector.pictureGestureFinished()
    }
    onVideoEffectEdited: function(action, uid, values) {
        if (!entry) return
        const frame = Math.max(0, Math.min(entry.frames - 1, backend.position - entry.start))
        if (action === "pickBackdrop") {
            // Armed: the next click on the program picture picks it.
            win.keyPick = win.keyPick && win.keyPick.uid === uid ? null : {track: entry.track, index: entry.index, uid: uid, clipFrame: frame}
            return
        }
        if (action === "maskView") {
            // The mask's handles on the program picture: on for this effect, or off.
            win.maskEditUid = values.on === true || (values.on === undefined && win.maskEditUid !== uid) ? uid : ""
            return
        }
        if (action === "findBackdrop") values = {clipFrame: frame}
        backend.editVideoEffect(entry.track, entry.index, action, uid, values)
    }
    onStabilizeRequested: function (shakiness, smoothing) {
        if (entry)
            backend.stabilize(entry.track, entry.index, shakiness, smoothing)
    }
    onRemoveStabRequested: if (entry) backend.removeStab(entry.track, entry.index)

    canOpenSpeedDuration: win.retiming.canRetimeSelection()
    onSpeedDurationRequested: speedDurationDialog.openForSelection()
    onSpeedSelected: function (speed) { win.retiming.setSelectedSpeed(speed) }
    onBypassToggled: function (key, on) {
        if (entry)
            backend.setBypass(entry.track, entry.index, key, on)
    }
    onOpacityEdited: function (alpha) {
        if (!pictureEditable) return
        const animated = entry.opacityKf && entry.opacityKf.length > 0
        backend.setEditProperties(entry.track,entry.index, animated
            ? {opacityKf: withKey(entry.opacityKf, "frame", "alpha", alpha)} : {opacity:alpha})
    }
    onRectEdited: function (x, y, w, h) {
        picturePatch({rectX:x,rectY:y,rectW:w,rectH:h})
    }
    onRotationEdited: function (degrees) {
        picturePatch({rotation:degrees})
    }
    onAudioMapSelected: function (mode) {
        if (audioMate >= 0) backend.setAudioMap(audioEntry.track, audioEntry.index, mode)
        else if (trackSelected) backend.setAudioMap(entry.track, entry.index, mode)
        else if (win.selectedTimelineIndex >= 0) backend.setAudioMap(0, win.selectedTimelineIndex, mode)
    }
    onNormalizeToggled: function (on) {
        if (audioMate >= 0) backend.setNormalize(audioEntry.track, audioEntry.index, on)
        else if (trackSelected) backend.setNormalize(entry.track, entry.index, on)
        else if (win.selectedTimelineIndex >= 0) backend.setNormalize(0, win.selectedTimelineIndex, on)
    }
    onFlipEdited: function (h, v) {
        if (trackSelected) backend.setFlip(entry.track, entry.index, h, v)
        else if (win.selectedTimelineIndex >= 0) backend.setFlip(0, win.selectedTimelineIndex, h, v)
    }
    onCropEdited: function(l,r,t,b) { picturePatch({cropL:l,cropR:r,cropT:t,cropB:b}) }
    onCropEdgeEdited: function(feather,invert) { picturePatch({cropFeather:feather,cropInvert:invert}) }
    onAnchorEdited: function (ax, ay) {
        picturePatch({anchorX:ax,anchorY:ay})
    }
    onFrameBlendSelected: function (mode) { if (entry) backend.setFrameBlend(entry.track, entry.index, mode) }
    onGeneratorColorSelected: function (color) { if (entry) backend.setGeneratorColor(entry.track, entry.index, color) }

    // ── Transcript: the selected clip's library entry, else the browser's ──
    readonly property int transcriptIndex: entry !== null && entry.path !== undefined
                                           ? win.matching.libraryIndexForEntry(entry, undefined) : win.selectedLibraryIndex
    readonly property var transcriptEntry: transcriptIndex >= 0 && transcriptIndex < backend.library.length
                                           ? backend.library[transcriptIndex] : null
    transcriptClip: transcriptEntry === null ? null : {
        name: transcriptEntry.name, libraryIndex: transcriptIndex,
        transcript: transcriptEntry.transcript !== undefined ? transcriptEntry.transcript : [],
        language: transcriptEntry.transcriptLanguage !== undefined ? transcriptEntry.transcriptLanguage : "",
        hasAudio: transcriptEntry.hasAudio !== false
    }
    transcribing: backend.transcriber.job.index !== undefined && backend.transcriber.job.index >= 0
                  && backend.transcriber.job.index === transcriptIndex
    transcribeStage: transcribing ? backend.transcriber.job.stage : ""
    transcribePercent: transcribing ? backend.transcriber.job.percent : 0
    transcriptError: backend.transcriber.job.error !== undefined && backend.transcriber.job.forIndex === transcriptIndex
                     ? backend.transcriber.job.error : ""
    transcriptAvailable: backend.transcriber.available
    transcriptStatus: backend.transcriber.status
    captionsAvailable: backend.hasVideo
    onTranscribeRequested: if (transcriptIndex >= 0) backend.transcribe(transcriptIndex)
    onMakeCaptionsRequested: backend.generateCaptions()
    onTranscribeCancelled: backend.transcriber.cancel()
    onTranscriptCleared: if (transcriptIndex >= 0) backend.clearTranscript(transcriptIndex)
    // A line → the I/O range over those frames of the selected timeline
    // clip (speed honoured); a library-only clip, or a line outside what
    // the clip uses, ranges the SOURCE viewer instead.
    onTranscriptSegmentActivated: function (startMs, endMs) {
        var fps = backend.fps > 0 ? backend.fps : 25
        var srcIn = Math.round(startMs / 1000 * fps)
        var srcOut = Math.max(srcIn, Math.round(endMs / 1000 * fps) - 1)
        if (entry !== null && entry.path !== undefined && transcriptIndex >= 0) {
            var a = Math.abs(entry.speed !== undefined && entry.speed !== 0 ? entry.speed : 1)
            var tIn = Math.round(entry.start + srcIn / a - entry.in)
            var tOut = Math.round(entry.start + (srcOut + 1) / a - entry.in) - 1
            var clipEnd = entry.start + entry.frames - 1
            if (tOut >= entry.start && tIn <= clipEnd && entry.speed >= 0) {
                tIn = Math.max(entry.start, tIn)
                tOut = Math.max(tIn, Math.min(clipEnd, tOut))
                backend.viewTimeline()
                win.timelineRangeIn = tIn
                win.timelineRangeOut = tOut
                backend.seek(tIn)
                backend.announce(qsTr("Ranged %1 – %2 on the timeline").arg(backend.timecode(tIn)).arg(backend.timecode(tOut)))
                return
            }
        }
        if (transcriptIndex >= 0)
            backend.previewClipAt(transcriptIndex, srcIn, srcIn, srcOut)
    }
    onFadeLengthsEdited: function (fadeIn, fadeOut) {
        if (trackSelected) backend.setConnectedFadeLengths(win.selectedConnectedIndex, fadeIn, fadeOut)
        else if (win.selectedTimelineIndex >= 0) backend.setFadeLengths(win.selectedTimelineIndex, fadeIn, fadeOut)
    }
    onDissolveLengthEdited: function (frames) {
        if (trackSelected) backend.resizeConnectedDissolve(win.selectedConnectedIndex, frames)
        else if (win.selectedTimelineIndex >= 0) backend.resizeDissolve(win.selectedTimelineIndex, frames)
    }
    onTrimEdited: function (inPoint, outPoint) {
        if (!entry || outPoint < inPoint)
            return
        if (trackSelected) backend.trimConnectedClip(win.selectedConnectedIndex, inPoint, outPoint, entry.start)
        else if (win.selectedTimelineIndex >= 0) backend.trimClip(win.selectedTimelineIndex, inPoint, outPoint)
    }
    onColorEdited: function(b,c,s) { picturePatch({colorB:b,colorC:c,colorS:s}) }
    // The Inspector's own text field: the same title, new words.
    onTitleTextEdited: function (text) {
        if (!trackSelected || !entry) return
        backend.updateTitle(win.selectedConnectedIndex, text, win.catalogue.titleStyle(entry))
    }
    onCaptionLookEdited: function (look) { if (trackSelected && entry) backend.setCaptionLook(win.selectedConnectedIndex, look) }
    onCaptionSpeakerEdited: function (speaker) { if (trackSelected && entry) backend.setCaptionSpeaker([win.selectedConnectedIndex], speaker) }
    onCaptionTrackStyleToggled: function (on) { if (trackSelected && entry) backend.useTrackCaptionStyle(win.selectedConnectedIndex, on) }
    onTitleEditRequested: if (trackSelected && entry)
        titleDialog.openFor(win.selectedConnectedIndex, entry.title, entry.titlePlace,
                            entry.titleSize, entry.titleFont, entry.titleColor,
                            win.catalogue.titleExtras(entry))
    onVolumeEdited: function (db) { audioMate >= 0 ? backend.setConnectedVolume(audioMate, db) : win.sound.setVolume(db) }
    onPanEdited: function (pan) { audioMate >= 0 ? backend.setConnectedPan(audioMate, pan) : win.sound.setPan(pan) }
    onEqSelected: function (preset) { audioMate >= 0 ? backend.setConnectedEq(audioMate, preset) : win.sound.setEq(preset) }
    onFadeInToggled: trackSelected ? backend.toggleConnectedFade(win.selectedConnectedIndex, false)
                             : backend.toggleFade(win.selectedTimelineIndex, false)
    onFadeOutToggled: trackSelected ? backend.toggleConnectedFade(win.selectedConnectedIndex, true)
                              : backend.toggleFade(win.selectedTimelineIndex, true)
    onDissolveToggled: trackSelected ? backend.toggleConnectedDissolve(win.selectedConnectedIndex)
                               : backend.toggleDissolve(win.selectedTimelineIndex)
    onEnabledToggled: if (!trackSelected && win.selectedTimelineIndex >= 0)
        backend.toggleClipDisabled(win.selectedTimelineIndex)

    // Seam against the viewer well.
    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: win.seamColor
        z: 2
    }
}
