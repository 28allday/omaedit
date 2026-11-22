pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// Track the selected window from the playhead in either direction.
Item {
    id: root

    property var node: ({})
    property var job: ({})               // backend.trackJob for this clip/node ({} when idle)
    property bool canTrack: false
    property int currentWindow: 0
    property bool showTable: true
    signal trackRequested(int direction, bool singleFrame)
    signal stopRequested(bool keep)
    signal clearRequested()
    signal motionCopyRequested(var points)
    signal frameRequested(int clipFrame)

    readonly property var w: currentWindow > 0 && node.windows ? node.windows[currentWindow - 1] || ({}) : node.window || ({})
    readonly property var track: w.track !== undefined ? w.track : []
    readonly property bool tracking: job.percent !== undefined

    Row {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3

        Column {
            width: 240
            spacing: Theme.s2
            Flow {
                width: parent.width
                spacing: Theme.s1
                ToolButton { objectName: "trackBackward"; showLabel: true; iconName: "rewind"; text: qsTr("Track Backward"); enabled: root.canTrack && !root.tracking; onClicked: root.trackRequested(-1, false) }
                ToolButton { objectName: "trackForward"; showLabel: true; iconName: "fast-forward"; text: qsTr("Track Forward"); enabled: root.canTrack && !root.tracking; onClicked: root.trackRequested(1, false) }
            }
            Flow {
                width: parent.width
                spacing: Theme.s1
                ToolButton { showLabel: true; iconName: "square"; text: qsTr("Stop"); enabled: root.tracking; onClicked: root.stopRequested(true) }
                ToolButton { showLabel: true; iconName: "x"; text: qsTr("Cancel"); enabled: root.tracking; onClicked: root.stopRequested(false) }
                ToolButton { objectName: "trackClear"; showLabel: true; iconName: "trash-2"; text: qsTr("Clear Track"); enabled: root.track.length > 0 && !root.tracking; onClicked: root.clearRequested() }
            }
            Flow {
                width: parent.width
                spacing: Theme.s1
                // The tracked path as position keyframes for another clip:
                // Edit > Keyframes > Paste puts them on the clip chosen.
                ToolButton { objectName: "trackCopyMotion"; showLabel: true; iconName: "copy"; text: qsTr("Copy Track as Motion"); enabled: root.track.length > 1 && !root.tracking; onClicked: root.motionCopyRequested(root.track) }
            }
            Flow {
                width: parent.width
                spacing: Theme.s1
                ToolButton { objectName: "trackFrameBackward"; showLabel: true; iconName: "skip-back"; text: qsTr("Frame Back"); enabled: root.canTrack && !root.tracking; onClicked: root.trackRequested(-1, true) }
                ToolButton { objectName: "trackFrameForward"; showLabel: true; iconName: "skip-forward"; text: qsTr("Frame Forward"); enabled: root.canTrack && !root.tracking; onClicked: root.trackRequested(1, true) }
            }
            Item {
                width: parent.width
                height: Theme.hControl
                visible: root.tracking
                CapacityBar { anchors.verticalCenter: parent.verticalCenter; width: parent.width - 44; fraction: (root.job.percent !== undefined ? root.job.percent : 0) / 100 }
                Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: (root.job.percent !== undefined ? root.job.percent : 0) + "%"; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: !root.canTrack ? qsTr("Switch the layer's shape mask on first; the tracker follows that shape.")
                    : root.tracking ? qsTr("Tracking from the playhead…")
                    : root.track.length > 1 ? qsTr("%1 positions, clip-relative. The window rides them; drag it to offset the path.").arg(root.track.length)
                    : qsTr("Tracks position from the playhead. Include a visible edge or texture. Stop keeps completed positions; Cancel discards this pass.")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: root.canTrack ? Theme.textMuted : Theme.warning
            }
        }

        Rectangle { visible: root.showTable; width: Theme.hairline; height: parent.height; color: Theme.border }

        // Tracking data: as wide as the panel allows, up to a readable table.
        Rectangle {
            visible: root.showTable
            width: Math.max(240, Math.min(720, root.width - 2 * Theme.s2 - 240 - 2 * Theme.s3 - Theme.hairline))
            height: parent.height
            color: Theme.meterWell
            border.color: Theme.border
            border.width: Theme.hairline
            ListView {
                id: list
                objectName: "trackList"
                anchors.fill: parent
                anchors.margins: Theme.s1
                clip: true
                model: root.track.length
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                header: Row {
                    spacing: Theme.s2
                    height: Theme.hRow
                    Text { width: 70; text: qsTr("Frame"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    Text { width: 70; text: "X"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    Text { width: 70; text: "Y"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                }
                delegate: Rectangle {
                    id: rowItem
                    required property int index
                    readonly property var k: root.track[index] !== undefined ? root.track[index] : ({ frame: 0, x: 0, y: 0 })
                    width: list.width
                    height: Theme.hRow
                    color: rowHover.hovered ? Theme.hovered(Theme.meterWell) : "transparent"
                    Row {
                        spacing: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        Text { width: 70; text: rowItem.k.frame; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textPrimary }
                        Text { width: 70; text: Number(rowItem.k.x).toFixed(3); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                        Text { width: 70; text: Number(rowItem.k.y).toFixed(3); font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
                    }
                    HoverHandler { id: rowHover }
                    TapHandler { onTapped: root.frameRequested(rowItem.k.frame) }
                }
            }
            Text { anchors.centerIn: parent; visible: root.track.length === 0; text: qsTr("No tracking data"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        }
    }
}
