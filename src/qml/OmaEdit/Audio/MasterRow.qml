import QtQuick
import OmaEdit.Ui

// The row under the tracks: "Master", M / S-safe, the master fader's dB,
// and (in the lane area) a horizontal meter of the mix.
Rectangle {
    id: root

    property var master: ({})
    property real levelLeft: -100
    property real levelRight: -100
    property bool lane: false
    property bool muted: false
    property bool selected: false
    signal selectedRequested()
    signal muteToggled(bool on)
    signal volumeEdited(real db)

    readonly property real volume: master.volume !== undefined ? master.volume : 0

    height: Theme.hMasterRow
    color: lane ? Theme.panelBg : selected ? Theme.controlBg : Theme.panelRaised
    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.borderStrong }

    TapHandler { enabled: !root.lane; onTapped: root.selectedRequested() }

    Row {
        visible: !root.lane
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 3; height: Theme.hMasterRow - 10; color: Theme.roleMaster }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: 52
            text: qsTr("Master")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 22; height: Theme.hControl - 4; radius: Theme.rControl
            color: root.muted ? Theme.warning : Theme.panelBg
            border.color: Theme.border; border.width: Theme.hairline
            Text { anchors.centerIn: parent; text: "M"; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading; color: root.muted ? Theme.accentText : Theme.textSecondary }
            TapHandler { onTapped: root.muteToggled(!root.muted) }
        }
        Text {
            objectName: "masterDb"
            anchors.verticalCenter: parent.verticalCenter
            width: 44
            horizontalAlignment: Text.AlignRight
            text: root.volume <= -100 ? "−∞" : (root.volume > 0 ? "+" : "") + root.volume.toFixed(1)
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textSecondary
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (e) { root.volumeEdited(Math.max(-100, Math.min(12, root.volume + (e.angleDelta.y > 0 ? 0.5 : -0.5)))) }
            }
        }
    }

    // The lane version: a wide stereo bar.
    Item {
        visible: root.lane
        anchors.fill: parent
        anchors.margins: Theme.s1
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        HMeter {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.hHMeter * 2 + 4
            levelLeft: root.levelLeft
            levelRight: root.levelRight
            clip: true
        }
    }
}
