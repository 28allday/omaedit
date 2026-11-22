import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

Ui.ModalDialog {
    id: root

    property var controller: win.activeLayout
    property string selectedPreset: ""
    property string message: ""

    function save(replace) {
        const name = replace ? selectedPreset : presetName.text.trim();
        if (controller && controller.savePreset(name, replace)) {
            selectedPreset = name;
            message = qsTr("Layout saved.");
        } else {
            message = qsTr("Choose a new name, or select an existing layout and use Update.");
        }
    }

    objectName: "workspaceLayoutDialog"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    height: Math.min(640, parent ? parent.height - 40 : 640)
    modal: true
    title: qsTr("Workspace layout")
    standardButtons: C.Dialog.Close
    onControllerChanged: {
        selectedPreset = "";
        message = "";
    }

    Ui.MessageDialog {
        id: deletePreset

        title: qsTr("Delete layout?")
        standardButtons: C.Dialog.Cancel | C.Dialog.Ok
        onAccepted: {
            backend.workspaceLayouts.removePreset(win.workspace, root.selectedPreset);
            root.selectedPreset = "";
        }

        message: qsTr("Delete layout “%1”? The current panel arrangement stays in place.").arg(root.selectedPreset)

    }

    background: Rectangle {
        color: Ui.Theme.panelBg
        radius: Ui.Theme.rMenu
        border.color: Ui.Theme.border
    }


    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2

        RowLayout {
            C.Label {
                text: qsTr("Workspace")
                color: Ui.Theme.textSecondary
            }

            Ui.ComboField {
                objectName: "layoutWorkspace"
                // The keys the window knows the pages by, and what each is called.
                readonly property var keys: ["Media", "Edit", "Color", "Audio", "Deliver"]
                model: [qsTr("Media"), qsTr("Edit"), qsTr("Colour"), qsTr("Audio"), qsTr("Export")]
                tip: qsTr("Switch the workspace whose panels and saved layouts are shown here")
                currentIndex: keys.indexOf(win.workspace)
                onActivated: function(i) {
                    win.workspace = keys[i];
                }
            }

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                text: qsTr("Restore All Panels")
                tip: qsTr("Show every hidden panel, dock detached ones and leave maximised or full screen")
                onClicked: {
                    if (root.controller)
                        root.controller.restorePanels();

                }
            }

        }

        C.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: qsTr("Drag panel dividers to resize. Layouts are remembered for each workspace. Detached windows can be moved to another monitor.")
        }

        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                model: root.controller ? root.controller.panels.filter(function (p) { return p.offered !== false }) : []
                spacing: Ui.Theme.s1

                delegate: Rectangle {
                    required property var modelData

                    width: ListView.view.width
                    height: modelData.detached ? 76 : 40
                    color: Ui.Theme.panelRaised
                    radius: Ui.Theme.rControl

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Ui.Theme.s1
                        spacing: Ui.Theme.s1

                        RowLayout {
                            Layout.fillWidth: true

                            C.CheckBox {
                                text: modelData.title
                                checked: !modelData.hidden
                                C.ToolTip.text: qsTr("Show or hide this panel in the workspace"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450
                                onClicked: modelData.setShown(checked)
                                Layout.fillWidth: true
                            }

                            FlatButton {
                                text: modelData.maximized ? qsTr("Restore") : qsTr("Maximise")
                                enabled: !modelData.fullscreen
                                tip: qsTr("Fill the workspace with this panel alone, or bring the other panels back")
                                onClicked: modelData.toggleMaximize()
                            }

                            FlatButton {
                                visible: modelData.detachable
                                text: modelData.detached ? qsTr("Dock") : qsTr("Detach")
                                enabled: !modelData.fullscreen
                                tip: qsTr("Move this panel into its own window, or back into the workspace")
                                onClicked: modelData.setDetached(!modelData.detached)
                            }

                            FlatButton {
                                visible: modelData.viewer
                                text: qsTr("Full Screen")
                                tip: qsTr("Close this window and show the viewer full screen")
                                onClicked: {
                                    root.close();
                                    modelData.toggleFullscreen();
                                }
                            }

                        }

                        RowLayout {
                            visible: modelData.detached

                            C.Label {
                                text: qsTr("Monitor")
                                color: Ui.Theme.textSecondary
                            }

                            Ui.ComboField {
                                Layout.fillWidth: true
                                tip: qsTr("Display the detached window is moved to")
                                model: backend.workspaceLayouts.screens.map(function(s) {
                                    return s.name || qsTr("Display");
                                })
                                currentIndex: backend.workspaceLayouts.screens.findIndex(function(s) {
                                    return modelData.window.screen && s.name === modelData.window.screen.name;
                                })
                                onActivated: function(i) {
                                    modelData.moveToScreen(backend.workspaceLayouts.screens[i].name);
                                }
                            }

                        }

                    }

                }

            }

        }

        C.Label {
            text: qsTr("Named layouts for %1").arg(win.workspace === "Color" ? qsTr("Colour") : win.workspace)
            color: Ui.Theme.textSecondary
        }

        RowLayout {
            Ui.ComboField {
                objectName: "layoutPresets"
                Layout.fillWidth: true
                tip: qsTr("Saved layout for this workspace to apply, update or delete")
                model: root.controller ? root.controller.presetNames : []
                currentIndex: model.indexOf(root.selectedPreset)
                onActivated: function(i) {
                    root.selectedPreset = model[i];
                }
            }

            FlatButton {
                text: qsTr("Apply")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Arrange the panels as this saved layout has them")
                onClicked: root.controller.restorePreset(root.selectedPreset)
            }

            FlatButton {
                text: qsTr("Update")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Overwrite this saved layout with the current panel arrangement")
                onClicked: root.save(true)
            }

            FlatButton {
                text: qsTr("Delete…")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Remove this saved layout after confirming; the panels stay as they are")
                onClicked: deletePreset.open()
            }

        }

        RowLayout {
            Ui.LineEdit {
                id: presetName

                objectName: "layoutPresetName"
                Layout.fillWidth: true
                placeholderText: qsTr("New layout name")
                tip: qsTr("Name for a new saved layout of the current panel arrangement")
                maximumLength: 80
            }

            FlatButton {
                objectName: "saveLayoutPreset"
                text: qsTr("Save New")
                enabled: presetName.text.trim().length > 0
                tip: qsTr("Store the current panel arrangement as a new layout under this name")
                onClicked: root.save(false)
            }

            FlatButton {
                objectName: "resetWorkspaceLayout"
                text: qsTr("Reset Workspace")
                tip: qsTr("Return this workspace to its default panel arrangement")
                onClicked: {
                    root.controller.reset();
                    root.message = qsTr("Default layout restored.");
                }
            }

        }

        C.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.message
        }

    }

}
