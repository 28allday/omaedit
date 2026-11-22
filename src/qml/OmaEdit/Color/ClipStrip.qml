pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The clip strip under the viewer: every video clip in time order as a
// thumbnail with its number and name; the current one (under the
// playhead) ringed; a label flag, a marker dot, a graded dot; a mini
// ruler above; click to jump.
Rectangle {
    id: root

    readonly property bool compact: height < 90
    property var clips: []              // backend.colorClips
    property int currentTrack: -1
    property int currentIndex: -1
    property int position: 0
    property int duration: 0
    property real fps: 25
    property var timecoder: null
    property var selection: []          // [{track, index}] multi-select for apply
    signal clipActivated(int track, int index, int start)
    signal selectionEdited(var addrs)
    signal previousRequested()
    signal nextRequested()

    signal decorationWindowChanged(real first, real last, int pixels)
    function requestDecorations() {
        if (!visible || clips.length === 0) return
        const first = Math.max(0, Math.floor(list.contentX / (cardWidth + list.spacing)) - 1)
        const last = Math.min(clips.length - 1, Math.ceil((list.contentX + list.width) / (cardWidth + list.spacing)) + 1)
        let start = duration, end = 0
        for (let i = first; i <= last; ++i) { start = Math.min(start, clips[i].start); end = Math.max(end, clips[i].start + clips[i].frames) }
        decorationWindowChanged(start, end, Math.ceil(list.width * 2))
    }
    Timer { id: decorationTimer; interval: 60; onTriggered: root.requestDecorations() }
    onVisibleChanged: if (visible) decorationTimer.restart()
    onClipsChanged: if (!decorationTimer.running) decorationTimer.start()
    Component.onCompleted: decorationTimer.restart()

    color: Theme.panelBg
    implicitHeight: Theme.hTimeRuler + 72 + Theme.s2
    readonly property int cardWidth: compact ? 142 : 124

    function isSelected(t, i) {
        for (var k = 0; k < selection.length; ++k) if (selection[k].track === t && selection[k].index === i) return true
        return false
    }
    function tc(f) { if (timecoder) return timecoder(f); var s = Math.floor(f / fps); var pad = function (n) { return (n < 10 ? "0" : "") + n }; return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(Math.round(f - s * fps)) }

    // Mini ruler with timecode marks and the playhead.
    Item {
        id: ruler
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.hTimeRuler
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        Repeater {
            model: 9
            delegate: Text {
                required property int index
                x: index * ruler.width / 8 + 4
                anchors.verticalCenter: parent.verticalCenter
                text: root.tc(Math.round(index * Math.max(0, root.duration - 1) / 8))
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel - 1
                color: Theme.textMuted
                visible: index < 8
            }
        }
        Rectangle {
            x: root.duration > 0 ? root.position / root.duration * parent.width : 0
            width: 2
            height: parent.height
            color: Theme.playhead
        }
        TapHandler { onTapped: function (ev) { root.clipActivated(-1, -1, Math.round(ev.position.x / ruler.width * Math.max(0, root.duration - 1))) } }
    }

    ListView {
        id: list
        onContentXChanged: decorationTimer.restart()
        onWidthChanged: decorationTimer.restart()
        objectName: "clipStrip"
        anchors.top: ruler.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s1
        orientation: ListView.Horizontal
        spacing: Theme.s1
        clip: true
        model: root.clips.length
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.horizontal: ScrollBar {}
        delegate: Item {
            id: cell
            required property int index
            readonly property var c: root.clips[index] !== undefined ? root.clips[index] : ({ track: -1, index: -1, name: "", start: 0, frames: 0 })
            readonly property bool current: c.track === root.currentTrack && c.index === root.currentIndex
            readonly property bool picked: root.isSelected(c.track, c.index)
            objectName: "colorClip:" + index
            width: root.cardWidth
            height: list.height
            Rectangle {
                id: frame
                anchors.top: parent.top
                anchors.left: parent.left
                width: root.compact ? 58 : parent.width
                height: root.compact ? parent.height : parent.height - name.height - 4
                color: Theme.meterWell
                border.color: cell.current ? Theme.accent : cell.picked ? Theme.warning : Theme.border
                border.width: cell.current || cell.picked ? Theme.selectionRing : Theme.hairline
                Image {
                    anchors.fill: parent
                    anchors.margins: Theme.selectionRing
                    source: cell.c.thumb !== undefined ? cell.c.thumb : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    visible: status === Image.Ready
                }
                Icon { anchors.centerIn: parent; name: "film"; color: Theme.textMuted; visible: !cell.c.thumb }
                Rectangle { // label flag
                    anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 4
                    width: 8; height: 8; radius: 2
                    visible: cell.c.label !== undefined && cell.c.label !== ""
                    color: Theme.labelColor(cell.c.label !== undefined ? cell.c.label : "")
                }
                Rectangle { // marker dot
                    anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 4
                    width: 8; height: 8; radius: 4
                    visible: cell.c.markers > 0
                    color: Theme.markerStandard
                }
                Rectangle { // graded dot
                    anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 4
                    width: 8; height: 8; radius: 4
                    visible: cell.c.graded === true
                    color: Theme.audio
                }
            }
            Row {
                id: name
                anchors.bottom: parent.bottom
                anchors.left: root.compact ? frame.right : parent.left
                anchors.leftMargin: root.compact ? Theme.s1 : 0
                anchors.right: parent.right
                anchors.bottomMargin: root.compact ? Math.max(0, (parent.height - height) / 2) : 0
                height: Theme.fsLabel + 4
                spacing: Theme.s1
                Text {
                    text: (cell.index + 1 < 10 ? "0" : "") + (cell.index + 1)
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel
                    font.weight: Theme.wHeading
                    color: cell.current ? Theme.accent : Theme.textSecondary
                }
                Text {
                    width: parent.width - 22
                    text: cell.c.name
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: cell.current ? Theme.textPrimary : Theme.textSecondary
                }
            }
            // Taken outright: a hand that moves a little while it clicks still
            // picks the clip (the strip scrolls by wheel and by its bar).
            TapHandler {
                acceptedModifiers: Qt.NoModifier
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: root.clipActivated(cell.c.track, cell.c.index, cell.c.start)
            }
            TapHandler {
                acceptedModifiers: Qt.ControlModifier
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: {
                    var sel = root.selection.slice()
                    var found = -1
                    for (var k = 0; k < sel.length; ++k) if (sel[k].track === cell.c.track && sel[k].index === cell.c.index) found = k
                    if (found >= 0) sel.splice(found, 1); else sel.push({ track: cell.c.track, index: cell.c.index })
                    root.selectionEdited(sel)
                }
            }
        }
        onCountChanged: positionViewAtIndex(Math.max(0, currentIndexInList()), ListView.Contain)
        function currentIndexInList() {
            for (var i = 0; i < root.clips.length; ++i) if (root.clips[i].track === root.currentTrack && root.clips[i].index === root.currentIndex) return i
            return -1
        }
    }
    Connections {
        target: root
        function onCurrentIndexChanged() { var i = list.currentIndexInList(); if (i >= 0) list.positionViewAtIndex(i, ListView.Contain) }
    }
    EmptyState {
        anchors.centerIn: parent
        visible: root.clips.length === 0
        iconName: "film"
        title: qsTr("No clips to grade")
        description: qsTr("Cut something on the Edit page first.")
    }
}
