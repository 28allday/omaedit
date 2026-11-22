import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// The sequence tabs above the timeline: one tab per timeline in the
// project, the active one lit. Click switches, + adds, right-click (or
// double-click) renames, duplicates, closes or deletes. Switching starts a
// fresh undo history, so the selection is dropped first.
Rectangle {
    id: sequenceBar
    objectName: "sequenceBar"
    Layout.fillWidth: true
    implicitHeight: 34
    readonly property bool renameOpen: renameDialog.visible || binDialog.visible
    color: Ui.Theme.panelBg

    // Tabs closed this session, by timeline id. A closed timeline stays in
    // the project and its bin, untouched; opening it there (or stepping back
    // to it) gives it its tab again.
    property var closedTabs: []
    property string closingTab: "" // the shown one, closed once a neighbour is shown
    function tabOpen(index) {
        const timeline = backend.sequences[index]
        return timeline !== undefined && (index === backend.activeSequence
            || (timeline.synced !== true && closedTabs.indexOf(timeline.id) < 0))
    }
    readonly property int openTabs: {
        var count = 0
        for (var i = 0; i < backend.sequences.length; ++i) if (tabOpen(i)) ++count
        return count
    }
    // The shown one cannot hand over while a render runs (the switch waits for it).
    function canClose(index) {
        return openTabs > 1 && tabOpen(index) && (index !== backend.activeSequence || !backend.exporting)
    }
    function closeTab(index) {
        if (!canClose(index)) return
        const id = backend.sequences[index].id
        if (index !== backend.activeSequence) { closedTabs = closedTabs.concat([id]); return }
        // The shown one hands over to the tab on its left, or else its right.
        var to = -1
        for (var i = index - 1; i >= 0 && to < 0; --i) if (tabOpen(i)) to = i
        for (var j = index + 1; j < backend.sequences.length && to < 0; ++j) if (tabOpen(j)) to = j
        if (to < 0) return
        closingTab = id
        win.selection.clearSelection()
        backend.switchSequence(to)
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Ui.Theme.hairline
        color: Ui.Theme.border
    }

    Flickable {
        id: tabsScroll
        anchors.left: parent.left; anchors.right: addTimeline.left
        anchors.leftMargin: Ui.Theme.s2; anchors.rightMargin: Ui.Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        height: 26; contentWidth: tabsRow.width; contentHeight: height
        clip: true; boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        Row {
        id: tabsRow
        spacing: Ui.Theme.s1

        Repeater {
            id: timelineTabs
            model: backend.hasTimeline ? backend.sequences : []
            delegate: Rectangle {
                id: tab
                required property var modelData
                required property int index
                objectName: "sequenceTab:" + index
                readonly property bool active: index === backend.activeSequence
                // A synced clip lives in the library; it has a tab only while open.
                visible: sequenceBar.tabOpen(index)
                width: Math.min(220, tabLabel.implicitWidth + Ui.Theme.s3 * 2)
                height: 22
                radius: Ui.Theme.rControl
                color: active ? Ui.Theme.panelRaised : "transparent"
                Text {
                    id: tabLabel
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - Ui.Theme.s3 * 2)
                    elide: Text.ElideRight
                    text: tab.modelData.name
                    font.family: Ui.Theme.fontFamily
                    font.pixelSize: Ui.Theme.fsLabel
                    font.weight: tab.active ? Ui.Theme.wHeading : Font.Normal
                    color: tab.active ? Ui.Theme.textPrimary : Ui.Theme.textMuted
                }
                Rectangle { // the active underline
                    visible: tab.active
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - Ui.Theme.s2
                    height: 2
                    color: Ui.Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !backend.projectBusy && !backend.exporting
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 600
                    ToolTip.text: tab.modelData.name
                    onClicked: function (mouse) {
                        if (mouse.button === Qt.RightButton) {
                            tabMenu.tabIndex = tab.index
                            tabMenu.popup()
                        } else if (!tab.active) {
                            win.selection.clearSelection()
                            backend.switchSequence(tab.index)
                        }
                    }
                    // The active tab only: the first click of a double-click
                    // on another tab has already switched to it (dropping the
                    // selection and the undo history); Rename lives on the
                    // right-click menu for those.
                    onDoubleClicked: function (mouse) {
                        if (mouse.button === Qt.LeftButton && tab.active)
                            renameDialog.openFor(tab.index, tab.modelData.name)
                    }
                }
            }
        }

        }
    }
    Ui.ToolButton {
        id: addTimeline
        objectName: "addSequenceButton"
        anchors.right: parent.right; anchors.rightMargin: Ui.Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("+ New Timeline")
        enabled: !backend.projectBusy && !backend.exporting
        onClicked: Ui.Commands.trigger("sequence.new")
    }
    Connections {
        target: backend
        function onSequencesChanged() {
            const shown = backend.sequences[backend.activeSequence]
            if (shown !== undefined) {
                var closed = sequenceBar.closedTabs
                if (sequenceBar.closingTab !== "" && shown.id !== sequenceBar.closingTab) {
                    closed = closed.concat([sequenceBar.closingTab])
                    sequenceBar.closingTab = ""
                }
                if (closed.indexOf(shown.id) >= 0) closed = closed.filter(function (id) { return id !== shown.id })
                if (closed !== sequenceBar.closedTabs) sequenceBar.closedTabs = closed
            }
            Qt.callLater(function() {
                var item = timelineTabs.itemAt(backend.activeSequence)
                if (!item) return
                if (item.x < tabsScroll.contentX) tabsScroll.contentX = item.x
                else if (item.x + item.width > tabsScroll.contentX + tabsScroll.width)
                    tabsScroll.contentX = Math.max(0, item.x + item.width - tabsScroll.width)
            })
        }
    }

    // The timeline tiles offer the same menu as the tabs, on every page.
    function openMenu(index) {
        tabMenu.tabIndex = index
        tabMenu.popup()
    }
    AppMenu {
        id: tabMenu
        objectName: "timelineMenu"
        parent: Overlay.overlay
        property int tabIndex: -1
        // Counted as the menu opens: what Collapse Multicam would change.
        property int multicamPieces: 0
        onAboutToShow: multicamPieces = backend.multicamPiecesIn(tabIndex)
        KeyMenuItem {
            text: qsTr("Open Timeline")
            enabled: tabMenu.tabIndex !== backend.activeSequence
            onTriggered: {
                win.selection.clearSelection()
                backend.switchSequence(tabMenu.tabIndex)
            }
        }
        KeyMenuItem {
            objectName: "closeTimelineTab"
            text: qsTr("Close Timeline")
            enabled: sequenceBar.canClose(tabMenu.tabIndex)
            onTriggered: sequenceBar.closeTab(tabMenu.tabIndex)
        }
        KeyMenuItem {
            text: qsTr("Rename…")
            onTriggered: renameDialog.openFor(tabMenu.tabIndex,
                                              backend.sequences[tabMenu.tabIndex].name)
        }
        KeyMenuItem {
            text: qsTr("Move to Bin…")
            enabled: binDialog.destinations.length > 1
            onTriggered: binDialog.openFor(tabMenu.tabIndex)
        }
        KeyMenuItem {
            text: qsTr("Duplicate Timeline")
            onTriggered: {
                win.selection.clearSelection()
                backend.duplicateSequence(tabMenu.tabIndex)
            }
        }
        KeyMenuItem {
            // Every multicam piece on the timeline becomes an ordinary clip
            // of the angle it shows; the timeline need not be the open one.
            objectName: "collapseMulticam"
            text: qsTr("Collapse Multicam")
            enabled: tabMenu.multicamPieces > 0
            onTriggered: backend.collapseMulticam(tabMenu.tabIndex)
        }
        KeyMenuItem {
            text: qsTr("Insert as Clip at Playhead")
            enabled: tabMenu.tabIndex !== backend.activeSequence
            onTriggered: {
                backend.viewTimeline()
                backend.nestSequence(tabMenu.tabIndex)
            }
        }
        KeyMenuItem {
            text: backend.stackedSequence === tabMenu.tabIndex ? qsTr("Unstack") : qsTr("Stack Above Timeline")
            enabled: tabMenu.tabIndex !== backend.activeSequence
            onTriggered: backend.stackedSequence = backend.stackedSequence === tabMenu.tabIndex ? -1 : tabMenu.tabIndex
        }
        KeyMenuItem {
            text: qsTr("Delete Timeline…")
            enabled: backend.sequences.length > 1
            onTriggered: {
                deleteDialog.timelineId = backend.sequences[tabMenu.tabIndex].id
                deleteDialog.timelineName = backend.sequences[tabMenu.tabIndex].name
                deleteDialog.open()
            }
        }
    }

    Ui.MessageDialog {
        id: deleteDialog
        objectName: "deleteTimelineDialog"
        title: qsTr("Delete Timeline?")
        property string timelineId: ""
        property string timelineName: ""
        message: qsTr("Delete “%1” from this project?").arg(timelineName)
        detail: qsTr("Its edits will be removed. Source media stays in the project. This cannot be undone.")
        standardButtons: Dialog.NoButton
        footer: Ui.DialogButtons {
            Ui.ToolButton { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Ui.ToolButton { text: qsTr("Delete Timeline"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
        onAccepted: {
            for (var i=0; i<backend.sequences.length; ++i)
                if (backend.sequences[i].id === timelineId) {
                    win.selection.clearSelection(); backend.closeSequence(i); break
                }
        }
    }

    Ui.FormDialog {
        width: Math.min(420, parent ? parent.width - Ui.Theme.s5 * 2 : 420)
        id: binDialog
        objectName: "timelineBinDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Move Timeline to Bin")
        standardButtons: Dialog.Ok | Dialog.Cancel
        property int tabIndex: -1
        readonly property var destinations: {
            const revision = backend.mediaCatalog.bins
            return [{id: "", name: qsTr("Library root")}].concat(backend.mediaCatalog.tree().filter(function (bin) { return !bin.smart }))
        }
        function openFor(index) {
            tabIndex = index
            const current = backend.sequences[index].binId || ""
            binField.currentIndex = Math.max(0, destinations.findIndex(function (bin) { return bin.id === current }))
            open()
        }
        Ui.ComboField {
            id: binField
            objectName: "timelineBinField"
            tip: qsTr("Bin this timeline is filed in")
            Layout.fillWidth: true
            caption: qsTr("Destination")
            model: binDialog.destinations.map(function (bin) { return "  ".repeat(bin.depth || 0) + bin.name })
        }
        onAccepted: backend.moveSequenceToBin(tabIndex, destinations[binField.currentIndex].id)
    }

    Ui.FormDialog {
        width: Math.min(420, parent ? parent.width - Ui.Theme.s5 * 2 : 420)
        id: renameDialog
        objectName: "renameSequenceDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Rename Timeline")
        standardButtons: Dialog.Ok | Dialog.Cancel
        property int tabIndex: -1
        function openFor(index, name) {
            tabIndex = index
            nameField.text = name
            open()
        }
        Ui.LineEdit {
            id: nameField
            objectName: "sequenceNameField"
            hoverEnabled: true
            tip: qsTr("New name for this timeline and its tab")
            Layout.fillWidth: true
            hint: qsTr("Timeline name")
            onAccepted: renameDialog.accept()
        }
        onAboutToShow: { nameField.forceActiveFocus(); nameField.selectAll() }
        onAccepted: if (nameField.text.trim() !== "")
                        backend.renameSequence(tabIndex, nameField.text.trim())
    }
}
