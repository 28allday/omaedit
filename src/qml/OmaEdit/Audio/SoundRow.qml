import QtQuick
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// One sound in the library list: a peak thumbnail, the name, duration,
// sample rate and channel count, tinted by category. Drag it to a track.
Rectangle {
    id: root

    property var sound: ({})
    readonly property bool detailed: width >= 360
    readonly property bool showDuration: width >= 240
    property bool selected: false
    property bool auditioning: false
    property real fps: 25
    signal activated()
    signal doubleActivated()
    signal contextRequested()
    signal favoriteToggled(bool on)

    readonly property string category: sound.category !== undefined ? sound.category : ""
    readonly property color tint: Theme.roleColor(category)
    readonly property bool missing: sound.missing === true

    height: Theme.hSoundRow
    color: selected ? Theme.controlBg : hover.hovered ? Theme.hovered(Theme.panelBg) : "transparent"
    opacity: missing ? Theme.disabledOpacity : 1

    Accessible.role: Accessible.ListItem
    Accessible.name: sound.name !== undefined ? sound.name : ""

    // Lengths read as time, the same way everywhere else in the app.
    function durationText(seconds) {
        if (!seconds || seconds <= 0) return "—"
        return MediaTime.durationText(seconds * 100, 100)
    }
    function rateText(rate) {
        if (!rate) return "—"
        return rate % 1000 === 0 ? (rate / 1000) + "k" : (rate / 1000).toFixed(1) + "k"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hairline
        color: Theme.border
        opacity: 0.6
    }
    Rectangle {
        anchors.left: parent.left
        width: 2
        height: parent.height
        color: root.selected ? Theme.accent : "transparent"
    }

    Row {
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        spacing: Theme.s2

        Canvas { // peak thumbnail
            id: thumb
            visible: root.detailed
            width: Theme.wSoundWave
            height: parent.height
            property var peaks: root.sound.peaks !== undefined ? root.sound.peaks : []
            onPeaksChanged: requestPaint()
            Connections { target: root; function onTintChanged() { thumb.requestPaint() } }
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var n = peaks.length
                var mid = height / 2
                ctx.fillStyle = Qt.lighter(root.tint, 1.5)
                if (n === 0) {
                    ctx.fillRect(0, mid - 0.5, width, 1)
                    return
                }
                var bw = width / n
                for (var i = 0; i < n; ++i) {
                    var h = Math.max(1, peaks[i] * (height - 6))
                    ctx.fillRect(i * bw, mid - h / 2, Math.max(1, bw - 0.5), h)
                }
            }
        }
        Text {
            id: nameText
            objectName: "soundName"
            width: Math.max(0, parent.width - (root.detailed ? thumb.width + rate.width + ch.width + parent.spacing * 3 : 0) - (root.showDuration ? dur.width + parent.spacing : 0) - star.width - parent.spacing)
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideMiddle
            text: root.sound.name !== undefined ? root.sound.name : ""
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsBase
            color: root.auditioning ? Theme.accent : Theme.textPrimary
        }
        Text {
            id: dur
            objectName: "soundDuration"
            visible: root.showDuration
            width: 58
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            text: root.durationText(root.sound.duration)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }
        Text {
            id: rate
            visible: root.detailed
            width: 30
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            text: root.rateText(root.sound.sampleRate)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }
        Text {
            id: ch
            visible: root.detailed
            width: 14
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            text: root.sound.channels ? String(root.sound.channels) : "—"
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
        }
        IconButton {
            id: star
            width: Theme.szIconHit
            height: parent.height
            iconName: "star"
            iconColor: root.sound.favorite === true ? Theme.warning : hover.hovered ? Theme.textMuted : "transparent"
            checked: root.sound.favorite === true
            Accessible.name: qsTr("Favourite")
            onClicked: root.favoriteToggled(!(root.sound.favorite === true))
        }
    }

    HoverHandler { id: hover }
    Tooltip {
        text: (root.sound.name || "") + "\n" + root.durationText(root.sound.duration) + " · " + root.rateText(root.sound.sampleRate) + qsTr(" · %1 channels").arg(root.sound.channels || 0)
        visible: hover.hovered && !root.detailed
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activated()
        onDoubleTapped: root.doubleActivated()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: { root.activated(); root.contextRequested() }
    }
}
