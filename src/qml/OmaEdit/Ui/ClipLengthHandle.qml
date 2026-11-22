import QtQuick
import "MediaTime.js" as MediaTime

// A timeline duration grip. Preview stays local during a drag; the owner
// commits one edit on release. Coordinates stay in the lane as the grip moves.
MouseArea {
    id: root
    property int value: 0
    property int maximumValue: 0
    property real pixelsPerFrame: 1
    property real fps: 25
    property bool fromEnd: false
    property bool transition: false
    property string label: fromEnd ? qsTr("Fade out") : qsTr("Fade in")
    property Item coordinateItem: parent
    property real otherHandleX: NaN
    // Pixels at the block's start and at its end that are a cut's, which the
    // mouse drags: a press there is left to the block under the grip.
    property real cutAtStart: 0
    property real cutAtEnd: 0
    readonly property int displayValue: editing ? previewValue : value
    property bool editing: false
    property int previewValue: 0
    property int startValue: 0
    property real pressX: 0
    signal valueEdited(int frames)

    width: Math.min(20, parent.width)
    height: transition ? Math.max(4, parent.height - 20) : 20
    y: transition ? 20
       : fromEnd && !isNaN(otherHandleX) && Math.abs(x - otherHandleX) < width
         ? Math.max(0, Math.min(16, parent.height - height)) : 0
    x: Math.max(0, Math.min(parent.width - width,
        (fromEnd ? parent.width - displayValue * pixelsPerFrame
                 : displayValue * pixelsPerFrame) - width / 2))
    z: 11
    hoverEnabled: true
    preventStealing: true
    cursorShape: Qt.SizeHorCursor
    acceptedButtons: Qt.LeftButton

    onPressed: function(mouse) {
        if (x + mouse.x < cutAtStart || x + mouse.x >= parent.width - cutAtEnd) {
            mouse.accepted = false
            return
        }
        pressX = mapToItem(coordinateItem, mouse.x, 0).x
        startValue = value
        previewValue = value
        editing = true
    }
    onPositionChanged: function(mouse) {
        if (!pressed || !editing) return
        const delta = Math.round((mapToItem(coordinateItem, mouse.x, 0).x - pressX)
                                 / Math.max(0.000001, pixelsPerFrame))
        previewValue = Math.max(0, Math.min(maximumValue,
                                           startValue + (fromEnd ? -delta : delta)))
    }
    onReleased: {
        const frames = previewValue
        if (frames !== value) valueEdited(frames)
        editing = false
    }
    onCanceled: editing = false

    // Overview clips keep the gesture object but instantiate no hidden grips
    // or tooltips. A fit view may contain thousands of these clips.
    Loader {
        anchors.fill: parent
        active: root.visible
        sourceComponent: Item {
            Rectangle {
                visible: !root.transition && root.y > 0
                anchors.horizontalCenter: parent.horizontalCenter
                y: -root.y + 1
                width: 1
                height: root.y + 9
                color: Theme.accent
            }
            Rectangle {
                objectName: "durationGrip"
                anchors.centerIn: parent
                width: root.transition ? 6 : 10
                height: root.transition ? Math.max(2, Math.min(24, parent.height - 4)) : 10
                radius: root.transition ? 2 : width / 2
                color: root.containsMouse || root.editing || root.displayValue > 0
                       ? Theme.accent : Theme.clipLabelOnMedia
                border.width: 1
                border.color: Theme.scrimStrong
                Rectangle {
                    visible: root.transition
                    anchors.centerIn: parent
                    width: 1
                    height: Math.max(0, parent.height - 6)
                    color: Theme.scrimStrong
                }
            }
            Tooltip {
                delay: root.editing ? 0 : Theme.tooltipDelay
                visible: root.containsMouse || root.editing
                text: qsTr("%1: %2\nDrag to change length").arg(root.label)
                      .arg(MediaTime.durationText(root.displayValue, root.fps))
            }
        }
    }
}
