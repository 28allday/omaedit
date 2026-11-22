import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui
import "../Ui/Timecode.js" as Timecode

Ui.ModalDialog {
    id: root
    objectName: "markerDialog"
    title: clipMode ? qsTr("Edit Clip Marker") : qsTr("Edit Timeline Marker")
    width: Math.min(600, parent ? parent.width - Ui.Theme.s5 * 2 : 600)
    standardButtons: C.Dialog.NoButton
    closePolicy: pending ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property int frame: -1
    property int clipTrack: -1
    property int clipIndex: -1
    readonly property bool clipMode: clipTrack >= 0
    readonly property var typeKeys: ["standard", "chapter", "todo", "done"]
    property var target: ({})
    // The colour it was given ("" = its type's own).
    property string colour: ""
    property int pending: 0
    property string error: ""
    property string problem: ""
    readonly property string targetName: clipMode ? (target.entry || {}).name || qsTr("Clip") : target.sequenceName || qsTr("Timeline")
    readonly property int rate: Math.max(1,Math.round(target.fps || 25))
    readonly property int parsedFrame: parsePosition(positionField.text)
    readonly property string positionError: parsedFrame < 0 ? qsTr("Enter a timecode, such as %1.").arg(formatPosition(frame))
        : parsedFrame > (target.maximum || 0) ? qsTr("Position must be between %1 and %2.").arg(formatPosition(0)).arg(formatPosition(target.maximum || 0)) : ""
    // A timeline marker's timecode counts from the timeline's starting
    // timecode, as the ruler does; a clip marker's from the clip's first frame.
    readonly property int origin: clipMode ? 0 : backend.startFrame
    function formatPosition(value) { return Timecode.format(Math.max(0,value) + origin, rate) }
    // Read as every timecode field reads: pairs not typed keep the marker's
    // own hours, minutes and seconds. A timecode before the start is no
    // position (one past the last frame says so).
    function parsePosition(text) {
        var named = Timecode.read(text, rate, frame + origin)
        if (named < 0) return -1
        if (origin <= 0) return named
        var at = Timecode.place(named, origin, rate)
        return at.outside === 0 ? at.frame : (target.maximum || 0) + 1
    }
    function begin(track,index,marker) {
        clipTrack=track; clipIndex=index; frame=marker.frame
        target=backend.markerEditTarget(track,index,frame)
        error=""; problem=backend.markerEditProblem(target); pending=0
        markerName.text=(target.marker || marker).text || ""
        markerNotes.text=(target.marker || marker).notes || ""
        markerType.currentIndex=Math.max(0,typeKeys.indexOf((target.marker || marker).type || "standard"))
        colour=(target.marker || marker).colour || ""
        positionField.text=formatPosition(frame)
        open()
    }
    function openFor(marker) { begin(-1,-1,marker) }
    function openForClip(track,index,marker) { begin(track,index,marker) }
    function submit(remove) {
        problem=backend.markerEditProblem(target)
        if (pending || problem || (!remove && positionError)) return
        error=""
        pending=backend.applyMarkerEdit(target,remove ? frame : parsedFrame,typeKeys[markerType.currentIndex],markerName.text,markerNotes.text,remove,colour)
    }
    function refreshProblem() { if (visible && !pending) problem=backend.markerEditProblem(target) }
    onOpened: { markerName.forceActiveFocus(); markerName.selectAll() }
    Connections {
        target: backend
        function onTimelineEntriesChanged() { Qt.callLater(root.refreshProblem) }
        function onMarkersChanged() { Qt.callLater(root.refreshProblem) }
        function onTracksChanged() { Qt.callLater(root.refreshProblem) }
        function onSequencesChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectBusyChanged() { Qt.callLater(root.refreshProblem) }
        function onProjectPlaybackReset() { root.close(); root.target=({}) }
        function onMarkerEditFinished(request,changed,message) {
            if (request !== root.pending) return
            root.pending=0
            if (message) root.error=message
            else root.close()
        }
    }
    component Caption: C.Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        color: Ui.Theme.textSecondary; font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s3
        Caption { objectName: "markerTargetLabel"; text: qsTr("%1 · Marker at %2").arg(root.targetName).arg(root.formatPosition(root.frame)) }
        GridLayout {
            Layout.fillWidth: true; columns: 2
            rowSpacing: Ui.Theme.s2; columnSpacing: Ui.Theme.s3
            enabled: !root.pending && !root.problem
            Caption { Layout.fillWidth: false; text: qsTr("Name") }
            Ui.LineEdit { id: markerName; objectName: "markerNameField"; Layout.fillWidth: true; tip: qsTr("Name shown on the marker in the timeline and index"); hint: qsTr("Marker name"); onAccepted: root.submit(false) }
            Caption { Layout.fillWidth: false; text: qsTr("Position") }
            Ui.LineEdit { id: positionField; objectName: "markerPositionField"; Layout.fillWidth: true; tip: qsTr("Where the marker sits, as a timecode"); hint: qsTr("Marker position as a timecode"); font.family: Ui.Theme.monoFamily; onAccepted: root.submit(false) }
            Caption { Layout.fillWidth: false; text: qsTr("Type") }
            RowLayout {
                Ui.ComboField { id: markerType; objectName: "markerTypeField"; Layout.fillWidth: true; Accessible.name: qsTr("Marker type"); tip: qsTr("Marker kind and colour; To Do and Completed track tasks"); model: [qsTr("Standard"),qsTr("Chapter"),qsTr("To Do"),qsTr("Completed")] }
                Rectangle { Layout.preferredWidth: Ui.Theme.szIcon; Layout.preferredHeight: Ui.Theme.szIcon; radius: Ui.Theme.rControl; color: Ui.Theme.markerTint({ type: root.typeKeys[markerType.currentIndex], colour: root.colour }) }
            }
            Caption { Layout.fillWidth: false; text: qsTr("Colour") }
            Flow {
                objectName: "markerColourRow"
                Layout.fillWidth: true
                spacing: Ui.Theme.s1
                Repeater {
                    // First: the type's own colour; then the named ones.
                    model: [""].concat(backend.markerColours())
                    Rectangle {
                        id: swatch
                        required property string modelData
                        readonly property bool chosen: root.colour === modelData
                        objectName: "markerColour:" + (modelData === "" ? "type" : modelData)
                        width: Ui.Theme.hControl; height: Ui.Theme.hControl
                        radius: Ui.Theme.rControl
                        color: Ui.Theme.markerTint({ type: root.typeKeys[markerType.currentIndex], colour: modelData })
                        border.width: chosen ? 2 : Ui.Theme.hairline
                        border.color: chosen ? Ui.Theme.textPrimary : Ui.Theme.border
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData === "" ? qsTr("The type's own colour") : Ui.Theme.markerColourNames[modelData]
                        Accessible.checked: chosen
                        Text {
                            anchors.centerIn: parent
                            visible: swatch.modelData === ""
                            text: qsTr("A"); font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel; font.bold: true
                            color: "#111111"
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.colour = swatch.modelData
                            C.ToolTip.visible: containsMouse
                            C.ToolTip.delay: 500
                            C.ToolTip.text: swatch.Accessible.name
                        }
                    }
                }
            }
        }
        Caption { text: root.clipMode ? qsTr("Clip positions use retimed source frames and follow trims and moves. Last available frame: %1.").arg(root.formatPosition(root.target.maximum || 0)) : qsTr("Position is the timeline's timecode. Last available frame: %1.").arg(root.formatPosition(root.target.maximum || 0)) }
        Caption { text: qsTr("Notes") }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            Layout.fillWidth: true; Layout.preferredHeight: Ui.Theme.hControl * 4
            clip: true; enabled: !root.pending && !root.problem
            C.TextArea {
                id: markerNotes; objectName: "markerNotesField"
                placeholderText: ""; Accessible.name: qsTr("Marker notes")
                wrapMode: TextEdit.Wrap; selectByMouse: true
                leftPadding: Ui.Theme.s2; rightPadding: Ui.Theme.s2
                topPadding: Ui.Theme.s2; bottomPadding: Ui.Theme.s2
                topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
                color: Ui.Theme.textPrimary; placeholderTextColor: Ui.Theme.textMuted
                selectionColor: Ui.Theme.accent; selectedTextColor: Ui.Theme.accentText
                font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsControl
                background: Rectangle { color: Ui.Theme.controlBg; radius: Ui.Theme.rControl; border.width: Ui.Theme.hairline; border.color: markerNotes.activeFocus ? Ui.Theme.accent : Ui.Theme.border }
            }
        }
        Caption { objectName: "markerValidation"; visible: text !== ""; color: Ui.Theme.warning; text: root.problem || root.error || root.positionError }
        Caption { text: root.pending ? qsTr("Updating marker…") : qsTr("Changes and deletion can be undone in one step. Cancel leaves this marker unchanged.") }
    }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
        spacing: Ui.Theme.s2
        Ui.ToolButton { objectName: "markerDelete"; text: qsTr("Delete Marker"); enabled: !root.pending && !root.problem; onClicked: root.submit(true) }
        Item { Layout.fillWidth: true }
        Ui.ToolButton { objectName: "markerCancel"; text: qsTr("Cancel"); enabled: !root.pending; onClicked: root.close() }
        Ui.ToolButton { objectName: "markerSave"; text: qsTr("Save"); enabled: !root.pending && !root.problem && !root.positionError; onClicked: root.submit(false) }
        }
    }
}
