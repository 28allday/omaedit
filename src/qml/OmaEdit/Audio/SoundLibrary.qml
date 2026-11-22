pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The Audio page's left panel: LIBRARY / SOUNDS / FAVORITES tabs, search,
// categories with counts, smart collections, the sound list with a peak
// thumbnail per row, and an audition footer. Pure: sounds come in, every
// action goes out as a signal.
Rectangle {
    id: root

    property var sounds: []            // SoundLibrary.items
    property var folders: []
    property var collections: []       // [{name, query, category}]
    property bool scanning: false
    property int pending: 0
    property string scanStatus: ""
    property real auditionVolume: 1
    signal auditionVolumeEdited(real value)
    property string auditionPath: ""
    property bool auditionPlaying: false
    property bool loop: false
    property real auditionLeft: -100
    property real auditionRight: -100
    property real fps: 25
    property var timecoder: null

    signal auditionRequested(string path)
    signal auditionStopped()
    signal loopToggled(bool on)
    signal favoriteEdited(string path, bool on)
    signal categoryEdited(string path, string category)
    signal tagsEdited(string path, var tags)
    signal folderAddRequested()
    signal folderRemoved(string path)
    signal rescanRequested()
    signal scanCancelRequested()
    signal collectionAdded(string name, string query, string category)
    signal collectionRemoved(string name)
    signal soundDragStarted(string path)
    signal soundDropped(string path, int track, int frame)
    signal soundOpened(string path)   // double-click: put it at the playhead

    readonly property bool textActive: tagPrompt.visible || search.active || collectionName.activeFocus

    property int tab: 0                // 0 library, 1 sounds (project), 2 favourites
    property string category: ""       // "" = all
    property string collection: ""     // a smart collection's name
    property string selectedPath: ""

    color: Theme.panelBg

    // Read the incoming Qt sequence once per update. Category badges share
    // these counts, and search/selection reuse the same rows while typing.
    readonly property var catalog: {
        var rows = [], byPath = Object.create(null)
        var all = { "": 0 }, favorites = { "": 0 }
        var source = sounds, length = source.length
        for (var i = 0; i < length; ++i) {
            var sound = source[i]
            rows.push(sound)
            byPath[sound.path] = sound
            var cat = sound.category || "effects"
            ++all[""]
            all[cat] = (all[cat] || 0) + 1
            if (sound.favorite === true) {
                ++favorites[""]
                favorites[cat] = (favorites[cat] || 0) + 1
            }
        }
        return { rows: rows, byPath: byPath, all: all, favorites: favorites }
    }
    readonly property var filtered: {
        var out = [], source = catalog.rows
        var query = search.text.toLowerCase(), cat = category, selectedTab = tab
        if (collection !== "") {
            for (var c = 0; c < collections.length; ++c)
                if (collections[c].name === collection) {
                    query = String(collections[c].query).toLowerCase()
                    cat = collections[c].category
                    break
                }
        }
        for (var i = 0; i < source.length; ++i) {
            var sound = source[i]
            if (selectedTab === 2 && sound.favorite !== true) continue
            if (selectedTab === 1 && sound.folder !== "project") continue
            if (cat !== "" && sound.category !== cat) continue
            if (query !== "") {
                var hay = (sound.name + " " + (sound.tags !== undefined ? sound.tags.join(" ") : "")
                           + " " + sound.category).toLowerCase()
                if (hay.indexOf(query) < 0) continue
            }
            out.push(sound)
        }
        return out
    }
    function countFor(cat) {
        return (tab === 2 ? catalog.favorites : catalog.all)[cat] || 0
    }
    function selectedSound() {
        return catalog.byPath[selectedPath] || null
    }
    function audition(path) {
        if (auditionPath === path && auditionPlaying) auditionStopped()
        else auditionRequested(path)
    }

    Column {
        anchors.fill: parent

        // Tabs
        Row {
            id: tabs
            width: parent.width
            height: Theme.hTab
            Repeater {
                model: [qsTr("Library"), qsTr("Sounds"), qsTr("Favourites")]
                delegate: TabButton {
                    required property int index
                    required property string modelData
                    objectName: "soundTab:" + index
                    tip: qsTr("Choose which set of sounds the list below shows")
                    height: tabs.height
                    text: modelData
                    checked: root.tab === index
                    onClicked: { root.tab = index; root.collection = ""; checked = Qt.binding(() => root.tab === index) }
                }
            }
        }
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

        // Search + folder actions
        Item {
            id: searchBar
            width: parent.width
            height: Theme.hControl + Theme.s2 * 2
            SearchField {
                id: search
                objectName: "soundSearch"
                anchors.left: parent.left
                anchors.right: addFolder.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.s2
                anchors.rightMargin: Theme.s1
                placeholder: qsTr("Search audio…")
                tip: qsTr("Match sounds by name, tag or category as you type")
                onTextChanged: root.collection = ""
            }
            IconButton {
                id: addFolder
                objectName: "soundAddFolder"
                anchors.right: rescan.left
                anchors.verticalCenter: parent.verticalCenter
                iconName: "folder-plus"
                Accessible.name: qsTr("Add sound folder")
                onClicked: root.folderAddRequested()
            }
            IconButton {
                id: rescan
                objectName: "soundRescan"
                anchors.right: parent.right
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                iconName: root.scanning ? "square" : "refresh-cw"
                Accessible.name: root.scanning ? qsTr("Stop sound scan") : qsTr("Rescan folders")
                tip: root.scanning ? qsTr("Stop scanning and keep the sounds already listed") : qsTr("Check sound folders for new or changed audio")
                onClicked: root.scanning ? root.scanCancelRequested() : root.rescanRequested()
            }
        }

        // Keep rows and scan status visible when this panel is short. Long
        // collection lists share the same scrollable category section.
        Flickable {
            id: categoryScroll
            objectName: "soundCategories"
            width: parent.width
            height: Math.min(sidebar.implicitHeight, Math.max(0, root.height - tabs.height
                    - searchBar.height - headings.height - footer.height - Theme.hairline * 2 - Theme.hSoundRow * 2))
            contentWidth: width
            contentHeight: sidebar.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            C.ScrollBar.vertical: ScrollBar {}
            Column {
                id: sidebar
                width: categoryScroll.width
                Media.SidebarRow {
                    activeFocusOnTab: true
                    property bool keyboardConsumesKeys: true
                    Keys.onSpacePressed: activated()
                    Keys.onReturnPressed: activated()
                    Keys.onEnterPressed: activated()
                    width: parent.width
                    iconName: "audio-waveform"
                    label: qsTr("All Audio")
                    count: root.countFor("")
                    selected: root.category === "" && root.collection === ""
                    onActivated: { root.category = ""; root.collection = "" }
                }
                Repeater {
                    model: Theme.roleNames.slice(0, 6)
                    delegate: Media.SidebarRow {
                        activeFocusOnTab: true
                        property bool keyboardConsumesKeys: true
                        Keys.onSpacePressed: activated()
                        Keys.onReturnPressed: activated()
                        Keys.onEnterPressed: activated()
                        required property string modelData
                        objectName: "soundCategory:" + modelData
                        width: sidebar.width
                        iconName: modelData === "music" ? "music" : modelData === "dialogue" ? "mic" : modelData === "ambience" ? "waves" : "audio-waveform"
                        label: Theme.roleTitle(modelData)
                        count: root.countFor(modelData)
                        depth: 1
                        selected: root.category === modelData && root.collection === ""
                        onActivated: { root.category = modelData; root.collection = "" }
                    }
                }
                Media.SidebarSection {
                    width: parent.width
                    title: qsTr("Saved Searches")
                    actionIcon: "plus"
                    actionText: qsTr("New collection from this search")
                    onActionTriggered: collectionPrompt.open()
                    Repeater {
                        model: root.collections
                        delegate: Media.SidebarRow {
                            activeFocusOnTab: true
                            property bool keyboardConsumesKeys: true
                            Keys.onSpacePressed: activated()
                            Keys.onReturnPressed: activated()
                            Keys.onEnterPressed: activated()
                            required property var modelData
                            width: sidebar.width
                            iconName: "sparkles"
                            label: modelData.name
                            depth: 1
                            selected: root.collection === modelData.name
                            onActivated: root.collection = modelData.name
                            onContextRequested: { root.collection = modelData.name; collectionMenu.popup() }
                        }
                    }
                }
            }
        }
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }

        // Column headings
        Item {
            id: headings
            width: parent.width
            height: Theme.hRow
            Row {
                anchors.fill: parent
                anchors.leftMargin: Theme.s2 + (root.width >= 360 ? Theme.wSoundWave + Theme.s2 : 0)
                anchors.rightMargin: Theme.s2
                spacing: Theme.s2
                Text { width: Math.max(0, parent.width - (root.width >= 240 ? 58 + Theme.s2 : 0) - (root.width >= 360 ? 30 + 14 + Theme.s2 * 2 : 0) - Theme.szIconHit - Theme.s2); height: parent.height; verticalAlignment: Text.AlignVCenter; text: qsTr("Name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { visible: root.width >= 240; width: 58; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: qsTr("Dur."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { visible: root.width >= 360; width: 30; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: qsTr("SR"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Text { visible: root.width >= 360; width: 14; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: qsTr("Ch"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                Item { width: Theme.szIconHit; height: 1 }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
        }

        // The list
        Item {
            width: parent.width
            height: Math.max(0, root.height - tabs.height - searchBar.height - categoryScroll.height
                             - headings.height - footer.height - Theme.hairline * 2)
            clip: true
            ListView {
                id: list
                objectName: "soundList"
                anchors.fill: parent
                clip: true
                model: root.filtered
                boundsBehavior: Flickable.StopAtBounds
                C.ScrollBar.vertical: ScrollBar {}
                delegate: SoundRow {
                    required property var modelData
                    required property int index
                    objectName: "sound:" + index
                    width: list.width
                    sound: modelData
                    fps: root.fps
                    selected: root.selectedPath === modelData.path
                    auditioning: root.auditionPath === modelData.path && root.auditionPlaying
                    onActivated: root.selectedPath = modelData.path
                    onDoubleActivated: root.soundOpened(modelData.path)
                    onContextRequested: soundMenu.popup()
                    onFavoriteToggled: function (on) { root.favoriteEdited(modelData.path, on) }
                    // An internal drag: a ghost in the window overlay follows the
                    // pointer (the list clips its rows) and carries the path for
                    // the timeline's DropArea.
                    Rectangle {
                        id: ghost
                        parent: C.Overlay.overlay
                        width: Theme.wSoundWave * 4
                        height: Theme.hSoundRow
                        radius: Theme.rControl
                        color: Theme.roleColor(modelData.category !== undefined ? modelData.category : "")
                        border.color: Theme.dropValid
                        border.width: Theme.hairline
                        opacity: 0.85
                        visible: dragHandler.active
                        property string soundPath: modelData.path
                        Drag.active: dragHandler.active
                        Drag.keys: ["omaedit-sound"]
                        Drag.hotSpot.x: width / 2
                        Drag.hotSpot.y: height / 2
                        Drag.source: ghost
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s1
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            text: modelData.name
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel
                            color: Theme.textPrimary
                        }
                    }
                    DragHandler {
                        id: dragHandler
                        target: ghost
                        onActiveChanged: {
                            if (active) {
                                root.selectedPath = modelData.path
                                root.soundDragStarted(modelData.path)
                                var p = parent.mapToItem(C.Overlay.overlay, centroid.pressPosition.x, centroid.pressPosition.y)
                                ghost.x = p.x - ghost.width / 2
                                ghost.y = p.y - ghost.height / 2
                            } else {
                                ghost.Drag.drop()
                            }
                        }
                    }
                }
            }
            EmptyState {
                anchors.centerIn: parent
                visible: root.filtered.length === 0 && parent.height >= 150
                iconName: "audio-waveform"
                objectName: "soundEmpty"
                title: root.scanning ? qsTr("Scanning sounds…") : root.sounds.length === 0 ? qsTr("No sounds yet") : qsTr("Nothing matches")
                description: root.sounds.length === 0 ? qsTr("Add a folder of audio files — ~/Music is scanned by default.") : qsTr("Try another search or category.")
                actionText: root.sounds.length === 0 ? qsTr("Add Folder…") : ""
                onActionTriggered: root.folderAddRequested()
            }
            Text {
                anchors.centerIn: parent
                visible: root.filtered.length === 0 && parent.height < 150 && parent.height >= 16
                objectName: "soundEmptyShort"
                text: root.scanning ? qsTr("Scanning sounds…") : root.sounds.length === 0 ? qsTr("No sounds yet — add a folder above") : qsTr("No matching sounds")
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
            }
        }

        // Footer: audition transport + count
        Rectangle {
            id: footer
            width: parent.width
            height: Theme.hControl + Theme.s2
            color: Theme.panelRaised
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
            Row {
                anchors.left: parent.left
                anchors.leftMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                IconButton {
                    objectName: "auditionPlay"
                    iconName: root.auditionPlaying && root.auditionPath === root.selectedPath ? "pause" : "play"
                    enabled: root.selectedPath !== ""
                    Accessible.name: qsTr("Audition")
                    onClicked: root.audition(root.selectedPath)
                }
                IconButton {
                    iconName: "square"
                    enabled: root.auditionPlaying
                    Accessible.name: qsTr("Stop audition")
                    onClicked: root.auditionStopped()
                }
                IconButton {
                    objectName: "auditionLoop"
                    iconName: "refresh-cw"
                    checked: root.loop
                    Accessible.name: qsTr("Loop audition")
                    onClicked: root.loopToggled(!root.loop)
                }
            }
            C.Slider {
                id: auditionLevel
                objectName: "auditionLevel"
                anchors.right: count.left;anchors.rightMargin: Theme.s2;anchors.verticalCenter: parent.verticalCenter
                width: 72;from: 0;to: 1;value: root.auditionVolume
                Accessible.name: qsTr("Audition level")
                C.ToolTip.text: qsTr("How loud the previewed sound plays; the mix is unaffected"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450
                onMoved: root.auditionVolumeEdited(value)
            }
            HMeter {
                anchors.left: parent.left
                anchors.leftMargin: Theme.szIconHit * 3 + Theme.s3
                anchors.right: auditionLevel.left
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                height: Theme.hHMeter * 2 + 4
                visible: width > 60
                levelLeft: root.auditionPlaying ? root.auditionLeft : -100
                levelRight: root.auditionPlaying ? root.auditionRight : -100
                clip: true
            }
            Text {
                id: count
                objectName: "soundCount"
                anchors.right: parent.right
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(0, Math.min(implicitWidth, parent.width - Theme.szIconHit * 3 - 72 - Theme.s2 * 4))
                elide: Text.ElideRight
                text: (root.filtered.length === 1 ? qsTr("1 item") : qsTr("%1 items").arg(root.filtered.length))
                      + (root.scanStatus ? " · " + root.scanStatus : "")
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                HoverHandler { id: countHover }
                C.ToolTip.text: text
                C.ToolTip.visible: countHover.hovered
                C.ToolTip.delay: Theme.tooltipDelay
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }

    ContextMenu {
        id: soundMenu
        MenuAction { text: qsTr("Audition"); iconName: "play"; onTriggered: root.audition(root.selectedPath) }
        MenuAction { text: qsTr("Edit Tags…"); iconName: "tag"; onTriggered: { tagPrompt.soundPath=root.selectedPath; var item=root.selectedSound(); tags.text=item && item.tags ? item.tags.join(", ") : ""; tagPrompt.open() } }
        MenuAction { text: qsTr("Place at Playhead"); iconName: "plus"; onTriggered: root.soundOpened(root.selectedPath) }
        MenuAction {
            text: root.selectedSound() && root.selectedSound().favorite ? qsTr("Remove from Favourites") : qsTr("Add to Favourites")
            iconName: "star"
            onTriggered: root.favoriteEdited(root.selectedPath, !(root.selectedSound() && root.selectedSound().favorite))
        }
        C.Menu {
            title: qsTr("Category")
            Repeater {
                model: Theme.roleNames.slice(0, 6)
                delegate: MenuAction {
                    required property string modelData
                    text: Theme.roleTitle(modelData)
                    checkable: true
                    checked: root.selectedSound() && root.selectedSound().category === modelData
                    onTriggered: root.categoryEdited(root.selectedPath, modelData)
                }
            }
            C.MenuSeparator {}
            MenuAction { text: qsTr("Automatic"); onTriggered: root.categoryEdited(root.selectedPath, "") }
        }
        C.MenuSeparator {}
        C.Menu {
            title: qsTr("Folders")
            Repeater {
                model: root.folders
                delegate: MenuAction {
                    required property string modelData
                    text: qsTr("Remove %1").arg(modelData)
                    iconName: "trash-2"
                    onTriggered: root.folderRemoved(modelData)
                }
            }
            MenuAction { text: qsTr("Add Folder…"); iconName: "folder-plus"; onTriggered: root.folderAddRequested() }
        }
    }
    ContextMenu {
        id: collectionMenu
        MenuAction { text: qsTr("Delete Collection"); iconName: "trash-2"; onTriggered: { root.collectionRemoved(root.collection); root.collection = "" } }
    }

    ModalDialog {
        id: collectionPrompt
        objectName: "collectionPrompt"
        title: qsTr("New Saved Search")
        modal: true
        anchors.centerIn: C.Overlay.overlay
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onAccepted: {
            root.collectionAdded(collectionName.text, search.text, root.category)
            collectionName.text = ""
        }
        onOpened: collectionName.forceActiveFocus()
        Column {
            spacing: Theme.s2
            Text {
                text: qsTr("Saves the current search (\"%1\") and category as a collection.").arg(search.text)
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textSecondary
                width: 280
                wrapMode: Text.WordWrap
            }
            C.TextField { id: collectionName; width: 280; placeholderText: qsTr("Collection name"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; C.ToolTip.text: qsTr("Name this saved search is listed under in Saved Searches"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true }
        }
    }
    ModalDialog {
        id: tagPrompt
        property string soundPath: ""
        title: qsTr("Sound tags")
        modal: true
        parent: C.Overlay.overlay
        anchors.centerIn: parent
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onAccepted: root.tagsEdited(soundPath,tags.text.split(",").map(function(t){return t.trim()}).filter(function(t){return t!==""}))
        C.TextField { id: tags; width: 320; placeholderText: qsTr("Comma-separated tags"); C.ToolTip.text: qsTr("Tags kept with this sound and matched by the search box"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true }
    }

}
