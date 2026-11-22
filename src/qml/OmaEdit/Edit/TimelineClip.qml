pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import omaedit 1.0
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// One clip block on a lane. Purely the picture: tint by kind, filmstrip,
// waveform band, label, fade wedges, dissolve overlay, badges, the 2 px cyan
// selection ring and the in/out edge marks. Every gesture (drag, trim, the
// rubber bands, context menus) is layered on top by the owner as children,
// so this file never touches the engine or the window.
//
// Fractions are of the block's own length (0…1), so the owner maps frames
// once and the block stays zoom-agnostic.
Rectangle {
    id: root

    property string name: ""
    property string kind: "video"     // video | audio | title | gap
    property bool selected: false
    property bool dropTarget: false   // a dragged clip would land here
    property int edge: -1             // 0 = in point, 1 = out point selected
    property bool dragging: false
    property bool disabled: false
    property bool hasFx: false
    property bool pip: false          // picture-in-picture (rect < full)
    // Colour label ("" = none): the block wears it instead of its kind tint.
    property string label: ""
    // The clip's category colour, as a strip along its foot.
    property color categoryColor: "transparent"
    // Fade knobs on the top corners (the owner layers the drag on them).
    property bool showFadeHandles: false

    // Filmstrip tile URLs; the plain tint shows until they decode.
    property var stripUrls: []
    property real stripFirstFrac: 0
    property real stripStepFrac: stripUrls.length > 0 ? 1 / stripUrls.length : 1
    // The width the strip's fractions are of, and where that width starts
    // from the block's left: the block's own (-1, 0) unless an edge of it is
    // being dragged, when the pictures stay where they were.
    property real stripSpan: -1
    property real stripShift: 0
    readonly property real stripWidth: stripSpan >= 0 ? stripSpan : width
    // The clip whose sound the block shows (its entry: its file, its in
    // point, its speed, its level and fades; null for none) and the timeline
    // frames the block lies over, from `waveStart` for `waveFrames`: the wave
    // is that stretch of the clip's sound (WaveformItem). On a sound it has
    // the block's whole height, about a centre line; under a picture it
    // stands from the foot of a short band.
    property var waveEntry: null
    property real waveStart: 0
    property real waveFrames: 0
    property bool showWave: true
    property bool waveRectified: kind !== "audio"
    // The clip as an edit in hand would leave it (WaveformItem.pending).
    property var wavePending: ({})
    // What is written on the block lies over its picture or its wave: each
    // piece of text has a backing of its own there, and the wave keeps its room.
    readonly property bool backed: onMedia || (showWave && waveEntry !== null)
    // The part of the block on screen, in its own pixels (viewTo < 0: all
    // of it). Canvases cover only that and a margin: a long clip zoomed
    // right in is far wider than any picture the graphics card can hold,
    // and one that size comes back shrunk and blurred.
    property real viewFrom: 0
    property real viewTo: -1
    readonly property real paintFrom: viewTo < 0 ? 0
        : Math.max(0, Math.min(width, Math.floor(viewFrom / 512) * 512 - 512))
    readonly property real paintTo: viewTo < 0 ? width
        : Math.max(paintFrom, Math.min(width, Math.ceil(viewTo / 512) * 512 + 512))
    property real waveBand: kind === "audio" ? 1 : 0.32 // fraction of the height
    // A wave with the whole height stops short of the outline at the top,
    // and at the foot short of the category strip when the block has one.
    readonly property real waveInset: Theme.clipOutlineWidth + 1
    readonly property real waveFootInset: kind !== "gap" && categoryColor.a > 0 ? Theme.clipOutlineWidth + Theme.s1 : waveInset

    property real fadeInFrac: 0
    property real fadeOutFrac: 0
    // A transition lies across the cut: the part before the cut over the end
    // of the outgoing block, the part after it over the start of the
    // incoming one. Each block draws the whole box and shows what falls
    // inside itself, so the cut reads at its own frame, between the two.
    property real mixOutFrac: 0       // into the next clip: its frames before the cut, at this block's end
    property real mixOutPastFrac: 0   // … and its frames after the cut, over the next block
    property real mixInFrac: 0        // out of the clip before: its frames after the cut, at this block's start
    property real mixInBeforeFrac: 0  // … and its frames before the cut, over the block before
    property real fps: 25            // the timeline rate, for lengths shown as time
    property int mixFrames: 0
    property string mixName: qsTr("Dissolve")
    property int mixInLength: 0
    property string mixInName: qsTr("Dissolve")
    // The ↔ badge sits in the block's bottom corner at its end: this many
    // pixels in from it, to leave the edge itself to a tool that works it.
    property real badgeInset: 0
    // Bottom-left and top-right badge text ("-6 dB · L", "50%"); "" hides.
    property string badge: ""
    property string speedBadge: ""
    property var effectKeys: []
    signal effectRemoveRequested(string key)
    // which: "dissolve" (the transition into the next clip), "dissolveIn"
    // (the one out of the clip before, whose box reaches into this block),
    // "in" / "out" (a fade).
    signal transitionRemoveRequested(string which)
    // The transition the remove menu means: the one into this clip when it
    // was opened on that box's part of the block.
    property bool transitionMenuIncoming: false
    // A right-click at x (in this item) on a dissolve or a fade ramp opens
    // the remove menu; the timeline asks this before its clip menu.
    function openTransitionMenuAt(x) {
        const onMix = mixOutFrac > 0 && x >= width - Math.max(2, mixOutFrac * width)
        const onMixIn = mixInFrac > 0 && x <= Math.max(2, mixInFrac * width)
        const onFade = (fadeInFrac > 0 && x <= fadeInFrac * width)
                       || (fadeOutFrac > 0 && x >= width - fadeOutFrac * width)
        if (!detailed || (!onMix && !onMixIn && !onFade))
            return false
        transitionMenuIncoming = onMixIn && !onMix
        transitionMenuWanted()
        return true
    }
    signal transitionMenuWanted()
    signal fxRequested() // the fx badge was clicked: open the effects
    // Clip markers as flags along the top edge: [{frac, type, text}];
    // a click reports the index and the owner seeks there.
    property var markers: []
    signal markerClicked(int index)
    // Source in-point timecode, bottom right, when the block is wide enough.
    property string sourceTc: ""

    readonly property color kindTint: kind === "title" ? Theme.clipTitle
                                    : kind === "audio" ? Theme.clipAudio
                                    : kind === "gap" ? Theme.clipGap
                                    : Theme.clipVideo
    readonly property color tint: label !== "" && kind !== "gap"
                                  ? Theme.labelColor(label) : kindTint
    readonly property bool onMedia: stripUrls.length > 0
    readonly property bool ringed: selected || dropTarget
    readonly property color outline: kind === "gap" ? Qt.darker(tint, 1.5) : Theme.clipOutline

    radius: Theme.rControl
    color: disabled && !detailed ? Theme.clipGap : tint
    border.color: ringed || (!detailed && hasTransitions) ? Theme.accent : outline
    border.width: detailed ? 0 : (ringed || hasTransitions ? Theme.selectionRing : Theme.hairline)
    clip: true
    opacity: dragging ? 0.75 : 1
    z: dragging ? 10 : 0

    Accessible.role: Accessible.ListItem
    Accessible.name: name

    // At overview zoom, text, canvases and handles cannot be read. Keep the
    // clip body and its owner's editing gestures, and create detail on zoom
    // or selection. This also bounds fit-to-project scene graph complexity.
    readonly property bool detailed: width >= 24 || dragging
    readonly property bool hasTransitions: fadeInFrac > 0 || fadeOutFrac > 0 || mixOutFrac > 0 || mixInFrac > 0
    Loader {
        anchors.fill: parent
        active: root.detailed
        sourceComponent: Item {
    Item { // the pictures stop short of the clip's two ends, which show its own colour
        objectName: "timelineFilmstripFrame"
        x: Theme.clipOutlineWidth + Theme.clipPictureInset
        width: Math.max(0, root.width - 2 * x)
        height: root.height
        clip: true
        visible: root.onMedia
    Row { // filmstrip
        objectName: "timelineFilmstrip"
        x: root.stripShift + root.stripFirstFrac * root.stripWidth - parent.x
        height: root.height
        Repeater {
            model: root.stripUrls
            Image {
                objectName: "timelineThumbnail"
                required property var modelData
                source: modelData
                height: root.height
                width: Math.max(1, root.stripStepFrac * root.stripWidth)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: Theme.scrimLight
                }
            }
        }
    }
    }

    Rectangle { // the band a picture's sound lies in
        visible: clipWave.visible && root.kind !== "audio"
        x: clipWave.x
        y: clipWave.y
        width: clipWave.width
        height: clipWave.height
        color: Theme.scrimLight
        opacity: 0.92
    }
    WaveformItem { // waveform band
        id: clipWave
        objectName: "clipWave"
        visible: root.showWave && root.waveEntry !== null && ready && width > 0
        // Only the part of the block on screen, and a margin (see viewFrom).
        readonly property real fullWidth: Math.max(0, root.width - 2 * Theme.hairline)
        readonly property real from: Math.max(0, Math.min(fullWidth, root.paintFrom))
        x: Theme.hairline + from
        width: Math.max(0, Math.min(fullWidth, root.paintTo) - from)
        readonly property bool whole: root.waveBand >= 1
        y: whole ? root.waveInset : root.height - Theme.hairline - height
        height: whole ? Math.max(0, root.height - root.waveInset - root.waveFootInset) : Math.round(root.height * root.waveBand)
        entry: root.waveEntry !== null ? root.waveEntry : ({})
        pending: root.wavePending
        fps: root.fps
        firstFrame: root.waveStart + (fullWidth > 0 ? from / fullWidth : 0) * root.waveFrames
        lastFrame: root.waveStart + (fullWidth > 0 ? (from + width) / fullWidth : 1) * root.waveFrames
        rectified: root.waveRectified
        // The peaks in the sound's own green, the body of the sound (its
        // RMS) lighter inside them; lighter than the clip it lies on.
        peakColor: Theme.audio
        bodyColor: Qt.lighter(Theme.audio, 1.8)
        clipColor: Theme.danger
    }

    // Filled ramps and outlined diagonals keep fades legible over footage.
    Canvas {
        id: fadeRamps
        objectName: "fadeRamps"
        x: root.paintFrom
        width: root.paintTo - root.paintFrom
        height: parent.height
        z: 1
        visible: (root.fadeInFrac > 0 || root.fadeOutFrac > 0) && width > 0
        // In the block's own pixels; the canvas covers only the part on
        // screen, so the ramps are drawn shifted by where it starts.
        readonly property real inWidth: Math.min(root.width, root.fadeInFrac * root.width)
        readonly property real outWidth: Math.min(root.width, root.fadeOutFrac * root.width)
        onXChanged: requestPaint()
        onInWidthChanged: requestPaint()
        onOutWidthChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections { target: Theme; function onAccentChanged() { fadeRamps.requestPaint() } }
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            function ramp(edge, inner) {
                ctx.beginPath()
                ctx.moveTo(edge, height)
                ctx.lineTo(edge, 0)
                ctx.lineTo(inner, 0)
                ctx.closePath()
                ctx.fillStyle = String(Theme.scrimStrong)
                ctx.fill()
                ctx.beginPath()
                ctx.moveTo(edge, height - 1)
                ctx.lineTo(inner, 1)
                ctx.strokeStyle = String(Theme.scrimStrong)
                ctx.lineWidth = 4
                ctx.stroke()
                ctx.strokeStyle = String(Theme.accent)
                ctx.lineWidth = 2
                ctx.stroke()
            }
            ctx.save()
            ctx.translate(-x, 0)
            if (inWidth > 0) ramp(0, inWidth)
            if (outWidth > 0) ramp(root.width, root.width - outWidth)
            ctx.restore()
        }
    }

    // The full transition span is bounded, even on an unselected clip.
    TransitionSide {
        objectName: "dissolveRegion"
        ownWidth: root.mixOutFrac * root.width
        otherWidth: root.mixOutPastFrac * root.width
        frames: root.mixFrames
        name: root.mixName
    }
    TransitionSide {
        objectName: "dissolveRegionIn"
        incoming: true
        ownWidth: root.mixInFrac * root.width
        otherWidth: root.mixInBeforeFrac * root.width
        frames: root.mixInLength
        name: root.mixInName
    }

    Rectangle { // disabled wash
        anchors.fill: parent
        radius: root.radius
        color: Theme.scrim
        visible: root.disabled
        z: 4
    }

    Rectangle { // selection tint above the media
        anchors.fill: parent
        radius: root.radius
        color: Theme.accent
        opacity: root.selected ? 0.18 : 0
    }
    Rectangle { // ring above everything
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        objectName: "clipRing"
        border.color: root.ringed ? Theme.accent : root.outline
        border.width: root.ringed ? Theme.selectionRing : root.kind !== "gap" ? Theme.clipOutlineWidth : Theme.hairline
        z: 3
    }
    Rectangle { // selected in point, nudged by , .
        visible: root.selected && root.edge === 0
        anchors.left: parent.left
        width: Theme.s1
        height: parent.height
        color: Theme.accent
        z: 3
    }
    Rectangle { // selected out point
        visible: root.selected && root.edge === 1
        anchors.right: parent.right
        width: Theme.s1
        height: parent.height
        color: Theme.accent
        z: 3
    }

    Rectangle { // category strip
        objectName: "clipCategoryStrip"
        // Inside the outline, which runs all the way round the clip.
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.clipOutlineWidth
        anchors.rightMargin: Theme.clipOutlineWidth
        anchors.bottomMargin: Theme.clipOutlineWidth
        height: Theme.s1
        color: root.categoryColor
        visible: root.kind !== "gap"
        z: 4
    }

    // One block's share of a transition's box: the whole box is laid out
    // (`otherWidth` of it lies in the neighbouring block) and this window on
    // it shows the part inside this block. The two windows meet at the cut.
    component TransitionSide: Item {
        id: side
        property bool incoming: false
        property real ownWidth: 0
        property real otherWidth: 0
        property int frames: 0
        property string name: ""
        // The length is written once, on the side with more room for it.
        readonly property bool labelled: incoming ? ownWidth > otherWidth : ownWidth >= otherWidth
        visible: ownWidth > 0
        x: incoming ? 0 : parent.width - width
        width: Math.min(parent.width, Math.max(2, ownWidth))
        height: parent.height
        z: 2
        clip: true
        Rectangle {
            id: mixOverlay
            objectName: "dissolveBox"
            x: side.incoming ? -side.otherWidth : 0
            width: side.width + side.otherWidth
            height: parent.height
            color: Theme.scrim
            border.width: 1
            border.color: Theme.accent
            Rectangle { anchors.fill: parent; anchors.margins: 1; color: Theme.transitionWash }
            Canvas {
                id: dissolveLine
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Connections { target: Theme; function onAccentChanged() { dissolveLine.requestPaint() } }
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.beginPath()
                    ctx.moveTo(0, height)
                    ctx.lineTo(width, 0)
                    ctx.moveTo(0, 0)
                    ctx.lineTo(width, height)
                    ctx.strokeStyle = String(Theme.scrimStrong)
                    ctx.lineWidth = 4
                    ctx.stroke()
                    ctx.strokeStyle = String(Theme.accent)
                    ctx.lineWidth = 1.5
                    ctx.stroke()
                }
            }
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 12, transitionLabel.implicitWidth + 8)
            height: transitionLabel.implicitHeight + 4
            visible: side.labelled && parent.width >= 32 && parent.height >= 28
            radius: Theme.rControl
            color: Theme.scrimStrong
            Text {
                id: transitionLabel
                anchors.centerIn: parent
                width: parent.width - 8
                text: side.width >= 140
                      ? side.name + (side.frames > 0 ? " · " + MediaTime.durationText(side.frames, root.fps) : "")
                      : side.width >= 70 ? side.name : "↔"
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.clipLabelOnMedia
            }
        }
    }

    component Badge: Rectangle {
        property string text: ""
        visible: text !== ""
        z: 8
        width: badgeText.implicitWidth + Theme.s2 + 2
        height: badgeText.implicitHeight + 2
        radius: Theme.rControl
        color: Theme.scrim
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: parent.text
            color: Theme.clipLabelOnMedia
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
        }
    }
        }
    }
    Loader {
        anchors.fill: parent
        active: root.detailed
        z: 8
        sourceComponent: Item {
    // What is written on the block, over the level line the owner lays
    // across it (which would strike it through).
    Badge { // retime, top right
        text: root.speedBadge
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s1
    }
    Badge { // volume · pan · EQ, bottom left
        text: root.badge
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s1
    }
    Rectangle { // source timecode, bottom right, backed as the name is
        objectName: "clipSourceTc"
        visible: root.sourceTc !== "" && root.width > 220 && !root.dragging
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s1
        // Clear of the transition mark when the block shows one in this corner.
        anchors.rightMargin: Theme.s1 + (root.mixOutFrac > 0 || root.fadeInFrac > 0 || root.fadeOutFrac > 0 ? Theme.s4 : 0)
        z: 8
        width: sourceTcText.implicitWidth + Theme.s2
        height: sourceTcText.implicitHeight + 2
        radius: Theme.rControl
        color: root.backed ? Theme.scrim : "transparent"
        Text {
            id: sourceTcText
            anchors.centerIn: parent
            text: root.sourceTc
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: root.backed ? Theme.clipLabelOnMedia : Theme.textMuted
            opacity: root.backed ? 1 : 0.8
        }
    }
    // The name, backed when it sits on media or on the clip's wave.
    Rectangle {
        id: labelBox
        objectName: "clipNameBacking"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.s1
        // Clear of the fade knob when the block shows one, and of the part
        // of a transition's box that lies over the block's start.
        anchors.leftMargin: (root.showFadeHandles ? Theme.szFadeKnob + Theme.s2 : Theme.s1)
                            + (root.mixInFrac > 0 ? Math.min(root.width, root.mixInFrac * root.width) : 0)
        width: Math.min(nameText.implicitWidth + Theme.s2,
                        parent.width - Theme.s2 - anchors.leftMargin
                        - (root.hasFx ? Theme.fsLabel * 1.5 + Theme.s1 : 0))
        height: nameText.implicitHeight + 2
        radius: Theme.rControl
        // On media the backing carries the label colour, so a labelled
        // clip still reads as labelled under its filmstrip.
        color: root.onMedia ? (root.label !== "" ? Theme.labelColor(root.label)
                                                 : Theme.scrim)
                            : root.backed ? Theme.scrim : "transparent"
        visible: width > Theme.s2
        z: 8
        Text {
            id: nameText
            anchors.left: parent.left
            anchors.leftMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - Theme.s2
            text: (root.pip ? "▣ " : "") + root.name
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: root.backed ? Theme.clipLabelOnMedia : Theme.textPrimary
        }
    }

    Rectangle { // the fx badge's backing, over media or the clip's wave
        anchors.fill: fxBadge
        anchors.leftMargin: -Theme.s1
        anchors.rightMargin: -Theme.s1
        anchors.topMargin: -1
        anchors.bottomMargin: -1
        radius: Theme.rControl
        color: Theme.scrim
        visible: fxBadge.visible && root.backed
        z: 8
    }
    Text { // fx badge, top right (yields to the speed badge)
        id: fxBadge
        objectName: "fxBadge"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s1
        // Clear the fade-out knob on a selected block (its 16 px MouseArea
        // sits over this corner), the way the label clears the fade-in one.
        anchors.rightMargin: root.showFadeHandles ? Theme.szFadeKnob + Theme.s2 : Theme.s1
        visible: root.hasFx
        z: 8 // above the fade knobs (7) and the volume line's full-block MouseArea
        text: qsTr("fx")
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        font.italic: true
        color: root.backed ? Theme.clipLabelOnMedia : Theme.textSecondary
        // A MouseArea, not a TapHandler: the volume line's full-block
        // MouseArea underneath treats the top band as the 0 dB line and
        // takes a TapHandler's press over; among MouseAreas the higher z
        // wins outright.
        MouseArea {
            anchors.fill: parent
            anchors.margins: -Theme.s1 // a finger-sized target
            cursorShape: Qt.PointingHandCursor
            acceptedButtons:Qt.LeftButton|Qt.RightButton
            onClicked:function(mouse){if(mouse.button===Qt.RightButton)fxMenuLoader.active = true;else root.fxRequested()}
        }
    }

    Loader {
        id: fxMenuLoader
        active: false
        onLoaded: item.popup()
        sourceComponent: C.Menu {
        id:fxMenu
        onClosed: Qt.callLater(function() { fxMenuLoader.active = false })
        onVisibleChanged: Commands.popupVisibility(fxMenu, visible)
        Component.onDestruction: Commands.popupVisibility(fxMenu, false)
        objectName:"clipEffectsMenu"
        C.MenuItem {text:qsTr("Open effects");onTriggered:root.fxRequested()}
        Instantiator {
            model:root.effectKeys
            delegate:C.MenuItem {
                required property var modelData
                text:qsTr("Remove %1").arg(modelData)
                onTriggered:root.effectRemoveRequested(modelData)
            }
            onObjectAdded:function(index,object){fxMenu.insertItem(index+1,object)}
            onObjectRemoved:function(index,object){fxMenu.removeItem(object)}
        }
    }
    }
    Connections {
        target: root
        function onTransitionMenuWanted() { transitionMenuLoader.active = true }
    }
    Loader {
        id: transitionMenuLoader
        active: false
        onLoaded: item.popup()
        sourceComponent: C.Menu {
        id:transitionMenu
        onClosed: Qt.callLater(function() { transitionMenuLoader.active = false })
        onVisibleChanged: Commands.popupVisibility(transitionMenu, visible)
        Component.onDestruction: Commands.popupVisibility(transitionMenu, false)
        objectName:"clipTransitionMenu"
        C.MenuItem {text:qsTr("Remove transition");enabled:root.transitionMenuIncoming ? root.mixInFrac>0 : root.mixOutFrac>0;onTriggered:root.transitionRemoveRequested(root.transitionMenuIncoming ? "dissolveIn" : "dissolve")}
        C.MenuItem {text:qsTr("Remove fade in");enabled:root.fadeInFrac>0;onTriggered:root.transitionRemoveRequested("in")}
        C.MenuItem {text:qsTr("Remove fade out");enabled:root.fadeOutFrac>0;onTriggered:root.transitionRemoveRequested("out")}
    }
    }
    Text {
        objectName:"transitionActionsBadge"
        anchors.right:parent.right;anchors.bottom:parent.bottom;anchors.margins:3
        anchors.rightMargin:3 + root.badgeInset
        visible:root.mixOutFrac>0 || root.fadeInFrac>0 || root.fadeOutFrac>0
        text:"↔";font.pixelSize:14;color:Theme.textPrimary;z:9
        MouseArea {anchors.fill:parent;anchors.margins:-3;onClicked:{root.transitionMenuIncoming = false;transitionMenuLoader.active = true}}
    }

    Repeater { // clip markers
        model: root.markers
        Item {
            id: flag
            required property var modelData
            required property int index
            x: Math.round(modelData.frac * root.width) - 1
            y: 0
            width: 7
            height: 9
            z: 8 // above the volume line's full-block MouseArea, like the fx badge
            objectName: "clipMarker:" + index
            Accessible.role: Accessible.Button
            Accessible.name: modelData.text !== "" ? modelData.text : qsTr("Clip marker")
            Rectangle { x: 0; y: 0; width: 2; height: 9; color: Theme.markerTint(flag.modelData) }
            Rectangle { x: 2; y: 0; width: 5; height: 5; color: Theme.markerTint(flag.modelData) }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: root.markerClicked(flag.index)
            }
        }
    }


        }
    }
}
