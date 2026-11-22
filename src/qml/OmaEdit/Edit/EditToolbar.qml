pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Editing tools and selected-clip controls live above the ruler.
// The owner supplies menus and handles edits; the toolbar only presents them.
Rectangle {
    id: root

    // The active tool: select | trackselect | trim | blade | slip | slide | hand | zoom.
    property string tool: "select"
    property bool snapping: true
    property bool linked: true // linked selection (video + detached audio)
    // The beats of mapped music shown as lines down the tracks; there are
    // some to show only with mapped music on the timeline.
    property bool beatMap: true
    property bool canBeatMap: false
    // Timeline zoom 0…1 (0 = fit).
    property real zoom: 0
    property bool canZoom: false
    property bool indexVisible: false
    property bool effectsVisible: false
    property bool canExport: false
    property bool exporting: false
    property int exportPercent: 0
    property var actionMenus: null
    readonly property bool stacked: width < editingRow.implicitWidth + viewRow.implicitWidth + Theme.s4 * 2
    readonly property real rowHeight: Math.max(Theme.hEditToolbar, Theme.szIconHit + Theme.s2)

    signal toolSelected(string tool)
    signal zoomEdited(real level)
    signal fitRequested()
    signal snappingToggled(bool on)
    signal linkedToggled(bool on)
    signal beatMapToggled(bool on)
    signal speedRequested()
    signal markerRequested()
    signal titleRequested()
    signal indexToggled(bool on)
    signal effectsToggled(bool on)
    signal exportRequested()

    readonly property var tools: [
        { id: "select", icon: "mouse-pointer-2", name: qsTr("Select"), key: "A", ready: true },
        { id: "trackselect", icon: "track-select", name: qsTr("Track Select — click a clip to take it and everything after it on its track (Ctrl: every track, Alt: everything before)"), key: "", ready: true },
        { id: "trim",   icon: "move-horizontal", name: qsTr("Trim — edge drags ripple"), key: "T", ready: true },
        { id: "blade",  icon: "scissors",        name: qsTr("Blade — click a clip to cut it"), key: "B", ready: true },
        { id: "slip",   icon: "arrow-left-right",name: qsTr("Slip — drag a clip's content"), key: "Y", ready: true },
        { id: "slide",  icon: "move",            name: qsTr("Slide — drag a clip between its neighbours"), key: "U", ready: true },
        { id: "hand",   icon: "hand",            name: qsTr("Hand — drag the view"), key: "H", ready: true },
        { id: "zoom",   icon: "zoom-in",         name: qsTr("Zoom — click in, Alt-click out"), key: "Z", ready: true }
    ]


    readonly property var toolLabels: ({
        select: qsTr("Select"), trackselect: qsTr("Track Select"), trim: qsTr("Trim"),
        blade: qsTr("Blade"), slip: qsTr("Slip"), slide: qsTr("Slide"),
        hand: qsTr("Hand"), zoom: qsTr("Zoom")
    })
    implicitHeight: rowHeight * (stacked ? 2 : 1)
    color: Theme.panelBg
    objectName: "timelineToolbar"

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: Theme.hairline; color: Theme.border
    }

    ContextMenu {
        id: toolsMenu
        objectName: "timelineToolsMenu"
        Instantiator {
            model: root.tools
            delegate: MenuAction {
                required property var modelData
                text: root.toolLabels[modelData.id]
                shortcut: modelData.key
                objectName: "tool:" + modelData.id
                checkable: true
                checked: root.tool === modelData.id
                onTriggered: {
                    root.toolSelected(modelData.id)
                    checked = Qt.binding(function() { return root.tool === modelData.id })
                }
            }
            onObjectAdded: function(index, object) { toolsMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { toolsMenu.removeItem(object) }
        }
    }

    Row {
        id: editingRow
        objectName: "timelineEditingControls"
        anchors.left: parent.left; anchors.leftMargin: Theme.s2
        y: (root.rowHeight - height) / 2
        spacing: Theme.s1

        ToolButton {
            id: toolPicker
            objectName: "timelineToolPicker"
            text: (root.toolLabels[root.tool] || qsTr("Select")) + " ▾"
            tip: qsTr("Editing tool")
            showLabel: true
            onClicked: toolsMenu.popup(toolPicker, 0, height)
        }
        IconButton {
            iconName: "magnet"; text: qsTr("Snapping"); shortcut: "N"
            checkable: true; checked: root.snapping
            onClicked: root.snappingToggled(!root.snapping)
        }
        IconButton {
            iconName: "link"; text: qsTr("Linked selection — a clip and its detached audio select together (Alt-click picks one)"); shortcut: "Ctrl+Shift+L"
            objectName: "linkedSelectionButton"
            checkable: true; checked: root.linked
            onClicked: root.linkedToggled(!root.linked)
        }
        IconButton {
            iconName: "music"; text: qsTr("Show beat map — the beats and bars of mapped music as lines down the tracks; drags snap to them")
            objectName: "beatMapButton"
            commandId: "view.beat-map"
            checkable: true; checked: root.beatMap
            enabled: root.canBeatMap
            onClicked: root.beatMapToggled(!root.beatMap)
        }
        Rectangle { width: Theme.hairline; height: Theme.szIconHit - Theme.s1 * 2; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }
        ToolButton {
            id: trimButton
            objectName: "timelineTrimButton"
            text: qsTr("Trim") + " ▾"; tip: qsTr("Trim and arrange selected clips")
            enabled: root.actionMenus !== null && root.actionMenus.canTrim
            onClicked: root.actionMenus.trimMenu.popup(trimButton, 0, height)
        }
        ToolButton {
            objectName: "timelineSpeedButton"
            text: qsTr("Speed"); tip: qsTr("Change clip speed and duration")
            commandId: "edit.speed-duration"
            enabled: root.actionMenus !== null && root.actionMenus.canSpeed
                && (!Commands.command(commandId) || Commands.command(commandId).available)
            onClicked: root.speedRequested()
        }
        ToolButton {
            id: videoButton
            objectName: "timelineVideoButton"
            text: qsTr("Video") + " ▾"; tip: qsTr("Picture controls for the selected clip")
            enabled: root.actionMenus !== null && root.actionMenus.canVideo
            onClicked: root.actionMenus.videoMenu.popup(videoButton, 0, height)
        }
        ToolButton {
            id: audioButton
            objectName: "timelineAudioButton"
            text: qsTr("Audio") + " ▾"; tip: qsTr("Sound controls for the selected clip")
            enabled: root.actionMenus !== null && root.actionMenus.canAudio
            onClicked: root.actionMenus.audioMenu.popup(audioButton, 0, height)
        }
        Rectangle { width: Theme.hairline; height: Theme.szIconHit - Theme.s1 * 2; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }
        IconButton {
            id: markersButton
            objectName: "timelineMarkersButton"
            iconName: "flag"; text: qsTr("Markers"); shortcut: "M"
            enabled: root.actionMenus !== null && root.actionMenus.canMarkers
            onClicked: root.actionMenus.markersMenu.popup(markersButton, 0, height)
        }
        IconButton {
            id: labelsButton
            objectName: "timelineLabelButton"
            iconName: "tag"; text: qsTr("Clip label")
            enabled: root.actionMenus !== null && root.actionMenus.canLabel
            onClicked: root.actionMenus.labelMenu.popup(labelsButton, 0, height)
        }
        IconButton {
            iconName: "type"; text: qsTr("Add title"); shortcut: "Alt+T"
            onClicked: root.titleRequested()
        }
    }

    Row {
        id: viewRow
        objectName: "timelineViewControls"
        anchors.right: parent.right; anchors.rightMargin: Theme.s2
        y: (root.stacked ? root.rowHeight : 0) + (root.rowHeight - height) / 2
        spacing: Theme.s1

        IconButton {
            iconName: "zoom-out"; text: qsTr("Zoom out"); shortcut: "Ctrl+-"
            enabled: root.canZoom && root.zoom > 0
            onClicked: root.zoomEdited(Math.max(0, root.zoom - 0.1))
        }
        SliderField {
            objectName: "zoomSlider"
            tip: qsTr("Timeline zoom, from the whole sequence out to single frames")
            width: 96; anchors.verticalCenter: parent.verticalCenter
            from: 0; to: 1; stepSize: 0.01; value: root.zoom; showReadout: false
            enabled: root.canZoom
            onEdited: function(level) { root.zoomEdited(level) }
        }
        IconButton {
            iconName: "zoom-in"; text: qsTr("Zoom in"); shortcut: "Ctrl+="
            enabled: root.canZoom && root.zoom < 1
            onClicked: root.zoomEdited(Math.min(1, root.zoom + 0.1))
        }
        IconButton {
            iconName: "minimize"; text: qsTr("Fit timeline"); shortcut: "Shift+Z"
            enabled: root.canZoom && root.zoom > 0
            onClicked: root.fitRequested()
        }
        Rectangle { width: Theme.hairline; height: Theme.szIconHit - Theme.s1 * 2; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }
        IconButton {
            iconName: "list"; text: qsTr("Timeline list"); shortcut: "Ctrl+Shift+2"
            checkable: true; checked: root.indexVisible
            onClicked: root.indexToggled(!root.indexVisible)
        }
        IconButton {
            iconName: "wand-sparkles"; text: qsTr("Effects"); shortcut: "Ctrl+5"
            checkable: true; checked: root.effectsVisible
            onClicked: root.effectsToggled(!root.effectsVisible)
        }
        ToolButton {
            iconName: "share-2"
            text: root.exporting ? qsTr("Exporting %1%").arg(root.exportPercent) : qsTr("Export Movie")
            shortcut: root.exporting ? "" : "Ctrl+E"
            showLabel: true
            enabled: root.canExport && !root.exporting
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.exportRequested()
        }
    }
}
