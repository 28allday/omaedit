import QtQuick
import QtQuick.Controls as C
import "MediaTime.js" as MediaTime
import "Timecode.js" as Timecode

// Typed timecode entry. Shows hh:mm:ss:ff for a frame and takes a timecode
// back as Timecode.js reads one: digits from the right, frames last, a full
// stop for a pair of noughts. Pairs not typed on the left keep the hours,
// minutes and seconds the field shows; in a field that holds a starting
// timecode (`start: true`) they are nought. As a length (`length: true`) it
// shows and takes time instead — 2.5 s, 1:30 — and still hands on whole frames.
Rectangle {
    id: root

    property int frame: 0
    property real fps: 25
    property bool length: false
    property bool start: false
    // The timecode of frame 0 where the frames are a timeline's: they are
    // shown, and typed, counted from it.
    property int origin: 0
    property bool editable: true
    property string label: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""

    signal edited(int frame)
    // Enter was pressed (after any edit was handed on).
    signal accepted()

    readonly property bool active: input.activeFocus
    // The last entry when it could not be read ("" once one is read or
    // something else is typed): the value before it stays, the border
    // warns, and `refusedText` says why, in the form's own warning line
    // where it has one (`refusalInForm`) and beside the field otherwise.
    property string refused: ""
    property bool refusalInForm: false
    // A field whose timecode applies at any rate (its `fps` only counts):
    // its warning names none.
    property bool anyRate: false
    readonly property string refusedText: refused === "" ? ""
        : anyRate ? qsTr("“%1” is not a timecode. Type hours:minutes:seconds:frames, such as 09:59:50:00.").arg(refused)
        : qsTr("“%1” is not a timecode at %2 fps. Type hours:minutes:seconds:frames, such as 09:59:50:00.")
              .arg(refused).arg(Number((fps > 0 ? fps : 25).toFixed(3)))
    property bool refusalHeld: false
    // For the button that confirms a form: takes what is typed, and is true
    // the first time an entry that could not be read meets it, so the form
    // stays open with the warning showing rather than going on with the
    // value before. A second press goes on.
    function commitHolds() {
        commit()
        if (refused === "" || refusalHeld) return false
        refusalHeld = true
        return true
    }
    function clearRefused() { refused = ""; refusalHeld = false; said.stop() }

    implicitWidth: 100
    implicitHeight: Theme.hControl
    radius: Theme.rControl
    color: editable ? Theme.controlBg : "transparent"
    border.width: editable ? (input.activeFocus || refused !== "" ? Theme.focusRing : Theme.hairline) : 0
    border.color: refused !== "" ? Theme.warning : input.activeFocus ? Theme.accent : Theme.border
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    function format(f) {
        return length ? MediaTime.durationText(f, fps) : Timecode.format(Math.max(0, Math.round(f)) + origin, fps)
    }
    // Takes what is typed now, for a button that does not take focus.
    // A field still holding the cursor is ready to be typed in again.
    function commit() { input.finish(true); if (input.activeFocus) input.selectAll() }
    // Puts the cursor in the field with its timecode selected.
    function focusInput() { input.forceActiveFocus() }
    function parse(text) {
        if (length) {
            const frames = MediaTime.parseDuration(text, fps)
            return frames <= 2147483647 ? frames : -1
        }
        const named = Timecode.read(text, fps, start ? -1 : frame + origin)
        return named < 0 || origin <= 0 ? named : Timecode.place(named, origin, fps).frame
    }

    C.TextField {
        id: input
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        readOnly: !root.editable
        readonly property bool keyboardConsumesEscape: true
        property bool editing: false
        property bool changed: false
        function finish(accept) {
            if (!editing) return
            editing = false
            const f = root.parse(text)
            if (accept && changed && f >= 0 && f !== root.frame) root.edited(f)
            if (!root.length && accept && changed) {
                root.clearRefused()
                if (f < 0 && text.trim() !== "") { root.refused = text.trim(); if (!root.refusalInForm) said.restart() }
            }
            text = Qt.binding(() => root.format(root.frame))
            // Taken by a button while the cursor stays here: what is typed
            // next is a new entry, not one to be dropped.
            editing = activeFocus; changed = false
        }
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        text: root.format(root.frame)
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsControl
        color: Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        verticalAlignment: Text.AlignVCenter
        background: null
        focusPolicy: root.editable ? Qt.StrongFocus : Qt.NoFocus

        Accessible.role: Accessible.EditableText
        Accessible.name: root.label
        Accessible.description: root.tip

        onAccepted: {
            finish(true)
            focus = false
            root.accepted()
        }
        Keys.onEscapePressed: {
            finish(false)
            focus = false
        }
        onTextEdited: { changed = true; if (root.refused !== "") root.clearRefused() }
        onActiveFocusChanged: {
            // Selected after the click that focused it has placed the
            // cursor, so typing replaces the whole timecode.
            if (activeFocus) { editing = true; changed = false; selectAll(); Qt.callLater(() => { if (input.activeFocus && !input.changed) input.selectAll() }) }
            else finish(true)
        }
    }

    HoverHandler { id: tipHover }
    // A refusal with no line of the form's to stand in is said here, once,
    // without waiting for the pointer.
    Timer { id: said; interval: 6000 }
    Tooltip {
        objectName: "timecodeMessage"
        text: root.refused !== "" ? root.refusedText : root.tip
        visible: text !== "" && (said.running || (tipHover.hovered && !input.activeFocus))
    }
}
