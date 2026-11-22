import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

// Named alternatives for this clip's grade. Commands carry intent, never a
// stale copy of the grade: the worker snapshots its latest applied values.
Rectangle {
    id: root

    property var versions: []
    property int activeVersion: 0
    property bool hasClip: false
    property bool editable: false
    property string clipKey: ""
    property string clipName: ""
    readonly property bool textActive: namePrompt.visible || deletePrompt.visible
                                        || actions.visible || chooser.popup.visible
    readonly property var names: !hasClip ? [] : versions.length > 0
        ? versions.map(function (v, i) { return v.name || qsTr("Version %1").arg(i + 1) })
        : [qsTr("Version 1")]
    readonly property int current: Math.max(0, Math.min(activeVersion, names.length - 1))
    signal actionRequested(string action, int version, string name)

    implicitHeight: Theme.hControl + Theme.s2
    color: Theme.panelBg
    function taken(name, except) {
        var key = name.trim().toLowerCase()
        return names.some(function (n, i) { return i !== except && n.toLowerCase() === key })
    }
    function nextName() {
        var n = names.length + 1
        while (taken(qsTr("Version %1").arg(n), -1)) ++n
        return qsTr("Version %1").arg(n)
    }
    function openName(action) {
        if (!editable || !hasClip) return
        namePrompt.action = action
        namePrompt.version = current
        nameField.text = action === "add" ? nextName() : names[current]
        namePrompt.open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function stepVersion(delta) {
        var target = current + delta
        if (editable && target >= 0 && target < names.length)
            root.actionRequested("switch", target, "")
    }
    function closePrompts() {
        namePrompt.reject()
        deletePrompt.reject()
        actions.close()
        chooser.popup.close()
    }
    onClipKeyChanged: closePrompts()
    onEditableChanged: if (!editable) closePrompts()
    onVisibleChanged: if (!visible) closePrompts()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        spacing: Theme.s1
        Text {
            text: qsTr("Version")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
        }
        ComboField {
            id: chooser
            objectName: "gradeVersionCombo"
            Layout.fillWidth: true
            Layout.minimumWidth: Theme.szIconHit * 2
            tip: qsTr("Which saved version of this clip's grade is shown and edited")
            model: root.names
            // Changing the names resets ComboBox's index; depend on the
            // names too so a rename keeps the selected version displayed.
            currentIndex: root.names.length > 0 ? root.current : -1
            enabled: root.editable
            Accessible.name: qsTr("Grade version")
            onActivated: function (index) { root.actionRequested("switch", index, "") }
        }
        IconButton {
            objectName: "previousGradeVersion"
            iconName: "chevron-left"
            text: qsTr("Previous grade version")
            commandId: "color.previous-version"; shortcut: "Alt+["
            enabled: root.editable && root.current > 0
            onClicked: Commands.registry ? Commands.trigger(commandId) : root.stepVersion(-1)
        }
        IconButton {
            objectName: "nextGradeVersion"
            iconName: "chevron-right"
            text: qsTr("Next grade version")
            commandId: "color.next-version"; shortcut: "Alt+]"
            enabled: root.editable && root.current + 1 < root.names.length
            onClicked: Commands.registry ? Commands.trigger(commandId) : root.stepVersion(1)
        }
        IconButton {
            objectName: "addGradeVersion"
            iconName: "plus"
            text: qsTr("New grade version")
            commandId: "color.new-version"; shortcut: "Ctrl+Alt+V"
            enabled: root.editable
            onClicked: Commands.registry ? Commands.trigger(commandId) : root.openName("add")
        }
        IconButton {
            objectName: "gradeVersionMenu"
            iconName: "more-horizontal"
            text: qsTr("Grade version actions")
            enabled: root.editable
            onClicked: actions.popup()
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    ContextMenu {
        id: actions
        MenuAction {
            text: qsTr("New Version…")
            iconName: "plus"
            commandId: "color.new-version"; shortcut: "Ctrl+Alt+V"
            onTriggered: Commands.registry ? Commands.trigger(commandId) : root.openName("add")
        }
        MenuAction {
            objectName: "renameGradeVersion"
            text: qsTr("Rename Version…")
            iconName: "pencil"
            onTriggered: root.openName("rename")
        }
        MenuAction {
            objectName: "deleteGradeVersion"
            text: qsTr("Delete Version…")
            iconName: "trash-2"
            enabled: root.names.length > 1
            onTriggered: {
                deletePrompt.version = root.current
                deletePrompt.versionName = root.names[root.current]
                deletePrompt.open()
            }
        }
    }
    component VersionDialog: FormDialog {
        footer: DialogButtons {
            delegate: ToolButton { tip: C.DialogButtonBox.buttonRole === C.DialogButtonBox.AcceptRole ? qsTr("Confirm this change to the grade versions") : qsTr("Close and leave the grade versions unchanged") }
        }
    }
    VersionDialog {
        id: namePrompt
        objectName: "gradeVersionNameDialog"
        parent: C.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(Theme.wNameDialog, parent ? parent.width - Theme.s5 * 2 : Theme.wNameDialog)
        modal: true
        focus: true
        title: action === "add" ? qsTr("New Grade Version") : qsTr("Rename Grade Version")
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        property string action: "add"
        property int version: -1
        readonly property bool nameOk: nameField.text.trim() !== ""
            && !root.taken(nameField.text, action === "rename" ? version : -1)
        onAccepted: if (nameOk && root.editable)
            root.actionRequested(action, version, nameField.text.trim())
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Text {
                Layout.fillWidth: true
                text: namePrompt.action === "add"
                    ? qsTr("Start with the current look. Changes stay with each version.")
                    : qsTr("Name this version of the grade for %1.").arg(root.clipName)
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
            }
            C.TextField {
                id: nameField
                objectName: "gradeVersionNameField"
                Layout.fillWidth: true
                hoverEnabled: true
                C.ToolTip.text: qsTr("Name this version is listed under; it must not already exist")
                C.ToolTip.visible: hovered && !activeFocus
                C.ToolTip.delay: 450
                // The dialog supplies the label; suppress Material's
                // floating placeholder inside this compact themed field.
                placeholderText: ""
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentText
                implicitHeight: Theme.hControl + Theme.s2
                padding: Theme.s2
                topInset: 0
                bottomInset: 0
                background: Rectangle {
                    color: Theme.controlBg
                    radius: Theme.rControl
                    border.width: Theme.hairline
                    border.color: nameField.activeFocus ? Theme.accent : Theme.border
                }
                Accessible.name: qsTr("Grade version name")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                selectByMouse: true
                Keys.onReturnPressed: if (namePrompt.nameOk) namePrompt.accept()
            }
            Text {
                Layout.fillWidth: true
                visible: nameField.text.trim() !== "" && !namePrompt.nameOk
                text: qsTr("A version with that name already exists.")
                wrapMode: Text.Wrap
                color: Theme.warning
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
            }
        }
        Component.onCompleted: standardButton(C.Dialog.Ok).enabled = Qt.binding(function () { return namePrompt.nameOk })
    }
    VersionDialog {
        id: deletePrompt
        objectName: "deleteGradeVersionDialog"
        parent: C.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(Theme.wNameDialog, parent ? parent.width - Theme.s5 * 2 : Theme.wNameDialog)
        modal: true
        focus: true
        title: qsTr("Delete Grade Version")
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        property int version: -1
        property string versionName: ""
        onAccepted: if (root.editable) root.actionRequested("remove", version, "")
        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: qsTr("Delete “%1” from “%2”? You can undo this change.").arg(deletePrompt.versionName).arg(root.clipName)
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsBase
        }
        Component.onCompleted: standardButton(C.Dialog.Ok).text = qsTr("Delete Version")
    }
}
