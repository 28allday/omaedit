pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The gallery: All / Favorites / Recents, albums (groups), a search, a
// grid of stills sized by the zoom slider, and a context menu — apply the
// grade, rename, move to an album, delete.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property var stills: []              // StillsGallery.items
    property var albums: []
    property var timecoder: null
    property string selectedId: ""
    signal stillActivated(string id)
    signal applyRequested(string id, bool toAll)
    signal assetRequested(string action,string id)
    signal grabRequested()
    signal renameRequested(string id, string name)
    signal albumRequested(string id, string album)
    signal favoriteEdited(string id, bool on)
    signal removeRequested(string id)
    signal wipeRequested(string id)   // compare against this still in the viewer

    readonly property bool textActive: search.active || renamePrompt.visible || albumPrompt.visible

    property string filter: "all"       // all | favorites | recents
    property string album: ""
    property real zoom: 1.0
    property int columns: zoom < 0.7 ? 3 : zoom > 1.3 ? 1 : 2

    color: Theme.panelBg

    function matches(s) {
        if (filter === "favorites" && s.favorite !== true) return false
        if (album !== "" && s.album !== album) return false
        var q = search.text.toLowerCase()
        if (q !== "" && (String(s.name) + " " + String(s.album) + " " + String(s.clipName)).toLowerCase().indexOf(q) < 0) return false
        return true
    }
    readonly property var filtered: {
        var out = []
        var list = stills
        if (filter === "recents") list = stills.slice(0, 24)
        for (var i = 0; i < list.length; ++i) if (matches(list[i])) out.push(list[i])
        return out
    }
    function countFor(f, a) {
        var n = 0
        for (var i = 0; i < stills.length; ++i) {
            if (f === "favorites" && stills[i].favorite !== true) continue
            if (a !== undefined && a !== "" && stills[i].album !== a) continue
            ++n
        }
        return f === "recents" ? Math.min(24, n) : n
    }
    function selectedStill() { for (var i = 0; i < stills.length; ++i) if (stills[i].id === selectedId) return stills[i]; return null }

    Column {
        anchors.fill: parent
        Item {
            id: heading
            width: parent.width
            height: galleryActions.inBar ? 0 : Theme.hTab
            visible: !galleryActions.inBar
            Text { visible: root.dockBar === null; anchors.left: parent.left; anchors.leftMargin: Theme.s3; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Gallery"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading; color: Theme.textPrimary }
            DockBarActions {
                id: galleryActions
                dockBar: root.dockBar
                fallback: heading
            IconButton { objectName: "importGradeStill"; anchors.verticalCenter: parent.verticalCenter; iconName: "folder-open"; text: qsTr("Import still and grade…"); onClicked: root.assetRequested("importStill", "") }
            IconButton { objectName: "grabStill"; anchors.verticalCenter: parent.verticalCenter; iconName: "camera"; Accessible.name: qsTr("Grab still"); onClicked: root.grabRequested() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        }
        Item {
            id: searchRow
            width: parent.width
            height: Theme.hControl + Theme.s2 * 2
            SearchField { id: search; objectName: "gallerySearch"; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.s2; anchors.verticalCenter: parent.verticalCenter; placeholder: qsTr("Search gallery…"); tip: qsTr("Filter the gallery by still name") }
        }
        Flickable {
            id: categories
            objectName: "galleryCategories"
            width: parent.width
            height: Math.min(contentHeight, Math.max(Theme.hRow + Theme.s1,
                             (root.height - heading.height - searchRow.height - footer.height) * 0.45))
            contentWidth: width
            contentHeight: categoryRows.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar { policy: categories.contentHeight > categories.height ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded }
            Column {
            id: categoryRows
            width: categories.width
            Media.SidebarRow { width: parent.width; iconName: "image"; label: qsTr("All"); count: root.countFor("all"); selected: root.filter === "all" && root.album === ""; onActivated: { root.filter = "all"; root.album = "" } }
            Media.SidebarRow { width: parent.width; iconName: "star"; label: qsTr("Favourites"); count: root.countFor("favorites"); selected: root.filter === "favorites"; onActivated: { root.filter = "favorites"; root.album = "" } }
            Media.SidebarRow { width: parent.width; iconName: "clock"; label: qsTr("Recents"); count: root.countFor("recents"); selected: root.filter === "recents"; onActivated: { root.filter = "recents"; root.album = "" } }
            Media.SidebarSection {
                width: parent.width
                title: qsTr("Groups")
                actionIcon: "plus"
                actionText: qsTr("New group from the selected still")
                actionEnabled: root.selectedId !== ""
                onActionTriggered: albumPrompt.open()
                Repeater {
                    model: root.albums
                    delegate: Media.SidebarRow {
                        required property string modelData
                        width: root.width
                        iconName: "folder"
                        label: modelData
                        count: root.countFor("all", modelData)
                        depth: 1
                        selected: root.album === modelData
                        onActivated: { root.album = modelData; root.filter = "all" }
                    }
                }
            }
        }
        }
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Item {
            width: parent.width
            id: galleryBody
            objectName: "galleryBody"
            height: Math.max(0, root.height - heading.height - searchRow.height - categories.height - Theme.hairline - footer.height)
            clip: true
            GridView {
                id: grid
                objectName: "stillsGrid"
                anchors.fill: parent
                anchors.margins: Theme.s2
                clip: true
                model: root.filtered
                cellWidth: Math.floor(width / root.columns)
                cellHeight: Math.floor(cellWidth * 9 / 16) + Theme.fsLabel * 2 + Theme.s3 + 8
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    required property var modelData
                    required property int index
                    width: grid.cellWidth
                    height: grid.cellHeight
                    StillCard {
                        objectName: "still:" + index
                        anchors.fill: parent
                        anchors.margins: Theme.s1
                        still: modelData
                        selected: root.selectedId === modelData.id
                        thumbSource: modelData.path !== "" ? "image://thumbs/still:" + modelData.id : ""
                        timecoder: root.timecoder
                        onActivated: { root.selectedId = modelData.id; root.stillActivated(modelData.id) }
                        onDoubleActivated: root.applyRequested(modelData.id, false)
                        onContextRequested: menu.popup()
                        onFavoriteToggled: function (on) { root.favoriteEdited(modelData.id, on) }
                    }
                }
            }
            C.ScrollView {
                objectName: "galleryEmptyScroll"
                anchors.fill: parent
                visible: root.filtered.length === 0
                contentWidth: availableWidth
                clip: true
                C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
                C.ScrollBar.vertical: ScrollBar { policy: C.ScrollBar.AlwaysOn }
                Item {
                    width: parent.width
                    implicitHeight: empty.implicitHeight + Theme.s3 * 2
                    EmptyState {
                        id: empty
                        objectName: "galleryEmptyState"
                        width: parent.width
                        anchors.centerIn: parent
                        iconName: "image"
                        title: root.stills.length === 0 ? qsTr("No stills yet") : qsTr("Nothing matches")
                        description: root.stills.length === 0 ? qsTr("Grab the current frame to keep its grade for other clips.") : ""
                        actionText: root.stills.length === 0 ? qsTr("Grab Still") : ""
                        onActionTriggered: root.grabRequested()
                    }
                }
            }
        }
        Rectangle {
            id: footer
            width: parent.width
            height: Theme.hControl + Theme.s2
            color: Theme.panelRaised
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
            SliderField { anchors.left: parent.left; anchors.leftMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; width: 120; label: ""; labelWidth: 0; from: 0.5; to: 1.5; stepSize: 0.1; showReadout: false; tip: qsTr("How large the still thumbnails are drawn"); value: root.zoom; onEdited: function (v) { root.zoom = v } }
            Text { anchors.right: parent.right; anchors.rightMargin: Theme.s2; anchors.verticalCenter: parent.verticalCenter; text: root.filtered.length === 1 ? qsTr("1 item") : qsTr("%1 items").arg(root.filtered.length); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        }
    }

    ContextMenu {
        id: menu
        MenuAction { text: qsTr("Apply Grade to Current Clip"); iconName: "wand-sparkles"; onTriggered: root.applyRequested(root.selectedId, false) }
        MenuAction { text: qsTr("Apply Grade to All Clips"); onTriggered: root.applyRequested(root.selectedId, true) }
        MenuAction { text: qsTr("Compare in Viewer"); iconName: "compare"; onTriggered: root.wipeRequested(root.selectedId) }
        C.MenuSeparator {}
        MenuAction { text: qsTr("Rename…"); iconName: "pencil"; onTriggered: renamePrompt.open() }
        MenuAction { text: qsTr("Move to Group…"); iconName: "folder"; onTriggered: albumPrompt.open() }
        MenuAction { text: root.selectedStill() && root.selectedStill().favorite ? qsTr("Remove from Favourites") : qsTr("Add to Favourites"); iconName: "star"; onTriggered: root.favoriteEdited(root.selectedId, !(root.selectedStill() && root.selectedStill().favorite)) }
        C.MenuSeparator {}
        MenuAction { text: qsTr("Export Still and Grade…"); iconName: "download"; onTriggered: root.assetRequested("exportStill", root.selectedId) }
        MenuAction { text: qsTr("Delete Still"); iconName: "trash-2"; onTriggered: root.removeRequested(root.selectedId) }
    }
    FormDialog {
        width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
        id: renamePrompt
        title: qsTr("Rename Still")
        modal: true
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onOpened: { renameField.text = root.selectedStill() ? root.selectedStill().name : ""; renameField.forceActiveFocus(); renameField.selectAll() }
        onAccepted: root.renameRequested(root.selectedId, renameField.text)
        LineEdit { id: renameField; Layout.fillWidth: true; hoverEnabled: true; tip: qsTr("New name for the selected still"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
    }
    FormDialog {
        width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
        id: albumPrompt
        title: qsTr("Move to Group")
        modal: true
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onOpened: { albumField.text = root.selectedStill() ? root.selectedStill().album : ""; albumField.forceActiveFocus() }
        onAccepted: root.albumRequested(root.selectedId, albumField.text)
        LineEdit { id: albumField; Layout.fillWidth: true; hint: qsTr("Group name"); hoverEnabled: true; tip: qsTr("Group the selected still is filed under"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
    }
}
