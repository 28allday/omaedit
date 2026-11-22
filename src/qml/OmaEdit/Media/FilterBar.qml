import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// Search, the four filter menus, view mode, thumbnail size and sort. All state
// lives here as plain properties the browser binds to; nothing filters on its
// own.
Rectangle {
    id: root

    property alias searchText: search.text
    readonly property bool searchActive: search.active

    // Options the owner computes from the library (types and tags in use).
    property var typeOptions: []      // e.g. ["MP4", "MOV"]
    property var tagOptions: []

    property int mediaFilter: 0       // 0 all, 1 video, 2 audio only, 3 stills
    property string typeFilter: ""    // "" = all
    property int ratingFilter: 0      // 0 all, 1 ★5, 2 ★4+, 3 ★3+, 4 unrated, 5 favourites, 6 rejected
    property string tagFilter: ""

    property int viewMode: 0          // 0 grid, 1 list
    property real thumbSize: 0.5      // 0..1
    property int sortKey: 0           // 0 name, 1 created, 2 duration, 3 rating, 4 imported
    property bool sortDescending: false

    // Below ~1000 px the bar drops the thumbnail slider and tightens every
    // control; the Edit page's narrow policy, applied to a toolbar.
    readonly property bool compact: width < 1000 + (Theme.densityScale - 1) * 400
    readonly property bool tight: width < 860
    readonly property int comboWidth: tight ? 92 : compact ? 100 : 108

    readonly property bool stacked: filters.width + sorting.width + Theme.s3 * 2 + Theme.s2 > width
    implicitHeight: stacked ? Theme.hControl * 2 + Theme.s2 * 3 : Theme.hTab + Theme.s2
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    Row {
        id: filters
        objectName: "mediaFilterControls"
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        y: root.stacked ? Theme.s2 : (root.height - height) / 2
        spacing: Theme.s2

        SearchField {
            id: search
            objectName: "mediaSearch"
            tip: qsTr("Searches names, logging, custom fields and transcripts")
            width: root.tight ? 120 : root.compact ? 140 : 200
            placeholder: qsTr("Search media…")
            anchors.verticalCenter: parent.verticalCenter
        }

        ComboField {
            objectName: "mediaFilter"
            tip: qsTr("Show every clip, or only video, audio-only or stills")
            width: root.comboWidth
            model: [qsTr("All Media"), qsTr("Video"), qsTr("Audio Only"), qsTr("Stills")]
            currentIndex: root.mediaFilter
            onActivated: function (index) { root.mediaFilter = index }
            anchors.verticalCenter: parent.verticalCenter
        }
        ComboField {
            objectName: "typeFilter"
            tip: qsTr("Show only clips of one file type in use in the library")
            width: root.comboWidth
            model: [qsTr("All Types")].concat(root.typeOptions)
            currentIndex: Math.max(0, root.typeOptions.indexOf(root.typeFilter) + 1)
            onActivated: function (index) { root.typeFilter = index === 0 ? "" : root.typeOptions[index - 1] }
            anchors.verticalCenter: parent.verticalCenter
        }
        ComboField {
            objectName: "ratingFilter"
            tip: qsTr("Show only clips at a rating, or unrated, favourites or rejects")
            width: root.comboWidth + (root.tight ? 4 : 8)
            model: [qsTr("All Ratings"), "★★★★★", qsTr("★★★★ and up"), qsTr("★★★ and up"),
                    qsTr("Unrated"), qsTr("Favourites"), qsTr("Rejected")]
            currentIndex: root.ratingFilter
            onActivated: function (index) { root.ratingFilter = index }
            anchors.verticalCenter: parent.verticalCenter
        }
        ComboField {
            objectName: "tagFilter"
            tip: qsTr("Show only clips carrying the chosen tag")
            width: root.comboWidth + (root.tight ? 4 : 8)
            model: [qsTr("All Tags")].concat(root.tagOptions)
            currentIndex: Math.max(0, root.tagOptions.indexOf(root.tagFilter) + 1)
            onActivated: function (index) { root.tagFilter = index === 0 ? "" : root.tagOptions[index - 1] }
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Row {
        id: sorting
        objectName: "mediaSortControls"
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        y: root.stacked ? Theme.hControl + Theme.s2 * 2 : (root.height - height) / 2
        spacing: Theme.s2

        ComboField {
            objectName: "sortKey"
            tip: qsTr("Which property the browser puts the clips in order by")
            width: root.compact ? 96 : 118
            caption: root.compact ? "" : qsTr("Sort")
            model: [qsTr("Name"), qsTr("Created"), qsTr("Duration"), qsTr("Rating"), qsTr("Imported"), qsTr("Scene"), qsTr("Number")]
            currentIndex: root.sortKey
            onActivated: function (index) { root.sortKey = index }
            anchors.verticalCenter: parent.verticalCenter
        }
        IconButton {
            objectName: "sortDirection"
            iconName: root.sortDescending ? "chevron-down" : "chevron-up"
            text: root.sortDescending ? qsTr("Descending") : qsTr("Ascending")
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.sortDescending = !root.sortDescending
        }

        Rectangle { width: Theme.hairline; height: Theme.hControl; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

        SegmentedControl {
            objectName: "viewMode"
            tip: qsTr("Show the browser as a grid, a list or a filmstrip")
            icons: ["grid-2x2", "list", "film"]
            names: [qsTr("Grid"), qsTr("List"), qsTr("Filmstrip")]
            currentIndex: root.viewMode
            onActivated: function (index) { root.viewMode = index }
            anchors.verticalCenter: parent.verticalCenter
        }

        Icon { visible: !root.compact; name: "image"; size: Theme.szIcon - 4; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
        C.Slider {
            visible: !root.compact
            id: sizeSlider
            objectName: "thumbSize"
            C.ToolTip.text: qsTr("How large the thumbnails are drawn in grid view"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450
            width: 90
            from: 0; to: 1
            value: root.thumbSize
            enabled: root.viewMode === 0
            opacity: enabled ? 1 : Theme.disabledOpacity
            onMoved: root.thumbSize = value
            anchors.verticalCenter: parent.verticalCenter
            Accessible.name: qsTr("Thumbnail size")
            background: Rectangle {
                x: sizeSlider.leftPadding
                y: sizeSlider.topPadding + sizeSlider.availableHeight / 2 - height / 2
                width: sizeSlider.availableWidth
                height: 2
                radius: 1
                color: Theme.border
                Rectangle { width: sizeSlider.visualPosition * parent.width; height: parent.height; color: Theme.accent; radius: 1 }
            }
            handle: Rectangle {
                x: sizeSlider.leftPadding + sizeSlider.visualPosition * (sizeSlider.availableWidth - width)
                y: sizeSlider.topPadding + sizeSlider.availableHeight / 2 - height / 2
                width: 10; height: 10; radius: 5
                color: sizeSlider.pressed ? Theme.accentPressed : Theme.textPrimary
                border.width: sizeSlider.visualFocus ? Theme.focusRing : 0
                border.color: Theme.accent
            }
        }
        Icon { visible: !root.compact; name: "image"; size: Theme.szIcon; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
    }
}
