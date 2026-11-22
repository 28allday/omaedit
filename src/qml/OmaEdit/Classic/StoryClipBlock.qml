import QtQuick
import QtQuick.Controls
import QtQuick.Controls as C
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Edit as Edit
import "../Ui/MediaTime.js" as MediaTime

// One clip of the main timeline (V1): its block, and the drags that move,
// trim, roll, slip and slide it. `story` is the row it sits in (drag and trim
// coordinates map through it).
Edit.TimelineClip {
    // The timeline's scroll area: zoom, the view's edges, pending trims and drops.
    required property Item scroller
    required property Item story
    fps: backend.fps > 0 ? backend.fps : 25
    id: clipBlock
    required property var modelData
    required property int sourceIndex
    // The block's own place: it meets its neighbour at the cut a transition
    // sits across, where the entry runs to the transition's end (see
    // TimelineWindowModel). blockLead frames of it lie before the entry's
    // start and blockTail fewer at its end; blockBefore is the rest of the
    // transition into this clip, over the block before.
    required property real timelineStart
    required property real timelineFrames
    required property int blockLead
    required property int blockTail
    required property int blockBefore
    required property int mixMostBefore
    required property int mixMostAfter
    readonly property int index: sourceIndex
    objectName: "clip:" + index
    // A pending in-edge trim shows what release does: without
    // ripple the left edge follows the pointer and the end
    // stays; with ripple the start stays and the end moves.
    // A roll moves the cut itself, so the edge follows.
    x: scroller.shownFrames > 0
       ? (timelineStart + (backend.rippleTrim && rollIn === 0 ? 0 : dIn))
         / scroller.shownFrames * story.width
       : 0
    selected: index === win.selectedTimelineIndex
              || win.selection.isSelectedAt(0, index)
    // Live trim preview: pending frame deltas on the
    // clip's in/out points, committed on release.
    property int dIn: 0
    property int dOut: 0
    // A cut being rolled is drawn by both of its blocks: the one whose end
    // it is and the one whose start it is take the same frames from the
    // scroller, whichever of them the drag began on.
    readonly property int rollIn: scroller.rollOf(0, index, false)
    readonly property int rollOut: scroller.rollOf(0, index, true)
    readonly property bool rolling: rollIn !== 0 || rollOut !== 0
    onRollInChanged: dIn = rollIn
    onRollOutChanged: dOut = rollOut
    property real dragOffset: 0
    function shareTrim() {
        // (The sound inside the two blocks of a rolled cut reads the roll itself.)
        if ((dIn !== 0 || dOut !== 0) && !rolling) {
            scroller.trimIn = dIn
            scroller.trimOut = dOut
            scroller.trimMovesStart = !backend.rippleTrim
            scroller.trimIndex = index
        } else if (scroller.trimIndex === index) {
            scroller.trimIndex = -1
        }
        scroller.setLinkTrim(modelData.link, dIn, dOut, !(backend.rippleTrim && rollIn === 0), rolling)
    }
    onDInChanged: shareTrim()
    onDOutChanged: shareTrim()
    // A drag that never gets its release (the pointer was taken away by a
    // dialog, or this block went while the button was held, as it does when
    // an undo or a finished render rebuilds the row) leaves nothing behind:
    // no block left displaced, no linked sound left drawn as if trimmed.
    function abandonDrag() {
        const lead = groupLead, shuffling = bodyArea.mode === "shuffle" && dragOffset !== 0
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
        if (shuffling) {
            scroller.shuffleLink = ""
            scroller.shufflePx = 0
            story.dropTargetIndex = -1
        }
    }
    readonly property bool dragPending: dIn !== 0 || dOut !== 0 || slipDelta !== 0 || slideDelta !== 0
                                        || dragOffset !== 0 || dragRows !== 0 || groupLead
    // A cut pressed on here and being dragged: the press goes with the
    // block (made again for a change elsewhere in the timeline), and the
    // roll goes with the press.
    Component.onDestruction: {
        if (bodyArea.pressed && bodyArea.rollsCut && scroller.rollIndex >= 0 && !scroller.rollHeld)
            scroller.endRoll()
        if (dragPending)
            abandonDrag()
    }
    // Slip tool: pending in/out shift, same length. The sound under the
    // picture draws it while the drag lasts.
    property int slipDelta: 0
    onSlipDeltaChanged: {
        if (slipDelta !== 0) {
            scroller.slipBy = slipDelta
            scroller.slipIndex = index
        } else if (scroller.slipIndex === index) {
            scroller.slipIndex = -1
        }
    }
    // Slide tool: pending move between the neighbours.
    property int slideDelta: 0
    function clampSlide(d) {
        var b = win.trimming.slideBounds(0, index)
        return b === null ? 0 : Math.max(b[0], Math.min(b[1], d))
    }
    function commitSlide() {
        var d = slideDelta
        slideDelta = 0
        if (d !== 0)
            backend.slideClip(0, index, d)
    }
    // A drag moves the clip in time and up into the
    // lanes (dragRows, video rows above V1); in a group
    // it leads, or rides the clip that does.
    property int dragRows: 0
    property bool groupLead: false
    readonly property bool groupRiding:
        win.groupDragActive && selected && !groupLead
    readonly property int shownRows: groupRiding && !win.groupDragAudio
                                     ? -win.groupDragRows : dragRows
    dragging: dragOffset !== 0 || groupRiding
    z: dragging ? 5 : 0
    transform: Translate {
        x: (clipBlock.groupRiding ? win.groupDragPx : clipBlock.dragOffset)
           + (scroller.shownFrames > 0
              ? clipBlock.slideDelta / scroller.shownFrames * story.width : 0)
        y: clipBlock.shownRows > 0 ? scroller.videoRowY(clipBlock.shownRows) - story.y : 0
    }
    function clampSlip(d) {
        return Math.max(-modelData.in,
                        Math.min(modelData.length - 1 - modelData.out, d))
    }
    function commitSlip() {
        var d = slipDelta
        slipDelta = 0
        if (d !== 0)
            backend.slipClip(index, d)
    }
    width: scroller.shownFrames > 0
           ? Math.max(6, (timelineFrames - dIn + dOut)
                         / scroller.shownFrames * story.width)
           : 0
    height: story.height

    readonly property bool isGap:
        modelData.gap === true
    readonly property bool inView:
        scroller.spans(x + dragOffset, width)
    visible: inView || dragging || selected

    // The picture (Edit.TimelineClip) from the entry.
    name: modelData.mcam !== undefined ? win.mcamAngleName(modelData)
        : modelData.nest === true ? win.clipCommands.nestName(modelData) : modelData.name
    // (A recording an older project holds on V1 is drawn as the sound it is.)
    kind: isGap ? "gap" : modelData.pictureless === true ? "audio" : "video"
    label: modelData.label !== undefined ? modelData.label : ""
    categoryColor: win.categories.colorOf(modelData)
    disabled: modelData.disabled === true
    showFadeHandles: (selected || fadeInFrames > 0 || fadeOutFrames > 0) && !isGap
    dropTarget: index === story.dropTargetIndex
    edge: win.selectedEdge
    // Strips and waves arrive on their own signal, so a
    // late thumbnail never rebuilds this block.
    // Overview slivers draw no strip, wave, badge or
    // marker, so they ask the engine and window for none.
    readonly property var decor: {
        const revision = backend.decorationRevision
        return inView && detailed ? backend.timelineDecoration(false, index) : ({})
    }
    stripUrls: inView && decor.strip !== undefined ? decor.strip : []
    stripFirstFrac: decor.stripFirst || 0
    stripStepFrac: decor.stripStep || 1
    // While its cut is rolled the pictures stay on their frames: the block's
    // edge uncovers or covers them, and they are not squeezed into it.
    stripSpan: rolling ? timelineFrames / scroller.shownFrames * story.width : -1
    stripShift: rolling ? -dIn / scroller.shownFrames * story.width : 0
    // Only the part on screen is painted (see TimelineClip.viewFrom).
    viewFrom: scroller.contentX - x
    viewTo: scroller.contentX + scroller.width - x
    // The sound lives on the attached-audio row below V1.
    showWave: false
    // The frames the block is drawn over: a roll in hand moves an end of it,
    // and a transition's box keeps its length as it goes with the cut.
    readonly property real drawnFrames: rolling ? timelineFrames - dIn + dOut : timelineFrames
    fadeInFrac: (fadeInFrames + dFadeIn) / Math.max(1, drawnFrames)
    fadeOutFrac: (fadeOutFrames + dFadeOut) / Math.max(1, drawnFrames)
    mixOutFrac: mixWashWidth / Math.max(1, width)
    mixOutPastFrac: blockTail / Math.max(1, drawnFrames)
    mixFrames: mixOutFrames + dMix
    mixName: modelData.mixStyle ? qsTr("Transition") : qsTr("Dissolve")
    // Under the Trim tool the edge a transition sits on is worked like any
    // cut: the badge stands clear of it, and the fade knob (which that edge
    // cannot take) is not shown there.
    badgeInset: win.tool === "trim" && mixOutFrames > 0 ? 14 : 0
    mixInFrac: mixInFrames > 0 ? Math.min(1, (blockLead + dMixIn) / Math.max(1, drawnFrames)) : 0
    mixInBeforeFrac: blockBefore / Math.max(1, drawnFrames)
    mixInLength: mixInFrames > 0 ? blockLead + blockBefore + dMixIn : 0
    mixInName: mixInFrames > 0 && index > 0 && backend.timeline[index - 1].mixStyle ? qsTr("Transition") : qsTr("Dissolve")
    effectKeys: detailed ? win.clipCommands.effectKeys(modelData) : []
    onEffectRemoveRequested:function(key){backend.setEditProperties(modelData.track,modelData.index,{resetEffect:key})}
    onTransitionRemoveRequested:function(which){
    const e=modelData
    if(which==="dissolve"){if(e.track===0)backend.toggleDissolve(e.index);else backend.toggleConnectedDissolve(index)}
    else if(which==="dissolveIn")backend.toggleDissolve(e.index-1)
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
             : ""
    // Pending fade-length deltas while a knob drags.
    readonly property int dFadeIn: grips ? grips.fadeIn.displayValue - fadeInFrames : 0
    readonly property int dFadeOut: grips ? grips.fadeOut.displayValue - fadeOutFrames : 0

    // Signed speed: negative plays backwards.
    speedBadge: MediaTime.isHeldPicture(modelData) ? "❄ " + qsTr("freeze")
                : clipSpeed === 1 ? ""
                : (clipSpeed < 0 ? "◀ " : "")
                  + (Math.abs(clipSpeed) < 1
                     ? Math.round(Math.abs(clipSpeed) * 100) + "%"
                     : (Math.round(Math.abs(clipSpeed) * 100) / 100) + "×")
    sourceTc: detailed && !isGap && modelData.in !== undefined && backend.fps > 0
              ? backend.frameTimecode(modelData.in) : ""
    onFxRequested: {
        win.selectedTimelineIndex = clipBlock.index
        win.selectedConnectedIndex = -1
        win.showEffectsTab()
    }
    markers: detailed ? scroller.blockFlags(win.clipMarkers.flags(modelData), modelData.frames, blockLead, timelineFrames) : []
    onMarkerClicked: function (i) {
        backend.viewTimeline()
        backend.seek(timelineStart + Math.round(markers[i].frac * timelineFrames))
    }

    // Fade ramps trace the faded spans over the footage.
    readonly property int fadeInFrames:
        modelData.fadeIn !== undefined ? modelData.fadeIn : 0
    readonly property int fadeOutFrames:
        modelData.fadeOut !== undefined ? modelData.fadeOut : 0
    // Dissolve into the next clip: the overlap
    // region gets an outlined span and duration label.
    readonly property int mixOutFrames:
        modelData.mixOut !== undefined ? modelData.mixOut : 0
    readonly property int mixInFrames:
        modelData.mixIn !== undefined ? modelData.mixIn : 0
    // The transition into the next clip has mixBeforeFrames ahead of the cut
    // (this block's end) and blockTail after it. A plain overlap is all
    // before it: its one grip changes the overlap, as it always has.
    readonly property int mixBeforeFrames: Math.max(0, mixOutFrames - blockTail)
    readonly property bool mixOverlaps: scroller.overlapsOnly(modelData)
    // Pending resizes (a grip on either edge of the box), committed on
    // release: its start, on this block, and the end of the one into this
    // clip, on this block too.
    readonly property int dMix: grips ? grips.mix.displayValue - mixBeforeFrames : 0
    readonly property int dMixIn: grips ? grips.mixIn.displayValue - blockLead : 0
    readonly property real mixWashWidth:
        Math.min(width,
            (mixBeforeFrames + dMix)
            / Math.max(1, drawnFrames) * width)
    function framesFromPx(px) {
        return Math.round(px / story.width * scroller.shownFrames)
    }
    // How far each edge can be dragged. Under the Trim tool an edge a
    // transition sits on (the cut, in the middle of the transition's box)
    // trims and rolls like any other, and the transition stays across it:
    // the clip must go on covering its half of the transition from its spare
    // frames, which its entry's in and out already count, and keep a frame
    // of its own outside the transition. The grips that change the
    // transition's length are at the two ends of its box, drawn above the
    // block: where a side of it has no frames, so that its grip sits on the
    // cut, the grip has the edge below the name bar and the trim the bar.
    readonly property int ownFrames: modelData.frames - mixOutFrames
    function clampIn(d) { return Math.max(-modelData.in, Math.min(ownFrames - 1, d)) }
    function clampOut(d) { return Math.max(-(ownFrames - 1), Math.min(modelData.length - 1 - modelData.out, d)) }
    function commitTrim() {
        // An out-edge trim snaps the clip's END
        // to edits/playhead/markers (trim to
        // playhead, the usual staple). The in edge
        // doesn't move positionally, so it has
        // nothing to snap to.
        if (dOut !== 0 && dIn === 0) {
            // (The block's end: the cut, where a transition lies across it.)
            var end = timelineStart + timelineFrames + dOut
            var snapped = win.snapping.edge(
                end,
                story.width
                / Math.max(1, scroller.shownFrames),
                timelineStart,
                timelineStart + timelineFrames)
            dOut = clampOut(dOut + (snapped - end))
        }
        var newIn = modelData.in + dIn
        var newOut = modelData.out + dOut
        dIn = 0
        dOut = 0
        if (newIn !== modelData.in || newOut !== modelData.out)
            backend.trimClip(index, newIn, newOut)
    }

    readonly property real clipSpeed:
        modelData.speed !== undefined
        ? modelData.speed : 1
    MouseArea {
        id: bodyArea
        anchors.fill: parent
        preventStealing: true
        property real pressX: 0
        property real pressY: 0
        property bool didDrag: false
        // What a drag does, fixed at the press: "move" (to
        // a time and track), "shuffle" (Ctrl+Shift: into a
        // new place in V1's order, the rest closing up),
        // "slip", "slide", or under the Trim tool by where
        // it starts (scroller.trimZone): "in"/"out"
        // ripple an edge, "rollIn"/"rollOut" roll a cut.
        // The Select tool rolls a cut too, pressed on the
        // cut itself (scroller.cutZone).
        property string mode: "move"
        readonly property bool trimsEdge: mode === "in" || mode === "out" || rollsCut
        readonly property bool rollsCut: mode === "rollIn" || mode === "rollOut"
        // The cut a roll moves (the clip before it), how far
        // it can go, and how far the pointer has asked for.
        readonly property int cutIndex: mode === "rollIn" ? clipBlock.index - 1 : clipBlock.index
        property var rollLimits: null
        property int rollWanted: 0
        onPressed: function (mouse) {
            pressX = bodyArea.mapToItem(story, mouse.x, 0).x
            pressY = bodyArea.mapToItem(story, 0, mouse.y).y
            const shuffle = (mouse.modifiers & Qt.ControlModifier) !== 0
                            && (mouse.modifiers & Qt.ShiftModifier) !== 0
            mode = win.tool === "trim"
                   ? scroller.trimZone(0, clipBlock.index, mouse.x, mouse.y, width, height)
                   : win.tool === "slip" || win.tool === "slide" ? win.tool
                   : shuffle ? "shuffle"
                   : win.tool === "select" && scroller.cutZone(0, clipBlock.index, mouse.x, width) !== ""
                     ? scroller.cutZone(0, clipBlock.index, mouse.x, width) : "move"
            rollLimits = rollsCut ? win.trimming.rollLimits(0, cutIndex) : null
            rollWanted = 0
            didDrag = false
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            var x = bodyArea.mapToItem(story, mouse.x, 0).x
            var y = bodyArea.mapToItem(story, 0, mouse.y).y
            if (!didDrag && (Math.abs(x - pressX) > (trimsEdge ? 2 : 8)
                             || (mode === "move" && Math.abs(y - pressY) > 12))) {
                didDrag = true
                if (mode === "shuffle" || mode === "slip" || mode === "slide") {
                    win.selection.selectOnly(0, clipBlock.index, true) // one clip
                } else if (trimsEdge) {
                    win.selection.setPrimary(0, clipBlock.index)
                    win.selectedEdge = mode === "in" || mode === "rollIn" ? 0 : 1
                } else {
                    if (!win.selection.isSelectedAt(0, clipBlock.index))
                        win.selection.selectOnly(0, clipBlock.index, false)
                    win.selectedEdge = -1
                    clipBlock.groupLead = win.selectionCount > 1
                }
            }
            if (!didDrag)
                return
            var d = clipBlock.framesFromPx(x - pressX)
            var m = clipBlock.modelData
            if (mode === "slip") {
                // Content follows the pointer: dragging right
                // reveals earlier source frames.
                clipBlock.slipDelta = clipBlock.clampSlip(-d)
            } else if (mode === "slide") {
                clipBlock.slideDelta = clipBlock.clampSlide(d)
            } else if (rollsCut) {
                // Both blocks follow, as far as the cut can go.
                rollWanted = d
                scroller.showRoll(0, cutIndex, rollLimits === null ? 0
                                  : Math.max(rollLimits.minimum, Math.min(rollLimits.maximum, d)))
            } else if (mode === "in") {
                clipBlock.dIn = clipBlock.clampIn(d)
            } else if (mode === "out") {
                clipBlock.dOut = clipBlock.clampOut(d)
            } else if (mode === "shuffle") {
                clipBlock.dragOffset = x - pressX
                scroller.shuffleLink = m.link || ""
                scroller.shufflePx = clipBlock.dragOffset
                story.dropTargetIndex = story.indexAt(x)
            } else {
                var row = scroller.videoRowAt(story.y + y)
                if (clipBlock.groupLead)
                    row = -win.selection.clampGroupRows(clipBlock.modelData, -row)
                // It moves by whole frames, meeting the clips on the track it
                // is over and snapping as it goes.
                const perFrame = story.width / Math.max(1, scroller.shownFrames)
                clipBlock.dragOffset = (win.snapping.drag(
                    m.start + (x - pressX) / perFrame, m.frames, perFrame,
                    row <= 0 ? 0 : win.lanes.videoTrackAtRow(row),
                    {track: 0, index: clipBlock.index, start: m.start, frames: m.frames}).frame - m.start) * perFrame
                if (clipBlock.groupLead) {
                    win.groupDragPx = clipBlock.dragOffset
                    win.groupDragRows = -row
                    win.groupDragAudio = false
                    win.groupDragActive = true
                }
                clipBlock.dragRows = row
            }
        }
        onCanceled: {
            if (rollsCut)
                scroller.endRoll()
            clipBlock.abandonDrag()
        }
        onReleased: function (mouse) {
            if (!didDrag)
                return
            if (mode === "slip") {
                clipBlock.commitSlip()
                return
            }
            if (mode === "slide") {
                clipBlock.commitSlide()
                return
            }
            if (mode === "in" || mode === "out") {
                clipBlock.commitTrim()
                return
            }
            if (rollsCut) { // the cut before this clip, or after it
                scroller.commitRoll(0, cutIndex, rollWanted)
                return
            }
            if (mode === "move") {
                // Snapped as it was dragged.
                // Counted from the clip's own start: the block of a clip
                // a transition leads into begins before it, at the cut.
                var newStart = Math.max(0, clipBlock.modelData.start + Math.round(
                    clipBlock.dragOffset / story.width * scroller.shownFrames))
                var rows = clipBlock.dragRows
                clipBlock.dragOffset = 0
                clipBlock.dragRows = 0
                if (clipBlock.groupLead) { // the whole set travels
                    clipBlock.groupLead = false
                    win.groupDragActive = false
                    win.groupDragPx = 0
                    win.groupDragRows = 0
                    var moves = win.selection.groupMoves(clipBlock.modelData,
                        newStart - clipBlock.modelData.start, -rows)
                    win.selection.clearSelection()
                    if (mouse.modifiers & Qt.AltModifier) // Alt: copies
                        win.selection.copyMoves(moves)
                    else
                        backend.moveClips(moves)
                    return
                }
                if (rows === 0 && newStart === clipBlock.modelData.start)
                    return
                win.selection.clearSelection()
                var single = { track: 0, index: clipBlock.index,
                               newTrack: win.lanes.videoTrackAtRow(rows), newStart: newStart }
                const alone = win.selection.hasUnselectedMate(0, clipBlock.index)
                if (mouse.modifiers & Qt.AltModifier)
                    win.selection.copyMoves([single])
                else if (alone) // its sound stays where it is
                    backend.moveClips([Object.assign(single, { alone: true })])
                else
                    backend.moveClipTo(0, clipBlock.index, single.newTrack, newStart)
                return
            }
            var target = story.dropTargetIndex
            clipBlock.dragOffset = 0
            scroller.shuffleLink = ""
            scroller.shufflePx = 0
            story.dropTargetIndex = -1
            if (target >= 0 && target !== clipBlock.index) {
                // The engine answers later (or refuses): the
                // dragged clip and its linked mate are found
                // again in the NEW entries, not assumed here.
                const moved = backend.timeline[clipBlock.index]
                win.reorderIdentity = { path: moved.path, "in": moved["in"], out: moved.out, title: moved.title || "", target: target }
                backend.moveClip(clipBlock.index, target)
                win.relinkPending = true
            }
        }
        onClicked: function (mouse) {
            if (didDrag)
                return
            backend.viewTimeline()
            if (win.tool === "blade") { // cut here
                backend.bladeAt(0, story.frameAt(
                    bodyArea.mapToItem(story, mouse.x, 0).x))
                return
            }
            win.selection.clickSelect(0, clipBlock.index, mouse.modifiers,
                            win.selectedTimelineIndex === clipBlock.index)
        }
        // Double-click an edit point opens the precision
        // editor for that boundary; the middle follows
        // Preferences ▸ Input.
        onDoubleClicked: function (mouse) {
            if (mouse.x < 14)
                win.openPrecision(clipBlock.index - 1)
            else if (mouse.x > width - 14)
                win.openPrecision(clipBlock.index)
            else if (win.doubleClickClip === "precision")
                win.openPrecision(mouse.x < width / 2 ? clipBlock.index - 1 : clipBlock.index)
            else if (win.doubleClickClip === "open" && clipBlock.modelData.nest === true)
                win.clipCommands.openNestedSequence(clipBlock.modelData) // edit it in its own tab
        }
    }

    MouseArea { // right-click: select + context menu
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onPressed: function (mouse) {
            if (clipBlock.openTransitionMenuAt(mapToItem(clipBlock, mouse.x, 0).x))
                return
            backend.viewTimeline()
            win.selectedEdge = -1
            win.selection.promote(0, clipBlock.index)
            clipContextMenu.popup()
        }
    }

    // (The volume line sits on the attached-audio block below.)

    // Grips exist only where they can be used: fade and
    // dissolve grips on blocks wide enough to show them,
    // trim edges on the selected block. A fit view of a
    // long edit holds thousands of blocks; each grip keeps
    // its own visibility rule once it exists.
    readonly property Item grips: gripLoader.item as Item
    // The pixels at each end that are a cut the Select tool drags
    // (scroller.cutZone), which a fade knob lying over them leaves alone.
    readonly property bool knobsShown: detailed && win.tool === "select" && !isGap
                                       && (selected || fadeInFrames > 0 || fadeOutFrames > 0)
    readonly property real cutAtStart: knobsShown ? scroller.cutReach(0, index, true, width) : 0
    readonly property real cutAtEnd: knobsShown ? scroller.cutReach(0, index, false, width) : 0
    Loader {
        id: trimLoader
        anchors.fill: parent
        active: clipBlock.selected && win.tool !== "trim" // Trim works the edges itself
        sourceComponent: Item {
            // Trim handles on the selected clip. Dragging
            // ripples — downstream clips slide.
            MouseArea {
                id: leftHandle
                objectName: "trimInHandle"
                // A transition rides this edge: a trim that leaves the clips
                // after it where they are would part the pair. The Trim tool
                // trims it (rippling) and rolls it, with the transition kept.
                visible: clipBlock.selected
                         && clipBlock.mixInFrames === 0
                // (It stands clear of a cut the pointer can move: scroller.cutZone.)
                readonly property real cut: scroller.cutReach(0, clipBlock.index, true, clipBlock.width)
                x: cut
                width: 10 - cut
                height: parent.height
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                property real pressX: 0
                // Mapping through the handle keeps deltas in
                // story space even as the edge moves
                // under the cursor mid-drag.
                onPressed: function (mouse) {
                    pressX = leftHandle.mapToItem(story, mouse.x, 0).x
                    clipBlock.dIn = 0
                    win.selection.setPrimary(0, clipBlock.index) // the V1 clip, not a lane mate left primary
                    win.selectedEdge = 0
                }
                onPositionChanged: function (mouse) {
                    if (!pressed)
                        return
                    var d = clipBlock.framesFromPx(
                        leftHandle.mapToItem(story, mouse.x, 0).x - pressX)
                    clipBlock.dIn = clipBlock.clampIn(d)
                }
                onReleased: clipBlock.commitTrim()
                onCanceled: clipBlock.abandonDrag()
            }
            MouseArea {
                id: rightHandle
                objectName: "trimOutHandle"
                visible: clipBlock.selected
                         && clipBlock.mixOutFrames === 0
                readonly property real cut: scroller.cutReach(0, clipBlock.index, false, clipBlock.width)
                x: parent.width - 10
                width: 10 - cut
                height: parent.height
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                property real pressX: 0
                onPressed: function (mouse) {
                    pressX = rightHandle.mapToItem(story, mouse.x, 0).x
                    clipBlock.dOut = 0
                    win.selection.setPrimary(0, clipBlock.index) // the V1 clip, not a lane mate left primary
                    win.selectedEdge = 1
                }
                onPositionChanged: function (mouse) {
                    if (!pressed)
                        return
                    var d = clipBlock.framesFromPx(
                        rightHandle.mapToItem(story, mouse.x, 0).x - pressX)
                    clipBlock.dOut = clipBlock.clampOut(d)
                }
                onReleased: clipBlock.commitTrim()
                onCanceled: clipBlock.abandonDrag()
            }

        }
    }
    // Fade grips stay visible while a fade is applied:
    // drag to set the fade lengths.
    Loader {
        id: gripLoader
        anchors.fill: parent
        z: 11 // above the block's badges, as the grips were
        active: clipBlock.detailed
        sourceComponent: Item {
            property alias fadeIn: fadeInHandle
            property alias fadeOut: fadeOutHandle
            property alias mix: mixHandle
            property alias mixIn: mixInHandle
            Ui.ClipLengthHandle {
                id: fadeInHandle
                objectName: "fadeInHandle"
                visible: clipBlock.detailed && (clipBlock.selected || value > 0) && !clipBlock.isGap
                         && !(win.tool === "trim" && clipBlock.mixInFrames > 0)
                enabled: backend.tracks.length > 0 && !backend.tracks[0].locked
                value: clipBlock.fadeInFrames
                maximumValue: Math.max(0, clipBlock.modelData.frames - 1)
                pixelsPerFrame: clipBlock.width / Math.max(1, clipBlock.timelineFrames)
                fps: backend.fps
                fromEnd: false
                coordinateItem: story
                cutAtStart: clipBlock.cutAtStart
                cutAtEnd: clipBlock.cutAtEnd
                onValueEdited: function(frames) { backend.setFadeLengths(clipBlock.index, frames, clipBlock.fadeOutFrames) }
            }
            Ui.ClipLengthHandle {
                id: fadeOutHandle
                objectName: "fadeOutHandle"
                visible: clipBlock.detailed && (clipBlock.selected || value > 0) && !clipBlock.isGap
                         && !(win.tool === "trim" && clipBlock.mixOutFrames > 0)
                enabled: backend.tracks.length > 0 && !backend.tracks[0].locked
                value: clipBlock.fadeOutFrames
                maximumValue: Math.max(0, clipBlock.modelData.frames - 1)
                pixelsPerFrame: clipBlock.width / Math.max(1, clipBlock.timelineFrames)
                fps: backend.fps
                fromEnd: true
                otherHandleX: fadeInHandle.visible ? fadeInHandle.x : NaN
                coordinateItem: story
                cutAtStart: clipBlock.cutAtStart
                cutAtEnd: clipBlock.cutAtEnd
                onValueEdited: function(frames) { backend.setFadeLengths(clipBlock.index, clipBlock.fadeInFrames, frames) }
            }

            // A grip on each edge of the transition's box. This one is
            // its start: left lengthens it ahead of the cut, right shortens
            // it, and the cut and the end stay where they are.
            Ui.ClipLengthHandle {
                id: mixHandle
                objectName: "dissolveHandle"
                visible: clipBlock.detailed && clipBlock.mixOutFrames > 0
                enabled: backend.tracks.length > 0 && !backend.tracks[0].locked
                value: clipBlock.mixBeforeFrames
                maximumValue: clipBlock.mixOverlaps ? scroller.dissolveLimit(clipBlock.modelData) : clipBlock.mixMostBefore
                pixelsPerFrame: clipBlock.width / Math.max(1, clipBlock.timelineFrames)
                fps: backend.fps
                fromEnd: true
                transition: true
                label: clipBlock.mixOverlaps ? clipBlock.mixName : qsTr("%1, before the cut").arg(clipBlock.mixName)
                coordinateItem: story
                onValueEdited: function(frames) {
                    if (clipBlock.mixOverlaps) backend.resizeDissolve(clipBlock.index, frames)
                    else backend.setTransitionSides(0, clipBlock.index, frames, clipBlock.blockTail)
                }
            }
            // The end of the transition into this clip: right lengthens it
            // past the cut (this block's start), left shortens it.
            Ui.ClipLengthHandle {
                id: mixInHandle
                objectName: "dissolveEndHandle"
                visible: clipBlock.detailed && clipBlock.mixInFrames > 0
                enabled: backend.tracks.length > 0 && !backend.tracks[0].locked
                value: clipBlock.blockLead
                maximumValue: clipBlock.mixMostAfter
                pixelsPerFrame: clipBlock.width / Math.max(1, clipBlock.timelineFrames)
                fps: backend.fps
                fromEnd: false
                transition: true
                label: qsTr("%1, after the cut").arg(clipBlock.mixInName)
                coordinateItem: story
                onValueEdited: function(frames) { backend.setTransitionSides(0, clipBlock.index - 1, clipBlock.blockBefore, frames) }
            }
        }
    }
}
