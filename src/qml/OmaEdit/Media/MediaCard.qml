import QtQuick
import omaedit 1.0
import "../Ui/MediaTime.js" as MediaTime
import OmaEdit.Ui

// One grid card: poster, status badges, name, then duration · resolution ·
// frame rate and the rating, with the first tag as a chip. Selection is the
// 2 px cyan ring plus a tick; offline media is dimmed with a red badge.
Item {
    id: root

    property var entry: ({})
    property bool selected: false
    property bool current: false
    property bool used: false
    property bool skimEnabled: false
    property string skimThumb: ""
    property int stackCount: 0
    signal skimmed(int frame)
    signal stackOpened()
    property string durationText: ""
    property string resolutionText: ""
    property string fpsText: ""

    signal activated(int modifiers)
    signal opened()
    signal contextRequested()

    readonly property bool offline: entry.offline !== undefined && entry.offline
    readonly property bool hasThumb: entry.thumb !== undefined && entry.thumb !== ""

    Accessible.role: Accessible.ListItem
    Accessible.name: entry.name !== undefined ? entry.name : ""
    Accessible.selected: selected

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.panelRaised
             : hover.hovered ? Theme.hovered(Theme.panelBg) : Theme.panelBg
        radius: Theme.rControl
        border.width: root.selected ? Theme.selectionRing : Theme.hairline
        border.color: root.selected ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: Theme.dFast; easing.type: Theme.easing }
        }
    }

    HoverHandler {
        id: hover
        onPointChanged: if (root.skimEnabled && hovered && point.position.y < thumb.y + thumb.height)
            root.skimmed(Math.round(Math.max(0, Math.min(1, (point.position.x-thumb.x)/Math.max(1, thumb.width))) * Math.max(0, MediaTime.sourceFrames(root.entry)-1)))
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated(point.modifiers)
        onDoubleTapped: root.opened()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.contextRequested()
    }

    // Poster, or a tonal plate until the thumbnail lands.
    Rectangle {
        id: thumb
        objectName: "sourcePoster"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.s1 + 1
        height: Math.round(width * 9 / 16)
        radius: Theme.rControl
        clip: true
        opacity: root.offline ? 0.5 : 1
        readonly property real tone: root.entry.tone !== undefined ? root.entry.tone : 0.4
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.hsla(0.55, 0.18, 0.16 + thumb.tone * 0.16, 1) }
            GradientStop { position: 1.0; color: Qt.hsla(0.55, 0.22, 0.09 + thumb.tone * 0.10, 1) }
        }

        Image {
            anchors.fill: parent
            source: root.skimEnabled && hover.hovered && root.skimThumb ? root.skimThumb : root.hasThumb ? root.entry.thumb : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }
        // A sound file's card shows its sound: the file's waveform, whole.
        WaveformItem {
            id: cardWave
            objectName: "cardWave"
            anchors.fill: parent
            anchors.margins: Theme.s2
            readonly property bool wanted: !root.hasThumb && !root.offline && !MediaTime.hasPicture(root.entry) && root.entry.wave !== undefined
            visible: wanted && ready
            path: wanted && root.entry.path !== undefined ? root.entry.path : ""
            fps: 25
            firstFrame: 0
            lastFrame: Math.max(1, (root.entry.duration || 0) * 25 / (root.entry.fps > 0 ? root.entry.fps : 25))
            stereoHeight: 1e9
            peakColor: Theme.audio
            bodyColor: Qt.lighter(Theme.audio, 1.8)
            clipColor: Theme.danger
            opacity: 0.85
        }
        Icon {
            anchors.centerIn: parent
            visible: !root.hasThumb && !cardWave.visible
            name: root.offline ? "circle-alert"
                : (root.entry.hasVideo !== undefined && !root.entry.hasVideo) ? "audio-waveform" : "film"
            size: 20
            color: Qt.rgba(1, 1, 1, 0.18)
        }

        // Marked range across the source duration.
        Rectangle {
            readonly property int rIn: root.entry.rangeIn !== undefined ? root.entry.rangeIn : -1
            readonly property int rOut: root.entry.rangeOut !== undefined ? root.entry.rangeOut : -1
            readonly property int frames: Math.max(1, MediaTime.sourceFrames(root.entry))
            visible: rIn >= 0 || rOut >= 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 3
            color: Qt.rgba(0, 0, 0, 0.5)
            Rectangle {
                x: (parent.rIn >= 0 ? parent.rIn : 0) / parent.frames * parent.width
                width: Math.max(2, (((parent.rOut >= 0 ? parent.rOut : parent.frames - 1) + 1)
                                    - (parent.rIn >= 0 ? parent.rIn : 0)) / parent.frames * parent.width)
                height: parent.height
                color: Theme.accent
            }
        }
    }

    ToolButton {
        visible: root.stackCount > 1
        anchors.top: thumb.top; anchors.right: thumb.right; anchors.margins: Theme.s1
        text: qsTr("Stack · %1").arg(root.stackCount)
        onClicked: root.stackOpened()
    }
    // Selected tick, top-left.
    Rectangle {
        visible: root.selected
        anchors.top: thumb.top
        anchors.left: thumb.left
        anchors.margins: Theme.s1
        width: 16; height: 16
        radius: Theme.rControl
        color: Theme.accent
        Icon { anchors.centerIn: parent; name: "check"; size: 12; color: Theme.accentText }
    }

    // Status badges, bottom-right of the poster.
    Row {
        anchors.bottom: thumb.bottom
        anchors.right: thumb.right
        anchors.margins: Theme.s1
        spacing: Theme.s1

        Repeater {
            model: [
                { text: qsTr("OFFLINE"), color: Theme.danger, on: root.offline },
                { text: qsTr("PROXY"), color: Theme.accent, on: root.entry.proxy !== undefined && root.entry.proxy },
                { text: qsTr("PROXY MISSING"), color: Theme.warning, on: !!root.entry.proxyPath && !root.entry.proxy },
                { text: qsTr("SUB"), color: Theme.clipTitle, on: root.entry.subclip !== undefined && root.entry.subclip },
                { text: root.entry.angles !== undefined ? qsTr("MULTICAM ×%1").arg(root.entry.angles.length) : qsTr("MULTICAM"),
                  color: Theme.accent, on: root.entry.multicam !== undefined && root.entry.multicam },
                { text: qsTr("USED"), color: Theme.audio, on: root.used }
            ]
            delegate: Rectangle {
                required property var modelData
                visible: modelData.on
                width: badgeText.implicitWidth + Theme.s2
                height: 14
                radius: 2
                color: modelData.color
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: modelData.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 8
                    font.weight: Theme.wHeading
                    color: Theme.accentText
                }
            }
        }
    }

    Column {
        anchors.top: thumb.bottom
        anchors.topMargin: Theme.s1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        spacing: 2

        Text {
            width: parent.width
            text: root.entry.name !== undefined ? root.entry.name : ""
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: root.selected ? Theme.textPrimary : Theme.textSecondary
        }
        Row {
            spacing: Theme.s2
            Text { text: root.durationText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
            Text { text: root.resolutionText; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
            Text { text: root.fpsText; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
        }
        Row {
            spacing: Theme.s2
            StarRating {
                tip: qsTr("Rating given to this clip; set it in the inspector")
                value: root.entry.rating !== undefined ? root.entry.rating : 0
                editable: false
                starSize: Theme.fsLabel
                visible: value > 0
            }
            Repeater {
                model: root.entry.tags !== undefined ? root.entry.tags.slice(0, 2) : []
                delegate: Chip {
                    required property string modelData
                    tip: qsTr("A tag on this clip; filter by tag in the bar above")
                    label: modelData
                    tint: Theme.labelNames[Math.abs(hashOf(modelData)) % Theme.labelNames.length]
                    function hashOf(s) { var h = 0; for (var i = 0; i < s.length; ++i) h = (h * 31 + s.charCodeAt(i)) | 0; return h }
                }
            }
        }
    }
}
