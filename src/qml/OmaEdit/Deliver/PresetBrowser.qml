pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The Render Presets column: search, a filter, the five built-in groups
// (read-only; each folds), the user's custom presets, and New Preset.
// Selecting a row is the only thing that changes settings; the ⋮ menu
// does the rest (favourite, duplicate, rename, update, export, delete).
Rectangle {
    id: root

    // [{id, name, group, builtin, favorite, spec}]
    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property var presets: []
    property string currentId: ""
    property bool currentModified: false
    // What a preset's spec means at this timeline's size — the seam's summariser.
    property var describer: null
    property int timelineWidth: 0
    property int timelineHeight: 0

    signal presetSelected(string id)
    signal newPresetRequested()
    signal importRequested()
    signal favoriteToggled(string id, bool on)
    signal duplicateRequested(string id)
    signal renameRequested(string id)
    signal updateRequested(string id)
    signal exportRequested(string id)
    signal deleteRequested(string id)

    readonly property bool textActive: search.active
    property string filter: "all" // all | favorites | video | audio | custom

    color: Theme.panelBg
    implicitWidth: 400

    readonly property var groupDefs: [
        { key: "master", label: qsTr("Master"), icon: "film" },
        { key: "web", label: qsTr("Web"), icon: "monitor" },
        { key: "broadcast", label: qsTr("Broadcast"), icon: "scan-line" },
        { key: "social", label: qsTr("Social"), icon: "share-2" },
        { key: "audio", label: qsTr("Audio Only"), icon: "music" }
    ]
    property var expanded: ({ custom: true })

    function resolutionOf(p) {
        var s = p.spec || {}
        var w = s.width !== undefined && s.width > 0 ? s.width : timelineWidth
        var h = s.height !== undefined && s.height > 0 ? s.height : timelineHeight
        if (s.video !== undefined && !s.video) return qsTr("audio")
        return w > 0 ? w + "\u00d7" + h : ""
    }
    function badgeOf(p) {
        var s = p.spec || {}
        if (s.video !== undefined && !s.video) return ""
        var h = s.height !== undefined && s.height > 0 ? s.height : timelineHeight
        var w = s.width !== undefined && s.width > 0 ? s.width : timelineWidth
        if (Math.max(w, h) >= 3840) return "4K"
        if (Math.max(w, h) >= 1920) return "HD"
        return ""
    }
    function iconOf(p) {
        var s = p.spec || {}
        if (s.video !== undefined && !s.video) return "music"
        var g = p.group || ""
        for (var i = 0; i < groupDefs.length; ++i) if (groupDefs[i].key === g) return groupDefs[i].icon
        return "film"
    }
    function matches(p) {
        var q = search.text.trim().toLowerCase()
        if (q !== "" && String(p.name).toLowerCase().indexOf(q) < 0
                && (describer ? String(describer(p.spec)).toLowerCase().indexOf(q) < 0 : true))
            return false
        var audioOnly = p.spec !== undefined && p.spec.video !== undefined && !p.spec.video
        switch (filter) {
        case "favorites": return p.favorite === true
        case "video": return !audioOnly
        case "audio": return audioOnly
        case "custom": return p.builtin !== true
        }
        return true
    }
    function inGroup(key) {
        var out = []
        for (var i = 0; i < presets.length; ++i)
            if (presets[i].group === key && matches(presets[i])) out.push(presets[i])
        return out
    }
    readonly property bool searching: search.text.trim() !== "" || filter !== "all"
    function isExpanded(key) {
        if (searching) return true
        if (expanded[key] !== undefined) return expanded[key]
        // The group holding the current preset starts open.
        for (var i = 0; i < presets.length; ++i)
            if (presets[i].id === currentId) return presets[i].group === key
        return key === "custom"
    }
    function toggle(key) {
        var next = {}
        for (var k in expanded) next[k] = expanded[k]
        next[key] = !isExpanded(key)
        expanded = next
    }
    function openMenu(p, item, at) {
        rowMenu.preset = p
        rowMenu.parent = item
        rowMenu.popup(at)
    }

    Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: Theme.border; z: 2 }

    Media.SectionTitle {
        id: sectionTitle
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3 + Theme.hairline
        height: presetActions.inBar ? Theme.s1 : implicitHeight
        text: root.dockBar === null ? qsTr("Render Presets") : ""
    }

    DockBarActions {
        id: presetActions
        dockBar: root.dockBar
        fallback: sectionTitle
        fallbackMargin: 0

        IconButton {
            iconName: "menu"
            text: qsTr("Preset browser menu")
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            onClicked: browserMenu.popup()
        }
    }

    Row {
        id: searchRow
        anchors.top: sectionTitle.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3 + Theme.hairline
        spacing: Theme.s2

        SearchField {
            id: search
            objectName: "presetSearch"
            width: parent.width - filterButton.width - parent.spacing
            placeholder: qsTr("Search presets…")
            tip: qsTr("Show only presets whose name or settings summary contains this text")
        }
        IconButton {
            id: filterButton
            iconName: "filter"
            text: qsTr("Filter presets")
            flat: false
            checked: root.filter !== "all"
            implicitWidth: Theme.hControl
            implicitHeight: Theme.hControl
            onClicked: filterMenu.popup()
        }
    }

    C.ScrollView {
        anchors.top: searchRow.bottom
        anchors.topMargin: Theme.s2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: footer.top
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            id: list
            width: parent.width
            spacing: 0
            leftPadding: Theme.s2
            rightPadding: Theme.s2

            Repeater {
                model: root.groupDefs
                delegate: Column {
                    id: group
                    required property var modelData
                    required property int index
                    readonly property var members: root.inGroup(modelData.key)
                    readonly property bool open: root.isExpanded(modelData.key)
                    readonly property bool holdsCurrent: {
                        for (var i = 0; i < members.length; ++i) if (members[i].id === root.currentId) return true
                        return false
                    }
                    width: list.width - list.leftPadding - list.rightPadding
                    visible: members.length > 0 || !root.searching
                    spacing: 0

                    PresetRow {
                        objectName: "presetGroup:" + group.modelData.key
                        iconName: group.modelData.icon
                        label: group.modelData.label
                        badge: group.holdsCurrent && !group.open ? qsTr("%1").arg(group.members.length) : ""
                        detail: group.open ? "" : qsTr("%1").arg(group.members.length)
                        selected: group.holdsCurrent && !group.open
                        onActivated: root.toggle(group.modelData.key)
                        onMenuRequested: root.toggle(group.modelData.key)
                        Icon {
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.s2 + Theme.szIcon + Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            name: group.open ? "chevron-up" : "chevron-down"
                            size: Theme.szIcon - 2
                            color: Theme.textMuted
                            visible: !group.holdsCurrent || group.open
                        }
                    }
                    Repeater {
                        model: group.open ? group.members : []
                        delegate: PresetRow {
                            id: row
                            required property var modelData
                            objectName: "preset:" + modelData.id
                            depth: 1
                            iconName: root.iconOf(modelData)
                            label: modelData.name
                            detail: root.resolutionOf(modelData)
                            favorite: modelData.favorite === true
                            selected: modelData.id === root.currentId
                            modified: selected && root.currentModified
                            onActivated: root.presetSelected(modelData.id)
                            onMenuRequested: function (at) { root.openMenu(modelData, row, at) }
                        }
                    }
                    Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border; opacity: 0.6 }
                }
            }

            // Custom presets.
            Column {
                id: customGroup
                readonly property var members: root.inGroup("custom")
                readonly property bool open: root.isExpanded("custom")
                width: list.width - list.leftPadding - list.rightPadding
                spacing: 0

                PresetRow {
                    objectName: "presetGroup:custom"
                    iconName: "folder"
                    label: qsTr("Custom Presets")
                    detail: customGroup.open ? "" : qsTr("%1").arg(customGroup.members.length)
                    onActivated: root.toggle("custom")
                    onMenuRequested: root.toggle("custom")
                    Icon {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s2 + Theme.szIcon + Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        name: customGroup.open ? "chevron-up" : "chevron-down"
                        size: Theme.szIcon - 2
                        color: Theme.textMuted
                    }
                }
                Repeater {
                    model: customGroup.open ? customGroup.members : []
                    delegate: PresetRow {
                        id: customRow
                        required property var modelData
                        objectName: "preset:" + modelData.id
                        depth: 1
                        iconName: root.iconOf(modelData)
                        label: modelData.name
                        detail: root.resolutionOf(modelData)
                        favorite: modelData.favorite === true
                        selected: modelData.id === root.currentId
                        modified: selected && root.currentModified
                        onActivated: root.presetSelected(modelData.id)
                        onMenuRequested: function (at) { root.openMenu(modelData, customRow, at) }
                    }
                }
                Text {
                    visible: customGroup.open && customGroup.members.length === 0
                    width: parent.width
                    leftPadding: Theme.s3 + Theme.s4
                    topPadding: Theme.s1
                    bottomPadding: Theme.s2
                    text: root.searching ? qsTr("No custom presets match") : qsTr("Save the current settings with New Preset")
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Item {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.hairline
        anchors.bottom: parent.bottom
        height: Theme.hControl + Theme.s3 * 2

        Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }

        ToolButton {
            objectName: "newPreset"
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            iconName: "plus"
            text: qsTr("New Preset")
            showLabel: true
            onClicked: root.newPresetRequested()
        }
        ToolButton {
            objectName: "importPresets"
            anchors.right: parent.right
            anchors.rightMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            iconName: "download"
            text: qsTr("Import…")
            showLabel: true
            onClicked: root.importRequested()
        }
    }

    ContextMenu {
        id: filterMenu
        parent: filterButton
        y: filterButton.height
        MenuAction { text: qsTr("All presets"); checkable: true; checked: root.filter === "all"; onTriggered: root.filter = "all" }
        MenuAction { text: qsTr("Favourites"); checkable: true; checked: root.filter === "favorites"; onTriggered: root.filter = "favorites" }
        MenuAction { text: qsTr("Video"); checkable: true; checked: root.filter === "video"; onTriggered: root.filter = "video" }
        MenuAction { text: qsTr("Audio only"); checkable: true; checked: root.filter === "audio"; onTriggered: root.filter = "audio" }
        MenuAction { text: qsTr("Custom only"); checkable: true; checked: root.filter === "custom"; onTriggered: root.filter = "custom" }
    }

    ContextMenu {
        id: browserMenu
        parent: sectionTitle
        y: sectionTitle.height
        MenuAction { text: qsTr("New Preset…"); iconName: "plus"; onTriggered: root.newPresetRequested() }
        MenuAction { text: qsTr("Import Presets…"); iconName: "download"; onTriggered: root.importRequested() }
        MenuAction { text: qsTr("Expand All"); onTriggered: root.expanded = ({ master: true, web: true, broadcast: true, social: true, audio: true, custom: true }) }
        MenuAction { text: qsTr("Collapse All"); onTriggered: root.expanded = ({ master: false, web: false, broadcast: false, social: false, audio: false, custom: false }) }
    }

    ContextMenu {
        id: rowMenu
        objectName: "presetMenu"
        property var preset: ({})
        readonly property bool custom: preset.builtin === false
        MenuAction { text: qsTr("Use Preset"); iconName: "check"; onTriggered: root.presetSelected(rowMenu.preset.id) }
        MenuAction {
            text: rowMenu.preset.favorite === true ? qsTr("Remove from Favourites") : qsTr("Add to Favourites")
            iconName: "star"
            onTriggered: root.favoriteToggled(rowMenu.preset.id, rowMenu.preset.favorite !== true)
        }
        MenuAction { text: qsTr("Duplicate"); iconName: "copy"; onTriggered: root.duplicateRequested(rowMenu.preset.id) }
        MenuAction { text: qsTr("Rename…"); iconName: "pencil"; visible: rowMenu.custom; height: visible ? implicitHeight : 0; onTriggered: root.renameRequested(rowMenu.preset.id) }
        MenuAction { text: qsTr("Update with Current Settings"); iconName: "save"; visible: rowMenu.custom; height: visible ? implicitHeight : 0; onTriggered: root.updateRequested(rowMenu.preset.id) }
        MenuAction { text: qsTr("Export…"); iconName: "share-2"; onTriggered: root.exportRequested(rowMenu.preset.id) }
        MenuAction { text: qsTr("Delete"); iconName: "trash-2"; visible: rowMenu.custom; height: visible ? implicitHeight : 0; onTriggered: root.deleteRequested(rowMenu.preset.id) }
    }
}
