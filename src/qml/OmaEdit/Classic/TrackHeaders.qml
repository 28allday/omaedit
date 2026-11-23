import QtQuick
import QtQuick.Controls
import omaedit
import OmaEdit.Ui as Ui
import OmaEdit.Edit as Edit

// Track headers: one per visible track, aligned with the rows inside the
// scroller (lane rows are 30 px; V1 is as tall as the storyline). The seam
// between Edit.TrackHeader and the engine's track flags.
Item {
    id: trackHeaders
    objectName: "trackHeaders"
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.left: indexPanel.right
    anchors.margins: 12
    anchors.leftMargin: win.showTimelineIndex ? 19 : 12
    width: Ui.Theme.wTrackHeader
    visible: backend.hasVideo
    clip: true

    // The plain wheel over the headers scrolls the track stack up and down,
    // the same as the scroll bar beside the tracks.
    WheelHandler {
        objectName: "trackHeadersWheel"
        acceptedModifiers: Qt.NoModifier
        enabled: timelineScroll.contentHeight > timelineScroll.height
        onWheel: function (event) { win.scrollTracks(event, timelineScroll); event.accepted = true }
    }

    component Header: Edit.TrackHeader {
        id: header
        property int track: -1
        readonly property var info: track >= 0 && track < backend.tracks.length
                                    ? backend.tracks[track] : null
        readonly property bool audio: info !== null && info.kind === "audio"
        width: trackHeaders.width
        visible: info !== null
        name: info !== null ? (info.mix && info.mix.editKind === "caption" ? "CC · " : "") + info.name : ""
        kind: audio ? "audio" : "video"
        destination: backend.destTrack === track
        locked: info !== null && info.locked === true
        armed: info !== null && info.mix && info.mix.armed === true
        onArmToggled:function(on){backend.setTrackMix(track,"armed",on)}
        autoSelect: info !== null && info.autoSelect === true
        visibleTrack: !(info !== null && info.disabled === true)
        muted: info !== null && info.muted === true
        soloed: info !== null && info.solo === true
        function sizeByWheel(event){const turn=win.wheelTurn(event);if(turn!==0)win.lanes.setOneTrackHeight(header.track,win.lanes.trackHeightFor(header.track)+(turn>0?4:-4));event.accepted=true}
        WheelHandler {acceptedModifiers:Qt.ShiftModifier;onWheel:function(event){header.sizeByWheel(event)}}
        WheelHandler {acceptedModifiers:Qt.ShiftModifier;orientation:Qt.Horizontal;onWheel:function(event){if(event.angleDelta.y===0)header.sizeByWheel(event)}}
        MouseArea {
            anchors.left:parent.left;anchors.right:parent.right;anchors.bottom:parent.bottom;height:4
            cursorShape:Qt.SizeVerCursor
            property real startY:0
            property int startHeight:0
            onPressed:function(mouse){startY=mapToItem(trackHeaders,0,mouse.y).y;startHeight=win.lanes.trackHeightFor(header.track)}
            onReleased:function(mouse){win.lanes.setOneTrackHeight(header.track,startHeight+mapToItem(trackHeaders,0,mouse.y).y-startY)}
        }
        onDestinationRequested: backend.destTrack = track
        onLockToggled: function (on) { backend.setTrackFlag(track, "locked", on) }
        onAutoSelectToggled: function (on) { backend.setTrackFlag(track, "autoSelect", on) }
        onVisibilityToggled: function (on) { backend.setTrackFlag(track, "disabled", !on) }
        onMuteToggled: function (on) { backend.setTrackFlag(track, "muted", on) }
        onSoloToggled: function (on) { backend.setTrackFlag(track, "solo", on) }
        onRenameRequested: function (name) { backend.renameTrack(track, name) }
        onContextRequested: {
            trackContextMenu.track = track
            trackContextMenu.headers = [header]
            trackContextMenu.popup()
        }
    }

    AppMenu {
        id: trackContextMenu
        objectName: "trackContextMenu"
        property int track: -1
        property var headers: []
        // The menu of one track, opened without a click on its header.
        function openFor(t) {
            track = t
            headers = []
            popup()
        }
        function headerFor(t) {
            for (var i = 0; i < headers.length; i++)
                if (headers[i] !== null && headers[i].track === t)
                    return headers[i]
            return null
        }
        KeyMenuItem {
            text: qsTr("Rename Track…")
            onTriggered: {
                var h = trackContextMenu.headerFor(trackContextMenu.track)
                if (h !== null)
                    h.startRename()
            }
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Add Video Track")
            onTriggered: backend.addTrack(false)
        }
        KeyMenuItem {
            text: qsTr("Add Audio Track")
            onTriggered: backend.addTrack(true)
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Move Track Up")
            enabled: backend.tracks.length > 0 && backend.trackNeighbour(trackContextMenu.track, true) >= 0
            onTriggered: {
                win.selection.clearSelection() // track numbers reshuffle: no address may name another clip
                backend.moveTrack(trackContextMenu.track, true)
            }
        }
        KeyMenuItem {
            text: qsTr("Move Track Down")
            enabled: backend.tracks.length > 0 && backend.trackNeighbour(trackContextMenu.track, false) >= 0
            onTriggered: {
                win.selection.clearSelection()
                backend.moveTrack(trackContextMenu.track, false)
            }
        }
        MenuSeparator {}
        KeyMenuItem {
            text:qsTr("Burn captions into video")
            visible:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].mix.editKind === "caption"
            checkable:true
            checked:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].mix.captionBurnIn !== false
            onTriggered:backend.setTrackMix(trackContextMenu.track,"captionBurnIn",checked)
        }
        KeyMenuItem {
            text:qsTr("Caption Style…")
            visible:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].mix.editKind === "caption"
            onTriggered:captionStyleDialog.openDialog(trackContextMenu.track)
        }
        KeyMenuItem {
            objectName:"captionTrackLanguageItem"
            // Read afresh as the menu opens: the language set last time.
            text:qsTr("Language: %1…").arg(trackContextMenu.track>=0 && backend.tracks.length>trackContextMenu.track
                                            ? backend.captionTrackLanguage(trackContextMenu.track).shown || qsTr("Not set") : "")
            visible:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].mix.editKind === "caption"
            onTriggered:captionLanguageDialog.openFor(trackContextMenu.track, false)
        }
        KeyMenuItem {
            objectName:"captionTrackDuplicateItem"
            text:qsTr("Duplicate Caption Track as a New Language…")
            visible:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].mix.editKind === "caption"
            enabled:!backend.exporting
            onTriggered:captionLanguageDialog.openFor(trackContextMenu.track, true)
        }
        KeyMenuItem {
            text:backend.recordingTake?qsTr("Stop recording"):qsTr("Record on this track")
            visible:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && backend.tracks[trackContextMenu.track].kind === "audio"
            enabled:trackContextMenu.track>=0 && !!backend.tracks[trackContextMenu.track] && !backend.tracks[trackContextMenu.track].locked
            onTriggered:backend.recordingTake?backend.stopRecording():backend.startRecording(trackContextMenu.track)
        }
        MenuSeparator {}
        AppMenu {
            title: qsTr("Track Height")
            KeyMenuItem { text: qsTr("Small");  onTriggered: win.lanes.setOneTrackHeight(trackContextMenu.track,32) }
            KeyMenuItem { text: qsTr("Normal"); onTriggered: win.lanes.setOneTrackHeight(trackContextMenu.track,Ui.Theme.hLaneRow) }
            KeyMenuItem { text: qsTr("Large");  onTriggered: win.lanes.setOneTrackHeight(trackContextMenu.track,64) }
            KeyMenuItem { text: qsTr("Huge");   onTriggered: win.lanes.setOneTrackHeight(trackContextMenu.track,96) }
        }
        MenuSeparator {}
        KeyMenuItem {
            text: qsTr("Delete Track")
            enabled: trackContextMenu.track > 0
                     && trackContextMenu.track < backend.tracks.length
                     && backend.tracks[trackContextMenu.track].clips === 0
            onTriggered: backend.removeTrack(trackContextMenu.track)
        }
    }

    // Rows follow the scroller's content: the same y as the lane blocks,
    // shifted by whatever the timeline has scrolled vertically.
    Item {
        width: parent.width
        y: -timelineScroll.contentY

        // Video tracks above V1, highest on top — same rows as the lane
        // blocks (laneRow r sits at (videoLaneRows - r) * hLaneRow).
        Repeater {
            model: win.videoLaneRows
            delegate: Header {
                required property int index
                track: win.laneKinds.video[index]
                y: connectedLane.y + win.lanes.laneY(track,false) + 2
                height: win.lanes.trackHeightFor(track) - 4
            }
        }
        Header {
            track: 0
            y: storyline.y
            height: storyline.height
        }
        // Embedded audio still inside V1 clips (legacy / nests): mute and
        // solo are V1's own audio flags (the engine mutes a video track's
        // sound, not its picture).
        Edit.TrackHeader {
            objectName: "attachedAudioHeader"
            width: trackHeaders.width
            visible: backend.hasVideo && win.embeddedAudioCount > 0
            y: attachedLane.y + 2
            height: win.hLaneClip
            name: "AV"
            tip: qsTr("Sound carried inside the V1 clips. Mute and solo here follow V1.")
            kind: "audio"
            recordable: false
            locked: backend.tracks.length > 0 && backend.tracks[0].locked === true
            autoSelect: backend.tracks.length > 0 && backend.tracks[0].autoSelect === true
            muted: backend.tracks.length > 0 && backend.tracks[0].muted === true
            soloed: backend.tracks.length > 0 && backend.tracks[0].solo === true
            onLockToggled: function (on) { backend.setTrackFlag(0, "locked", on) }
            onAutoSelectToggled: function (on) { backend.setTrackFlag(0, "autoSelect", on) }
            onMuteToggled: function (on) { backend.setTrackFlag(0, "muted", on) }
            onSoloToggled: function (on) { backend.setTrackFlag(0, "solo", on) }
            onContextRequested: {
                trackContextMenu.track = 0
                trackContextMenu.headers = []
                trackContextMenu.popup()
            }
        }
        Repeater {
            model: win.audioLaneRows
            delegate: Header {
                required property int index
                track: win.laneKinds.audio[index]
                y: audioLane.y + win.lanes.laneY(track,true) + 2
                height: win.lanes.trackHeightFor(track) - 4
            }
        }
    }
    // Beside the ruler, which stays at the top: headers scrolled up pass
    // beneath it, as their tracks do beneath the ruler.
    Rectangle {
        objectName: "trackHeadersRulerCover"
        width: parent.width
        height: ruler.height
        visible: timelineScroll.contentY > 0
        color: win.wellColor
    }
}
