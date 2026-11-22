pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The strip above the audio timeline: tools, snap, the ruler unit, add
// track, record, loop, zoom, track height.
Rectangle {
    id: root

    property string tool: "select"
    property bool snapping: true
    property string units: "frames"      // frames | seconds | samples | milliseconds
    property string cursorText: ""
    signal cursorTextEdited(string text)
    signal sampleStepRequested(int samples)
    property bool recording: false
    property bool canRecord: false
    property bool loop: false
    property real zoom: 0
    property real fps: 25
    property int sampleRate: 48000
    property bool canZoom: false
    property bool automationVisible: true

    signal toolSelected(string tool)
    signal snappingToggled(bool on)
    signal unitsEdited(string units)
    signal addTrackRequested()
    signal recordToggled()
    signal loopToggled(bool on)
    signal zoomEdited(real level)
    signal fitRequested()
    signal tallerRequested()
    signal shorterRequested()
    signal automationToggled(bool on)
    signal markerRequested()
    signal mixToolsRequested()

    readonly property var tools: [
        { id: "select", icon: "mouse-pointer-2", name: qsTr("Select — move clips, drag edges to trim, drag the band for gain"), key: "A" },
        { id: "trim",   icon: "move-horizontal", name: qsTr("Trim — edge drags ripple the track"), key: "T" },
        { id: "blade",  icon: "scissors",        name: qsTr("Blade — click a clip to split it"), key: "B" },
        { id: "slip",   icon: "arrow-left-right",name: qsTr("Slip — drag a clip's content under its edges"), key: "Y" },
        { id: "range",  icon: "scan-line",       name: qsTr("Range — drag a loop / selection range"), key: "R" },
        { id: "hand",   icon: "hand",            name: qsTr("Hand — drag the view"), key: "H" },
        { id: "zoom",   icon: "zoom-in",         name: qsTr("Zoom — click in, Alt-click out"), key: "Z" }
    ]

    implicitHeight: Math.max(Theme.hEditToolbar, controls.implicitHeight + Theme.s1 * 2)
    color: Theme.panelBg

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }

    Flow {
        id: controls
        x: Theme.s2; y: Theme.s1
        width: Math.max(0, root.width - Theme.s2 * 2)
        spacing: Theme.s2
        Row {
            objectName: "audioToolbarGroup:tools"
            spacing: 2
            Repeater {
                model: root.tools
                delegate: IconButton {
                    required property var modelData
                    objectName: "audioTool:" + modelData.id
                    iconName: modelData.icon
                    shortcut: modelData.key
                    checked: root.tool === modelData.id
                    Accessible.name: modelData.name
                    onClicked: root.toolSelected(modelData.id)
                }
            }
        }
        Row {
            objectName: "audioToolbarGroup:editing"
            spacing: 2
            IconButton { iconName: "magnet"; shortcut: "N"; checked: root.snapping; Accessible.name: qsTr("Snap"); onClicked: root.snappingToggled(!root.snapping) }
            IconButton { objectName: "audioMixToolsButton"; iconName: "settings-2"; Accessible.name: qsTr("Audio tools: automation, routing and clips"); onClicked: root.mixToolsRequested() }
            IconButton { iconName: "flag"; shortcut: "M"; Accessible.name: qsTr("Add marker"); onClicked: root.markerRequested() }
            IconButton { iconName: "trending-up"; checked: root.automationVisible; Accessible.name: qsTr("Show automation"); onClicked: root.automationToggled(!root.automationVisible) }
        }
        Row {
            objectName: "audioToolbarGroup:recording"
            spacing: 2
            ToolButton { objectName: "audioAddTrack"; iconName: "plus"; text: qsTr("Track"); showLabel: true; Accessible.name: qsTr("Add audio track"); onClicked: root.addTrackRequested() }
            IconButton {
                objectName: "audioRecord"
                iconName: "circle-dot"
                iconColor: root.recording ? Theme.recordArm : root.canRecord ? Theme.textSecondary : Theme.textMuted
                enabled: root.canRecord || root.recording
                Accessible.name: root.recording ? qsTr("Stop recording") : qsTr("Record onto the armed track")
                onClicked: root.recordToggled()
            }
            IconButton { iconName: "refresh-cw"; checked: root.loop; Accessible.name: qsTr("Loop range"); onClicked: root.loopToggled(!root.loop) }
        }
        ComboField {
            objectName: "audioRulerUnits"
            width: 145
            caption: qsTr("Ruler")
            tip: qsTr("Timecode and Seconds move sound a whole picture frame at a time; Samples and Milliseconds move it finer")
            model: [qsTr("Timecode"), qsTr("Seconds"), qsTr("Samples"), qsTr("Milliseconds")]
            currentIndex: ["frames", "seconds", "samples", "milliseconds"].indexOf(root.units)
            onActivated: function(i) { root.unitsEdited(["frames", "seconds", "samples", "milliseconds"][i]) }
        }
        Row {
            objectName: "audioToolbarGroup:cursor"
            visible: root.units === "samples" || root.units === "milliseconds"
            spacing: 2
            IconButton {
                objectName: "audioPreviousSample"; iconName: "chevron-left"; enabled: root.canZoom
                Accessible.name: qsTr("Previous audio sample")
                onClicked: root.sampleStepRequested(-1)
            }
            C.TextField {
                id: cursorInput
                objectName: "audioCursorInput"
                readonly property string helpText: qsTr("Set the sound position in the selected unit. Fractions are accepted. Press Enter to seek or Escape to cancel.")
                width: 170; height: Theme.hControl; enabled: root.canZoom
                font.family: Theme.monoFamily; font.pixelSize: Theme.fsControl
                topPadding: 0; bottomPadding: 0; leftPadding: Theme.s2; rightPadding: Theme.s2
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.textPrimary; selectByMouse: true; hoverEnabled: true
                Accessible.name: root.units === "samples" ? qsTr("Sound position in samples") : qsTr("Sound position in milliseconds")
                Accessible.description: helpText
                C.ToolTip.text: helpText
                C.ToolTip.visible: hovered && !activeFocus
                C.ToolTip.delay: Theme.tooltipDelay
                text: root.cursorText
                onAccepted: { root.cursorTextEdited(text); focus = false; text = Qt.binding(() => root.cursorText) }
                onActiveFocusChanged: if (!activeFocus) text = Qt.binding(() => root.cursorText)
                Keys.onEscapePressed: { focus = false; text = Qt.binding(() => root.cursorText) }
                background: Rectangle {
                    radius: Theme.rControl; color: Theme.controlBg
                    border.color: cursorInput.activeFocus ? Theme.accent : Theme.border
                    border.width: Theme.hairline
                }
            }
            IconButton {
                objectName: "audioNextSample"; iconName: "chevron-right"; enabled: root.canZoom
                Accessible.name: qsTr("Next audio sample")
                onClicked: root.sampleStepRequested(1)
            }
        }
        Row {
            objectName: "audioToolbarGroup:zoom"
            spacing: 2
            IconButton { iconName: "zoom-out"; enabled: root.canZoom; Accessible.name: qsTr("Zoom out"); onClicked: root.zoomEdited(root.zoom / 1.5) }
            IconButton { iconName: "maximize"; enabled: root.canZoom; Accessible.name: qsTr("Fit timeline"); onClicked: root.fitRequested() }
            ToolButton {
                objectName: "audioSampleZoom"
                text: qsTr("Samples"); showLabel: true; enabled: root.canZoom
                tip: qsTr("Zoom to two pixels per project sample (%1 Hz)").arg(root.sampleRate)
                onClicked: { root.unitsEdited("samples"); root.zoomEdited(2 * Math.max(1, root.sampleRate) / Math.max(1, root.fps)) }
            }
            IconButton { iconName: "zoom-in"; enabled: root.canZoom; Accessible.name: qsTr("Zoom in"); onClicked: root.zoomEdited(root.zoom * 1.5) }
        }
        Row {
            objectName: "audioToolbarGroup:height"
            spacing: 2
            IconButton { iconName: "chevron-down"; Accessible.name: qsTr("Shorter tracks"); onClicked: root.shorterRequested() }
            IconButton { iconName: "chevron-up"; Accessible.name: qsTr("Taller tracks"); onClicked: root.tallerRequested() }
        }
    }
}
