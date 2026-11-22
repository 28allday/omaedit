import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// A monitor: label above, the picture, transport below — nothing overlapping
// the image, because an editor judges frames, not chrome. One component serves
// both the program monitor and, in the two-up layout, the source monitor.
//
// It owns no playback state. The owner binds the readouts and answers the
// transport signals, so the same file renders against a mock in the preview
// and against the engine in the app. Whatever the owner declares inside it
// (the video surface, a filmstrip, a badge) lands on the stage.
Rectangle {
    id: root

    property string title: qsTr("Program")
    property string projectName: ""
    property string timecode: "00:00:00:00"
    property string duration: "00:00:00:00"
    property bool playing: false
    property real speed: 1.0
    // False disables the transport and shows the empty prompt on the stage.
    property bool hasMedia: true
    property bool fullscreenAvailable: false
    signal fullscreenRequested()
    // Two-up layout: the monitor the transport currently drives.
    property bool highlighted: false
    // Viewing a source clip rather than the edit: accent timecode + badge.
    property bool sourceMode: false
    property string emptyText: ""
    // Source mode: what an edit takes of the clip, "both", "picture" or
    // "sound". `editTakesEffective` is what the clip shown can give: where it
    // differs (no sound, or no picture) the choice shows dimmed.
    property string editTakes: "both"
    property string editTakesEffective: editTakes
    // A multicam clip is taken whole: its sound follows the angles.
    property bool sourceMulticam: false
    signal editTakesSelected(string takes)

    // Overlays over the fitted picture: safe areas (90 % action, 80 % title)
    // and guides. `imageAspect` (w/h) says where the picture sits on the
    // black stage.
    property real imageAspect: 16 / 9
    property bool safeAreas: false
    property string guides: "none"      // none | thirds | cross
    // Transform overlay: the selected clip's rect as fractions of the
    // frame. Draggable (body moves, corners resize) when editable; the
    // Picture updates are live; the gesture boundary gives the owner one undo.
    property var transformRect: null    // { x, y, w, h } or null
    property bool transformEditable: false
    // The keyer's eyedropper: armed, a click on the picture gives its point
    // (0..1 across and down).
    property bool picking: false
    signal pointPicked(real x, real y)
    // Which overlay the stage shows for the selected clip: the transform
    // rect, or the crop box (dimmed outside, drag an edge). The owner keeps
    // the choice.
    property string overlayMode: "transform" // transform | crop
    signal overlayModeSelected(string mode)
    // Crop overlay: the selected clip's crop as frame fractions cut off
    // each edge, { l, r, t, b } or null. The picture follows the drag live.
    property var cropRect: null
    property bool cropEditable: false
    signal cropEdited(real l, real r, real t, real b)
    signal cropStarted()
    signal cropFinished()
    property bool multicamAvailable: false // the multicam viewer toggle shows
    // Three-up layouts (multicam grid + program) squeeze the transport:
    // the extras stand down before the three rows would overlap.
    readonly property bool roomy: width >= Math.max(640, leastWidth + transportExtras)
    // The narrowest the monitor can be with its three transport rows side by
    // side and none over another (the extras stood down); and what the extras
    // add. Neither depends on whether the extras show.
    function rowCore(row, extras) {
        let sum = 0, count = 0
        for (let i = 0; i < row.children.length; ++i) {
            const child = row.children[i]
            if (extras.indexOf(child) >= 0 || !child.visible)
                continue
            sum += child.width
            ++count
        }
        return sum + Math.max(0, count - 1) * row.spacing
    }
    readonly property real leastWidth: Theme.s3 * 2 + Theme.s2 * 2 + rowCore(transportLeft, [fit]) + transportCentre.width
                                       + rowCore(transportRight, [snapshotButton, durationText, fullscreenButton])
    readonly property real transportExtras: snapshotButton.width + durationText.implicitWidth
                                            + fullscreenButton.width + Theme.s1 * 3
    property bool multicam: false
    signal multicamToggled(bool on)
    signal frameExportRequested()
    // Comparison view: the owner wipes a "before" surface over the
    // picture; the monitor only carries the toggle and the split.
    property bool compareAvailable: false
    property bool compare: false
    property real compareSplit: 0.5
    signal compareToggled(bool on)

    // The colour around the picture, so a tall or narrow frame shows its edges.
    // Playback resolution: 1 (full), 2 (half) or 4 (quarter) of the timeline's
    // size while playing. The picture keeps its size in the viewer.
    property int resolutionDivisor: 1
    signal resolutionSelected(int divisor)
    // Which files playback reads: the lighter proxies or the originals.
    // `mediaLocked` is set while the timeline can only play originals.
    property bool mediaAvailable: false
    property bool proxyPlayback: true
    property bool mediaLocked: false
    signal mediaSelected(bool proxies)
    property color canvasColor: "black"
    signal canvasSelected(string colour)
    signal canvasCustomRequested()
    signal safeAreasToggled(bool on)
    signal guidesSelected(string mode)
    // An effect's mask shown for placing on the picture (see EffectMaskOverlay);
    // while it is, the transform and crop boxes stand aside.
    property var effectMask: null
    property bool effectMaskEditable: true
    signal effectMaskEdited(var mask)
    signal effectMaskStarted()
    signal effectMaskFinished()
    signal transformEdited(real x, real y, real w, real h)
    signal transformStarted()
    signal transformFinished()

    // Where the picture lands inside the stage (aspect-fit, 2 px inset like
    // the video surface the owner places there).
    property var surfaceRect: null
    readonly property rect imageRect: {
        if (surfaceRect !== null) return surfaceRect
        const sw = stage.width - 4, sh = stage.height - 4
        if (sw <= 0 || sh <= 0 || imageAspect <= 0)
            return Qt.rect(0, 0, 0, 0)
        let w = sw, h = sw / imageAspect
        if (h > sh) {
            h = sh
            w = sh * imageAspect
        }
        return Qt.rect(2 + (sw - w) / 2, 2 + (sh - h) / 2, w, h)
    }

    // Typing a timecode in the readout. The owner reads what was typed
    // (`timecodeTaker(text)`) and answers "" when it has gone there, or the
    // words that say why not; the entry then stays, outlined, with them.
    property var timecodeTaker: null
    property string timecodeRefused: ""
    readonly property bool timecodeEntering: timecodeEntry.visible
    // Opens the entry, with `first` already typed (the key that asked).
    function beginTimecode(first) {
        if (!hasMedia || typeof timecodeTaker !== "function") return
        timecodeRefused = ""
        timecodeEntry.text = first || ""
        timecodeEntry.visible = true
        timecodeEntry.forceActiveFocus()
        timecodeEntry.cursorPosition = timecodeEntry.text.length
    }
    function endTimecode() {
        if (!timecodeEntry.visible) return
        timecodeEntry.visible = false
        timecodeRefused = ""
        timecodeEntry.focus = false
    }

    signal playToggled()
    signal paused()
    signal shuttled(int direction)  // -1 reverse, +1 forward
    signal startRequested()
    signal endRequested()

    default property alias stageContent: stage.data

    color: Theme.panelBg

    // Declared through `data` explicitly: with a default alias pointing at the
    // stage, bare children here would land on the stage too.
    data: [
        Rectangle {
            id: label
            anchors.top: parent.top
            width: parent.width
            height: Theme.hRow + Theme.s2
            color: Theme.panelBg

            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s3
                anchors.right: media.visible ? media.left : resolution.visible ? resolution.left : parent.right
                anchors.rightMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
                text: root.projectName !== "" ? qsTr("%1: %2").arg(root.title).arg(root.projectName)
                                              : root.title
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
            }

            SegmentedControl {
                id: media
                objectName: "playbackMedia"
                visible: root.mediaAvailable && !root.sourceMode
                // Stays hoverable while locked, so its tip can say why.
                opacity: root.mediaLocked ? Theme.disabledOpacity : 1
                anchors.right: resolution.left
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                height: resolution.height
                labels: [qsTr("Proxy"), qsTr("Original")]
                names: [qsTr("Play proxies"), qsTr("Play original files")]
                tip: root.mediaLocked
                     ? qsTr("This timeline's colour processing plays the original files")
                     : qsTr("Playback media: play the lighter proxies, or the original files. Clips without a proxy always play the original; exports always use the originals.")
                currentIndex: root.proxyPlayback && !root.mediaLocked ? 0 : 1
                onActivated: function (index) {
                    if (!root.mediaLocked) root.mediaSelected(index === 0)
                    currentIndex = Qt.binding(function () { return root.proxyPlayback && !root.mediaLocked ? 0 : 1 })
                }
            }

            ComboField {
                id: resolution
                objectName: "playbackResolution"
                tip: qsTr("Playback resolution: play at half or quarter of the timeline's resolution so a heavy timeline keeps its frame rate. The picture stays the same size, and is full resolution again when paused; exports are always full resolution.")
                visible: !root.sourceMode
                anchors.right: parent.right
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                width: 92
                height: Math.min(implicitHeight, parent.height - Theme.s1)
                model: [qsTr("Full"), qsTr("Half"), qsTr("Quarter")]
                currentIndex: Math.max(0, [1, 2, 4].indexOf(root.resolutionDivisor))
                onActivated: function (index) {
                    root.resolutionSelected([1, 2, 4][index])
                    currentIndex = Qt.binding(function () { return Math.max(0, [1, 2, 4].indexOf(root.resolutionDivisor)) })
                }
                Accessible.name: qsTr("Playback resolution")
            }
        },

        // The image stage, in the editor's chosen canvas colour: it only
        // ever shows around the picture, never through it.
        Rectangle {
            id: stage
            objectName: "monitorStage"
            anchors.top: label.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: transport.top
            anchors.margins: Theme.s2
            color: root.canvasColor
            border.width: root.highlighted ? Theme.selectionRing : 0
            border.color: Theme.accent
        },

        // Stage overlays that belong to the monitor itself, kept above
        // whatever the owner placed on the stage.
        Item {
            anchors.fill: stage
            clip: true

            MouseArea {
                objectName: "keyPick"
                z: 10
                anchors.fill: parent
                visible: root.picking
                cursorShape: Qt.CrossCursor
                onClicked: function (ev) {
                    const r = root.imageRect, x = (ev.x - r.x) / r.width, y = (ev.y - r.y) / r.height
                    if (x >= 0 && x <= 1 && y >= 0 && y <= 1) root.pointPicked(x, y)
                }
            }
            Rectangle {
                z: 10
                anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: Theme.s2
                visible: root.picking
                width: pickText.width + Theme.s3; height: Theme.hControl - 4; radius: Theme.rControl
                color: Theme.scrimStrong
                Text { id: pickText; anchors.centerIn: parent; text: qsTr("CLICK THE SCREEN TO PICK THE BACKDROP"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.accent }
            }

            Icon {
                anchors.centerIn: parent
                visible: !root.hasMedia && root.emptyText === ""
                name: "film"
                size: 40
                color: Qt.rgba(1, 1, 1, 0.08)
            }

            Column {
                anchors.centerIn: parent
                visible: !root.hasMedia && root.emptyText !== ""
                spacing: Theme.s3

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.emptyText
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsTimecode
                    color: Theme.textSecondary
                }
            }

            // Safe areas: action (90 %) and title (80 %) as hairline frames.
            Item {
                id: safeOverlay
                objectName: "safeAreas"
                visible: root.safeAreas && root.hasMedia
                x: root.imageRect.x; y: root.imageRect.y
                width: root.imageRect.width; height: root.imageRect.height
                Repeater {
                    model: [0.9, 0.8]
                    Rectangle {
                        required property real modelData
                        anchors.centerIn: parent
                        width: parent.width * modelData
                        height: parent.height * modelData
                        color: "transparent"
                        border.width: Theme.hairline
                        border.color: Theme.textPrimary
                        opacity: 0.35
                    }
                }
            }
            // Guides: rule of thirds, or a centre cross.
            Item {
                id: guideOverlay
                objectName: "guides"
                visible: root.guides !== "none" && root.hasMedia
                x: root.imageRect.x; y: root.imageRect.y
                width: root.imageRect.width; height: root.imageRect.height
                opacity: 0.35
                Repeater {
                    model: root.guides === "thirds" ? [1 / 3, 2 / 3]
                         : root.guides === "cross" ? [0.5] : root.guides === "grid" ? [0.125,0.25,0.375,0.5,0.625,0.75,0.875] : []
                    Rectangle {
                        required property real modelData
                        x: parent.width * modelData
                        width: Theme.hairline; height: parent.height
                        color: Theme.textPrimary
                    }
                }
                Repeater {
                    model: root.guides === "thirds" ? [1 / 3, 2 / 3]
                         : root.guides === "cross" ? [0.5] : root.guides === "grid" ? [0.125,0.25,0.375,0.5,0.625,0.75,0.875] : []
                    Rectangle {
                        required property real modelData
                        y: parent.height * modelData
                        height: Theme.hairline; width: parent.width
                        color: Theme.textPrimary
                    }
                }
            }
            // Transform: the clip's rect, with a body drag and corner handles.
            Item {
                id: xform
                objectName: "transformOverlay"
                visible: root.transformRect !== null && root.hasMedia
                         && root.overlayMode === "transform" && root.effectMask === null
                property bool dragging: false
                property var startRect: ({ x: 0, y: 0, w: 1, h: 1 })
                readonly property var r: dragging ? startRect : root.transformRect !== null
                                         ? root.transformRect : { x: 0, y: 0, w: 1, h: 1 }
                // Pending drag, in frame fractions.
                property real dx: 0
                property real dy: 0
                property real dw: 0
                property real dh: 0
                readonly property real fx: r.x + dx
                readonly property real fy: r.y + dy
                readonly property real fw: Math.max(0.001, Math.min(20, r.w + dw))
                readonly property real fh: Math.max(0.001, Math.min(20, r.h + dh))
                x: root.imageRect.x + fx * root.imageRect.width
                y: root.imageRect.y + fy * root.imageRect.height
                width: fw * root.imageRect.width
                height: fh * root.imageRect.height
                function begin() {
                    startRect = {x:r.x, y:r.y, w:r.w, h:r.h}
                    dx = 0; dy = 0; dw = 0; dh = 0
                    dragging = true
                    root.transformStarted()
                }
                function preview() {
                    if (dragging) root.transformEdited(fx, fy, fw, fh)
                }
                function commit() {
                    if (!dragging) return
                    preview()
                    root.transformFinished()
                    dx = 0; dy = 0; dw = 0; dh = 0
                    dragging = false
                }
                // A lost grab puts the box back where the drag began.
                function cancel() {
                    if (!dragging) return
                    dx = 0; dy = 0; dw = 0; dh = 0
                    preview()
                    root.transformFinished()
                    dragging = false
                }
                onVisibleChanged: if (!visible && dragging) commit()
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: root.transformEditable ? Theme.selectionRing : Theme.hairline
                    border.color: Theme.accent
                }
                MouseArea { // body: move
                    anchors.fill: parent
                    enabled: root.transformEditable
                    cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
                    property point press
                    onPressed: function (mouse) { press = mapToItem(stage, mouse.x, mouse.y); xform.begin() }
                    onPositionChanged: function (mouse) {
                        if (!pressed) return
                        const p = mapToItem(stage, mouse.x, mouse.y)
                        xform.dx = (p.x - press.x) / root.imageRect.width
                        xform.dy = (p.y - press.y) / root.imageRect.height
                        xform.preview()
                    }
                    onReleased: xform.commit()
                    onCanceled: xform.cancel()
                }
                Repeater { // corners: resize (the opposite corner stays put)
                    model: [{ cx: 0, cy: 0 }, { cx: 1, cy: 0 }, { cx: 0, cy: 1 }, { cx: 1, cy: 1 }]
                    Rectangle {
                        required property var modelData
                        visible: root.transformEditable
                        x: modelData.cx * parent.width - width / 2
                        y: modelData.cy * parent.height - height / 2
                        width: Theme.s2; height: Theme.s2
                        color: Theme.accent
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -Theme.s1
                            cursorShape: (modelData.cx === modelData.cy)
                                         ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                            property point press
                            onPressed: function (mouse) { press = mapToItem(stage, mouse.x, mouse.y); xform.begin() }
                            onPositionChanged: function (mouse) {
                                if (!pressed) return
                                const p = mapToItem(stage, mouse.x, mouse.y)
                                const ddx = (p.x - press.x) / root.imageRect.width
                                const ddy = (p.y - press.y) / root.imageRect.height
                                if (modelData.cx === 1) { xform.dw = ddx } else { xform.dw = -ddx; xform.dx = ddx }
                                if (modelData.cy === 1) { xform.dh = ddy } else { xform.dh = -ddy; xform.dy = ddy }
                                xform.preview()
                            }
                            onReleased: xform.commit()
                            onCanceled: xform.cancel()
                        }
                    }
                }
            }

            EffectMaskOverlay {
                objectName: "effectMaskOverlay"
                anchors.fill: parent
                mask: root.hasMedia ? root.effectMask : null
                editable: root.effectMaskEditable
                // Inside the clip's rect, as the crop box is: the effect runs
                // on the clip's own frame, which the transform then places.
                readonly property var placed: root.transformRect !== null ? root.transformRect : { x: 0, y: 0, w: 1, h: 1 }
                area: Qt.rect(root.imageRect.x + placed.x * root.imageRect.width, root.imageRect.y + placed.y * root.imageRect.height,
                              Math.max(1, placed.w * root.imageRect.width), Math.max(1, placed.h * root.imageRect.height))
                onEdited: function(mask) { root.effectMaskEdited(mask) }
                onStarted: root.effectMaskStarted()
                onFinished: root.effectMaskFinished()
            }

            Item { // crop overlay: dim what the crop cuts, drag an edge
                id: cropBox
                objectName: "cropOverlay"
                visible: root.overlayMode === "crop" && root.cropRect !== null && root.hasMedia && root.effectMask === null
                property bool dragging: false
                property var startCrop: ({ l: 0, r: 0, t: 0, b: 0 })
                readonly property var c: dragging ? startCrop : root.cropRect !== null
                                         ? root.cropRect : { l: 0, r: 0, t: 0, b: 0 }
                // The crop is a fraction of the clip's own picture, which
                // the transform then places: the box lives inside the
                // clip's rect (the whole frame for a full-frame clip).
                readonly property var area: root.transformRect !== null
                                            ? root.transformRect : { x: 0, y: 0, w: 1, h: 1 }
                readonly property real ax: root.imageRect.x + area.x * root.imageRect.width
                readonly property real ay: root.imageRect.y + area.y * root.imageRect.height
                readonly property real aw: Math.max(1, area.w * root.imageRect.width)
                readonly property real ah: Math.max(1, area.h * root.imageRect.height)
                // Pending edge drags, in picture fractions.
                property real dl: 0
                property real dr: 0
                property real dt: 0
                property real db: 0
                readonly property real fl: Math.max(0, Math.min(0.95 - c.r, c.l + dl))
                readonly property real fr: Math.max(0, Math.min(0.95 - fl, c.r + dr))
                readonly property real ft: Math.max(0, Math.min(0.95 - c.b, c.t + dt))
                readonly property real fb: Math.max(0, Math.min(0.95 - ft, c.b + db))
                x: ax + fl * aw
                y: ay + ft * ah
                width: (1 - fl - fr) * aw
                height: (1 - ft - fb) * ah
                function begin() {
                    startCrop = {l:c.l, r:c.r, t:c.t, b:c.b}
                    dl = 0; dr = 0; dt = 0; db = 0
                    dragging = true
                    root.cropStarted()
                }
                function preview() { if (dragging) root.cropEdited(fl, fr, ft, fb) }
                function commit() {
                    if (!dragging) return
                    preview()
                    root.cropFinished()
                    dl = 0; dr = 0; dt = 0; db = 0
                    dragging = false
                }
                function cancel() {
                    if (!dragging) return
                    dl = 0; dr = 0; dt = 0; db = 0
                    preview()
                    root.cropFinished()
                    dragging = false
                }
                onVisibleChanged: if (!visible && dragging) commit()
                // The cut-off bands, drawn around the kept picture.
                Rectangle { // left
                    x: cropBox.ax - cropBox.x; y: cropBox.ay - cropBox.y
                    width: cropBox.x - cropBox.ax; height: cropBox.ah
                    color: Theme.scrim
                }
                Rectangle { // right
                    x: cropBox.width; y: cropBox.ay - cropBox.y
                    width: cropBox.ax + cropBox.aw - cropBox.x - cropBox.width
                    height: cropBox.ah
                    color: Theme.scrim
                }
                Rectangle { // top
                    x: 0; y: cropBox.ay - cropBox.y
                    width: cropBox.width; height: cropBox.y - cropBox.ay
                    color: Theme.scrim
                }
                Rectangle { // bottom
                    x: 0; y: cropBox.height
                    width: cropBox.width
                    height: cropBox.ay + cropBox.ah - cropBox.y - cropBox.height
                    color: Theme.scrim
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: root.cropEditable ? Theme.selectionRing : Theme.hairline
                    border.color: Theme.accent
                }
                Repeater { // edge handles
                    model: ["left", "right", "top", "bottom"]
                    Rectangle {
                        required property string modelData
                        objectName: "cropHandle:" + modelData
                        visible: root.cropEditable
                        readonly property bool horizontal: modelData === "left" || modelData === "right"
                        width: horizontal ? Theme.s2 : 28
                        height: horizontal ? 28 : Theme.s2
                        x: modelData === "left" ? -width / 2
                           : modelData === "right" ? parent.width - width / 2
                           : (parent.width - width) / 2
                        y: modelData === "top" ? -height / 2
                           : modelData === "bottom" ? parent.height - height / 2
                           : (parent.height - height) / 2
                        radius: 2
                        color: Theme.accent
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -Theme.s1
                            cursorShape: parent.horizontal ? Qt.SizeHorCursor : Qt.SizeVerCursor
                            property point press
                            onPressed: function (mouse) { press = mapToItem(stage, mouse.x, mouse.y); cropBox.begin() }
                            onPositionChanged: function (mouse) {
                                if (!pressed) return
                                const p = mapToItem(stage, mouse.x, mouse.y)
                                const ddx = (p.x - press.x) / cropBox.aw
                                const ddy = (p.y - press.y) / cropBox.ah
                                if (parent.modelData === "left") cropBox.dl = ddx
                                else if (parent.modelData === "right") cropBox.dr = -ddx
                                else if (parent.modelData === "top") cropBox.dt = ddy
                                else cropBox.db = -ddy
                                cropBox.preview()
                            }
                            onReleased: cropBox.commit()
                            onCanceled: cropBox.cancel()
                        }
                    }
                }
            }

            Rectangle {
                visible: root.sourceMode
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: Theme.s3
                width: badge.implicitWidth + Theme.s2 * 2
                height: Theme.hRow
                radius: Theme.rControl
                color: Theme.accent

                Text {
                    id: badge
                    anchors.centerIn: parent
                    text: qsTr("SOURCE")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    font.letterSpacing: 1.5
                    color: Theme.accentText
                }
            }
        },

        Rectangle {
            id: transport
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.hControl + Theme.s3
            color: Theme.panelBg

            Row {
                id: transportLeft
                objectName: "transportLeft"
                anchors.left: parent.left
                anchors.leftMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2

                // The timecode readout, and the place a timecode is typed to
                // go to: a click (or the owner's key) opens the entry where
                // the readout stands, Enter hands it on, Escape leaves it.
                Item {
                    id: readout
                    width: Math.max(readoutText.implicitWidth, timecodeEntry.visible ? timecodeEntry.contentWidth + 2 : 0)
                    height: readoutText.implicitHeight
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        id: readoutText
                        objectName: "viewerTimecode"
                        text: root.timecode
                        visible: !timecodeEntry.visible
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsTimecode
                        color: root.sourceMode ? Theme.accent : Theme.textPrimary
                        opacity: root.hasMedia ? 1 : Theme.disabledOpacity
                    }
                    Rectangle {
                        visible: timecodeEntry.visible
                        anchors.fill: parent
                        anchors.margins: -Theme.s1
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: Theme.focusRing
                        border.color: root.timecodeRefused !== "" ? Theme.warning : Theme.accent
                    }
                    TextInput {
                        id: timecodeEntry
                        objectName: "viewerTimecodeEntry"
                        readonly property bool keyboardConsumesEscape: true
                        visible: false
                        width: parent.width
                        font: readoutText.font
                        color: readoutText.color
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentText
                        maximumLength: 16
                        // Only what a timecode or an amount is made of.
                        validator: RegularExpressionValidator { regularExpression: /[-+−]?[0-9:;.,]*/ }
                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Go to timecode")
                        onTextEdited: root.timecodeRefused = ""
                        onAccepted: {
                            if (text.trim() === "") { root.endTimecode(); return }
                            const said = typeof root.timecodeTaker === "function" ? root.timecodeTaker(text) : ""
                            if (said === "") root.endTimecode()
                            else { root.timecodeRefused = said; selectAll() }
                        }
                        Keys.onEscapePressed: root.endTimecode()
                        onActiveFocusChanged: if (!activeFocus && visible) root.endTimecode()
                    }
                    MouseArea {
                        objectName: "viewerTimecodeArea"
                        anchors.fill: parent
                        enabled: root.hasMedia && !timecodeEntry.visible && typeof root.timecodeTaker === "function"
                        cursorShape: Qt.IBeamCursor
                        onClicked: root.beginTimecode("")
                    }
                    Tooltip {
                        objectName: "viewerTimecodeMessage"
                        text: root.timecodeRefused
                        visible: timecodeEntry.visible && text !== ""
                        delay: 0
                    }
                }

                // Shuttle speed, only while it is not plain 1×.
                Text {
                    visible: root.playing && root.speed !== 1
                    text: (root.speed < 0 ? "◀ " : "▶ ") + Math.abs(root.speed) + "×"
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.accent
                    anchors.verticalCenter: parent.verticalCenter
                }

                ComboField {
                    id: fit
                    tip: qsTr("How the picture is scaled in the viewer")
                    // Out of sight until there is a second choice: a box
                    // greyed on its only one is no use to anyone.
                    visible: false
                    width: 74
                    // Only Fit exists until the viewer grows a zoom model.
                    model: [qsTr("Fit")]
                    enabled: false
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                id: transportCentre
                objectName: "transportCentre"
                // In the middle while there is room; between the other two
                // rows, clear of both, when there is not.
                x: Math.max(transportLeft.x + transportLeft.width + Theme.s2,
                            Math.min((parent.width - width) / 2, transportRight.x - Theme.s2 - width))
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1
                enabled: root.hasMedia

                IconButton {
                    iconName: "skip-back"; text: qsTr("Go to start"); shortcut: "Home"
                    onClicked: root.startRequested()
                }
                IconButton {
                    iconName: "rewind"; text: qsTr("Shuttle backwards"); shortcut: "J"
                    onClicked: root.shuttled(-1)
                }
                IconButton {
                    iconName: "square"; text: qsTr("Stop"); shortcut: "K"
                    onClicked: root.paused()
                }
                IconButton {
                    iconName: root.playing ? "pause" : "play"
                    text: root.playing ? qsTr("Pause") : qsTr("Play")
                    shortcut: "Space"
                    iconColor: Theme.textPrimary
                    onClicked: root.playToggled()
                }
                IconButton {
                    iconName: "fast-forward"; text: qsTr("Shuttle forwards"); shortcut: "L"
                    onClicked: root.shuttled(1)
                }
                IconButton {
                    iconName: "skip-forward"; text: qsTr("Go to end"); shortcut: "End"
                    onClicked: root.endRequested()
                }
            }

            Row {
                id: transportRight
                objectName: "transportRight"
                anchors.right: parent.right
                anchors.rightMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1

                SegmentedControl {
                    id: takes
                    objectName: "editTakes"
                    visible: root.sourceMode
                    readonly property var values: ["both", "picture", "sound"]
                    readonly property bool applies: root.editTakesEffective === root.editTakes
                    // Stays hoverable while dimmed, so its tip can say why.
                    opacity: applies ? 1 : Theme.disabledOpacity
                    anchors.verticalCenter: parent.verticalCenter
                    icons: ["film", "image", "audio-waveform"]
                    names: [qsTr("Picture and Sound"), qsTr("Picture Only"), qsTr("Sound Only")]
                    tip: !applies ? (root.sourceMulticam ? qsTr("A multicam clip is taken whole, its sound following its angles: edits use it as it is")
                                                         : qsTr("This clip has only one of picture and sound: edits use it as it is"))
                         : qsTr("What Insert, Overwrite, Append and a drag to the timeline use of the clip: %1").arg(names[Math.max(0, currentIndex)])
                    currentIndex: Math.max(0, values.indexOf(root.editTakes))
                    onActivated: function (index) {
                        root.editTakesSelected(values[index])
                        currentIndex = Qt.binding(function () { return Math.max(0, values.indexOf(root.editTakes)) })
                    }
                }
                // The frame on screen as a still: Export Current Frame's own button.
                IconButton { id: snapshotButton; objectName: "exportFrameButton"; visible: root.roomy; iconName: "camera"; text: qsTr("Export current frame…"); enabled: root.hasMedia; onClicked: root.frameExportRequested() }
                IconButton {
                    objectName: "multicamButton"
                    visible: root.multicamAvailable
                    iconName: "grid-2x2"; text: qsTr("Multicam viewer"); shortcut: "Ctrl+Shift+M"
                    checkable: true; checked: root.multicam
                    onClicked: root.multicamToggled(!root.multicam)
                }
                IconButton {
                    objectName: "compareButton"
                    visible: root.compareAvailable
                    iconName: "compare"; text: qsTr("Before / after — wipe between the clip as shot and the edit")
                    checkable: true; checked: root.compare
                    onClicked: { root.compareToggled(!root.compare); checked = Qt.binding(() => root.compare) }
                }
                IconButton {
                    objectName: "safeAreasButton"
                    iconName: "scan-line"; text: qsTr("Safe areas")
                    checkable: true; checked: root.safeAreas
                    onClicked: { root.safeAreasToggled(!root.safeAreas); checked = Qt.binding(() => root.safeAreas) }
                }
                IconButton {
                    id: overlayButton
                    objectName: "overlayButton"
                    iconName: "crop"; text: qsTr("Overlay — transform rect or crop box on the selected clip")
                    checkable: true; checked: root.overlayMode === "crop"
                    onClicked: { checked = Qt.binding(() => root.overlayMode === "crop"); overlayMenu.popup(overlayButton, 0, overlayButton.height) }
                    ContextMenu {
                        id: overlayMenu
                        MenuAction { text: qsTr("Transform"); checkable: true; checked: root.overlayMode === "transform"; onTriggered: { root.overlayModeSelected("transform"); checked = Qt.binding(() => root.overlayMode === "transform") } }
                        MenuAction { text: qsTr("Crop"); checkable: true; checked: root.overlayMode === "crop"; onTriggered: { root.overlayModeSelected("crop"); checked = Qt.binding(() => root.overlayMode === "crop") } }
                    }
                }
                IconButton {
                    id: guidesButton
                    objectName: "guidesButton"
                    iconName: "layout-grid"; text: qsTr("Guides and canvas colour")
                    checkable: true; checked: root.guides !== "none"
                    onClicked: { checked = Qt.binding(() => root.guides !== "none"); guidesMenu.popup(guidesButton, 0, guidesButton.height) }
                    ContextMenu {
                        id: guidesMenu
                        MenuAction { text: qsTr("No guides"); checkable: true; checked: root.guides === "none"; onTriggered: { root.guidesSelected("none"); checked = Qt.binding(() => root.guides === "none") } }
                        MenuAction { text: qsTr("Rule of thirds"); checkable: true; checked: root.guides === "thirds"; onTriggered: { root.guidesSelected("thirds"); checked = Qt.binding(() => root.guides === "thirds") } }
                        MenuAction { text: qsTr("Centre cross"); checkable: true; checked: root.guides === "cross"; onTriggered: { root.guidesSelected("cross"); checked = Qt.binding(() => root.guides === "cross") } }
                        C.MenuSeparator {}
                        // What shows around the picture, so a tall or narrow
                        // frame's edges stand out.
                        ContextMenu {
                            id: canvasMenu
                            objectName: "canvasMenu"
                            title: qsTr("Canvas colour")
                            Repeater {
                                model: CanvasColours.presets
                                MenuAction {
                                    required property var modelData
                                    required property int index
                                    text: modelData.name; checkable: true
                                    checked: CanvasColours.indexOf(root.canvasColor) === index
                                    onTriggered: { root.canvasSelected(modelData.colour); checked = Qt.binding(() => CanvasColours.indexOf(root.canvasColor) === index) }
                                }
                            }
                            MenuAction {
                                text: qsTr("Custom colour…"); checkable: true
                                checked: CanvasColours.indexOf(root.canvasColor) < 0
                                onTriggered: { root.canvasCustomRequested(); checked = Qt.binding(() => CanvasColours.indexOf(root.canvasColor) < 0) }
                            }
                        }
                    }
                }

                Text {
                    id: durationText
                    visible: root.roomy
                    text: root.duration
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textSecondary
                    opacity: root.hasMedia ? 1 : Theme.disabledOpacity
                    anchors.verticalCenter: parent.verticalCenter
                }

                IconButton { id: fullscreenButton; visible: root.roomy; iconName: "maximize"; text: qsTr("Full screen viewer"); enabled: root.fullscreenAvailable; onClicked: root.fullscreenRequested() }
            }
        }
    ]
}
