import QtQuick
import QtQuick.Controls
import OmaEdit.Ui as Ui

// Adjust Volume: set the selected clip to a level, or change it by an amount.
// A clip with volume keyframes keeps them; its whole curve moves by the change.
Ui.FormDialog {
    id: root
    width: Math.min(360, parent ? parent.width - Ui.Theme.s5 * 2 : 360)
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: relative ? qsTr("Change Volume") : qsTr("Set Volume")
    standardButtons: Dialog.Ok | Dialog.Cancel
    property bool relative: false
    readonly property real amount: Number(field.text)

    function openFor(changeBy) {
        relative = changeBy
        field.text = relative ? "0" : String(win.sound.volume())
        open()
    }

    Ui.LineEdit {
        id: field
        objectName: "volumeField"
        hoverEnabled: true
        tip: root.relative ? qsTr("Decibels to add to the clip's volume; negative lowers it")
                           : qsTr("The clip's volume in decibels, from -60 to 12")
        hint: qsTr("Decibels")
        onAccepted: root.accept()
    }
    onAboutToShow: { field.forceActiveFocus(); field.selectAll() }
    onAccepted: {
        if (field.text.trim() === "" || isNaN(amount))
            return
        if (relative)
            win.sound.adjustVolume(amount)
        else
            win.sound.setVolume(Math.max(-60, Math.min(12, amount)))
    }
}
