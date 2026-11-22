import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Edit as Edit
import OmaEdit.Ui as Ui

// ── Timeline: skim ruler + connected lane + primary storyline ───────
// Hosts the timeline index (Ctrl+Shift+2), the precision editor, the track headers and the scroller itself.
Rectangle {
    // Handles the window reaches by id (zoom, centring, key focus).
    property alias timelineScroll: timelineScroll
    property alias indexSearch: indexPanel.indexSearch
    property alias ruler: timelineScroll.ruler
    property alias connectedLane: timelineScroll.connectedLane
    property alias storyline: timelineScroll.storyline
    property alias audioLane: timelineScroll.audioLane
    property alias attachedLane: timelineScroll.attachedLane

    Layout.fillWidth: true
    // The index list needs headroom the bare timeline doesn't.
    // The brief's split: 54 % working area over 46 % timeline. Tracks past
    // what fits scroll vertically inside the scroller.
    Layout.preferredHeight: Math.round(parent.height * 0.46)
                            + (win.showTimelineIndex
                               || win.precisionBoundary >= 0 ? 96 : 0)
    color: win.wellColor

    Rectangle { // seam against the transport bar
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: win.seamColor
        z: 2
    }


    TimelineIndex { id: indexPanel }

    // Stereo meters pinned to the far right, fed by the engine's dBFS levels
    // (−60…0 across the bar) with a peak hold that decays while playing.
    Edit.AudioMeters {
        id: meters
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        visible: backend.hasVideo
        width: visible ? implicitWidth : 0
        function fill(db) { return Math.max(0, Math.min(1, (db + 60) / 60)) }
        levelLeft: fill(backend.meters.left)
        levelRight: fill(backend.meters.right)
        peakLeft: 0
        peakRight: 0
        Connections {
            target: backend.meters
            function onLevelsChanged() {
                meters.peakLeft = Math.max(meters.peakLeft, meters.fill(backend.meters.left))
                meters.peakRight = Math.max(meters.peakRight, meters.fill(backend.meters.right))
            }
        }
        Timer {
            interval: 80; repeat: true; running: meters.visible
            onTriggered: {
                meters.peakLeft = Math.max(meters.levelLeft, meters.peakLeft - 0.01)
                meters.peakRight = Math.max(meters.levelRight, meters.peakRight - 0.01)
            }
        }
    }

    Rectangle { // seam between the index and the timeline
        visible: win.showTimelineIndex
        anchors.left: indexPanel.right
        anchors.leftMargin: 6
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        width: 1
        color: win.seamColor
    }


    PrecisionEditor { id: precisionPanel }

    TrackHeaders { id: trackHeaders }

    TimelineScroller { id: timelineScroll }
    ColumnLayout {
        objectName: "createFirstTimeline"
        anchors.centerIn: parent
        visible: !backend.hasTimeline
        spacing: Ui.Theme.s2
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Create your first timeline")
            color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsTitle
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Choose its name and format. You can add more timelines later.")
            color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel
        }
        Ui.ToolButton {
            objectName: "createFirstTimelineButton"
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Create Timeline…")
            enabled: !backend.projectBusy && !backend.exporting
            onClicked: Ui.Commands.trigger("sequence.new")
        }
    }
}
