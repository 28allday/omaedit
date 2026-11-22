import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// Skimmable filmstrip for the previewed source clip: press/drag seeks,
// the marked range dims everything outside itself, and its edges drag.
Item {
    id: strip
    readonly property var entry: backend.previewLibraryIndex >= 0
                                 && backend.previewLibraryIndex < backend.library.length
                                 ? backend.library[backend.previewLibraryIndex] : null
    readonly property var urls: entry && entry.sourceStrip !== undefined
                                ? entry.sourceStrip : []
    readonly property int frames: backend.sourceDuration
    readonly property int rIn: entry && entry.rangeIn !== undefined ? entry.rangeIn : -1
    readonly property int rOut: entry && entry.rangeOut !== undefined ? entry.rangeOut : -1
    readonly property bool hasRange: rIn >= 0 || rOut >= 0
    readonly property int effIn: rIn >= 0 ? rIn : 0
    readonly property int effOut: rOut >= 0 ? rOut : frames - 1
    height: 46

    function frameFor(x) {
        return Math.round(Math.max(0, Math.min(1, x / width))
                          * (Math.max(2, frames) - 1))
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: 0.4
        radius: win.rClip
    }
    Row {
        anchors.fill: parent
        anchors.margins: 2
        clip: true
        Repeater {
            model: strip.urls
            Image {
                required property var modelData
                source: modelData
                height: parent.height
                width: strip.urls.length > 0
                       ? (strip.width - 4) / strip.urls.length : 0
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
        }
    }
    Rectangle { // dim before the range
        visible: strip.hasRange
        x: 0
        width: strip.effIn / Math.max(1, strip.frames) * strip.width
        height: parent.height
        color: Qt.rgba(0, 0, 0, 0.62)
    }
    Rectangle { // dim after the range
        visible: strip.hasRange
        x: (strip.effOut + 1) / Math.max(1, strip.frames) * strip.width
        width: Math.max(0, strip.width - x)
        height: parent.height
        color: Qt.rgba(0, 0, 0, 0.62)
    }
    Rectangle { // range border
        visible: strip.hasRange
        x: strip.effIn / Math.max(1, strip.frames) * strip.width
        width: (strip.effOut + 1 - strip.effIn)
               / Math.max(1, strip.frames) * strip.width
        height: parent.height
        color: "transparent"
        border.color: win.accent
        border.width: 2
        radius: win.rClip
    }
    Rectangle { // source playhead
        width: 2
        height: parent.height
        color: win.accent
        x: strip.frames > 1
           ? backend.sourcePosition / (strip.frames - 1) * (strip.width - 2)
           : 0
    }
    MouseArea { // press/drag = seek the source
        anchors.fill: parent
        onPressed: function (mouse) {
            backend.pause()
            backend.seek(strip.frameFor(mouse.x))
        }
        onPositionChanged: function (mouse) {
            if (pressed)
                backend.seek(strip.frameFor(mouse.x))
        }
    }
    MouseArea { // drag the in edge
        visible: strip.hasRange
        x: strip.effIn / Math.max(1, strip.frames) * strip.width - 5
        width: 10
        height: parent.height
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        onPositionChanged: function (mouse) {
            if (pressed)
                backend.setRange(strip.frameFor(mapToItem(strip, mouse.x, 0).x),
                                 strip.rOut)
        }
    }
    MouseArea { // drag the out edge
        visible: strip.hasRange
        x: (strip.effOut + 1) / Math.max(1, strip.frames) * strip.width - 5
        width: 10
        height: parent.height
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        onPositionChanged: function (mouse) {
            if (pressed)
                backend.setRange(strip.rIn,
                                 strip.frameFor(mapToItem(strip, mouse.x, 0).x))
        }
    }
}
