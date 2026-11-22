pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Stereo meters on the far right of the timeline. Green up to −18, amber to
// −6, red above: the ranges an operator reads without thinking, with a peak
// hold that stays put long enough to be caught.
Rectangle {
    id: root

    property real levelLeft: 0.72     // 0..1 of full scale
    property real levelRight: 0.66
    property real peakLeft: 0.80
    property real peakRight: 0.74

    implicitWidth: 92
    color: Theme.panelBg

    Rectangle {
        anchors.left: parent.left
        width: Theme.hairline
        height: parent.height
        color: Theme.border
    }

    Text {
        id: title
        anchors.top: parent.top
        anchors.topMargin: Theme.s2
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        text: qsTr("Meters")
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }

    Row {
        id: meterRow
        anchors.top: title.bottom
        anchors.topMargin: Theme.s2
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.s3
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.s2

        Repeater {
            model: [
                { level: root.levelLeft,  peak: root.peakLeft,  label: "L" },
                { level: root.levelRight, peak: root.peakRight, label: "R" }
            ]

            delegate: Item {
                id: ch
                required property var modelData
                width: 18
                height: meterRow.height

                Rectangle {
                    id: well
                    anchors.top: parent.top
                    anchors.bottom: channelLabel.top
                    anchors.bottomMargin: Theme.s1
                    width: parent.width
                    color: Theme.windowBg
                    radius: 1

                    // Full-scale gradient, revealed from the bottom by the
                    // current level — so the colour at the top of the bar is
                    // always the colour that range means.
                    Item {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height * Math.max(0, Math.min(1, ch.modelData.level))
                        clip: true

                        Rectangle {
                            width: parent.width
                            height: well.height
                            anchors.bottom: parent.bottom
                            gradient: Gradient {
                                GradientStop { position: 0.00; color: Theme.danger }
                                GradientStop { position: 0.16; color: Theme.warning }
                                GradientStop { position: 0.34; color: Theme.audio }
                                GradientStop { position: 1.00; color: Theme.audio }
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        y: parent.height * (1 - Math.max(0, Math.min(1, ch.modelData.peak)))
                        height: Theme.hairline
                        color: Theme.textPrimary
                    }
                }

                Text {
                    id: channelLabel
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: ch.modelData.label
                    font.family: Theme.fontFamily
                    font.pixelSize: 9
                    color: Theme.textMuted
                }
            }
        }
    }

    // dBFS scale, aligned to the wells rather than to the whole column so the
    // numbers line up with the levels they label.
    Column {
        id: dbScale
        anchors.right: meterRow.left
        anchors.rightMargin: Theme.s1
        anchors.top: meterRow.top
        anchors.bottom: meterRow.bottom
        anchors.bottomMargin: Theme.hRow

        readonly property var stops: ["0", "-6", "-12", "-18", "-24",
                                      "-30", "-36", "-42", "-48", "-54"]

        Repeater {
            model: dbScale.stops
            delegate: Text {
                required property string modelData
                text: modelData
                height: dbScale.height / dbScale.stops.length
                font.family: Theme.monoFamily
                font.pixelSize: 8
                color: Theme.textMuted
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignTop
                width: 20
            }
        }
    }
}
