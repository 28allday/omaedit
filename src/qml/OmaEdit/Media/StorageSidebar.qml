pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The storage sidebar: drives, favourites, projects and smart bins. Every row
// is a filter over the library — click a drive and the browser shows what
// lives on it — so the counts are real and the selection lives in the owner.
Rectangle {
    id: root

    // [{name, path, totalLabel, freeLabel, fraction, removable, network, count,
    //   mounted, object, canMount, canUnmount, canEject, locked, tip}] (volumes.h)
    property var drives: []
    // A drive not mounted was taken (click, Space or Return) / asked for its menu.
    signal driveMountRequested(var drive)
    signal driveContextRequested(var drive)
    // [{key, label, icon, count}] — computed by the workspace from the library.
    property var smartBins: []
    property var bins: []
    property var locations: []
    property var watches: []
    property var providers: []
    signal providerAddRequested()
    signal providerContextRequested(string id)
    signal watchAddRequested()
    signal watchContextRequested(string id)
    property int favoriteCount: 0
    property int rejectedCount: 0
    property var recentProjects: []       // paths, most recent first
    property string currentProject: ""    // path of the open project ("" = unsaved)
    property string projectName: ""
    // The active filter: kind ∈ drive | favorites | rejected | smart | "" and its value.
    property string filterKind: ""
    property string filterValue: ""

    signal filterSelected(string kind, string value)
    signal projectActivated(string path)
    signal openProjectRequested()
    signal smartBinAddRequested()
    signal binAddRequested()
    signal binContextRequested(string id)
    signal locationAddRequested()
    signal locationContextRequested(int index)

    color: Theme.panelBg
    implicitWidth: 220

    function projectTitle(path) {
        var file = path.split("/").pop()
        return file.replace(/\.(omaedit|mlt)$/i, "")
    }

    C.ScrollView {
        anchors.fill: parent
        anchors.topMargin: Theme.s2
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            width: parent.width
            spacing: 0

            SidebarSection {
                title: qsTr("Storage")
                settingsKey: "storage"

                SidebarRow {
                    iconName: "hard-drive"
                    label: qsTr("Local Drives")
                    count: root.drives.filter(function (d) { return d.mounted !== false }).length
                    selected: root.filterKind === ""
                    onActivated: root.filterSelected("", "")
                }
                Repeater {
                    model: root.drives.filter(function (d) { return !d.network })
                    delegate: SidebarRow {
                        required property var modelData
                        readonly property bool mounted: modelData.mounted !== false
                        objectName: mounted ? "drive:" + modelData.path : "drive-unmounted:" + modelData.object
                        depth: 1
                        iconName: modelData.locked ? "lock" : "hard-drive"
                        label: modelData.name + " (" + modelData.totalLabel + ")"
                        detail: !mounted ? (modelData.locked ? qsTr("Locked") : qsTr("Not mounted"))
                              : modelData.freeLabel !== "" ? modelData.freeLabel + qsTr(" free") : ""
                        capacity: mounted ? modelData.fraction : -1
                        count: mounted && modelData.count !== undefined ? modelData.count : -1
                        faded: !mounted
                        tip: modelData.tip || ""
                        selected: mounted && root.filterKind === "drive" && root.filterValue === modelData.path
                        onActivated: mounted ? root.filterSelected("drive", modelData.path) : root.driveMountRequested(modelData)
                        onContextRequested: if (modelData.object) root.driveContextRequested(modelData)
                    }
                }
                SidebarRow {
                    iconName: "server"
                    label: qsTr("Network Shares")
                    count: root.drives.filter(function (d) { return d.network }).length
                    visible: count > 0
                    onActivated: root.filterSelected("network", "")
                    selected: root.filterKind === "network"
                }
                Repeater {
                    model: root.drives.filter(function (d) { return d.network })
                    delegate: SidebarRow {
                        required property var modelData
                        depth: 1
                        iconName: "server"
                        label: modelData.name
                        detail: modelData.freeLabel + qsTr(" free")
                        capacity: modelData.fraction
                        count: modelData.count !== undefined ? modelData.count : -1
                        selected: root.filterKind === "drive" && root.filterValue === modelData.path
                        onActivated: root.filterSelected("drive", modelData.path)
                    }
                }
                SidebarRow {
                    iconName: "cloud"
                    label: qsTr("Cloud Storage")
                    detail: root.providers.length > 0 ? qsTr("%1 mounted provider(s)").arg(root.providers.length) : qsTr("Add a mounted provider…")
                    onActivated: root.providerAddRequested()
                }
                Repeater {
                    model: root.providers
                    delegate: SidebarRow {
                        required property var modelData
                        depth: 1; iconName: modelData.kind === "network" ? "server" : "cloud"
                        label: modelData.name
                        detail: modelData.root
                        selected: root.filterKind === "drive" && root.filterValue === modelData.root
                        onActivated: root.filterSelected("drive", modelData.root)
                        onContextRequested: root.providerContextRequested(modelData.id)
                    }
                }
            }

            SidebarSection {
                title: qsTr("Watch Folders")
                settingsKey: "watch"
                actionIcon: "plus"; actionText: qsTr("Add watch folder")
                onActionTriggered: root.watchAddRequested()
                Repeater {
                    model: root.watches
                    delegate: SidebarRow {
                        required property var modelData
                        iconName: "folder"
                        label: modelData.name || modelData.path
                        detail: modelData.enabled ? qsTr("Automatic ingest on") : qsTr("Paused")
                        onActivated: root.filterSelected("drive", modelData.path)
                        onContextRequested: root.watchContextRequested(modelData.id)
                    }
                }
            }
            SidebarSection {
                title: qsTr("Favourites")
                settingsKey: "favorites"
                actionIcon: "plus"
                actionText: qsTr("Add location")
                onActionTriggered: root.locationAddRequested()
                Repeater {
                    model: root.locations
                    delegate: SidebarRow {
                        required property var modelData
                        required property int index
                        iconName: "folder"
                        label: modelData.name
                        selected: root.filterKind === "drive" && root.filterValue === modelData.path
                        onActivated: root.filterSelected("drive", modelData.path)
                        onContextRequested: root.locationContextRequested(index)
                    }
                }

                SidebarRow {
                    iconName: "heart"
                    label: qsTr("Favourite Clips")
                    count: root.favoriteCount
                    selected: root.filterKind === "favorites"
                    onActivated: root.filterSelected("favorites", "")
                }
                SidebarRow {
                    iconName: "thumbs-down"
                    label: qsTr("Rejected Clips")
                    count: root.rejectedCount
                    selected: root.filterKind === "rejected"
                    onActivated: root.filterSelected("rejected", "")
                }
            }

            SidebarSection {
                title: qsTr("Bins")
                settingsKey: "bins"
                actionIcon: "plus"
                actionText: qsTr("New bin")
                onActionTriggered: root.binAddRequested()
                SidebarRow { label: qsTr("Library root"); iconName: "folder-open"; selected: root.filterKind === "root"; onActivated: root.filterSelected("root", "") }
                Repeater {
                    model: root.bins
                    delegate: SidebarRow {
                        required property var modelData
                        objectName: "mediaBin:" + modelData.id
                        depth: modelData.depth
                        label: modelData.name
                        iconName: modelData.smart ? "search" : "folder"
                        count: modelData.count
                        selected: root.filterKind === "bin" && root.filterValue === modelData.id
                        onActivated: root.filterSelected("bin", modelData.id)
                        onContextRequested: root.binContextRequested(modelData.id)
                    }
                }
            }
            SidebarSection {
                title: qsTr("Projects")
                settingsKey: "projects"
                actionIcon: "folder-open"
                actionText: qsTr("Open project…")
                onActionTriggered: root.openProjectRequested()

                SidebarRow {
                    visible: root.currentProject === ""
                    iconName: "square-pen"
                    label: root.projectName !== "" ? root.projectName : qsTr("Untitled")
                    detail: qsTr("Unsaved project")
                    selected: true
                }
                Repeater {
                    model: root.recentProjects
                    delegate: SidebarRow {
                        required property string modelData
                        objectName: "project:" + modelData
                        iconName: "folder"
                        label: root.projectTitle(modelData)
                        selected: modelData === root.currentProject
                        onActivated: if (modelData !== root.currentProject) root.projectActivated(modelData)
                    }
                }
                SidebarRow {
                    visible: root.recentProjects.length === 0 && root.currentProject !== ""
                    iconName: "folder"
                    label: qsTr("No recent projects")
                    dimmed: true
                }
            }

            SidebarSection {
                title: qsTr("Saved Searches")
                settingsKey: "smartBins"
                actionIcon: "plus"
                actionText: qsTr("New saved search")
                onActionTriggered: root.smartBinAddRequested()

                Repeater {
                    model: root.smartBins
                    delegate: SidebarRow {
                        required property var modelData
                        objectName: "smartBin:" + modelData.key
                        iconName: modelData.icon
                        label: modelData.label
                        count: modelData.count
                        selected: root.filterKind === "smart" && root.filterValue === modelData.key
                        onActivated: root.filterSelected("smart", modelData.key)
                    }
                }
            }
        }
    }

    // Seam against the browser.
    Rectangle {
        anchors.right: parent.right
        width: Theme.hairline
        height: parent.height
        color: Theme.border
    }
}
