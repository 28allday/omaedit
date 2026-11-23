import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Speakers (Edit ▸ Captions ▸ Speakers… and the caption list's menu): the
// speaker of the selected captions, and the timeline's speakers — each a
// colour shown in the caption list and on the caption's timeline block, a
// name to change everywhere at once, or one to take off every caption.
// Each change is one undo step.
Ui.FormDialog {
    id: root
    objectName: "captionSpeakerDialog"
    width: Math.min(480, parent ? parent.width - Ui.Theme.s5 * 2 : 480)
    title: qsTr("Speakers")
    standardButtons: indices.length > 0 ? Dialog.Ok | Dialog.Cancel : Dialog.Close

    property var indices: []
    readonly property var speakers: {
        const settings = backend.captionSettings, entries = backend.connected
        return backend.captionSpeakers()
    }
    readonly property var colours: backend.captionSpeakerColours()

    function openFor(chosen) {
        indices = chosen || []
        // The speaker the first of them has, to start from.
        const first = indices.length > 0 ? backend.connected[indices[0]] : null
        speakerField.text = first && first.speaker ? first.speaker : ""
        newField.text = ""
        open()
    }
    onAccepted: {
        landTyped()
        if (indices.length > 0) backend.setCaptionSpeaker(indices, speakerField.text.trim())
    }
    function nextColour(colour) {
        const at = colours.indexOf(colour)
        return colours[(at + 1) % colours.length]
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        Label {
            visible: root.indices.length > 0
            text: qsTr("Speaker of the %n selected caption(s)", "", root.indices.length)
            color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading
        }
        RowLayout {
            visible: root.indices.length > 0
            Layout.fillWidth: true
            spacing: 8
            Ui.LineEdit {
                id: speakerField
                objectName: "captionSpeakerName"
                Layout.fillWidth: true
                hint: qsTr("No speaker")
                tip: qsTr("A speaker's name; one not listed below joins the list")
            }
            Ui.ComboField {
                objectName: "captionSpeakerPick"
                tip: qsTr("Choose one of the timeline's speakers")
                model: [qsTr("Choose…")].concat(root.speakers.map(function (s) { return s.name }))
                onActivated: function (i) { if (i > 0) speakerField.text = root.speakers[i - 1].name; currentIndex = 0 }
                Layout.preferredWidth: 150
            }
        }

        Label {
            text: qsTr("Speakers of this timeline")
            color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading
        }
        Label {
            visible: root.speakers.length === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: qsTr("None yet. Name one here, or give captions a speaker.")
        }
        Repeater {
            model: root.speakers
            delegate: RowLayout {
                id: speakerRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    objectName: "captionSpeakerColour:" + speakerRow.modelData.name
                    Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 11
                    color: speakerRow.modelData.colour
                    border.width: 1; border.color: Ui.Theme.border
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        Accessible.name: qsTr("Next colour for %1").arg(speakerRow.modelData.name)
                        onClicked: backend.setCaptionSpeakerColour(speakerRow.modelData.name, root.nextColour(speakerRow.modelData.colour))
                    }
                }
                Ui.LineEdit {
                    Layout.fillWidth: true
                    text: speakerRow.modelData.name
                    tip: qsTr("Rename this speaker on every caption: type and press Enter")
                    onAccepted: backend.renameCaptionSpeaker(speakerRow.modelData.name, text)
                }
                Label {
                    text: qsTr("%n caption(s)", "", speakerRow.modelData.captions)
                    color: Ui.Theme.textMuted
                }
                FlatButton {
                    text: qsTr("Remove")
                    tip: qsTr("Take this speaker off the list and off every caption")
                    quiet: true
                    implicitHeight: 26
                    onClicked: backend.renameCaptionSpeaker(speakerRow.modelData.name, "")
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Ui.LineEdit {
                id: newField
                objectName: "captionSpeakerNew"
                Layout.fillWidth: true
                hint: qsTr("New speaker")
                tip: qsTr("A name to add to this timeline's speakers")
                onAccepted: addButton.clicked()
            }
            FlatButton {
                id: addButton
                text: qsTr("Add")
                tip: qsTr("Add this speaker, in a colour of their own")
                quiet: true
                implicitHeight: 28
                enabled: newField.text.trim() !== ""
                onClicked: { if (backend.setCaptionSpeakerColour(newField.text.trim(), "")) newField.text = "" }
            }
        }
    }
}
