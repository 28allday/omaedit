import QtQuick
import OmaEdit.Ui

// One scope: the engine's rendered trace (image://thumbs/scope:<kind>?tick)
// under a QML graticule with the scale labels, and a kind selector.
Rectangle {
    id: root

    property string kind: "waveform"       // waveform | parade | vectorscope | histogram
    property int tick: 0
    property string scopeScale: "ire"      // ire | 8bit | 10bit | 12bit | float
    property bool clamp: false
    property bool showHeader: true
    property bool live: true
    signal kindEdited(string kind)
    signal settingsRequested()

    color: Theme.panelBg
    readonly property var kinds: ["waveform", "parade", "vectorscope", "histogram"]
    function kindTitle(k) { return k === "waveform" ? qsTr("Waveform") : k === "parade" ? qsTr("Parade") : k === "vectorscope" ? qsTr("Vectorscope") : qsTr("Histogram") }
    readonly property bool compact: height < 100
    readonly property real plotMargin: compact ? Theme.s1 : Theme.s2
    readonly property bool showBottomLabels: height - header.height - 2 * plotMargin - Theme.fsLabel - Theme.s1 >= 2 * Theme.fsLabel
    readonly property real low: clamp ? 0 : -0.1
    readonly property real high: clamp ? 1 : 1.1
    function label(frac) {
        if (scopeScale === "ire") return String(Math.round(frac * 100))
        if (scopeScale === "8bit") return String(Math.round(frac * 255))
        if (scopeScale === "10bit") return String(Math.round(frac * 1023))
        if (scopeScale === "12bit") return String(Math.round(frac * 4095))
        return frac.toFixed(2)
    }

    Item {
        id: header
        width: parent.width
        height: root.showHeader ? Theme.hControl + root.plotMargin : 0
        visible: root.showHeader
        ComboField {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(132, root.width - Theme.szIconHit - Theme.s2 * 2)
            tip: qsTr("Which scope this pane draws: waveform, parade, vectorscope or histogram")
            Accessible.name: qsTr("Scope")
            model: root.kinds.map(function (k) { return root.kindTitle(k) })
            currentIndex: root.kinds.indexOf(root.kind)
            onActivated: function (i) { root.kindEdited(root.kinds[i]) }
        }
        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: Theme.s1
            anchors.verticalCenter: parent.verticalCenter
            iconName: "settings-2"
            Accessible.name: qsTr("Scope settings")
            onClicked: root.settingsRequested()
        }
    }
    Item {
        id: plot
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.plotMargin
        anchors.leftMargin: root.plotMargin + (root.kind === "vectorscope" || root.kind === "histogram" ? 0 : 36)
        anchors.bottomMargin: root.plotMargin + (root.showBottomLabels && (root.kind === "parade" || root.kind === "histogram") ? Theme.fsLabel + Theme.s1 : 0)
        Image {
            id: trace
            objectName: "scopeTrace"
            anchors.fill: parent
            fillMode: root.kind === "vectorscope" ? Image.PreserveAspectFit : Image.Stretch
            cache: false
            asynchronous: false
            smooth: true
            // Pausing stops the producer; retain its last published reading.
            source: root.tick > 0 ? "image://thumbs/scope:" + root.kind + "?" + root.tick : ""
            // The provider always loads off the GUI thread, so every new
            // reading passes through Loading: keep the last one on screen
            // meanwhile, or playback blinks the placeholder at the scope rate.
            retainWhileLoading: true
            property bool shown: false
            onStatusChanged: if (status === Image.Ready) shown = true; else if (status === Image.Null) shown = false
            visible: shown
        }
        Rectangle { anchors.fill: parent; color: Theme.meterWell; border.color: Theme.border; border.width: Theme.hairline; visible: !trace.shown }
        Text {
            anchors.centerIn: parent
            visible: !trace.shown
            text: root.live ? qsTr("Waiting for a frame…") : qsTr("Scopes paused")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        // Horizontal histogram scale and parade channel labels.
        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.bottom
            visible: root.kind === "histogram" && root.showBottomLabels
            Repeater {
                // Under the black, mid and white lines, kept inside the plot.
                model: [0, 0.5, 1]
                delegate: Text {
                    required property var modelData
                    x: Math.max(0, Math.min(plot.width - width, (modelData - root.low) / (root.high - root.low) * (plot.width - 1) - width / 2))
                    text: root.label(modelData)
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel - 1
                    color: Theme.meterScale
                }
            }
        }
        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: -Theme.fsLabel - 2
            visible: root.kind === "parade" && root.showBottomLabels
            Repeater {
                model: ["R", "G", "B"]
                delegate: Text { required property string modelData; width: plot.width / 3; horizontalAlignment: Text.AlignHCenter; text: modelData; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
            }
        }
    }
    // Scale down the left for waveform / parade.
    Item {
        visible: (root.kind === "waveform" || root.kind === "parade") && plot.height >= 2 * Theme.fsLabel
        anchors.left: parent.left
        anchors.leftMargin: root.plotMargin
        anchors.top: plot.top
        anchors.bottom: plot.bottom
        Repeater {
            // One label per graticule line; the −10/110 headroom has none.
            model: [1, 0.75, 0.5, 0.25, 0]
            delegate: Item {
                required property var modelData
                width: 32
                visible: plot.height >= 100 || modelData !== 0.75 && modelData !== 0.25 && (plot.height >= 48 || modelData !== 0.5)
                y: (root.high - modelData) / (root.high - root.low) * (plot.height - 1)
                Text {
                    anchors.right: parent.right
                    // Centred on its line, but never past the plot's edges.
                    y: Math.max(-parent.y, Math.min(-height / 2, plot.height - parent.y - height))
                    objectName: "scopeLevelLabel"
                    text: root.label(modelData)
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsLabel - 1
                    color: Theme.meterScale
                }
            }
        }
    }
}
