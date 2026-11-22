import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import "../Ui/MediaTime.js" as MediaTime

// The stacked ("pancake") timeline: another sequence drawn compactly above
// the main timeline, one row per track, to pull clips from. Drag a block
// down onto a track (it overwrites at the frame, attributes and all) or
// double-click it to place it at the playhead on the destination track.
// Sequence tab ▸ right-click ▸ Stack Above Timeline puts it here; × or
// switching to that sequence takes it away.
Rectangle {
    id: strip
    objectName: "pancakeStrip"
    Layout.fillWidth: true
    readonly property bool stacked: backend.stackedSequence >= 0
    readonly property var entries: backend.stackedEntries
    readonly property int rows: {
        var top = 0
        for (var i = 0; i < entries.length; i++)
            top = Math.max(top, entries[i].track)
        return Math.min(6, top + 1)
    }
    readonly property int duration: {
        var end = 1
        for (var i = 0; i < entries.length; i++)
            end = Math.max(end, entries[i].start + entries[i].frames)
        return end
    }
    readonly property int rowHeight: 16
    implicitHeight: stacked ? 22 + rows * rowHeight + Ui.Theme.s1 : 0
    visible: stacked
    clip: true
    color: Ui.Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Ui.Theme.hairline
        color: Ui.Theme.border
    }

    Item { // header: what is stacked, and the way out
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 22
        Ui.Icon {
            id: stackIcon
            anchors.left: parent.left
            anchors.leftMargin: Ui.Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            name: "layout-grid"
            size: 12
            color: Ui.Theme.accent
        }
        Text {
            anchors.left: stackIcon.right
            anchors.leftMargin: Ui.Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            text: strip.stacked && backend.stackedSequence < backend.sequences.length
                  ? qsTr("Stacked: %1 — drag clips down, or double-click to place at the playhead")
                        .arg(backend.sequences[backend.stackedSequence].name) : ""
            font.family: Ui.Theme.fontFamily
            font.pixelSize: Ui.Theme.fsLabel
            color: Ui.Theme.textSecondary
            elide: Text.ElideRight
            width: parent.width - stackIcon.width - closeButton.width - Ui.Theme.s2 * 4
        }
        Ui.IconButton {
            id: closeButton
            objectName: "unstackButton"
            anchors.right: parent.right
            anchors.rightMargin: Ui.Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            iconName: "x"
            text: qsTr("Unstack")
            implicitWidth: 18
            implicitHeight: 18
            onClicked: backend.stackedSequence = -1
        }
    }

    Item { // the rows
        id: rowsArea
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Ui.Theme.s2
        anchors.rightMargin: Ui.Theme.s2
        height: strip.rows * strip.rowHeight

        Repeater {
            model: strip.visible ? strip.rows : []
            delegate: Rectangle {
                required property int index
                y: (strip.rows - 1 - index) * strip.rowHeight
                width: rowsArea.width
                height: strip.rowHeight - 1
                color: Ui.Theme.clipGap
                opacity: 0.6
            }
        }
        Repeater {
            model: strip.visible ? strip.entries : []
            delegate: Rectangle {
                id: block
                required property int index
                required property var modelData
                objectName: "pancake:" + index
                readonly property bool isAudio: modelData.audioOnly === true
                readonly property bool isTitle: modelData.title !== undefined && modelData.title !== ""
                x: Math.floor(modelData.start / strip.duration * rowsArea.width)
                y: (strip.rows - 1 - Math.min(strip.rows - 1, modelData.track)) * strip.rowHeight
                width: Math.max(3, Math.floor(modelData.frames / strip.duration * rowsArea.width) - 1)
                height: strip.rowHeight - 1
                radius: Ui.Theme.rControl
                color: isAudio ? Ui.Theme.clipAudio : isTitle ? Ui.Theme.clipTitle : Ui.Theme.clipVideo
                border.width: blockHover.hovered ? 1 : 0
                border.color: Ui.Theme.accent
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 3
                    anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    text: block.modelData.name !== undefined ? block.modelData.name : ""
                    elide: Text.ElideRight
                    font.family: Ui.Theme.fontFamily
                    font.pixelSize: Ui.Theme.fsLabel - 1
                    color: Ui.Theme.clipLabelOnMedia
                    visible: block.width > 24
                }
                HoverHandler { id: blockHover }
                Ui.Tooltip {
                    visible: blockHover.hovered
                    text: qsTr("%1 · %2 — drag onto a track, or double-click to place at the playhead")
                              .arg(block.modelData.name).arg(MediaTime.durationText(block.modelData.frames, backend.fps))
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        backend.viewTimeline()
                        backend.placeEntryAt(block.modelData, backend.destTrack, backend.position)
                    }
                }
                DragHandler {
                    id: blockDrag
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: {
                        if (active) {
                            ghost.entry = block.modelData
                            ghost.frames = block.modelData.frames
                            ghost.name = block.modelData.name !== undefined ? block.modelData.name : ""
                            var host = block.Window.window ? block.Window.window.contentItem : strip
                            ghost.parent = host
                            ghost.x = Qt.binding(function () {
                                return host.mapFromItem(null, blockDrag.centroid.scenePosition).x - ghost.width / 2
                            })
                            ghost.y = Qt.binding(function () {
                                return host.mapFromItem(null, blockDrag.centroid.scenePosition).y - ghost.height / 2
                            })
                            ghost.Drag.active = true
                        } else {
                            ghost.Drag.drop()
                            ghost.Drag.active = false
                            ghost.x = 0
                            ghost.y = 0
                            ghost.parent = strip
                        }
                    }
                }
            }
        }
    }

    // The drag payload: a stacked clip, under its own key so the lane drop
    // places the ENTRY rather than a library clip.
    Item {
        id: ghost
        objectName: "pancakeGhost"
        property var entry: null
        property int frames: 0
        property string name: ""
        readonly property bool pancake: true
        width: 120
        height: strip.rowHeight + 4
        visible: Drag.active
        z: 1000
        Drag.keys: ["omaedit/pancake"]
        Drag.hotSpot.x: 0
        Drag.hotSpot.y: height / 2
        Rectangle {
            anchors.fill: parent
            radius: Ui.Theme.rControl
            color: Ui.Theme.clipVideo
            border.width: 1
            border.color: Ui.Theme.accent
            opacity: 0.9
            Text {
                anchors.centerIn: parent
                text: ghost.name
                elide: Text.ElideRight
                width: parent.width - 8
                horizontalAlignment: Text.AlignHCenter
                font.family: Ui.Theme.fontFamily
                font.pixelSize: Ui.Theme.fsLabel
                color: Ui.Theme.clipLabelOnMedia
            }
        }
    }
}
