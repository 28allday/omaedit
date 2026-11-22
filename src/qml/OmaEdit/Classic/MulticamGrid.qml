import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// The multicam viewer: every angle of the multicam clip under the playhead
// at the playhead's moment, the angle on the program outlined. Click an
// angle to cut to it at the playhead, stopped or playing; keys 1–9 do the
// same. With Shift (or Alt on a click) the whole segment changes, uncut.
Rectangle {
    id: grid
    color: win.wellColor
    readonly property var multicam: backend.multicamClip
    readonly property bool has: multicam !== undefined && multicam.has === true
    readonly property var angles: has ? multicam.angles : []
    readonly property bool fixedSound: has && multicam.soundFrom !== undefined && multicam.soundFrom !== ""
    readonly property int columns: angles.length <= 1 ? 1
                                   : angles.length <= 4 ? 2
                                   : angles.length <= 9 ? 3 : 4
    readonly property int rows: Math.max(1, Math.ceil(angles.length / columns))

    // Plain click: cut at the playhead. Shift+click and Alt+click change
    // the whole segment, as Shift+1–9 do.
    function pick(angle, cut) {
        if (has)
            backend.cutToAngle(angle, cut)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 2
        spacing: 0

        // Header, like the monitors': MULTICAM · clip name · audio option.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Ui.Theme.s3
                anchors.rightMargin: Ui.Theme.s3
                spacing: Ui.Theme.s2
                Text {
                    text: qsTr("Multicam")
                    font.family: Ui.Theme.fontFamily
                    font.pixelSize: Ui.Theme.fsLabel
                    font.weight: Ui.Theme.wHeading
                    color: Ui.Theme.accent
                }
                Text {
                    objectName: "multicamTitle"
                    Layout.fillWidth: true
                    // (A clip made with a sound source of its own says so.)
                    text: !grid.has ? "" : grid.fixedSound
                          ? qsTr("%1 · sound from %2").arg(grid.multicam.name).arg(grid.multicam.soundFrom)
                          : grid.multicam.name
                    elide: Text.ElideRight
                    font.family: Ui.Theme.fontFamily
                    font.pixelSize: Ui.Theme.fsLabel
                    color: Ui.Theme.textSecondary
                }
                Ui.IconButton {
                    objectName: "audioFollowsButton"
                    // The switch is for clips whose sound follows the picture.
                    // A clip made with a sound source of its own never changes
                    // its sound, so the switch is out of use while one is shown.
                    enabled: !grid.fixedSound
                    iconName: backend.multicamAudioFollows && !grid.fixedSound ? "volume-2" : "volume-x"
                    text: grid.fixedSound
                          ? qsTr("This clip's sound is from %1 — cuts change the picture only").arg(grid.multicam.soundFrom)
                          : backend.multicamAudioFollows
                          ? qsTr("Audio follows video — a cut takes the new angle's sound")
                          : qsTr("Audio stays — cuts change the picture only")
                    checkable: true
                    checked: backend.multicamAudioFollows && !grid.fixedSound
                    onClicked: win.multicamAudioFollows = !win.multicamAudioFollows
                }
                Ui.IconButton {
                    iconName: "x"
                    text: qsTr("Close the multicam viewer")
                    shortcut: "Ctrl+Shift+M"
                    onClicked: backend.multicamMode = false
                }
            }
        }

        Item {
            id: stage
            Layout.fillWidth: true
            Layout.fillHeight: true

            Text {
                objectName: "multicamNothing"
                anchors.centerIn: parent
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: !grid.has
                text: backend.hasVideo
                      ? qsTr("Park the playhead on a multicam clip to see its angles")
                      : qsTr("Build a multicam clip from the library (right-click ▸ New Multicam Clip…)")
                font.family: Ui.Theme.fontFamily
                font.pixelSize: Ui.Theme.fsBase
                color: Ui.Theme.textSecondary
            }

            Grid {
                objectName: "angleGrid"
                visible: grid.has
                anchors.centerIn: parent
                columns: grid.columns
                spacing: 4
                readonly property real cellW: Math.floor((stage.width - (grid.columns - 1) * spacing) / grid.columns)
                readonly property real cellH: Math.floor((stage.height - (grid.rows - 1) * spacing) / grid.rows)
                // Cells keep the picture's aspect and never overflow the stage.
                readonly property real aspect: backend.videoWidth > 0 && backend.videoHeight > 0
                                               ? backend.videoDisplayAspect : 16 / 9
                readonly property real w: Math.max(40, Math.min(cellW, cellH * aspect))
                readonly property real h: Math.max(24, w / aspect)

                Repeater {
                    model: grid.angles.length
                    delegate: Rectangle {
                        id: cell
                        required property int index
                        readonly property bool active: grid.has && grid.multicam.angle === index
                        readonly property string url: index < backend.angleFrames.length
                                                      ? backend.angleFrames[index] : ""
                        width: parent.w
                        height: parent.h
                        color: "black"
                        border.width: active ? 2 : 1
                        border.color: active ? Ui.Theme.accent : Ui.Theme.border
                        Image {
                            anchors.fill: parent
                            anchors.margins: 2
                            source: cell.url
                            cache: false
                            asynchronous: true
                            // Each frame of playback gives the cell a new
                            // picture, and a picture is always loaded behind
                            // the scenes: the last one stays until the new
                            // one is there. (Dropped at once, as an Image
                            // does unless told, every cell drew its black
                            // ground between pictures: the whole grid
                            // flickered while the timeline played.)
                            retainWhileLoading: true
                            fillMode: Image.PreserveAspectFit
                            visible: cell.url !== ""
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: cell.url === ""
                            text: qsTr("no picture here")
                            font.family: Ui.Theme.fontFamily
                            font.pixelSize: Ui.Theme.fsLabel
                            color: Ui.Theme.textSecondary
                        }
                        // Number + name, bottom-left; accent when on air.
                        Rectangle {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 4
                            width: cellLabel.implicitWidth + Ui.Theme.s2
                            height: 18
                            radius: 3
                            color: cell.active ? Ui.Theme.accent : "#B0000000"
                            Text {
                                id: cellLabel
                                anchors.centerIn: parent
                                text: (cell.index + 1) + "  " + grid.angles[cell.index].name
                                font.family: Ui.Theme.fontFamily
                                font.pixelSize: Ui.Theme.fsLabel
                                font.weight: Ui.Theme.wHeading
                                color: cell.active ? Ui.Theme.accentText : "white"
                            }
                        }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            acceptedModifiers: Qt.NoModifier
                            onTapped: grid.pick(cell.index, true)
                        }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            acceptedModifiers: Qt.ShiftModifier
                            onTapped: grid.pick(cell.index, false)
                        }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            acceptedModifiers: Qt.AltModifier
                            onTapped: grid.pick(cell.index, false)
                        }
                        HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                        Rectangle {
                            anchors.fill: parent
                            color: Ui.Theme.accent
                            opacity: cellHover.hovered && !cell.active ? 0.12 : 0
                        }
                    }
                }
            }
        }

        Text {
            objectName: "multicamHint"
            Layout.fillWidth: true
            Layout.margins: Ui.Theme.s2
            horizontalAlignment: Text.AlignHCenter
            // Onto a second line in a narrow viewer, never cut off at its ends.
            wrapMode: Text.WordWrap
            visible: grid.has
            text: qsTr("click / 1–9: cut at the playhead · Shift+click / Shift+1–9: switch the whole segment")
            font.family: Ui.Theme.fontFamily
            font.pixelSize: Ui.Theme.fsLabel
            color: Ui.Theme.textSecondary
        }
    }
}
