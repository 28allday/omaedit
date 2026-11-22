import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import "../Ui/MediaTime.js" as MediaTime

// ── Precision editor (guide p. 548): expanded view of one edit
// point. Outgoing clip on top, incoming below; dim zones are the
// unused media handles. Dragging the centre line ROLLS the edit;
// dragging either clip's edge (or clicking a strip) RIPPLES it.
// Keyboard: Tab reaches the outgoing edge, the incoming edge and the
// edit line. Left/Right (or , and .) move one frame, Shift moves ten,
// Home/End go to the limit; each key press is one undo step. Up/Down
// still walk edit points and Esc still closes. Reaching a limit is
// announced. The global ',' '.' edge nudges act on the timeline
// selection, so these handles take those keys while they have focus.
Rectangle {
    id: precisionPanel
    visible: win.precisionBoundary >= 0
    anchors.fill: timelineScroll
    color: Ui.Theme.windowBg
    function lengthText(frames) { return MediaTime.durationText(frames, backend.fps) }
    z: 30

    readonly property var clipA:
        win.precisionBoundary >= 0
        ? win.selection.trackEntry(win.precisionTrack, win.precisionBoundary) : null
    readonly property var clipB:
        win.precisionBoundary >= 0
        ? win.selection.trackEntry(win.precisionTrack, win.precisionBoundary + 1) : null
    // A transition on this cut lies across it: aLent frames of the outgoing
    // clip's media after the cut and bLent of the incoming clip's before it
    // are in use by it (the entries count them). The strips are drawn as the
    // clips are, cut to cut, with all the media past the cut dimmed; an edge
    // can move by what is spare beyond the transition (aSpare, bSpare), and
    // each clip keeps a frame of its own outside it (aRoom, bRoom).
    readonly property bool across: !!clipA && !!clipB && clipA.mixOut > 0 && clipB.mixIn > 0 && !!clipA.mixAlign
                                   && Math.max(0, clipA.mixExtA || 0) + Math.max(0, clipA.mixExtB || 0) >= clipA.mixOut
    readonly property int aLent: across ? Math.max(0, clipA.mixExtA) : 0
    readonly property int bLent: across ? Math.max(0, clipA.mixExtB) : 0
    readonly property int aFrames: clipA ? clipA.frames - aLent : 1
    readonly property int aSpare:
        clipA ? Math.max(0, clipA.length - 1 - clipA.out) : 0
    readonly property int aTail: aSpare + aLent
    readonly property int bFrames:
        clipB ? clipB.frames + aLent - (clipB.mixOut > 0 && !!clipB.mixAlign ? Math.max(0, clipB.mixExtA) : 0) : 1
    readonly property int bSpare: clipB ? clipB.in : 0
    readonly property int bHead: bSpare + bLent
    readonly property int aRoom: clipA ? Math.max(0, clipA.frames - (clipA.mixOut > 0 ? clipA.mixOut : 0) - 1) : 0
    readonly property int bRoom: clipB ? Math.max(0, clipB.frames - (clipB.mixOut > 0 ? clipB.mixOut : 0) - 1) : 0
    // Live drag previews, in frames.
    property int dRoll: 0
    property int dOut: 0
    property int dIn: 0
    readonly property real pxf:
        (width - 48) / Math.max(1, Math.max(aFrames, bHead)
                                   + Math.max(aTail, bFrames))
    readonly property real baseX:
        24 + Math.max(aFrames, bHead) * pxf
    readonly property real lineX: baseX + dRoll * pxf
    readonly property bool aMaxed: aSpare - dRoll - dOut <= 0
    readonly property bool bMaxed: bSpare + dRoll + dIn <= 0

    // The last keyboard limit reached; spoken and shown until the next key.
    property string announcement: ""
    function say(text) {
        announcement = text
        backend.announce(text)
    }
    // One keyboard step on a handle: a single ripple or roll, clamped to the
    // media. A step that cannot move says which limit it met.
    function keyStep(kind, delta) {
        if (!clipA || !clipB) return
        var b = win.precisionBoundary
        var t = win.precisionTrack
        var lo, hi
        if (kind === "roll") {
            lo = Math.max(-aRoom, -bSpare)
            hi = Math.min(aSpare, bRoom)
        } else if (kind === "out") {
            lo = -aRoom
            hi = aSpare
        } else {
            lo = -bSpare
            hi = bRoom
        }
        var d = Math.max(lo, Math.min(hi, delta))
        if (d !== delta)
            say(limitText(kind, delta > 0))
        else
            announcement = ""
        if (d === 0)
            return
        if (kind === "roll")
            backend.rollEditOn(t, b, d)
        else if (kind === "out")
            backend.trimClipOn(t, b, clipA.in, clipA.out + d)
        else
            backend.trimClipOn(t, b + 1, clipB.in + d, clipB.out)
    }
    function limitText(kind, later) {
        if (kind === "roll")
            return later ? qsTr("The edit can't roll later: a clip has no more media.")
                         : qsTr("The edit can't roll earlier: a clip has no more media.")
        if (kind === "out")
            return later ? qsTr("The outgoing clip has no more media after this frame.")
                         : qsTr("The outgoing clip is already one frame long.")
        return later ? qsTr("The incoming clip is already one frame long.")
                     : qsTr("The incoming clip has no earlier media.")
    }
    function limitDelta(kind, later) { // Home/End: all the way
        if (kind === "roll")
            return later ? Math.min(aSpare, bRoom) : Math.max(-aRoom, -bSpare)
        if (kind === "out")
            return later ? aSpare : -aRoom
        return later ? bRoom : -bSpare
    }

    function commit() {
        var b = win.precisionBoundary
        var t = win.precisionTrack
        if (dRoll !== 0)
            backend.rollEditOn(t, b, dRoll)
        else if (dOut !== 0)
            backend.trimClipOn(t, b, clipA.in, clipA.out + dOut)
        else if (dIn !== 0)
            backend.trimClipOn(t, b + 1, clipB.in + dIn, clipB.out)
        dRoll = 0
        dOut = 0
        dIn = 0
    }

    Label {
        x: 24
        y: 8
        text: qsTr("Trim Editor — %1")
              .arg(qsTr("drag the line to roll, a clip edge to ripple; Tab then arrows trim; ↑/↓ walk edit points, Esc closes"))
        color: Ui.Theme.textSecondary
        font.pixelSize: Ui.Theme.fsLabel
    }
    Label {
        objectName: "precisionAnnouncement"
        x: 24
        y: 184
        visible: precisionPanel.announcement !== ""
        text: precisionPanel.announcement
        color: Ui.Theme.textPrimary
        font.pixelSize: Ui.Theme.fsLabel
    }

    // A keyboard handle over a pointer target: one tab stop, arrow keys
    // trim, a focus ring when reached from the keyboard.
    component KeyHandle: Item {
        id: handle
        property string kind: "out"
        property string label: ""
        property string detail: ""
        activeFocusOnTab: precisionPanel.visible
        readonly property bool keyboardConsumesKeys: true
        Accessible.role: Accessible.Slider
        Accessible.name: label
        Accessible.description: detail + " "
            + qsTr("Left and Right, or comma and full stop, move one frame; Shift moves ten; Home and End go to the limit. Up and Down move to other edit points; Escape closes.")
            + (precisionPanel.announcement !== "" ? " " + precisionPanel.announcement : "")
        Keys.onPressed: function (event) {
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) {
                event.accepted = false
                return
            }
            var step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Comma || event.key === Qt.Key_Less)
                precisionPanel.keyStep(kind, -step)
            else if (event.key === Qt.Key_Right || event.key === Qt.Key_Period || event.key === Qt.Key_Greater)
                precisionPanel.keyStep(kind, step)
            else if (event.key === Qt.Key_Home) {
                var early = precisionPanel.limitDelta(kind, false)
                if (early === 0) precisionPanel.say(precisionPanel.limitText(kind, false))
                else precisionPanel.keyStep(kind, early)
            } else if (event.key === Qt.Key_End) {
                var late = precisionPanel.limitDelta(kind, true)
                if (late === 0) precisionPanel.say(precisionPanel.limitText(kind, true))
                else precisionPanel.keyStep(kind, late)
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                precisionPanel.announcement = ""
                win.stepPrecision(event.key === Qt.Key_Up ? -1 : 1)
            } else {
                event.accepted = false // Escape and Tab keep their meaning
                return
            }
            event.accepted = true
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            color: "transparent"
            radius: Ui.Theme.rControl
            border.color: Ui.Theme.accent
            border.width: handle.activeFocus ? Ui.Theme.focusRing * 2 : 0
        }
    }
    FlatButton {
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: 4
        text: qsTr("Done")
        tip: qsTr("Close the trim editor and return to the timeline")
        kbd: "Esc"
        implicitHeight: 26
        onClicked: win.precisionBoundary = -1
    }

    component PrecisionStrip: Item {
        id: pStrip
        property var entry: null
        // Storyline index for the filmstrip decor (lanes have no tiles).
        property int decorIndex: -1
        property bool outgoing: true
        height: 52
        // Lit body span in frames, after the live previews.
        readonly property int litFrames: outgoing
            ? precisionPanel.aFrames + precisionPanel.dRoll
              + precisionPanel.dOut
            : precisionPanel.bFrames - precisionPanel.dRoll
              - precisionPanel.dIn
        readonly property real litX: outgoing
            ? 0
            : (precisionPanel.bHead + precisionPanel.dRoll
               + precisionPanel.dIn) * precisionPanel.pxf
        readonly property var stripUrls:
            decorIndex >= 0 && decorIndex < backend.timelineDecor.length
            && backend.timelineDecor[decorIndex].strip !== undefined
            ? backend.timelineDecor[decorIndex].strip : []

        Rectangle { // full media extent (body + handle), dim
            anchors.fill: parent
            color: Ui.Theme.controlBg
            border.color: Ui.Theme.border
            radius: win.rClip
        }
        Rectangle { // lit body
            x: pStrip.litX
            width: Math.max(2,
                            pStrip.litFrames * precisionPanel.pxf)
            height: parent.height
            radius: win.rClip
            clip: true
            color: Qt.rgba(Ui.Theme.accent.r, Ui.Theme.accent.g,
                           Ui.Theme.accent.b, 0.22)
            Row {
                anchors.fill: parent
                visible: pStrip.stripUrls.length > 0
                Repeater {
                    model: pStrip.stripUrls
                    Image {
                        required property var modelData
                        source: modelData
                        height: pStrip.height
                        width: pStrip.stripUrls.length > 0
                               ? pStrip.width
                                 / pStrip.stripUrls.length : 0
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                }
            }
        }
        Label {
            x: pStrip.litX + 6
            anchors.verticalCenter: parent.verticalCenter
            text: pStrip.entry ? pStrip.entry.name : ""
            color: Ui.Theme.clipLabelOnMedia
            font.pixelSize: Ui.Theme.fsLabel
            leftPadding: 4
            rightPadding: 4
            background: Rectangle {
                color: Ui.Theme.scrim
                radius: win.rPill(height)
            }
        }

        MouseArea { // click ripples the edit point to this frame
            anchors.fill: parent
            onClicked: function (mouse) {
                var b = win.precisionBoundary
                var frame = Math.floor(mouse.x
                                       / precisionPanel.pxf)
                if (pStrip.outgoing) {
                    var newOut = Math.max(
                        precisionPanel.clipA.in,
                        Math.min(precisionPanel.clipA.in + frame,
                                 precisionPanel.clipA.length - 1))
                    backend.trimClipOn(win.precisionTrack, b,
                                       precisionPanel.clipA.in, newOut)
                } else {
                    var newIn = Math.max(0,
                        Math.min(frame,
                                 precisionPanel.clipB.out))
                    backend.trimClipOn(win.precisionTrack, b + 1, newIn,
                                       precisionPanel.clipB.out)
                }
            }
        }
        MouseArea { // the clip's own edge — drag to ripple
            x: pStrip.litX
               + (pStrip.outgoing
                  ? pStrip.litFrames * precisionPanel.pxf - 5
                  : -5)
            width: 10
            height: parent.height
            cursorShape: Qt.SplitHCursor
            preventStealing: true
            property real pressX: 0
            onPressed: function (mouse) {
                pressX = mapToItem(precisionPanel, mouse.x, 0).x
            }
            onPositionChanged: function (mouse) {
                if (!pressed)
                    return
                var d = Math.round(
                    (mapToItem(precisionPanel, mouse.x, 0).x
                     - pressX) / precisionPanel.pxf)
                if (pStrip.outgoing)
                    precisionPanel.dOut = Math.max(
                        -precisionPanel.aRoom,
                        Math.min(d, precisionPanel.aSpare))
                else
                    precisionPanel.dIn = Math.max(
                        -precisionPanel.bSpare,
                        Math.min(d,
                                 precisionPanel.bRoom))
            }
            onReleased: precisionPanel.commit()
            Rectangle { // visible edge marker; red = out of media
                anchors.horizontalCenter: parent.horizontalCenter
                width: 3
                height: parent.height
                color: (pStrip.outgoing ? precisionPanel.aMaxed
                                        : precisionPanel.bMaxed)
                       ? Ui.Theme.danger : Ui.Theme.textPrimary
                opacity: 0.85
            }
        }
        KeyHandle { // the same edge, from the keyboard
            objectName: pStrip.outgoing ? "precisionOutEdge" : "precisionInEdge"
            kind: pStrip.outgoing ? "out" : "in"
            x: pStrip.litX
               + (pStrip.outgoing
                  ? pStrip.litFrames * precisionPanel.pxf - 5
                  : -5)
            width: 10
            height: parent.height
            label: pStrip.outgoing ? qsTr("Outgoing clip end") : qsTr("Incoming clip start")
            detail: pStrip.outgoing
                   ? qsTr("%1 used; %2 unused after it.")
                     .arg(precisionPanel.lengthText(precisionPanel.aFrames)).arg(precisionPanel.lengthText(precisionPanel.aTail))
                   : qsTr("%1 used; %2 unused before it.")
                     .arg(precisionPanel.lengthText(precisionPanel.bFrames)).arg(precisionPanel.lengthText(precisionPanel.bHead))
        }
    }

    PrecisionStrip { // outgoing clip + its unused tail
        entry: precisionPanel.clipA
        decorIndex: win.precisionTrack === 0 ? win.precisionBoundary : -1
        outgoing: true
        x: precisionPanel.baseX
           - precisionPanel.aFrames * precisionPanel.pxf
        y: 36
        width: (precisionPanel.aFrames + precisionPanel.aTail)
               * precisionPanel.pxf
    }
    PrecisionStrip { // incoming clip behind its unused head
        entry: precisionPanel.clipB
        decorIndex: win.precisionTrack === 0 ? win.precisionBoundary + 1 : -1
        outgoing: false
        x: precisionPanel.baseX
           - precisionPanel.bHead * precisionPanel.pxf
        y: 116
        width: (precisionPanel.bHead + precisionPanel.bFrames)
               * precisionPanel.pxf
    }

    Rectangle { // the edit line — drag to ROLL both sides
        x: precisionPanel.lineX - 1
        y: 28
        width: 2
        height: 148
        color: Ui.Theme.accent
    }
    MouseArea {
        id: rollGrip
        x: precisionPanel.lineX - 9
        y: 28
        width: 18
        height: 148
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        property real pressX: 0
        onPressed: function (mouse) {
            pressX = mapToItem(precisionPanel, mouse.x, 0).x
                     - precisionPanel.dRoll * precisionPanel.pxf
        }
        onPositionChanged: function (mouse) {
            if (!pressed)
                return
            var d = Math.round(
                (mapToItem(precisionPanel, mouse.x, 0).x - pressX)
                / precisionPanel.pxf)
            precisionPanel.dRoll = Math.max(
                Math.max(-precisionPanel.aRoom,
                         -precisionPanel.bSpare),
                Math.min(d, Math.min(precisionPanel.aSpare,
                                     precisionPanel.bRoom)))
        }
        onReleased: precisionPanel.commit()
        onDoubleClicked: win.precisionBoundary = -1
        KeyHandle { // the edit line, from the keyboard
            objectName: "precisionRoll"
            kind: "roll"
            anchors.fill: parent
            label: qsTr("Edit point")
            detail: qsTr("Rolls up to %1 earlier and %2 later.")
                   .arg(precisionPanel.lengthText(Math.min(precisionPanel.aRoom, precisionPanel.bSpare)))
                   .arg(precisionPanel.lengthText(Math.min(precisionPanel.aSpare, precisionPanel.bRoom)))
        }
        Rectangle { // grip knob between the strips
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 22
            radius: win.rPill(width)
            color: Ui.Theme.accent
        }
    }
}
