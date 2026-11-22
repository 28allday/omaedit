pragma ComponentBehavior: Bound

import QtQuick
import OmaEdit.Ui

// The timeline: fixed track headers on the left, scrolling content on the
// right, meters pinned to the far right. Headers never scroll horizontally —
// losing track identity while trimming is how you cut the wrong track.
Rectangle {
    id: root

    color: Theme.windowBg

    EditToolbar {
        id: toolbar
        width: parent.width
        anchors.top: parent.top
    }

    AudioMeters {
        id: meters
        anchors.top: toolbar.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    // Header column, aligned to the lanes by sharing the same track heights.
    Item {
        id: headers
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: Theme.wTrackHeader

        Rectangle {
            id: headerSpacer
            width: parent.width
            height: ruler.height
            color: Theme.panelBg

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: Theme.hairline
                color: Theme.border
            }
        }

        Column {
            anchors.top: headerSpacer.bottom
            width: parent.width
            spacing: 0

            Repeater {
                model: MockProject.tracks
                delegate: TrackHeader {
                    required property int index
                    required property var modelData

                    width: headers.width
                    height: modelData.height
                    name: modelData.name
                    kind: modelData.kind
                    // V1 is the destination track, as in the concept.
                    destination: modelData.name === "V1"
                }
            }
        }
    }

    // Lane area.
    Item {
        id: lanes
        anchors.top: toolbar.bottom
        anchors.left: headers.right
        anchors.right: meters.left
        anchors.bottom: parent.bottom
        clip: true

        TimeRuler {
            id: ruler
            width: parent.width
            anchors.top: parent.top
            duration: MockProject.frames
            fps: 24
            markers: MockProject.markers.map(function (m) {
                return { frame: Math.round(m * MockProject.frames), type: "standard", text: "" }
            })
        }

        Column {
            id: laneStack
            anchors.top: ruler.bottom
            width: parent.width
            spacing: 0

            Repeater {
                model: MockProject.tracks

                delegate: Rectangle {
                    id: lane
                    required property int index
                    required property var modelData

                    width: laneStack.width
                    height: modelData.height
                    color: Theme.panelBg

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: Theme.hairline
                        color: Theme.border
                    }

                    Repeater {
                        model: MockProject.clipsByTrack[lane.index]

                        delegate: TimelineClip {
                            required property int index
                            required property var modelData

                            x: modelData.start * lane.width
                            width: modelData.span * lane.width - 2
                            y: 1
                            height: lane.height - 3
                            name: modelData.label
                            kind: modelData.kind === "film" ? "video" : modelData.kind
                            hasFx: modelData.fx
                            // One selected clip, matching the concept.
                            selected: lane.index === 2 && index === 0
                        }
                    }
                }
            }
        }

        // Playhead last, so it draws over every lane.
        Playhead {
            x: MockProject.playhead * lanes.width
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            headHeight: ruler.height
        }
    }
}
