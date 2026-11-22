import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.ModalDialog {
    id: root
    objectName: "diagnosticsDialog"
    title: qsTr("About OmaEdit · Diagnostics")
    width: Math.min(820, parent ? parent.width - Ui.Theme.s5 * 2 : 820)
    height: Math.min(640, parent ? parent.height - Ui.Theme.s5 * 2 : 640)
    standardButtons: C.Dialog.NoButton
    property string report: ""
    property bool copied: false
    function openFor() { report = backend.systemInformation(); copied = false; open(); backend.hardware.probe() }
    onOpened: info.forceActiveFocus()
    Connections {
        target: backend.hardware
        function onInfoChanged() { if (root.visible) root.report = backend.systemInformation() }
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2
        C.Label {
            text: qsTr("OmaEdit · Development build")
            color: Ui.Theme.textPrimary
            font.pixelSize: Ui.Theme.fsHeading
        }
        C.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Local system and capability report. Copy places this text on your clipboard; nothing is sent.")
            color: Ui.Theme.textSecondary
        }
        C.Label {
            objectName: "diagnosticsNotices"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Ui.Theme.fsLabel
            color: Ui.Theme.textMuted
            readonly property string folder: backend.licensesFolder()
            text: qsTr("OmaEdit is free software under the GNU GPL-3.0-or-later. Icons: Lucide (ISC). Vulkan headers (MIT). Timelines: OpenTimelineIO (Apache-2.0). Engine: FFmpeg (GPL-3), OpenColorIO (BSD-3), OpenEXR (BSD-3), OpenCV (Apache-2.0), vid.stab and Rubber Band (GPL-2), Qt 6 (LGPL-3). Optional: pyaaf2 (MIT).")
                  + (folder !== "" ? " " + qsTr("Licence texts: %1").arg(folder) : "")
        }
        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            C.TextArea {
                id: info
                objectName: "diagnosticsText"
                text: root.report
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: Ui.Theme.textPrimary
                font.family: Ui.Theme.monoFamily
                font.pixelSize: Ui.Theme.fsLabel
                Accessible.name: qsTr("System information")
                topPadding: Ui.Theme.s2; bottomPadding: Ui.Theme.s2
                leftPadding: Ui.Theme.s2; rightPadding: Ui.Theme.s2
                topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
                background: Rectangle { color: Ui.Theme.panelBg; border.color: Ui.Theme.border }
            }
        }
    }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
            Ui.ToolButton {
                objectName: "diagnosticsRefresh"
                text: qsTr("Refresh")
                enabled: !backend.hardware.info.gpuBusy && !backend.hardware.info.encoderBusy
                onClicked: { root.copied = false; backend.hardware.probe(true) }
            }
            Item { Layout.fillWidth: true }
            Ui.ToolButton {
                objectName: "diagnosticsCopy"
                text: root.copied ? qsTr("Copied") : qsTr("Copy System Information")
                onClicked: root.copied = backend.copySystemInformation()
            }
            Ui.ToolButton { text: qsTr("Close"); onClicked: root.close() }
        }
    }
}
