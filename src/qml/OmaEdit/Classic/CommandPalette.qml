import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

Ui.ModalDialog {
    id: root

    property string selectedId: ""
    property string pendingCommand: ""
    readonly property var rows: {
        if (!visible) return [];
        const rev = backend.commands.revision;
        return backend.commands.search(searchField.text);
    }

    function openFor() {
        searchField.text = "";
        open();
        list.currentIndex = 0;
        searchField.forceActiveFocus();
    }

    function run(index) {
        if (index < 0 || index >= rows.length || !rows[index].available)
            return ;

        pendingCommand = rows[index].id;
        close();
    }

    onClosed: {
        const id = pendingCommand;
        pendingCommand = "";
        if (id)
            Qt.callLater(function() {
            backend.commands.trigger(id);
        });

    }
    objectName: "commandPalette"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    height: Math.min(570, parent ? parent.height - 48 : 570)
    modal: true
    title: qsTr("Command Palette")
    standardButtons: C.Dialog.Close

    background: Rectangle {
        color: Ui.Theme.panelBg
        radius: Ui.Theme.rMenu
        border.color: Ui.Theme.border
    }


    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2

        Ui.LineEdit {
            id: searchField

            objectName: "commandSearch"
            Layout.fillWidth: true
            tip: qsTr("Filters every action by name, category or shortcut")
            hint: qsTr("Type an action, category or shortcut…")
            Accessible.name: qsTr("Search commands")
            onTextChanged: list.currentIndex = 0
            Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
            Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
            Keys.onReturnPressed: root.run(list.currentIndex)
            Keys.onEnterPressed: root.run(list.currentIndex)
        }

        C.ScrollView {
            C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                id: list

                objectName: "commandResults"
                model: root.rows
                currentIndex: 0
                highlightMoveDuration: 0
                Keys.onReturnPressed: root.run(currentIndex)
                onCurrentIndexChanged: {
                    if (currentIndex >= 0) {
                        positionViewAtIndex(currentIndex, ListView.Contain);
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
                    height: 52
                    highlighted: ListView.isCurrentItem
                    enabled: modelData.available
                    onClicked: root.run(index)
                    opacity: enabled ? 1 : Ui.Theme.disabledOpacity

                    contentItem: RowLayout {
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                text: modelData.text
                                color: Ui.Theme.textPrimary
                                font.pixelSize: Ui.Theme.fsControl
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Text {
                                text: modelData.category + (modelData.available ? "" : qsTr(" — unavailable in the current state"))
                                color: Ui.Theme.textMuted
                                font.pixelSize: Ui.Theme.fsLabel
                            }

                        }

                        Text {
                            text: modelData.hint
                            color: Ui.Theme.textMuted
                            font.family: Ui.Theme.monoFamily
                            font.pixelSize: Ui.Theme.fsLabel
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

        C.Label {
            text: qsTr("↑ / ↓ to choose · Enter to run · Escape to close")
            color: Ui.Theme.textMuted
        }

    }

}
