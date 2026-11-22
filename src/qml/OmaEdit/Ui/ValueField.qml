import QtQuick
import QtQuick.Controls as C
import "MediaTime.js" as MediaTime

// Numeric field with horizontal drag-scrubbing — the Inspector's Position,
// Scale and Rotation boxes. Dragging is how these are actually used in an
// edit; typing is the precise fallback, so both must work and neither may
// fight the other: a press that never travels becomes a text edit, a press
// that travels becomes a scrub.
Rectangle {
    id: root

    property real value: 0
    property real from: -999999
    property real to: 999999
    // Value change per pixel dragged. Fine drag (Shift) divides this by ten.
    property real step: 1.0
    property int decimals: 1
    property string suffix: ""
    // A length: when set, `value` counts frames at this rate and the field
    // shows and takes time (2.5 s, 1:30) instead of a frame count.
    property real fps: 0
    readonly property bool timed: fps > 0
    property string label: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    // Off where the page already says the same beside the field: the tip is
    // then only read out, and no bubble covers the row above.
    property bool showsTip: true
    property bool editable: true
    readonly property bool scrubbing: scrub.scrubbing

    signal edited(real value)
    signal editStarted()
    signal editFinished()

    implicitWidth: 72
    implicitHeight: Theme.hControl
    color: Theme.controlBg
    radius: Theme.rControl
    border.width: input.activeFocus ? Theme.focusRing : Theme.hairline
    border.color: input.activeFocus ? Theme.accent
                : scrub.scrubbing ? Theme.borderStrong
                : scrub.containsMouse ? Theme.borderStrong : Theme.border
    opacity: enabled ? 1.0 : Theme.disabledOpacity

    Accessible.role: Accessible.SpinBox
    Accessible.name: label
    Accessible.description: tip !== "" ? tip + ". " + display() : display()

    function display() {
        if (timed) return MediaTime.durationText(value, fps)
        return value.toFixed(decimals) + (suffix.trim() ? " " + suffix.trim() : "")
    }
    function editText() { return timed ? MediaTime.clockText(value, fps) : value.toFixed(decimals) }
    function parsed(text) {
        if (!timed) return Number(text)
        const frames = MediaTime.parseDuration(text, fps)
        return frames < 0 ? NaN : frames
    }

    // Takes what is typed now, as Enter would: for a dialog whose OK takes no
    // focus from the field and reads its value at once.
    function commitTyped() { input.finish(true) }

    function commit(v) {
        if (!isFinite(v)) return
        const clamped = Math.max(from, Math.min(to, timed ? Math.round(v) : v))
        if (clamped !== value) {
            value = clamped
            edited(clamped)
        }
    }

    TextMetrics {
        id: displayMetrics
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsControl
        text: root.display()
    }

    C.TextField {
        id: input
        anchors.fill: parent
        // Leave enough room for common values such as "100.0 %" in the
        // inspector's 62 px fields at every interface density.
        anchors.leftMargin: Theme.s1
        anchors.rightMargin: Theme.s1
        readOnly: !root.editable
        focusPolicy: root.editable ? Qt.StrongFocus : Qt.NoFocus
        readonly property bool keyboardConsumesEscape: true
        property bool editing: false
        property bool changed: false
        function finish(accept) {
            if (!editing) return
            editing = false
            if (accept && changed && acceptableInput && text.trim() !== "")
                root.commit(root.parsed(text))
            text = Qt.binding(() => root.display())
        }
        function nudge(direction) {
            const typed = acceptableInput && text.trim() !== "" ? root.parsed(text) : NaN
            const base = isFinite(typed) ? typed : root.value
            root.commit(base + direction * root.step)
            text = root.editText()
            changed = false
            selectAll()
        }
        // Explicit: the app's Material style would otherwise pad the text
        // field by 12 px a side and scroll a centred "25.0 %" to its tail.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0

        text: root.display()
        color: Theme.textPrimary
        font.family: Theme.monoFamily
        // A formatted readout must retain its sign and units. Editing keeps
        // the normal font and the usual caret scrolling for long input.
        font.pixelSize: activeFocus ? Theme.fsControl : Math.max(8, Math.min(Theme.fsControl, Math.floor(Theme.fsControl * width / Math.max(1, displayMetrics.advanceWidth))))
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        padding: 0
        background: null
        validator: root.timed ? timeInput : numberInput
        DoubleValidator { id: numberInput; bottom: root.from; top: root.to; locale: "C" }
        RegularExpressionValidator { id: timeInput; regularExpression: /[0-9:.,sSfFecondr ]*/ }
        Accessible.name: root.label
        Accessible.description: root.tip
        onTextEdited: changed = true

        onActiveFocusChanged: {
            // Entering the field swaps the formatted display for a raw,
            // fully-selected number so the whole value is replaceable.
            if (activeFocus) {
                editing = true
                changed = false
                text = root.editText()
                selectAll()
            } else {
                finish(true)
            }
        }
        onAccepted: {
            finish(true)
            focus = false
        }
        Keys.onEscapePressed: { finish(false); focus = false }
        // A number has no spaces: Space lands the value and goes to the editor,
        // as it would from anywhere else.
        Keys.onSpacePressed: function (event) {
            if (Commands.popupOpen && !Commands.transportInPopup) { event.accepted = false; return }
            finish(true)
            focus = false
            Commands.trigger("transport.play")
        }
        Keys.onUpPressed: nudge(1)
        Keys.onDownPressed: nudge(-1)
    }

    // Sits above the field and swallows the press only while the field is not
    // being typed into; once it has focus, this steps aside so selection,
    // caret placement and drag-select all behave normally.
    MouseArea {
        id: scrub
        anchors.fill: parent
        enabled: root.editable && !input.activeFocus
        hoverEnabled: true
        cursorShape: scrubbing ? Qt.SizeHorCursor
                   : containsMouse ? Qt.SizeHorCursor : Qt.ArrowCursor

        property bool scrubbing: false
        property real startX: 0
        property real startValue: 0

        onPressed: function (mouse) {
            startX = mouse.x
            startValue = root.value
            scrubbing = false
        }

        onPositionChanged: function (mouse) {
            if (!pressed) return
            const dx = mouse.x - startX
            if (!scrubbing) {
                if (Math.abs(dx) < Theme.dragThreshold) return
                scrubbing = true
                root.editStarted()
            }
            const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
            root.commit(startValue + dx * root.step * (fine ? 0.1 : 1.0))
        }

        onReleased: {
            // A press that never travelled is a request to type, not to scrub.
            if (!scrubbing)
                input.forceActiveFocus()
            else
                root.editFinished()
            scrubbing = false
        }
        onCanceled: { if (scrubbing) root.editFinished(); scrubbing = false }
    }

    HoverHandler { id: tipHover }
    Tooltip { text: root.tip; visible: root.showsTip && tipHover.hovered && !input.activeFocus && root.tip !== "" }
}
