import QtQuick
import omaedit 1.0
import OmaEdit.Ui

// The previewed clip's waveform across its whole length, left over right, a
// timecode ruler, a dB scale, the marked range and the playhead. Click or
// drag to seek.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property string path: ""          // the clip's file: its sound is what is drawn
    property int frames: 0
    property real fps: 25
    property int position: 0
    property int rangeIn: -1
    property int rangeOut: -1
    property bool hasAudio: true
    property bool hasMedia: false
    property string pendingText: qsTr("Reading…") // what an empty wave means for this clip
    property var markers: []          // [{frame, type, text}]

    signal seekRequested(int frame)

    color: Theme.panelBg

    function timecode(f) {
        var rate = fps > 0 ? fps : 25
        var s = Math.floor(f / rate)
        var ff = Math.round(f - s * rate)
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff)
    }

    SectionTitle {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        height: root.dockBar !== null ? Theme.s1 : implicitHeight
        visible: root.dockBar === null
        text: qsTr("Audio Waveform")
    }

    Item {
        id: plot
        anchors.top: title.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.s3 + 12
        anchors.rightMargin: Theme.s3 + 26
        anchors.bottomMargin: Theme.s2
        readonly property int rulerH: 16
        readonly property int laneGap: 6
        readonly property real laneH: (height - rulerH - laneGap) / 2

        // Ruler: four evenly spaced timecodes plus ticks.
        Item {
            id: ruler
            width: parent.width
            height: plot.rulerH
            // As many timecodes as fit at ~90 px apiece, never fewer than two.
            readonly property int labels: Math.max(2, Math.min(5, Math.floor(plot.width / 90)))
            Repeater {
                model: root.frames > 0 ? ruler.labels : 0
                delegate: Item {
                    required property int index
                    x: Math.round(plot.width * index / (ruler.labels - 1))
                    Rectangle { width: 1; height: 4; y: plot.rulerH - 4; color: Theme.borderStrong }
                    Text {
                        y: 0
                        x: index === ruler.labels - 1 ? -implicitWidth : index === 0 ? 0 : -implicitWidth / 2
                        text: root.timecode(Math.round(root.frames * index / (ruler.labels - 1)))
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel - 2
                        color: Theme.textMuted
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        }

        // Marked range wash, over the lanes.
        Rectangle {
            z: 1
            visible: root.frames > 0 && (root.rangeIn >= 0 || root.rangeOut >= 0)
            y: plot.rulerH
            height: plot.height - plot.rulerH
            x: (root.rangeIn >= 0 ? root.rangeIn : 0) / Math.max(1, root.frames) * plot.width
            width: Math.max(1, (((root.rangeOut >= 0 ? root.rangeOut : root.frames - 1) + 1)
                                - (root.rangeIn >= 0 ? root.rangeIn : 0)) / Math.max(1, root.frames) * plot.width)
            color: Theme.rangeFill
            border.width: Theme.hairline
            border.color: Theme.rangeStroke
        }

        Repeater {
            model: 2
            delegate: Item {
                id: lane
                required property int index
                y: plot.rulerH + index * (plot.laneH + plot.laneGap)
                width: plot.width
                height: plot.laneH

                Rectangle { anchors.fill: parent; color: Theme.clipGap; radius: 2 }
                Text {
                    x: -12
                    anchors.verticalCenter: parent.verticalCenter
                    text: lane.index === 0 ? "L" : "R"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel - 1
                    color: Theme.textMuted
                }


                Text {
                    anchors.centerIn: parent
                    visible: root.hasMedia && !root.hasAudio
                    text: qsTr("No audio")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.hasMedia && root.hasAudio && !sound.ready
                    text: root.pendingText
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
            }
        }

        // The sound itself, over the two lanes (one channel a lane; a mono
        // sound's one channel across both).
        WaveformItem {
            id: sound
            objectName: "sourceWave"
            y: plot.rulerH
            width: plot.width
            height: plot.height - plot.rulerH
            visible: ready && root.hasAudio
            path: root.hasMedia ? root.path : ""
            fps: root.fps > 0 ? root.fps : 25
            firstFrame: 0
            lastFrame: Math.max(1, root.frames)
            // Always the two lanes, a mono sound's one channel in both.
            stereoHeight: 0
            laneGap: plot.laneGap
            peakColor: Theme.audio
            bodyColor: Qt.lighter(Theme.audio, 1.8)
            clipColor: Theme.danger
        }

        // dB scale down the right edge, a lane at a time: each label at the
        // height its level has on the wave beside it. The wave is drawn to a
        // linear scale (full scale at the lane's two edges, silence on its
        // centre line), so -6 dB is half way out from the centre and -12 a
        // quarter; a label that would run into the one before it, or into
        // its twin across the centre line, is left out.
        Repeater {
            model: 2
            delegate: Item {
                id: dbScale
                required property int index
                x: plot.width + Theme.s1
                y: plot.rulerH + index * (plot.laneH + plot.laneGap)
                height: plot.laneH
                readonly property real row: Theme.fsLabel + 1 // the room a label takes
                readonly property var marks: {
                    const half = height / 2, upper = []
                    let free = 0
                    for (const db of [0, -6, -12, -18, -24]) {
                        const top = Math.max(0, half * (1 - Math.pow(10, db / 20)) - row / 2)
                        if (top < free || top + row > half)
                            continue
                        upper.push({ "db": db, "top": top })
                        free = top + row
                    }
                    const out = upper.slice()
                    for (let i = upper.length - 1; i >= 0; --i)
                        out.push({ "db": upper[i].db, "top": height - upper[i].top - row })
                    return out
                }
                Repeater {
                    model: dbScale.marks
                    delegate: Text {
                        required property var modelData
                        objectName: "waveDbLabel"
                        y: modelData.top
                        height: dbScale.row
                        text: modelData.db
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsLabel - 2
                        color: Theme.textMuted
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        // Clip markers as ticks on the ruler.
        Repeater {
            model: root.frames > 0 ? root.markers : []
            delegate: Rectangle {
                required property var modelData
                x: Math.round(modelData.frame / Math.max(1, root.frames) * plot.width) - 1
                y: plot.rulerH - 6
                width: 3; height: 6
                color: Theme.markerTint(modelData)
            }
        }

        // Playhead.
        Rectangle {
            visible: root.frames > 0
            x: Math.round(Math.max(0, Math.min(1, root.position / Math.max(1, root.frames))) * plot.width) - 1
            y: plot.rulerH - 4
            width: 2
            height: plot.height - plot.rulerH + 4
            color: Theme.playhead
        }

        MouseArea {
            anchors.fill: parent
            enabled: root.frames > 0
            cursorShape: Qt.PointingHandCursor
            function seekTo(mx) {
                root.seekRequested(Math.round(Math.max(0, Math.min(1, mx / plot.width)) * (root.frames - 1)))
            }
            onPressed: function (mouse) { seekTo(mouse.x) }
            onPositionChanged: function (mouse) { if (pressed) seekTo(mouse.x) }
        }
    }
}
