import QtQuick
import OmaEdit.Ui

// The browser's top strip: project title, then the ingest actions. Import is
// the one real button; Clone / Sync / Create Proxies are honest disabled
// controls until their pipelines exist.
Rectangle {
    id: root

    property string projectName: ""
    property bool busy: false
    property string playbackMode: "preferred"
    signal playbackRequested(string mode)

    signal operationRequested(string kind)
    signal syncRequested()
    signal tasksRequested()
    signal importRequested()
    signal importFolderRequested()
    signal projectMenuRequested(Item anchor)
    signal moreRequested(Item anchor)

    // Narrow: the placeholder actions go icon-only and the title elides.
    readonly property bool compact: width < 980

    implicitHeight: Theme.hTab + Theme.s2
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    Row {
        id: titleRow
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Icon {
            name: "folder-open"
            size: Theme.szIcon
            color: Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: root.projectName !== "" ? root.projectName : qsTr("Untitled")
            width: Math.max(40, Math.min(implicitWidth, root.width - actions.width - Theme.szIcon - Theme.szIconHit - Theme.s3 * 4))
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsTimecode
            font.weight: Theme.wHeading
            color: Theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }
        IconButton {
            id: projectMenuButton
            iconName: "chevron-down"
            text: qsTr("Project")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.projectMenuRequested(projectMenuButton)
        }
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        // Import: the accent split button — files by default, a folder via
        // the chevron.
        Rectangle {
            id: importButton
            objectName: "importButton"
            width: importRow.implicitWidth + Theme.s2
            height: Theme.hControl + 2
            radius: Theme.rControl
            color: importHover.hovered ? Theme.hovered(Theme.accent) : Theme.accent
            opacity: root.busy ? Theme.disabledOpacity : 1
            anchors.verticalCenter: parent.verticalCenter

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Import Media")

            HoverHandler { id: importHover }

            Row {
                id: importRow
                anchors.centerIn: parent
                spacing: Theme.s1

                Item {
                    width: importLabel.implicitWidth + Theme.szIcon + Theme.s2
                    height: importButton.height
                    Icon {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        name: "download"
                        size: Theme.szIcon
                        color: Theme.accentText
                    }
                    Text {
                        id: importLabel
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Import Media")
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        font.weight: Theme.wHeading
                        color: Theme.accentText
                    }
                    TapHandler {
                        enabled: !root.busy
                        onTapped: root.importRequested()
                    }
                    Tooltip { text: qsTr("Import files"); shortcut: Commands.hintForDefault("Ctrl+I"); visible: importHover.hovered && !importMenu.visible }
                }
                Rectangle { width: Theme.hairline; height: importButton.height - Theme.s2; color: Theme.accentText; opacity: 0.4; anchors.verticalCenter: parent.verticalCenter }
                Item {
                    width: Theme.szIcon + Theme.s1
                    height: importButton.height
                    Icon {
                        anchors.centerIn: parent
                        name: "chevron-down"
                        size: Theme.szIcon
                        color: Theme.accentText
                    }
                    TapHandler {
                        enabled: !root.busy
                        onTapped: importMenu.popup(importButton, 0, importButton.height)
                    }
                }
            }

            ContextMenu {
                id: importMenu
                MenuAction { text: qsTr("Import Files…"); shortcut: "Ctrl+I"; onTriggered: root.importRequested() }
                MenuAction { text: qsTr("Import Folder…"); onTriggered: root.importFolderRequested() }
            }
        }

        ToolButton { iconName: "copy"; text: qsTr("Clone…"); onClicked: root.operationRequested("clone"); showLabel: !root.compact; anchors.verticalCenter: parent.verticalCenter }
        ToolButton { iconName: "refresh-cw"; text: qsTr("Sync…"); onClicked: root.syncRequested(); showLabel: !root.compact; anchors.verticalCenter: parent.verticalCenter }
        ToolButton { iconName: "film"; text: qsTr("Create Proxies…"); onClicked: root.operationRequested("proxy"); showLabel: !root.compact; anchors.verticalCenter: parent.verticalCenter }
        ComboField { tip: qsTr("Whether playback uses originals, proxies, or proxies where present"); width: 120; model: [qsTr("Originals"), qsTr("Proxies"), qsTr("Preferred")]; currentIndex: ["original", "proxy", "preferred"].indexOf(root.playbackMode); onActivated: function (i) { root.playbackRequested(["original", "proxy", "preferred"][i]) }; anchors.verticalCenter: parent.verticalCenter }
        IconButton { iconName: "activity"; text: qsTr("Media Tasks"); onClicked: root.tasksRequested(); anchors.verticalCenter: parent.verticalCenter }
        IconButton {
            id: moreButton
            iconName: "ellipsis"
            text: qsTr("More")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.moreRequested(moreButton)
        }
    }
}
