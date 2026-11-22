import QtQuick
import QtQuick.Window
import OmaEdit.Ui

// The 44 px Oma app bar: identity and menu at the left, workspace navigation
// centred, document actions and window controls at the right. The project
// title uses the remaining space without pushing into the navigation.
Rectangle {
    id: root
    objectName: "appTitleBar"
    property string keyboardRegion: qsTr("Application")

    // The window being driven. Kept as a property rather than reached for via
    // Window.window so this can be exercised in isolation by the harness.
    property Window target: null

    property string projectName: ""
    property bool dirty: false
    property alias workspace: nav.current
    property bool canUndo: false
    property bool canRedo: false
    property bool settingsAvailable: true

    signal saveRequested()
    signal undoRequested()
    signal redoRequested()
    signal settingsRequested()
    signal menuRequested(Item anchor)
    // The menu bar. A window too narrow for it has the ☰ button instead.
    property alias menuContent: menuSlot.data
    readonly property real menuBarWidth: menuSlot.children.length > 0 ? menuSlot.children[0].implicitWidth : 0
    readonly property bool menusFit: menuBarWidth > 0
        && width >= identity.width + menuBarWidth + nav.width + actions.width + windowControls.width + Theme.s5 * 2
    signal workspaceActivated(string name)

    implicitHeight: Theme.hTitleBar
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    // Window gestures cover the title and gaps, keeping the controls clear.
    component WindowDragArea: Item {
        TapHandler {
            onDoubleTapped: if (root.target)
                root.target.visibility = root.target.visibility === Window.Maximized
                                         ? Window.Windowed : Window.Maximized
        }
        DragHandler {
            target: null
            onActiveChanged: if (active && root.target) root.target.startSystemMove()
        }
    }
    WindowDragArea {
        anchors.left: parent.left
        anchors.right: root.menusFit ? menuSlot.left : menuButton.left
        height: parent.height
    }
    WindowDragArea {
        anchors.left: root.menusFit ? menuSlot.right : menuButton.right
        anchors.right: nav.left
        height: parent.height
    }
    WindowDragArea {
        anchors.left: nav.right
        anchors.right: actions.left
        height: parent.height
    }
    WindowDragArea {
        anchors.left: actions.right
        anchors.right: windowControls.left
        height: parent.height
    }

    // ── Identity ────────────────────────────────────────────────────────────
    Row {
        id: identity
        objectName: "titleIdentity"
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Icon {
            name: "circle-play"
            size: 20
            color: Theme.accent
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: Theme.appName
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsTitle
            font.weight: Theme.wHeading
            color: Theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    IconButton {
        id: menuButton
        objectName: "titleMenu"
        anchors.left: identity.right
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        iconName: "menu"
        text: qsTr("Main menu")
        visible: !root.menusFit
        onClicked: root.menuRequested(menuButton)
    }

    Item {
        id: menuSlot
        objectName: "titleMenuBar"
        anchors.left: identity.right
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        width: root.menuBarWidth
        height: Theme.hControl
        visible: root.menusFit
    }

    Item {
        id: documentTitle
        objectName: "titleProject"
        anchors.left: nav.right
        anchors.leftMargin: Theme.s4
        height: parent.height
        width: Math.min(260, Math.max(0, actions.x - x - Theme.s4))
        visible: root.projectName !== "" && width >= Theme.s5
        Accessible.role: Accessible.StaticText
        Accessible.name: root.dirty ? qsTr("%1, unsaved changes").arg(root.projectName)
                                    : root.projectName

        Text {
            id: documentLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, parent.width - (root.dirty ? dirtyDot.width + Theme.s2 : 0)))
            text: root.projectName
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: root.dirty ? Theme.textPrimary : Theme.textSecondary
            elide: Text.ElideMiddle
        }
        Text {
            id: dirtyDot
            anchors.left: documentLabel.right
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            visible: root.dirty
            text: "\u2022"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: Theme.accent
        }
        HoverHandler { id: documentHover }
        Tooltip {
            text: documentTitle.Accessible.name
            visible: documentTitle.visible && documentHover.hovered
        }
    }

    // ── Workspace navigation ────────────────────────────────────────────────
    WorkspaceBar {
        id: nav
        objectName: "titleWorkspaces"
        x: Math.max((root.menusFit ? menuSlot.x + menuSlot.width : menuButton.x + menuButton.width) + Theme.s3,
                    Math.min((root.width - width) / 2, actions.x - Theme.s4 - width))
        height: parent.height
        onActivated: function (name) { root.workspaceActivated(name) }
    }

    // ── Document actions ────────────────────────────────────────────────────
    Row {
        id: actions
        objectName: "titleActions"
        anchors.right: windowControls.left
        anchors.rightMargin: Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

        IconButton {
            objectName: "titleSave"
            commandId: "project.save"
            iconName: "save"
            text: root.dirty ? qsTr("Save (unsaved changes)") : qsTr("Save")
            shortcut: "Ctrl+S"
            iconColor: root.dirty ? Theme.accent : Theme.textSecondary
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.saveRequested()
        }
        IconButton {
            objectName: "titleUndo"; commandId: "edit.undo"; iconName: "undo-2"; text: Commands.title("edit.undo", qsTr("Undo")); shortcut: "Ctrl+Z"
            enabled: root.canUndo
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.undoRequested()
        }
        IconButton {
            objectName: "titleRedo"; commandId: "edit.redo"; iconName: "redo-2"; text: Commands.title("edit.redo", qsTr("Redo")); shortcut: "Ctrl+Shift+Z"
            enabled: root.canRedo
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.redoRequested()
        }
        IconButton {
            objectName: "titlePreferences"
            iconName: "settings"; text: qsTr("Settings")
            enabled: root.settingsAvailable
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.settingsRequested()
        }
    }

    // ── Window controls ─────────────────────────────────────────────────────
    Row {
        id: windowControls
        objectName: "titleWindowControls"
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
        IconButton {
            iconName: "minus"; text: qsTr("Minimise")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: if (root.target) root.target.showMinimized()
        }
        IconButton {
            objectName: "titleMaximize"
            readonly property bool maximized: root.target && root.target.visibility === Window.Maximized
            iconName: maximized ? "minimize" : "square"
            text: maximized ? qsTr("Restore window") : qsTr("Maximise")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: if (root.target)
                root.target.visibility = root.target.visibility === Window.Maximized
                                         ? Window.Windowed : Window.Maximized
        }
        IconButton {
            iconName: "x"; text: qsTr("Close")
            anchors.verticalCenter: parent.verticalCenter
            // The one control that must read as destructive on hover.
            iconColor: hovered ? Theme.danger : Theme.textSecondary
            onClicked: if (root.target) root.target.close()
        }
    }
}
