import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import omaedit

// The live Edit workspace; the existing editing surfaces retain their state
// as the shared panel hosts resize, maximise or move them to another window.
Item {
    id: root

    property var layoutStore: null
    property alias layoutController: layout
    property alias inspectorTab: inspectorPane.currentTab
    readonly property bool titleTextActive: viewerPane.titleTextActive
    function enterTimecode(first) { viewerPane.enterTimecode(first) }
    readonly property bool mediaSearchActive: browserPane.searchActive
    readonly property bool transcriptSearchActive: inspectorPane.visible && inspectorPane.transcriptSearchActive
    readonly property bool mediaDragActive: browserPane.dragActive
    property alias timelineScroll: timelinePane.timelineScroll
    property alias indexSearch: timelinePane.indexSearch
    property alias browserTab: browserPane.currentTab
    property alias ruler: timelinePane.ruler
    property alias storyline: timelinePane.storyline
    readonly property bool sequenceRenameOpen: sequenceBar.renameOpen

    function openTimelineMenu(index) {
        sequenceBar.openMenu(index);
    }

    function editPicture(patch) {
        inspectorPane.picturePatch(patch);
    }

    function pictureValue(key, fallback) {
        return inspectorPane.animationValue(key, fallback);
    }

    readonly property bool inspectorShown: !inspectorPanel.hidden

    function toggleInspector() {
        inspectorPanel.setShown(inspectorPanel.hidden)
    }

    function showInspector(tab) {
        layout.maximized = ""
        inspectorPanel.setShown(true)
        inspectorPane.showControls(tab)
        inspectorPane.forceActiveFocus()
    }

    anchors.fill: parent

    Ui.WorkspaceLayout {
        id: layout

        host: root
        store: root.layoutStore
        workspaceKey: "Edit"
    }

    Ui.WorkspaceSplit {
        id: rows

        anchors.fill: parent
        controller: layout
        splitId: "rows"
        orientation: Qt.Vertical

        Ui.WorkspaceSplit {
            id: columns

            controller: layout
            splitId: "columns"
            orientation: Qt.Horizontal
            C.SplitView.preferredHeight: root.height * 0.54
            C.SplitView.minimumHeight: 180
            visible: browserPanel.docked || viewerPanel.docked || inspectorPanel.docked

            Ui.DockPanel {
                id: browserPanel

                controller: layout
                panelId: "browser"
                title: qsTr("Project Media")
                // Opens at 28% of the page, 600 at the most; dragged, it is as
                // wide as the editor makes it (the viewer's least width stops it).
                openWidest: 600
                C.SplitView.preferredWidth: Math.min(openWidest, root.width * 0.28)
                C.SplitView.minimumWidth: 220

                BrowserPane {
                    id: browserPane

                    anchors.fill: parent
                }

            }

            Ui.DockPanel {
                id: viewerPanel

                controller: layout
                panelId: "viewer"
                title: qsTr("Viewer")
                detachable: true
                viewer: true
                C.SplitView.fillWidth: true
                // Never narrower than its transport row needs, nor asking
                // for more than the page has beside the other two at their least.
                C.SplitView.minimumWidth: Math.max(280, Math.min(viewerPane.leastWidth,
                    root.width - (browserPanel.docked ? 220 + Ui.Theme.wSplitter : 0) - (inspectorPanel.docked ? 280 + Ui.Theme.wSplitter : 0)))

                ViewerPane {
                    id: viewerPane

                    anchors.fill: parent
                }

            }

            Ui.DockPanel {
                id: inspectorPanel

                controller: layout
                panelId: "inspector"
                title: qsTr("Inspector")
                autoHidden: root.width < 1180
                openWidest: 500
                C.SplitView.preferredWidth: Math.min(openWidest, root.width * 0.22)
                C.SplitView.minimumWidth: 280

                InspectorPane {
                    id: inspectorPane

                    anchors.fill: parent
                }

            }

        }

        Ui.DockPanel {
            id: timelinePanel

            controller: layout
            panelId: "timeline"
            title: qsTr("Timeline")
            C.SplitView.fillHeight: true
            C.SplitView.minimumHeight: 160

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                SequenceBar {
                    id: sequenceBar
                }

                EditBar {
                    id: editBar
                }

                PancakeStrip {
                    id: pancakeStrip
                }

                TimelinePane {
                    id: timelinePane

                    Layout.fillHeight: true
                }

            }

        }

    }

}
