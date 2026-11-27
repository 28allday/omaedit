import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui
import "../Ui/MediaTime.js" as MediaTime

// New Multicam Clip: pick ≥ 2 library clips as angles (in list order), name
// the clip, choose how to line them up. The engine builds it into the
// library; placing it puts angle 1 on the timeline, and the multicam
// viewer (Ctrl+Shift+M) cuts between angles. Sync Clips asks the same things
// and makes one clip of them: pictures with the recordings that go with them.
Ui.FormDialog {
    id: multicamDialog
    width: Math.min(520, parent ? parent.width - Ui.Theme.s5 * 2 : 520)
    title: synced ? qsTr("Sync Clips") : qsTr("New Multicam Clip")
    property bool synced: false
    standardButtons: Dialog.Ok | Dialog.Cancel
    // The box grows with its angles as far as the window lets it; past
    // that the list scrolls, never showing fewer than three rows.
    bodySlack: list.Layout.preferredHeight - list.Layout.minimumHeight
    onOpened: nameField.forceActiveFocus()
    readonly property var methodKeys: ["audio", "timecode", "markers", "start"]
    property var picked: []   // library indices, in angle order
    // Where a multicam clip's sound comes from: -1 follows the picture (a
    // cut takes the new angle's sound), else the library index of one of the
    // picked clips, whose sound plays whatever angle is shown. A recording
    // (sound, no picture) chosen so is lined up with the angles but is not
    // one of them.
    property int soundFrom: -1
    readonly property var soundRows: {
        var rows = [-1]
        for (var i = 0; i < picked.length; ++i)
            if (backend.library[picked[i]] !== undefined && backend.library[picked[i]].hasAudio !== false)
                rows.push(picked[i])
        return rows
    }
    readonly property var soundNames: {
        var names = []
        for (var i = 0; i < soundRows.length; ++i)
            names.push(soundRows[i] < 0 ? qsTr("Follows the picture") : backend.library[soundRows[i]].name)
        return names
    }
    onSoundRowsChanged: {
        if (soundRows.indexOf(soundFrom) < 0)
            soundFrom = -1
    }
    function recording(i) {
        var e = backend.library[i]
        return e !== undefined && (e.audioOnly === true || e.hasVideo === false)
    }

    function openForSync(indices) {
        openFor(indices)
        synced = true
    }
    function openFor(indices) {
        synced = false
        // (Assigned whole: what is worked out from the list follows it.)
        var ticked = []
        for (var i = 0; i < indices.length; ++i)
            if (eligible(indices[i]))
                ticked.push(indices[i])
        picked = ticked
        nameField.text = ""
        methodBox.currentIndex = 0
        soundFrom = -1
        open()
    }
    function eligible(i) {
        if (i < 0 || i >= backend.library.length)
            return false
        var e = backend.library[i]
        return e.multicam !== true && e.offline !== true
    }
    function toggle(i) {
        var at = picked.indexOf(i)
        var next = picked.slice()
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(i)
        picked = next
    }
    // Its number among the angles (-1: not picked). The recording the sound
    // comes from is no angle and takes no number (-2).
    function angleOf(i) {
        var at = picked.indexOf(i)
        if (at < 0 || synced || soundFrom < 0 || !recording(soundFrom))
            return at
        if (i === soundFrom)
            return -2
        return at > picked.indexOf(soundFrom) ? at - 1 : at
    }

    onAccepted: {
        if (picked.length < 2)
            return
        if (synced)
            backend.createSyncedClip(nameField.text, picked, methodKeys[methodBox.currentIndex], silenceCameras.checked)
        else
            backend.createMulticam(nameField.text, picked, methodKeys[methodBox.currentIndex], soundFrom)
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 12

        Ui.LineEdit {
            id: nameField
            objectName: "multicamName"
            Layout.fillWidth: true
            hoverEnabled: true
            tip: multicamDialog.synced ? qsTr("Name the synced clip gets in the library") : qsTr("Name the new multicam clip gets in the library")
            hint: multicamDialog.synced ? qsTr("Synced clip name") : qsTr("Multicam clip name")
            selectByMouse: true
        }
        RowLayout {
            spacing: 12
            Label { text: qsTr("Sync by"); color: win.textColor }
            Ui.ComboField {
                id: methodBox
                objectName: "multicamMethod"
                tip: multicamDialog.synced ? qsTr("How the clips are lined up with one another") : qsTr("How the angles are lined up with one another")
                Layout.fillWidth: true
                model: ["Audio waveform", "Timecode / creation time",
                        "First marker", "Clip start"]
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: win.textDimColor
            font.pixelSize: 12
            text: multicamDialog.synced
                  ? methodBox.currentIndex === 0
                    ? qsTr("Lines the clips up on their sound (first three minutes). They need to have heard the same room.")
                    : methodBox.currentIndex === 1
                      ? qsTr("Uses the timecode track when every clip has one, otherwise the files' creation times.")
                      : methodBox.currentIndex === 2
                        ? qsTr("Each clip's first clip marker (Media page ▸ Clip Log) is the same moment — a clap, a flash.")
                        : qsTr("All clips start together.")
                  : methodBox.currentIndex === 0
                  ? "Lines the angles up on their sound (first three minutes). Cameras need to have heard the same room."
                  : methodBox.currentIndex === 1
                    ? "Uses the container's timecode track when every angle has one, otherwise the files' creation times."
                    : methodBox.currentIndex === 2
                      ? "Each angle's first clip marker (Media page ▸ Clip Log) is the same moment — a clap, a flash."
                      : "All angles start together."
        }
        RowLayout {
            spacing: 12
            visible: !multicamDialog.synced
            Label { text: qsTr("Sound from"); color: win.textColor }
            Ui.ComboField {
                id: soundBox
                objectName: "multicamSound"
                tip: qsTr("Where the clip's sound comes from. Follows the picture: a cut to another angle takes that angle's sound. One of the clips: its sound plays on whatever angle is shown, and cuts change the picture only. A sound recording chosen here is lined up with the angles but is not one of them.")
                Layout.fillWidth: true
                model: multicamDialog.soundNames
                currentIndex: Math.max(0, multicamDialog.soundRows.indexOf(multicamDialog.soundFrom))
                onActivated: function(index) { multicamDialog.soundFrom = multicamDialog.soundRows[index] }
            }
        }
        CheckBox {
            id: silenceCameras
            objectName: "syncSilenceCameras"
            visible: multicamDialog.synced
            checked: true
            text: qsTr("Use only the recordings' sound")
            ToolTip.text: qsTr("Leaves out the sound of clips that have a picture, so a camera's guide track does not play over the recorder")
            ToolTip.visible: hovered
            ToolTip.delay: 450
        }
        Label {
            text: multicamDialog.synced ? qsTr("Clips to sync (%n picked)", "", picked.length)
                                        : qsTr("Angles — tick in the order you want them (%n picked)", "", picked.length)
            color: win.textColor
            font.pixelSize: 12
        }
        ListView {
            id: list
            objectName: "multicamAngleList"
            readonly property int rowHeight: 32
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.max(rowHeight * 3, contentHeight) + bottomMargin
            Layout.minimumHeight: rowHeight * 3 + bottomMargin
            // Room under the last row, so the list never ends flush on a cut one.
            bottomMargin: Ui.Theme.s1
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: backend.library
            ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: list.width
                // (A clip that cannot be an angle takes no room: no gaps.)
                height: enabled ? list.rowHeight : 0
                enabled: multicamDialog.eligible(index)
                visible: enabled
                onClicked: multicamDialog.toggle(index)
                contentItem: RowLayout {
                    spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 4
                        color: multicamDialog.angleOf(index) !== -1 ? win.accent : "transparent"
                        border.color: win.seamColor
                        border.width: 1
                        Label {
                            anchors.centerIn: parent
                            text: multicamDialog.angleOf(index) >= 0
                                  ? String(multicamDialog.angleOf(index) + 1)
                                  : multicamDialog.angleOf(index) === -2 ? "♪" : ""
                            color: "#0B0E13"
                            font.bold: true
                            font.pixelSize: 12
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        elide: Text.ElideMiddle
                        color: win.textColor
                    }
                    Label {
                        text: modelData.duration !== undefined
                              ? MediaTime.durationText(MediaTime.sourceFrames(modelData), MediaTime.effectiveFps(modelData)) : ""
                        color: win.textColor
                        opacity: 0.6
                        font.family: Ui.Theme.monoFamily
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
