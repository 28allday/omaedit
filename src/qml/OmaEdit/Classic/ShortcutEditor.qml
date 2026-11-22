import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

Ui.ModalDialog {
    id: root

    property string selectedId: ""
    property string selectedPreset: ""
    property string message: ""
    property bool recording: false
    readonly property var registry: backend.commands
    readonly property var rows: {
        if (!visible) return [];
        const rev = registry.revision;
        return registry.search(searchField.text);
    }
    readonly property var proposed: draft.text.split("\n").map(function(s) {
        return s.trim();
    }).filter(function(s) {
        return s.length > 0;
    })
    readonly property var clashes: {
        const rev = registry.revision;
        return selectedId ? registry.conflicts(selectedId, proposed) : [];
    }

    function select(id) {
        selectedId = id;
        draft.text = registry.shortcuts(id).join("\n");
        message = "";
        recording = false;
    }

    function openFor() {
        open();
        if (!selectedId && rows.length)
            select(rows[0].id);

        searchField.forceActiveFocus();
    }

    function assign(reassign) {
        message = registry.assign(selectedId, proposed, reassign) ? qsTr("Shortcut changes saved.") : registry.error;
    }

    objectName: "shortcutEditor"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(980, parent ? parent.width - 32 : 980)
    height: Math.min(740, parent ? parent.height - 32 : 740)
    title: qsTr("Keyboard Shortcuts")
    modal: true
    standardButtons: C.Dialog.Close
    onClosed: recording = false

    Ui.ModalDialog {
        id: resetConfirm

        title: qsTr("Reset all keyboard shortcuts?")
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onAccepted: {
            root.registry.resetAll();
            if (root.selectedId)
                root.select(root.selectedId);

            root.message = qsTr("Default shortcuts restored. Saved presets are retained.");
        }

        C.Label {
            text: qsTr("Replace the current key assignments with OmaEdit defaults.")
            color: Ui.Theme.textSecondary
            wrapMode: Text.WordWrap
            width: parent.width
        }

    }

    Ui.ModalDialog {
        id: deleteConfirm

        title: qsTr("Delete shortcut preset “%1”?").arg(root.selectedPreset)
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onAccepted: {
            root.registry.deletePreset(root.selectedPreset);
            root.selectedPreset = "";
        }

        C.Label {
            text: qsTr("The current key assignments stay in place.")
            color: Ui.Theme.textSecondary
        }

    }

    background: Rectangle {
        color: Ui.Theme.panelBg
        radius: Ui.Theme.rMenu
        border.color: Ui.Theme.border
    }


    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2

        Ui.LineEdit {
            id: searchField

            objectName: "shortcutSearch"
            Layout.fillWidth: true
            hint: qsTr("Search actions, categories or keys…")
            Accessible.name: qsTr("Search keyboard shortcuts")
            tip: qsTr("Filter the command list by action name, category or assigned keys")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Ui.Theme.s3

            C.ScrollView {
                C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 340
                clip: true

                ListView {
                    id: list

                    objectName: "shortcutCommandList"
                    model: root.rows
                    currentIndex: root.rows.findIndex(function(r) {
                        return r.id === root.selectedId;
                    })
                    keyNavigationEnabled: true
                    Keys.onReturnPressed: {
                        if (currentIndex >= 0) {
                            root.select(root.rows[currentIndex].id);
                        }
                    }

                    C.Label {
                        anchors.centerIn: parent
                        visible: list.count === 0
                        text: qsTr("No matching commands")
                        color: Ui.Theme.textMuted
                    }

                    delegate: C.ItemDelegate {
                        required property var modelData
                        required property int index

                        width: ListView.view.width
                        height: 56
                        highlighted: root.selectedId === modelData.id
                        onClicked: root.select(modelData.id)

                        contentItem: Column {
                            spacing: 3

                            Text {
                                text: modelData.text
                                color: Ui.Theme.textPrimary
                                font.pixelSize: Ui.Theme.fsControl
                                width: parent.width
                                elide: Text.ElideRight
                            }

                            Text {
                                text: modelData.category + "  ·  " + (modelData.hint || qsTr("Unassigned")) + (modelData.modified ? "  *" : "")
                                color: Ui.Theme.textMuted
                                font.pixelSize: Ui.Theme.fsLabel
                                width: parent.width
                                elide: Text.ElideRight
                            }

                        }

                        background: Rectangle {
                            color: parent.highlighted ? Ui.Theme.controlBg : parent.hovered ? Ui.Theme.panelRaised : "transparent"
                            border.width: parent.visualFocus ? 1 : 0
                            border.color: Ui.Theme.accent
                        }

                    }

                }

            }

            ColumnLayout {
                Layout.preferredWidth: 350
                Layout.fillHeight: true

                C.Label {
                    text: Ui.Commands.title(root.selectedId, qsTr("Select a command"))
                    color: Ui.Theme.textPrimary
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                C.Label {
                    text: qsTr("Enter one shortcut per line (up to four). Use commas for a chord, such as Ctrl+K, Ctrl+C. Leave empty to unassign.")
                    color: Ui.Theme.textMuted
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                C.TextArea {
                    id: draft

                    objectName: "shortcutDraft"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 112
                    enabled: root.selectedId !== ""
                    placeholderText: ""
                    padding: Ui.Theme.s2
                    topPadding: Ui.Theme.s2
                    bottomPadding: Ui.Theme.s2
                    leftPadding: Ui.Theme.s2
                    rightPadding: Ui.Theme.s2
                    clip: true
                    topInset: 0
                    bottomInset: 0
                    font.pixelSize: Ui.Theme.fsBase
                    font.family: Ui.Theme.monoFamily
                    color: Ui.Theme.textPrimary
                    selectByMouse: true
                    Accessible.name: qsTr("Assigned shortcuts, one per line")

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: draft.padding
                        visible: !draft.text
                        text: qsTr("For example: Ctrl+Shift+B")
                        color: Ui.Theme.textMuted
                        font: draft.font
                    }

                    background: Rectangle {
                        color: Ui.Theme.windowBg
                        border.color: draft.activeFocus ? Ui.Theme.accent : Ui.Theme.border
                        radius: Ui.Theme.rControl
                    }

                }

                RowLayout {
                    FlatButton {
                        id: recordButton

                        objectName: "recordShortcut"
                        text: root.recording ? qsTr("Press keys… Esc cancels") : qsTr("Record Key")
                        enabled: root.selectedId !== ""
                        tip: qsTr("Capture the next key combination you press as this command's shortcut")
                        onClicked: {
                            root.recording = !root.recording;
                            forceActiveFocus();
                        }
                        Keys.onShortcutOverride: function(event) {
                            if (root.recording)
                                event.accepted = true;

                        }
                        Keys.onPressed: function(event) {
                            if (!root.recording)
                                return ;

                            event.accepted = true;
                            if (event.key === Qt.Key_Escape) {
                                root.recording = false;
                                return ;
                            }
                            const key = root.registry.captured(event.key, event.modifiers);
                            if (key) {
                                draft.text = key;
                                root.recording = false;
                            }
                        }
                    }

                    FlatButton {
                        text: qsTr("Clear")
                        enabled: root.selectedId !== ""
                        tip: qsTr("Empty the shortcut list; Assign then leaves the command unassigned")
                        onClicked: draft.text = ""
                    }

                }

                C.Label {
                    objectName: "shortcutConflicts"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: root.clashes.length ? Ui.Theme.warning : Ui.Theme.textMuted
                    text: root.clashes.length ? qsTr("Conflicts with: %1").arg(root.clashes.map(function(id) {
                        return Ui.Commands.title(id, id);
                    }).join(", ")) : qsTr("No conflicting assignments.")
                }

                RowLayout {
                    FlatButton {
                        objectName: "assignShortcut"
                        text: qsTr("Assign")
                        enabled: root.selectedId !== "" && root.clashes.length === 0
                        tip: qsTr("Save the keys above for this command; unavailable while they clash with another")
                        onClicked: root.assign(false)
                    }

                    FlatButton {
                        objectName: "reassignShortcut"
                        text: qsTr("Reassign Keys")
                        visible: root.clashes.length > 0
                        tip: qsTr("Save these keys here and remove them from the conflicting commands")
                        onClicked: root.assign(true)
                    }

                    FlatButton {
                        text: qsTr("Reset Action")
                        enabled: root.selectedId !== ""
                        tip: qsTr("Put this command back on its default keys and save straight away")
                        onClicked: {
                            draft.text = root.registry.defaultBindings(root.selectedId).join("\n");
                            root.assign(false);
                        }
                    }

                }

                C.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Ui.Theme.textMuted
                    text: qsTr("Reassign removes these keys from the conflicting actions. Text entry and dialog navigation keep their normal keys.")
                }

                Item {
                    Layout.fillHeight: true
                }

            }

        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Ui.Theme.border
        }

        RowLayout {
            C.Label {
                text: qsTr("Preset")
                color: Ui.Theme.textSecondary
            }

            Ui.ComboField {
                objectName: "shortcutPresets"
                Layout.fillWidth: true
                tip: qsTr("Saved shortcut preset to apply, update or delete")
                model: root.registry.presetNames
                currentIndex: model.indexOf(root.selectedPreset)
                onActivated: function(i) {
                    root.selectedPreset = model[i];
                }
            }

            FlatButton {
                text: qsTr("Apply")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Replace every current key assignment with this preset's")
                onClicked: {
                    root.message = root.registry.applyPreset(root.selectedPreset) ? qsTr("Preset applied.") : root.registry.error;
                    if (root.selectedId)
                        draft.text = root.registry.shortcuts(root.selectedId).join("\n");

                }
            }

            FlatButton {
                text: qsTr("Update")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Overwrite this preset with the current key assignments")
                onClicked: root.message = root.registry.savePreset(root.selectedPreset, true) ? qsTr("Preset updated.") : root.registry.error
            }

            FlatButton {
                text: qsTr("Delete…")
                enabled: root.selectedPreset !== ""
                tip: qsTr("Remove this preset after confirming; the current keys stay in place")
                onClicked: deleteConfirm.open()
            }

        }

        RowLayout {
            Ui.LineEdit {
                id: presetName

                objectName: "shortcutPresetName"
                Layout.fillWidth: true
                hint: qsTr("New shortcut preset name")
                tip: qsTr("Name for a new preset holding the current key assignments")
                maximumLength: 80
            }

            FlatButton {
                objectName: "saveShortcutPreset"
                text: qsTr("Save New Preset")
                enabled: presetName.text.trim() !== ""
                tip: qsTr("Store the current key assignments as a new preset under this name")
                onClicked: {
                    if (root.registry.savePreset(presetName.text)) {
                        root.selectedPreset = presetName.text.trim();
                        root.message = qsTr("Preset saved.");
                    } else {
                        root.message = root.registry.error;
                    }
                }
            }

            FlatButton {
                objectName: "resetAllShortcuts"
                text: qsTr("Reset All…")
                tip: qsTr("Restore the default keys for every command after confirming; presets are kept")
                onClicked: resetConfirm.open()
            }

        }

        C.Label {
            objectName: "shortcutMessage"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.message
            color: Ui.Theme.textMuted
        }

    }

}
