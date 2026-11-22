import QtQuick
import OmaEdit.Ui

// The A/B range bar under the delivery viewer: the whole timeline as a
// track, the render range as a cyan span with two handles, A and B tags
// above them, the playhead as a thin red line. Dragging a handle edits the
// timeline's In/Out; a click on the track seeks.
Item {
    id: root

    property int duration: 0
    property int rangeIn: -1
    property int rangeOut: -1
    property int position: 0
    property var timecoder: null

    signal rangeEdited(int rangeIn, int rangeOut)
    signal seekRequested(int frame)

    implicitHeight: 44

    readonly property int effIn: rangeIn >= 0 ? rangeIn : 0
    readonly property int effOut: rangeOut >= 0 ? rangeOut : Math.max(0, duration - 1)
    readonly property bool hasRange: rangeIn >= 0 || rangeOut >= 0
    function xOf(frame) { return duration > 1 ? track.x + frame / (duration - 1) * track.width : track.x }
    function frameAt(x) { return duration > 1 ? Math.round(Math.max(0, Math.min(1, (x - track.x) / track.width)) * (duration - 1)) : 0 }
    function tc(frame) { return timecoder ? timecoder(frame) : String(frame) }

    Rectangle {
        id: track
        objectName: "rangeTrack"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s4
        anchors.rightMargin: Theme.s4
        y: 18
        height: 4
        radius: 2
        color: Theme.controlBg

        TapHandler {
            onTapped: function (point) { root.seekRequested(root.frameAt(point.position.x + track.x)) }
        }
    }

    Rectangle {
        id: span
        objectName: "rangeSpan"
        x: root.xOf(root.effIn)
        width: Math.max(2, root.xOf(root.effOut) - x)
        y: track.y - 2
        height: 8
        radius: 2
        color: root.hasRange ? Theme.accent : Theme.borderStrong
        opacity: root.duration > 0 ? 1 : Theme.disabledOpacity
    }

    // Playhead.
    Rectangle {
        x: root.xOf(Math.max(0, Math.min(root.duration - 1, root.position))) - 1
        y: track.y - 6
        width: 2
        height: 16
        color: Theme.playhead
        visible: root.duration > 0
    }

    // A / B handles: a square knob with the letter above.
    Repeater {
        model: 2
        delegate: Item {
            id: handle
            required property int index
            readonly property bool isIn: index === 0
            readonly property int frame: isIn ? root.effIn : root.effOut
            objectName: isIn ? "rangeHandleIn" : "rangeHandleOut"
            x: root.xOf(frame) - width / 2
            y: track.y - 8
            width: 14
            height: 20
            visible: root.duration > 0
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: isIn ? qsTr("Render range start") : qsTr("Render range end")
            Accessible.description: root.tc(frame)
            function adjust(next) {
                var f = Math.max(0, Math.min(root.duration - 1, next))
                root.rangeEdited(isIn ? Math.min(f, root.effOut) : root.effIn,
                                 isIn ? root.effOut : Math.max(f, root.effIn))
            }
            Keys.onPressed: function(event) {
                var step = event.modifiers & Qt.ShiftModifier ? 10 : 1
                if (event.key === Qt.Key_Left) adjust(frame - step)
                else if (event.key === Qt.Key_Right) adjust(frame + step)
                else if (event.key === Qt.Key_Home) adjust(0)
                else if (event.key === Qt.Key_End) adjust(root.duration - 1)
                else return
                event.accepted = true
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: -14
                width: 16
                height: 14
                radius: Theme.rControl
                color: Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: handle.isIn ? "A" : "B"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel - 1
                    font.weight: Theme.wHeading
                    color: Theme.accentText
                }
            }
            Rectangle {
                anchors.centerIn: parent
                width: 8
                height: 14
                radius: 2
                color: drag.active || handle.activeFocus ? Theme.textPrimary : Theme.accent
                border.width: Theme.hairline
                border.color: Theme.windowBg
            }
            DragHandler {
                id: drag
                target: null
                xAxis.enabled: true
                yAxis.enabled: false
                onActiveChanged: if (!active) root.rangeEdited(root.effIn, root.effOut)
                onCentroidChanged: {
                    if (!active) return
                    var f = root.frameAt(centroid.scenePosition.x - root.mapToItem(null, 0, 0).x)
                    if (handle.isIn) root.rangeEdited(Math.min(f, root.effOut), root.effOut)
                    else root.rangeEdited(root.effIn, Math.max(f, root.effIn))
                }
            }
            HoverHandler { cursorShape: Qt.SizeHorCursor }
        }
    }

    Text {
        anchors.left: track.left
        anchors.top: track.bottom
        anchors.topMargin: Theme.s1
        text: root.tc(root.effIn)
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel - 1
        color: Theme.textMuted
    }
    Text {
        anchors.right: track.right
        anchors.top: track.bottom
        anchors.topMargin: Theme.s1
        text: root.tc(root.effOut)
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsLabel - 1
        color: Theme.textMuted
    }
}
