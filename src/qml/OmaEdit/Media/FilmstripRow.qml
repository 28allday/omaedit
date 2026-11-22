pragma ComponentBehavior: Bound
import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import OmaEdit.Ui
Rectangle {
    id: root
    property var entry: ({})
    property bool selected: false
    property bool skimEnabled: false
    property string skimThumb: ""
    readonly property int frames: Math.max(1, MediaTime.sourceFrames(entry))
    readonly property var strip: entry.strip || (entry.thumb ? [entry.thumb] : [])
    property int dragStart: -1
    property int dragEnd: -1
    // A press that never moves selects the clip; only a drag marks a range.
    property real pressX: 0
    property bool dragged: false
    signal activated(int modifiers)
    signal opened()
    signal contextRequested()
    signal rangeSelected(int start, int end)
    signal skimmed(int frame)
    color: selected ? Theme.panelRaised : Theme.panelBg
    radius: Theme.rControl; border.width: selected ? Theme.selectionRing : Theme.hairline; border.color: selected ? Theme.accent : Theme.border
    function frameAt(x) { return Math.max(0, Math.min(frames-1, Math.round(x / pictures.width * (frames-1)))) }
    Text { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: Theme.s2; elide: Text.ElideMiddle; text: root.entry.name || ""; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
    Item {
        id: pictures
        objectName: "sourceRangePictures"
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.margins: Theme.s2; anchors.topMargin: Theme.hRow + Theme.s1
        clip: true
        // Pictures at the clip's own shape, side by side across the row, each
        // the strip picture nearest its place in the clip: a short clip with
        // few strip pictures repeats them rather than stretching one.
        Row {
            id: cells
            objectName: "filmstripCells"
            anchors.fill: parent
            readonly property real aspect: root.entry.width > 0 && root.entry.height > 0 ? root.entry.width / root.entry.height : 16 / 9
            readonly property real cellWidth: Math.max(8, pictures.height * aspect)
            readonly property int count: root.strip.length > 0 ? Math.max(1, Math.ceil(pictures.width / cellWidth)) : 0
            Repeater {
                model: cells.count
                delegate: Image {
                    required property int index
                    source: root.strip[Math.min(root.strip.length - 1, Math.floor((index + 0.5) / cells.count * root.strip.length))]
                    width: cells.cellWidth; height: pictures.height
                    fillMode: Image.PreserveAspectCrop; asynchronous: true
                }
            }
        }
        Rectangle {
            readonly property int first: root.dragStart >= 0 ? Math.min(root.dragStart, root.dragEnd) : Math.max(0, root.entry.rangeIn || 0)
            readonly property int last: root.dragStart >= 0 ? Math.max(root.dragStart, root.dragEnd) : (root.entry.rangeOut >= 0 ? root.entry.rangeOut : root.frames-1)
            x: first / root.frames * parent.width
            width: Math.max(Theme.hairline, (last-first+1)/root.frames * parent.width); height: parent.height
            color: Qt.rgba(0.06, 0.75, 0.95, 0.16); border.width: Theme.selectionRing; border.color: Theme.accent
            visible: root.dragStart >= 0 || root.entry.rangeIn >= 0 || root.entry.rangeOut >= 0
        }
        Image { visible: hover.containsMouse && root.skimThumb !== "" && root.dragStart < 0; source: root.skimThumb; height: parent.height; width: cells.cellWidth; x: Math.min(parent.width-width, Math.max(0, hover.mouseX-width/2)); fillMode: Image.PreserveAspectFit }
        MouseArea {
            id: hover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: function (mouse) {
                if (mouse.button === Qt.RightButton) { root.contextRequested(); return }
                root.activated(mouse.modifiers); root.dragStart = root.frameAt(mouse.x); root.dragEnd = root.dragStart
                root.pressX = mouse.x; root.dragged = false
            }
            onPositionChanged: function (mouse) {
                if (pressed && root.dragStart >= 0) {
                    root.dragged = root.dragged || Math.abs(mouse.x - root.pressX) >= Qt.styleHints.startDragDistance
                    if (root.dragged) root.dragEnd = root.frameAt(mouse.x)
                }
                else if (root.skimEnabled) root.skimmed(root.frameAt(mouse.x))
            }
            onReleased: if (root.dragStart >= 0) {
                if (root.dragged) root.rangeSelected(Math.min(root.dragStart, root.dragEnd), Math.max(root.dragStart, root.dragEnd))
                root.dragStart = -1
            }
            onCanceled: root.dragStart = -1
            onDoubleClicked: root.opened()
        }
    }
}
