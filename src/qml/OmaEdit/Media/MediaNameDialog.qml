import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
ModalDialog {
    id: root
    width: Math.min(Theme.wNameDialog, parent ? parent.width - Theme.s5 * 2 : Theme.wNameDialog)
    property alias value: nameField.text
    onOpened: { nameField.forceActiveFocus(); nameField.selectAll() }
    LineEdit { id: nameField; objectName: "mediaNameField"; tip: qsTr("Name this item is given in the project"); width: parent.width; Accessible.name: root.title; Keys.onReturnPressed: if (text.trim()) root.accept() }
    Component.onCompleted: standardButton(C.Dialog.Ok).enabled = Qt.binding(function () { return nameField.text.trim() !== "" })
}
