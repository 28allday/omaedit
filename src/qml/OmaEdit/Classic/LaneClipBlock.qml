import QtQuick
import QtQuick.Controls
import QtQuick.Controls as C
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Edit as Edit
import "../Ui/MediaTime.js" as MediaTime

// One connected-clip block, shared by the video strip above
// the storyline and the audio strip below it. `strip` is the
// containing Item (drag/trim coordinates map through it);
// `below` says which strip this instance renders — blocks of
// the other kind simply stay invisible there.
Edit.TimelineClip {
    // The timeline's scroll area: zoom, the view's edges, pending trims and drops.
    required property Item scroller
    fps: backend.fps > 0 ? backend.fps : 25
    id: laneBlock
    required property var modelData
    required property int sourceIndex
    // The block's own place: it meets its neighbour at the cut a transition
    // sits across (see StoryClipBlock).
    required property real timelineStart
    required property real timelineFrames
    required property int blockLead
    required property int blockTail
    required property int blockBefore
    required property int mixMostBefore
    required property int mixMostAfter
    readonly property int index: sourceIndex
    objectName: "laneClip:" + index
    property Item strip
    property bool below: false
    selected: index === win.selectedConnectedIndex
              || win.selection.isSelectedAt(modelData.track, modelData.index)
    dropTarget: index === scroller.dropConnectedIndex
    // Group drag: this block leads it, or rides another's.
    property bool groupLead: false
    readonly property bool groupRiding:
        win.groupDragActive && selected && !groupLead
    readonly property bool audioOnly:
        modelData.audioOnly === true
    readonly property bool isTitle:
        modelData.title !== undefined
        && modelData.title !== ""
    // This clip's display row among lanes of its
    // kind, counted outward from the storyline.
    readonly property int laneRow: {
        var list = audioOnly ? win.laneKinds.audio
                             : win.laneKinds.video
        return list.indexOf(modelData.lane) + 1
    }
    readonly property bool inView:
        scroller.spans(x, width)
    // (A sound whose picture's cut is rolled right over it is gone from
    // under the picture, and is not drawn at the least width a block has.)
    visible: audioOnly === below
             && (inView || dragging || selected)
             && !(pictureTrim && pictureTrim.rolled && modelData.frames - followIn + followOut <= 0)
    property real dragOffset: 0
    // Vertical drag: whole rows, towards another track
    // of the same kind (or a new one past the last).
    property int dragRows: 0
    // Live trim preview, storyline-style: pending
    // frame deltas committed on release.
    property int dIn: 0
    property int dOut: 0
    // A picture shares its pending trim with its linked sound;
    // a sound draws the trim its picture is being given.
    onDInChanged: if (!audioOnly) scroller.setLinkTrim(modelData.link, dIn, dOut, true, rolling)
    onDOutChanged: if (!audioOnly) scroller.setLinkTrim(modelData.link, dIn, dOut, true, rolling)
    // A cut being rolled is drawn by both of its blocks (see StoryClipBlock).
    readonly property int rollIn: scroller.rollOf(modelData.track, modelData.index, false)
    readonly property int rollOut: scroller.rollOf(modelData.track, modelData.index, true)
    readonly property bool rolling: rollIn !== 0 || rollOut !== 0
    onRollInChanged: dIn = rollIn
    onRollOutChanged: dOut = rollOut
    // A drag that never gets its release (the pointer was taken away by a
    // dialog, or this block went while the button was held, as it does when
    // an undo or a finished render rebuilds the row) leaves nothing behind:
    // no block left displaced, no linked sound left drawn as if trimmed.
    function abandonDrag() {
        const lead = groupLead
        dIn = 0
        dOut = 0
        slipDelta = 0
        slideDelta = 0
        dragOffset = 0
        dragRows = 0
        if (lead) {
            groupLead = false
            win.groupDragActive = false
            win.groupDragPx = 0
            win.groupDragRows = 0
        }
    }
    readonly property bool dragPending: dIn !== 0 || dOut !== 0 || slipDelta !== 0 || slideDelta !== 0
                                        || dragOffset !== 0 || dragRows !== 0 || groupLead
    // A cut pressed on here and being dragged: the press goes with the
    // block (made again for a change elsewhere in the timeline), and the
    // roll goes with the press.
    Component.onDestruction: {
        if (laneArea.pressed && laneArea.rollsCut && scroller.rollIndex >= 0 && !scroller.rollHeld)
            scroller.endRoll()
        if (dragPending)
            abandonDrag()
    }
    readonly property var pictureTrim: audioOnly && modelData.link !== undefined && modelData.link !== ""
                                       ? scroller.linkTrims[modelData.link] : undefined
    // (Where its picture's cut is rolled it follows as far as it has sound:
    // the sound a multicam clip takes from one source ends where that does.)
    readonly property int followIn: !pictureTrim ? 0
        : pictureTrim.rolled ? Math.max(-modelData.in, -modelData.start, Math.min(modelData.frames, pictureTrim.dIn)) : pictureTrim.dIn
    readonly property int followOut: !pictureTrim ? 0
        : pictureTrim.rolled ? Math.max(-modelData.frames, Math.min(modelData.length - 1 - modelData.out, pictureTrim.dOut)) : pictureTrim.dOut
    readonly property int followShift: pictureTrim && pictureTrim.movesStart ? followIn : 0
    readonly property real followPx: audioOnly && scroller.shuffleLink !== ""
                                     && modelData.link === scroller.shuffleLink
                                     ? scroller.shufflePx : 0
    property int slipDelta: 0
    function clampSlip(d) {
        return Math.max(-modelData.in,
                        Math.min(modelData.length - 1 - modelData.out, d))
    }
    function commitSlip() {
        var d = slipDelta
        slipDelta = 0
        if (d !== 0)
            backend.trimConnectedClip(index, modelData.in + d,
                                      modelData.out + d, modelData.start)
    }
    // Slide tool: pending move between the neighbours.
    property int slideDelta: 0
    function clampSlide(d) {
        var b = win.trimming.slideBounds(modelData.track, modelData.index)
        return b === null ? 0 : Math.max(b[0], Math.min(b[1], d))
    }
    function commitSlide() {
        var d = slideDelta
        slideDelta = 0
        if (d !== 0)
            backend.slideClip(modelData.track, modelData.index, d)
    }
    x: (scroller.shownFrames > 0
        ? (timelineStart + dIn + followShift + slideDelta) / scroller.shownFrames
          * laneBlock.strip.width : 0)
       + (groupRiding ? win.groupDragPx : dragOffset) + followPx
    width: scroller.shownFrames > 0
           ? Math.max(modelData.audioTiming ? 1 : 6, (timelineFrames - dIn + dOut - followIn + followOut)
                         / scroller.shownFrames
                         * laneBlock.strip.width)
           : 0
    // Above the storyline higher lanes stack upwards;
    // below it audio lanes grow downwards.
    y: win.lanes.laneY(modelData.track,below) + 2
       + win.lanes.laneDragOffset(modelData.track,below,groupRiding && audioOnly === win.groupDragAudio ? win.groupDragRows : dragRows)
    height: win.lanes.trackHeightFor(modelData.track) - 4

    // The picture (Edit.TimelineClip) from the entry.
    markers: scroller.blockFlags(win.clipMarkers.flags(modelData), modelData.frames, blockLead, timelineFrames)
    onMarkerClicked: function (i) {
        backend.viewTimeline()
        backend.seek(Math.round(timelineStart + markers[i].frac * timelineFrames))
    }
    name: modelData.mcam !== undefined ? win.mcamAngleName(modelData)
          : modelData.caption === true ? qsTr("CC · %1").arg(modelData.name) : modelData.name
    // (A recording an older project holds on a video row is drawn as the sound it is.)
    kind: isTitle ? "title" : (audioOnly || modelData.pictureless === true ? "audio" : "video")
    speedBadge: MediaTime.isHeldPicture(modelData) ? "❄ " + qsTr("freeze") : ""
    onFxRequested: {
        win.selectedConnectedIndex = laneBlock.index
        win.selectedTimelineIndex = -1
        win.showEffectsTab()
    }
    sourceTc: !isTitle && modelData.in !== undefined && backend.fps > 0
              ? backend.frameTimecode(modelData.in) : ""
    label: modelData.label !== undefined ? modelData.label : ""
    categoryColor: win.categories.colorOf(modelData)
    pip: modelData.rectW !== undefined && modelData.rectW < 1
    dragging: dragOffset !== 0 || groupRiding
    disabled: modelData.disabled === true
    showFadeHandles: (selected || fadeInFrames > 0 || fadeOutFrames > 0)
    // As on V1: a sliver in a fit view, or a block out of view, draws no
    // strip or wave, so it asks the engine for none each time one arrives.
    readonly property var decor: {
        const revision = backend.decorationRevision
        return inView && detailed ? backend.timelineDecoration(true, index) : ({})
    }
    // Upper video tracks get filmstrips too (decoded off the
    // clip's own file), not just V1.
    // Pictures carry the filmstrip, sounds the waveform: a
    // picture's own sound sits on an audio track below it.
    stripUrls: inView && !audioOnly && decor.strip !== undefined ? decor.strip : []
    stripFirstFrac: decor.stripFirst || 0
    stripStepFrac: decor.stripStep || 1
    // While its cut is rolled the pictures stay on their frames (see StoryClipBlock).
    stripSpan: rolling ? timelineFrames / scroller.shownFrames * laneBlock.strip.width : -1
    stripShift: rolling ? -dIn / scroller.shownFrames * laneBlock.strip.width : 0
    // Its sound, over the frames the block lies on as it is drawn now, and
    // the clip as the drag in hand would leave it: a trim moves the block's
    // ends and not the sound under them (a picture's trim with ripple brings
    // its linked sound up under a start that stays), a slip moves the sound
    // under the block, a slide takes the sound along with it.
    waveEntry: !inView || !detailed ? null : modelData
    wavePending: dIn !== 0 || dOut !== 0 || followIn !== 0 || followOut !== 0 || slipDelta !== 0 || slideDelta !== 0
                 ? ({ "start": modelData.start + dIn + followShift + slideDelta,
                      "in": modelData.in + dIn + followIn + slipDelta,
                      "frames": modelData.frames - dIn + dOut - followIn + followOut })
                 : ({})
    waveStart: timelineStart + dIn + followShift + slideDelta
    waveFrames: timelineFrames - dIn + dOut - followIn + followOut
    // Only the part on screen is painted (see TimelineClip.viewFrom).
    viewFrom: scroller.contentX - x
    viewTo: scroller.contentX + scroller.width - x
    showWave: !isTitle && (audioOnly || (modelData.videoOnly !== true && modelData.silent !== true))
    // A sound's block is its waveform, at its whole height; a picture
    // still carrying its sound (a compound) keeps a slim band.
    waveBand: audioOnly ? 1 : 0.38
    // A sound's wave is about its centre line, or stands from the foot when
    // the editor has asked for that (View ▸ Waveforms From the Base); the slim
    // band under a picture always stands.
    waveRectified: !audioOnly || win.rectifiedWaveforms
    // The frames the block is drawn over while its cut is rolled (see StoryClipBlock).
    readonly property real drawnFrames: rolling ? timelineFrames - dIn + dOut : timelineFrames
    fadeInFrac: (fadeInFrames + dFadeIn) / Math.max(1, drawnFrames)
    fadeOutFrac: (fadeOutFrames + dFadeOut) / Math.max(1, drawnFrames)
    mixOutFrac: Math.min(1, (mixBeforeFrames + dMix) / Math.max(1, drawnFrames))
    mixOutPastFrac: blockTail / Math.max(1, drawnFrames)
    mixFrames: mixOutFrames + dMix
    mixName: modelData.mixStyle ? qsTr("Transition") : qsTr("Dissolve")
    mixInFrac: mixInFrames > 0 ? Math.min(1, (blockLead + dMixIn) / Math.max(1, drawnFrames)) : 0
    mixInBeforeFrac: blockBefore / Math.max(1, drawnFrames)
    mixInLength: mixInFrames > 0 ? blockLead + blockBefore + dMixIn : 0
    mixInName: mixInEntry && mixInEntry.mixStyle ? qsTr("Transition") : qsTr("Dissolve")
    // (Under the Trim tool the edge a transition sits on is clear to work: see StoryClipBlock.)
    badgeInset: win.tool === "trim" && mixOutFrames > 0 ? 14 : 0
    effectKeys: detailed ? win.clipCommands.effectKeys(modelData) : []
    onEffectRemoveRequested:function(key){backend.setEditProperties(modelData.track,modelData.index,{resetEffect:key})}
    onTransitionRemoveRequested:function(which){
        const e=modelData
        if(which==="dissolve"){if(e.track===0)backend.toggleDissolve(e.index);else backend.toggleConnectedDissolve(index)}
        else if(which==="dissolveIn")backend.setTransitionSides(e.track,e.index-1,0,0)
        else if(e.track===0)backend.setFadeLengths(e.index,which==="in"?0:e.fadeIn,which==="out"?0:e.fadeOut)
        else backend.setConnectedFadeLengths(index,which==="in"?0:e.fadeIn,which==="out"?0:e.fadeOut)
    }
    hasFx: effectKeys.length > 0
    badge: slipDelta !== 0
           ? qsTr("slip %1").arg((slipDelta > 0 ? "+" : "") + slipDelta)
           : slideDelta !== 0
             ? qsTr("slide %1").arg((slideDelta > 0 ? "+" : "") + slideDelta)
           : (dFadeIn !== 0 || dFadeOut !== 0)
             ? qsTr("fade %1").arg(dFadeIn !== 0 ? fadeInFrames + dFadeIn
                                                 : fadeOutFrames + dFadeOut)
             : win.sound.badge(modelData)
    // Pending fade-length deltas while a knob drags.
    readonly property int dFadeIn: laneFadeInHandle.displayValue - fadeInFrames
    readonly property int dFadeOut: laneFadeOutHandle.displayValue - fadeOutFrames

    function framesFromPx(px) {
        return Math.round(px / laneBlock.strip.width
                          * scroller.shownFrames)
    }
    // How far each edge can be dragged: to the end of the clip's media, and
    // to one frame of its own outside a transition (see StoryClipBlock).
    readonly property int ownFrames: modelData.frames - mixOutFrames
    function clampIn(d) { return Math.max(Math.max(-modelData.in, -modelData.start), Math.min(ownFrames - 1, d)) }
    function clampOut(d) { return Math.max(-(ownFrames - 1), Math.min(modelData.length - 1 - modelData.out, d)) }
    function commitTrim() {
        var newIn = modelData.in + dIn
        var newOut = modelData.out + dOut
        var newStart = modelData.start + dIn
        dIn = 0
        dOut = 0
        if (newIn !== modelData.in
                || newOut !== modelData.out)
            backend.trimConnectedClip(laneBlock.index,
                                      newIn, newOut,
                                      newStart)
    }

    // Fades and a dissolve into the next clip on this
    // track use the same ramps and outlined region as V1.
    readonly property int fadeInFrames:
        modelData.fadeIn !== undefined ? modelData.fadeIn : 0
    readonly property int fadeOutFrames:
        modelData.fadeOut !== undefined ? modelData.fadeOut : 0
    readonly property int mixOutFrames:
        modelData.mixOut !== undefined ? modelData.mixOut : 0
    readonly property int mixInFrames:
        modelData.mixIn !== undefined ? modelData.mixIn : 0
    // The clip the transition into this one comes from.
    readonly property var mixInEntry: mixInFrames > 0 ? win.selection.trackEntry(modelData.track, modelData.index - 1) : null
    // The transition into the next clip has mixBeforeFrames ahead of the cut
    // (this block's end) and blockTail after it. A plain overlap is all
    // before it: its one grip changes the overlap, as it always has.
    readonly property int mixBeforeFrames: Math.max(0, mixOutFrames - blockTail)
    readonly property bool mixOverlaps: scroller.overlapsOnly(modelData)
    // A grip on each edge of the transition's box. This one is its start:
    // left lengthens it ahead of the cut, right shortens it, and the cut
    // and the end stay where they are.
    readonly property int dMix: laneMixHandle.displayValue - mixBeforeFrames
    readonly property int dMixIn: laneMixInHandle.displayValue - blockLead
    Ui.ClipLengthHandle {
        id: laneMixHandle
        objectName: "dissolveHandle"
        visible: laneBlock.detailed && laneBlock.mixOutFrames > 0
        enabled: laneBlock.modelData.track < backend.tracks.length && !backend.tracks[laneBlock.modelData.track].locked
        value: laneBlock.mixBeforeFrames
        maximumValue: laneBlock.mixOverlaps ? scroller.dissolveLimit(laneBlock.modelData) : laneBlock.mixMostBefore
        pixelsPerFrame: laneBlock.width / Math.max(1, laneBlock.timelineFrames)
        fps: backend.fps
        fromEnd: true
        transition: true
        label: laneBlock.mixOverlaps ? laneBlock.mixName : qsTr("%1, before the cut").arg(laneBlock.mixName)
        coordinateItem: laneBlock.strip
        onValueEdited: function(frames) {
            if (laneBlock.mixOverlaps) backend.resizeConnectedDissolve(laneBlock.index, frames)
            else backend.setTransitionSides(laneBlock.modelData.track, laneBlock.modelData.index, frames, laneBlock.blockTail)
        }
    }
    // The end of the transition into this clip: right lengthens it past the
    // cut (this block's start), left shortens it.
    Ui.ClipLengthHandle {
        id: laneMixInHandle
        objectName: "dissolveEndHandle"
        visible: laneBlock.detailed && laneBlock.mixInFrames > 0
        enabled: laneBlock.modelData.track < backend.tracks.length && !backend.tracks[laneBlock.modelData.track].locked
        value: laneBlock.blockLead
        maximumValue: laneBlock.mixMostAfter
        pixelsPerFrame: laneBlock.width / Math.max(1, laneBlock.timelineFrames)
        fps: backend.fps
        fromEnd: false
        transition: true
        label: qsTr("%1, after the cut").arg(laneBlock.mixInName)
        coordinateItem: laneBlock.strip
        onValueEdited: function(frames) {
            backend.setTransitionSides(laneBlock.modelData.track, laneBlock.modelData.index - 1, laneBlock.blockBefore, frames)
        }
    }

    MouseArea {
        id: laneArea
        anchors.fill: parent
        preventStealing: true
        property real pressX: 0
        property real pressY: 0
        property bool didDrag: false
        // As on V1: "move", "slip", "slide", the Trim
        // tool's zone ("in"/"out"/"rollIn"/"rollOut"/…), or
        // the Select tool's roll of a cut pressed on.
        property string mode: "move"
        readonly property bool trimsEdge: mode === "in" || mode === "out" || rollsCut
        readonly property bool rollsCut: mode === "rollIn" || mode === "rollOut"
        readonly property int cutIndex: laneBlock.modelData.index - (mode === "rollIn" ? 1 : 0)
        property var rollLimits: null
        property int rollWanted: 0
        onPressed: function (mouse) {
            pressX = laneArea.mapToItem(laneBlock.strip,
                                        mouse.x, 0).x
            pressY = laneArea.mapToItem(laneBlock.strip,
                                        0, mouse.y).y
            mode = win.tool === "trim"
                   ? scroller.trimZone(laneBlock.modelData.track, laneBlock.modelData.index,
                                             mouse.x, mouse.y, width, height)
                   : win.tool === "slip" || win.tool === "slide" ? win.tool
                   : win.tool === "select" && scroller.cutZone(laneBlock.modelData.track, laneBlock.modelData.index,
                                                               mouse.x, width) !== ""
                     ? scroller.cutZone(laneBlock.modelData.track, laneBlock.modelData.index, mouse.x, width) : "move"
            rollLimits = rollsCut ? win.trimming.rollLimits(laneBlock.modelData.track, cutIndex) : null
            rollWanted = 0
            didDrag = false
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            var x = laneArea.mapToItem(laneBlock.strip,
                                       mouse.x, 0).x
            var y = laneArea.mapToItem(laneBlock.strip,
                                       0, mouse.y).y
            if (!didDrag && (Math.abs(x - pressX) > (trimsEdge ? 2 : 8)
                             || (mode === "move" && Math.abs(y - pressY) > 12))) {
                didDrag = true
                if (!win.selection.isSelectedAt(laneBlock.modelData.track,
                                      laneBlock.modelData.index) || mode !== "move")
                    win.selection.selectOnly(laneBlock.modelData.track,
                                   laneBlock.modelData.index, false)
                win.selectedEdge = mode === "in" || mode === "rollIn" ? 0
                                 : mode === "out" || mode === "rollOut" ? 1 : -1
                laneBlock.groupLead = win.selectionCount > 1 && mode === "move"
            }
            var lm = laneBlock.modelData
            var ld = laneBlock.framesFromPx(x - pressX)
            if (didDrag && rollsCut) {
                rollWanted = ld
                scroller.showRoll(lm.track, cutIndex, rollLimits === null ? 0
                                  : Math.max(rollLimits.minimum, Math.min(rollLimits.maximum, ld)))
            } else if (didDrag && mode === "in") {
                laneBlock.dIn = laneBlock.clampIn(ld)
            } else if (didDrag && mode === "out") {
                laneBlock.dOut = laneBlock.clampOut(ld)
            } else if (didDrag && mode === "slip") {
                laneBlock.slipDelta = laneBlock.clampSlip(
                    -laneBlock.framesFromPx(x - pressX))
            } else if (didDrag && mode === "slide") {
                laneBlock.slideDelta = laneBlock.clampSlide(
                    laneBlock.framesFromPx(x - pressX))
            } else if (didDrag) {
                laneBlock.dragOffset = x - pressX
                // Rows of this kind: 1..n, plus one
                // past the last for a NEW track; for
                // pictures V1 is row 0, below the lanes.
                var rows = laneBlock.below ? win.audioLaneRows
                                           : win.videoLaneRows
                const targetY=win.lanes.laneY(laneBlock.modelData.track,laneBlock.below)+win.lanes.trackHeightFor(laneBlock.modelData.track)/2+y-pressY
                const targetTrack=win.lanes.laneTrackAt(targetY,laneBlock.below)
                const kinds=laneBlock.below?win.laneKinds.audio:win.laneKinds.video
                var target=targetTrack>=0?kinds.indexOf(targetTrack)+1:laneBlock.below?(targetY<0?1:rows+1):(targetY<0?rows+1:0)
                target = Math.max(laneBlock.below ? 1 : 0, Math.min(rows + 1, target))
                laneBlock.dragRows = laneBlock.below
                    ? target - laneBlock.laneRow
                    : laneBlock.laneRow - target
                if (laneBlock.groupLead)
                    laneBlock.dragRows = win.selection.clampGroupRows(laneBlock.modelData, laneBlock.dragRows)
                // It moves by whole frames, meeting the clips on the track it
                // is over and snapping as it goes.
                const overRow = laneBlock.below ? laneBlock.laneRow + laneBlock.dragRows
                                                : laneBlock.laneRow - laneBlock.dragRows
                const perFrame = laneBlock.strip.width / Math.max(1, scroller.shownFrames)
                laneBlock.dragOffset = (win.snapping.drag(
                    lm.start + (x - pressX) / perFrame, lm.frames, perFrame,
                    overRow <= 0 ? 0 : overRow <= kinds.length ? kinds[overRow - 1] : -1,
                    lm).frame - lm.start) * perFrame
                if (laneBlock.groupLead) {
                    win.groupDragPx = laneBlock.dragOffset
                    win.groupDragRows = laneBlock.dragRows
                    win.groupDragAudio = laneBlock.audioOnly
                    win.groupDragActive = true
                }
            }
        }
        onCanceled: {
            if (rollsCut)
                scroller.endRoll()
            laneBlock.abandonDrag()
        }
        onReleased: function (mouse) {
            if (!didDrag)
                return
            if (mode === "slip") {
                laneBlock.commitSlip()
                return
            }
            if (mode === "slide") {
                laneBlock.commitSlide()
                return
            }
            if (mode === "in" || mode === "out") {
                laneBlock.commitTrim()
                return
            }
            if (rollsCut) {
                scroller.commitRoll(laneBlock.modelData.track, cutIndex, rollWanted)
                return
            }
            // Snapped as it was dragged, and counted in whole frames from the
            // clip's own frame: a sample-timed sound is drawn at its exact
            // start, part of a frame on, and reading the place back off the
            // block would round that part up into one frame too many.
            var newStart = Math.max(0, laneBlock.modelData.start + Math.round(
                laneBlock.dragOffset / laneBlock.strip.width * Math.max(1, scroller.shownFrames)))
            var list = laneBlock.below ? win.laneKinds.audio
                                       : win.laneKinds.video
            var targetRow = laneBlock.below
                ? laneBlock.laneRow + laneBlock.dragRows
                : laneBlock.laneRow - laneBlock.dragRows
            var newTrack = targetRow <= 0 ? 0 // V1
                : targetRow <= list.length
                ? list[targetRow - 1]
                : backend.tracks.length // past the last: a new track
            var rows = laneBlock.dragRows
            laneBlock.dragOffset = 0
            laneBlock.dragRows = 0
            if (laneBlock.groupLead) { // the whole set travels
                laneBlock.groupLead = false
                win.groupDragActive = false
                win.groupDragPx = 0
                win.groupDragRows = 0
                var moves = win.selection.groupMoves(
                    laneBlock.modelData,
                    newStart - laneBlock.modelData.start, rows)
                win.selection.clearSelection()
                if (mouse.modifiers & Qt.AltModifier) // Alt: copies
                    win.selection.copyMoves(moves)
                else
                    backend.moveClips(moves)
                return
            }
            if (rows === 0 && newStart === laneBlock.modelData.start)
                return // dragged out and back: nothing moved, as on V1
            if (mouse.modifiers & Qt.AltModifier) {
                win.selection.copyMoves([{ track: laneBlock.modelData.track, index: laneBlock.modelData.index,
                                 newTrack: newTrack, newStart: newStart }])
                return
            }
            win.selectedConnectedIndex = -1
            // Picked without its linked partner (linked
            // selection off, or Alt-click): it goes alone and
            // the partner stays where it is.
            if (win.selection.hasUnselectedMate(laneBlock.modelData.track, laneBlock.modelData.index))
                backend.moveClips([{ track: laneBlock.modelData.track, index: laneBlock.modelData.index,
                                     newTrack: newTrack, newStart: newStart, alone: true }])
            else if (newTrack === laneBlock.modelData.lane)
                backend.moveConnectedClip(laneBlock.index,
                                          newStart)
            else
                backend.moveClipTo(laneBlock.modelData.lane,
                                   laneBlock.modelData.index,
                                   newTrack, newStart)
        }
        onClicked: function (mouse) {
            if (didDrag)
                return
            backend.viewTimeline()
            if (win.tool === "blade") {
                var sx = laneArea.mapToItem(laneBlock.strip, mouse.x, 0).x
                backend.bladeAt(laneBlock.modelData.lane,
                                Math.floor(sx / laneBlock.strip.width
                                           * scroller.shownFrames))
                return
            }
            win.selection.clickSelect(laneBlock.modelData.track,
                            laneBlock.modelData.index, mouse.modifiers,
                            win.selectedConnectedIndex === laneBlock.index)
        }
        onDoubleClicked: function (mouse) {
            if (win.doubleClickClip === "select" && mouse.x >= 14 && mouse.x <= width - 14)
                return
            if (laneBlock.isTitle && win.doubleClickClip === "open")
                titleDialog.openFor(
                    laneBlock.index,
                    laneBlock.modelData.title,
                    laneBlock.modelData.titlePlace,
                    laneBlock.modelData.titleSize,
                    laneBlock.modelData.titleFont,
                    laneBlock.modelData.titleColor,
                    win.catalogue.titleExtras(laneBlock.modelData))
            // An edit point on this track opens the
            // precision editor, as on V1.
            else if (mouse.x < 14)
                win.openPrecision(laneBlock.modelData.index - 1,
                                  laneBlock.modelData.lane)
            else if (mouse.x > width - 14)
                win.openPrecision(laneBlock.modelData.index,
                                  laneBlock.modelData.lane)
        }
    }

    MouseArea { // right-click: select + context menu
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onPressed: function (mouse) {
            if (laneBlock.openTransitionMenuAt(mapToItem(laneBlock, mouse.x, 0).x))
                return
            backend.viewTimeline()
            win.selectedEdge = -1
            win.selection.promote(laneBlock.modelData.track,
                        laneBlock.modelData.index)
            laneContextMenu.popup()
        }
    }

    // The lines are drawn over the entry's own frames, which a block on a
    // transition's cut starts before and stops short of.
    readonly property real entryX: width * blockLead / Math.max(1, timelineFrames)
    readonly property real entryWidth: width * modelData.frames / Math.max(1, timelineFrames)
    VolumeLine {
        x: laneBlock.entryX
        width: laneBlock.entryWidth
        height: parent.height
        active: win.videoAnimIndex !== laneBlock.index
        entry: laneBlock.modelData
        clipIndex: laneBlock.index
        lane: true
    }
    VolumeLine { // the Video Animation opacity line
        x: laneBlock.entryX
        width: laneBlock.entryWidth
        height: parent.height
        active: win.videoAnimIndex === laneBlock.index
        entry: laneBlock.modelData
        clipIndex: laneBlock.index
        lane: true
        mode: "opacity"
    }

    // The pixels at each end that are a cut the Select tool drags
    // (scroller.cutZone), which a fade knob lying over them leaves alone.
    readonly property bool knobsShown: detailed && win.tool === "select" && (selected || fadeInFrames > 0 || fadeOutFrames > 0)
    readonly property real cutAtStart: knobsShown ? scroller.cutReach(modelData.track, modelData.index, true, width) : 0
    readonly property real cutAtEnd: knobsShown ? scroller.cutReach(modelData.track, modelData.index, false, width) : 0
    // Fade grips: drag the top-edge dots to set the
    // fade lengths (in knob rightwards = longer fade in).
    Ui.ClipLengthHandle {
        id: laneFadeInHandle
        objectName: "fadeInHandle"
        visible: laneBlock.detailed && (laneBlock.selected || value > 0)
                 && !(win.tool === "trim" && laneBlock.mixInFrames > 0)
        enabled: laneBlock.modelData.track < backend.tracks.length && !backend.tracks[laneBlock.modelData.track].locked
        value: laneBlock.fadeInFrames
        maximumValue: Math.max(0, laneBlock.modelData.frames - 1)
        pixelsPerFrame: laneBlock.width / Math.max(1, laneBlock.timelineFrames)
        fps: backend.fps
        fromEnd: false
        coordinateItem: laneBlock.strip
        cutAtStart: laneBlock.cutAtStart
        cutAtEnd: laneBlock.cutAtEnd
        onValueEdited: function(frames) { backend.setConnectedFadeLengths(laneBlock.index, frames, laneBlock.fadeOutFrames) }
    }
    Ui.ClipLengthHandle {
        id: laneFadeOutHandle
        objectName: "fadeOutHandle"
        visible: laneBlock.detailed && (laneBlock.selected || value > 0)
                 && !(win.tool === "trim" && laneBlock.mixOutFrames > 0)
        enabled: laneBlock.modelData.track < backend.tracks.length && !backend.tracks[laneBlock.modelData.track].locked
        value: laneBlock.fadeOutFrames
        maximumValue: Math.max(0, laneBlock.modelData.frames - 1)
        pixelsPerFrame: laneBlock.width / Math.max(1, laneBlock.timelineFrames)
        fps: backend.fps
        fromEnd: true
        otherHandleX: laneFadeInHandle.visible ? laneFadeInHandle.x : NaN
        coordinateItem: laneBlock.strip
        cutAtStart: laneBlock.cutAtStart
        cutAtEnd: laneBlock.cutAtEnd
        onValueEdited: function(frames) { backend.setConnectedFadeLengths(laneBlock.index, laneBlock.fadeInFrames, frames) }
    }

    // Trim handles on the selected lane clip. The in
    // edge moves the clip's start with it — nothing
    // ripples on the lane.
    MouseArea {
        id: laneLeftHandle
        objectName: "trimInHandle"
        visible: laneBlock.selected && win.tool !== "trim"
        // (It stands clear of a cut the pointer can move: scroller.cutZone.)
        readonly property real cut: laneBlock.selected
            ? scroller.cutReach(laneBlock.modelData.track, laneBlock.modelData.index, true, laneBlock.width) : 0
        x: cut
        width: 8 - cut
        height: parent.height
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        property real pressX: 0
        onPressed: function (mouse) {
            pressX = laneLeftHandle.mapToItem(
                laneBlock.strip, mouse.x, 0).x
            laneBlock.dIn = 0
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            var d = laneBlock.framesFromPx(
                laneLeftHandle.mapToItem(laneBlock.strip,
                                         mouse.x, 0).x
                - pressX)
            laneBlock.dIn = laneBlock.clampIn(d)
        }
        onReleased: laneBlock.commitTrim()
        onCanceled: laneBlock.abandonDrag()
    }
    MouseArea {
        id: laneRightHandle
        objectName: "trimOutHandle"
        visible: laneBlock.selected && win.tool !== "trim"
        readonly property real cut: laneBlock.selected
            ? scroller.cutReach(laneBlock.modelData.track, laneBlock.modelData.index, false, laneBlock.width) : 0
        x: parent.width - 8
        width: 8 - cut
        height: parent.height
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        property real pressX: 0
        onPressed: function (mouse) {
            pressX = laneRightHandle.mapToItem(
                laneBlock.strip, mouse.x, 0).x
            laneBlock.dOut = 0
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            var d = laneBlock.framesFromPx(
                laneRightHandle.mapToItem(laneBlock.strip,
                                          mouse.x, 0).x
                - pressX)
            laneBlock.dOut = laneBlock.clampOut(d)
        }
        onReleased: laneBlock.commitTrim()
        onCanceled: laneBlock.abandonDrag()
    }
}
