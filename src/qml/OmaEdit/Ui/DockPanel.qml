import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root

    default property alias panelData: body.data
    required property var controller
    required property string panelId
    required property string title
    // Muted text after the title: what the panel is showing (a clip, a preset,
    // a node path), where a panel's own header used to say it.
    property string detail: ""
    // Where a panel's header actions go (see DockBarActions). `available` is
    // the width the bar can spare them, and does not depend on what is in it.
    readonly property alias barSlot: actionSlot
    property bool detachable: false
    property bool viewer: false
    property bool autoHidden: false
    // The widest (or tallest) the panel opens, and goes back to when the
    // layout is reset. Dragged, it goes as far as its neighbours let it.
    property real openWidest: Infinity
    property var explicitVisibility: null
    // False for a panel this workspace does not offer at all: it never shows
    // and the layout dialog does not list it.
    property bool offered: true
    readonly property bool hidden: !offered || (explicitVisibility === null ? autoHidden : !explicitVisibility)
    property bool detached: false
    property bool fullscreen: false
    property var geometry: ({
    })
    property bool placing: false
    readonly property bool maximized: controller.maximized === panelId
    readonly property bool docked: !hidden && !detached
    readonly property var window: floating
    property string windowObjectName: "floating:" + controller.workspaceKey + ":" + panelId
    property Item surface
    property Window floating

    function capture() {
        return {
            "visibility": explicitVisibility,
            "detached": detached,
            "geometry": geometry
        };
    }

    function restore(state) {
        placing = true;
        fullscreen = false;
        geometry = state.geometry || {
        };
        explicitVisibility = state.visibility === undefined ? null : state.visibility;
        detached = detachable && !!state.detached;
        if (detached)
            Qt.callLater(prepareWindow);

    }

    function setShown(on) {
        if (on && detached)
            placing = true;
        if (!on) {
            exitFullscreen();
            if (maximized)
                controller.maximized = "";

        }
        explicitVisibility = on;
        controller.changed();
    }

    function toggleMaximize() {
        explicitVisibility = true;
        if (detached)
            setDetached(false);

        controller.maximized = maximized ? "" : panelId;
        surface.forceActiveFocus();
        controller.changed();
    }

    function setDetached(on) {
        if (!detachable)
            return ;

        if (!on && floating.visible && !fullscreen && controller.store)
            geometry = controller.store.windowGeometry(floating);

        if (on && maximized)
            controller.maximized = "";

        if (on)
            placing = true;
        explicitVisibility = true;
        detached = on;
        if (on)
            Qt.callLater(prepareWindow);

        controller.changed();
    }

    function toggleFullscreen() {
        if (!viewer)
            return ;

        explicitVisibility = true;
        fullscreen = !fullscreen;
        Qt.callLater(prepareWindow);
    }

    function exitFullscreen() {
        if (fullscreen) {
            placing = true;
            fullscreen = false;
            if (detached)
                Qt.callLater(prepareWindow);

            controller.host.forceActiveFocus();
        }
    }

    function prepareWindow() {
        if (!controller.host.visible || hidden || (!detached && !fullscreen))
            return ;

        placing = true;
        if (fullscreen) {
            floating.showFullScreen();
        } else {
            floating.showNormal();
            if (controller.store) {
                controller.store.placeWindow(floating, geometry);
                geometry = controller.store.windowGeometry(floating);
            }
        }
        floating.requestActivate();
        surface.forceActiveFocus();
        Qt.callLater(function() {
            placing = false;
        });
    }

    function rememberGeometry() {
        if (!placing && !fullscreen && detached && floating.visible && controller.ready && !controller.applying && controller.store) {
            geometry = controller.store.windowGeometry(floating);
            controller.changed();
        }
    }

    function moveToScreen(name) {
        if (!detached)
            setDetached(true);

        Qt.callLater(function() {
            if (controller.store) {
                placing = true;
                controller.store.placeWindow(floating, geometry, name);
                geometry = controller.store.windowGeometry(floating);
                placing = false;
                controller.changed();
            }
        });
    }

    objectName: "panel:" + controller.workspaceKey + ":" + panelId
    visible: docked
    clip: true
    onHiddenChanged: controller.changed()
    onDetachedChanged: controller.changed()
    onFullscreenChanged: controller.revision++
    Component.onCompleted: controller.addPanel(root)

    Connections {
        function onScreensChanged() {
            if (root.detached)
                Qt.callLater(root.prepareWindow);

        }

        target: root.controller.store
    }

    surface: FocusScope {
        property string keyboardRegion: root.title
        Accessible.role: Accessible.Pane
        Accessible.name: root.title
        objectName: "panelSurface:" + root.controller.workspaceKey + ":" + root.panelId
        parent: root.detached || root.fullscreen ? floating.contentItem : root.maximized ? root.controller.maximumArea : root
        anchors.fill: parent
        visible: !root.hidden

        Rectangle {
            anchors.fill: parent
            color: Theme.panelBg
        }

        Rectangle {
            id: bar

            anchors.top: parent.top
            width: parent.width
            height: Theme.hControl + Theme.s1
            color: Theme.panelRaised

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s2
                spacing: Theme.s1

                Text {
                    id: barTitle
                    Layout.maximumWidth: implicitWidth
                    Layout.minimumWidth: Math.min(implicitWidth, 56)
                    text: root.title
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    elide: Text.ElideRight
                }

                Text {
                    Layout.fillWidth: true
                    text: root.detail
                    textFormat: Text.PlainText // a clip or preset name
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    elide: Text.ElideMiddle
                }

                Item {
                    id: actionSlot
                    objectName: "dockBarActions:" + root.panelId
                    readonly property real available: Math.max(0, bar.width - barTitle.implicitWidth - 4 * (Theme.hControl + Theme.s1) - Theme.s2 * 2)
                    Layout.preferredWidth: childrenRect.width
                    Layout.fillHeight: true
                }

                Rectangle {
                    // The panel's own actions end here; what follows manages the panel.
                    visible: actionSlot.childrenRect.width > 0
                    Layout.preferredWidth: Theme.hairline
                    Layout.preferredHeight: Theme.szIcon
                    Layout.leftMargin: Theme.s1
                    Layout.rightMargin: Theme.s1
                    color: Theme.border
                }

                IconButton {
                    visible: root.viewer
                    iconName: root.fullscreen ? "minimize" : "maximize"
                    text: root.fullscreen ? qsTr("Exit full screen") : qsTr("Full screen viewer")
                    onClicked: root.toggleFullscreen()
                }

                IconButton {
                    visible: root.detachable && !root.fullscreen
                    iconName: root.detached ? "panel-left" : "monitor"
                    text: root.detached ? qsTr("Dock panel") : qsTr("Detach panel")
                    onClicked: root.setDetached(!root.detached)
                }

                IconButton {
                    visible: !root.detached && !root.fullscreen
                    iconName: root.maximized ? "minimize" : "maximize"
                    text: root.maximized ? qsTr("Restore panel") : qsTr("Maximise panel")
                    onClicked: root.toggleMaximize()
                }

                IconButton {
                    visible: !root.fullscreen
                    iconName: "x"
                    text: qsTr("Hide panel")
                    onClicked: root.setShown(false)
                }

            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: {
                    if (!root.detached && !root.fullscreen)
                        root.toggleMaximize();

                }
            }

        }

        Item {
            id: body

            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            clip: true
        }

    }

    floating: Window {
        readonly property bool typing: Commands.popupOpen || floatingNavigation.textFocused
        KeyboardNavigation { id: floatingNavigation; window: floating }
        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.next-panel") : [] }
            enabled: floating.visible && !Commands.popupOpen
            onActivated: floatingNavigation.cyclePanel(false)
        }
        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.previous-panel") : [] }
            enabled: floating.visible && !Commands.popupOpen
            onActivated: floatingNavigation.cyclePanel(true)
        }

        objectName: root.windowObjectName
        title: qsTr("OmaEdit — %1").arg(root.title)
        width: 800
        height: 500
        minimumWidth: 260
        minimumHeight: 180
        color: Theme.windowBg
        visible: !root.controller.suspended && root.controller.host.visible && !root.hidden && (root.detached || root.fullscreen)
        transientParent: null
        onVisibleChanged: {
            if (visible)
                Qt.callLater(root.prepareWindow);

        }
        onXChanged: root.rememberGeometry()
        onYChanged: root.rememberGeometry()
        onWidthChanged: root.rememberGeometry()
        onHeightChanged: root.rememberGeometry()
        onClosing: function(event) {
            event.accepted = false;
            if (root.fullscreen)
                root.exitFullscreen();
            else
                root.setDetached(false);
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.viewer-fullscreen") : [] }
            enabled: floating.visible && !floating.typing && root.viewer
            onActivated: Commands.trigger("view.viewer-fullscreen")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.fullscreen") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("view.fullscreen")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.layout") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: { root.controller.host.Window.window.requestActivate(); Commands.trigger("view.layout") }
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("view.escape") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("view.escape")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.play") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.play")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.reverse") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.reverse")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.stop") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.stop")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.forward") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.forward")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.previous-frame") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.previous-frame")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("transport.next-frame") : [] }
            enabled: floating.visible && !floating.typing
            onActivated: Commands.trigger("transport.next-frame")
        }

        Shortcut {
            sequences: { const rev = Commands.registry ? Commands.registry.bindingRevision : 0; return Commands.registry ? Commands.registry.dispatchSequences("project.save") : [] }
            enabled: floating.visible
            onActivated: Commands.trigger("project.save")
        }

    }

}
