import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui

// The encoder log of one queue job — what the encoder said, verbatim,
// so a failed render explains itself.
FormDialog {
    id: root
    objectName: "renderLogDialog"
    width: Math.min(760, parent ? parent.width - Theme.s5 * 2 : 760)
    title: qsTr("Render Log")
    standardButtons: Dialog.Close

    property var lines: []
    property string jobName: ""

    function openFor(name, log) {
        jobName = name
        lines = log || []
        open()
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.s2
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.jobName
            color: Theme.textPrimary
            font.weight: Theme.wHeading
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 320
            clip: true
            TextArea {
                objectName: "renderLogText"
                readOnly: true
                text: root.lines.length > 0 ? root.lines.join("\n") : qsTr("Nothing logged.")
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textSecondary
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                background: Rectangle { color: Theme.windowBg; radius: Theme.rControl }
            }
        }
    }
}
