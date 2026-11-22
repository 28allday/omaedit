pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui

// A clip's video effects beside its picture: the list on the left, top to
// bottom as they run; on the right the clip alone (over a check where it is
// clear), its matte, or the timeline, and under it the selected effect's
// settings. Every change is one undo step, through the Inspector's own path.
ModalDialog {
    id: sheet
    objectName: "videoEffectsWindow"
    property var entry: null
    property var catalogue: []
    signal edited(string action, string uid, var values)
    signal gestureStarted()
    signal gestureFinished()

    title: entry ? qsTr("Effects for %1").arg(entry.title || entry.name || "") : qsTr("Effects")
    width: Math.min(1120, parent ? parent.width - Theme.s4 : 1120)
    height: Math.min(820, parent ? parent.height - Theme.s4 : 820)
    standardButtons: C.Dialog.Close

    readonly property var effects: entry && entry.videoEffects ? entry.videoEffects : []
    property string selectedUid: ""
    readonly property int selectedIndex: effects.findIndex(function(e) { return e.uid === selectedUid })
    readonly property var selected: selectedIndex >= 0 ? effects[selectedIndex] : null
    readonly property int clipFrame: entry ? Math.max(0, Math.min(entry.frames - 1, backend.position - entry.start)) : 0
    // The timeline rather than the clip alone: how light wrap is seen.
    property bool inTimeline: false
    property bool showMatte: false
    property bool picking: false
    readonly property bool canMatte: effects.some(function(e) {
        return !e.bypass && ["chroma-key", "colour-key", "luma-key", "remove-background"].indexOf(e.id) >= 0
    })
    readonly property bool matteShown: showMatte && canMatte && !inTimeline
    readonly property var groups: {
        const by = {}
        for (const d of catalogue) {
            if (d.available !== true) continue
            if (!by[d.cat]) by[d.cat] = {name: d.cat.charAt(0).toUpperCase() + d.cat.slice(1), items: []}
            by[d.cat].items.push(d)
        }
        // Keying first: what this window is most often opened for.
        return Object.values(by).sort(function(a, b) { return a.name === "Keying" ? -1 : b.name === "Keying" ? 1 : a.name.localeCompare(b.name) })
    }
    function nameOf(e) {
        const d = catalogue.find(function(x) { return x.id === e.id })
        return d ? d.name : e.id
    }
    function move(from, to) {
        const uid = effects[from].uid
        const step = to > from ? 1 : -1
        for (let i = from; i !== to; i += step) edited("move", uid, {direction: step})
    }
    // A new effect is the one selected, as is the last when the selected one goes.
    property var knownUids: []
    onEffectsChanged: {
        const uids = effects.map(function(e) { return e.uid })
        const added = uids.find(function(uid) { return knownUids.indexOf(uid) < 0 })
        if (added !== undefined && knownUids.length > 0) selectedUid = added
        else if (selectedIndex < 0 && uids.length > 0) selectedUid = uids[uids.length - 1]
        knownUids = uids
        settle.restart()
    }
    onSelectedChanged: {
        picking = false
        if (selected && selected.id === "light-wrap") inTimeline = true
    }
    // Not restarted: while playing, frames come faster than the interval.
    onClipFrameChanged: settle.start()
    onMatteShownChanged: settle.restart()
    onInTimelineChanged: settle.restart()
    onOpened: {
        if (selectedIndex < 0 && effects.length > 0) selectedUid = effects[effects.length - 1].uid
        settle.restart()
    }
    onClosed: { picking = false; backend.clearEffectPreview() }
    Timer { id: settle; interval: 60; onTriggered: sheet.ask() }
    // Space plays here too: a picture to watch the effect on.
    onVisibleChanged: Commands.transportInPopup = visible
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: sheet.visible
        onActivated: Commands.trigger("transport.play")
    }
    function ask() {
        if (!visible || !entry || inTimeline) { backend.clearEffectPreview(); return }
        const ratio = pictureBox.Window.window ? pictureBox.Window.window.devicePixelRatio : 1
        backend.requestEffectPreview(entry.track, entry.index, clipFrame, matteShown,
                                     Math.round(pictureBox.width * ratio), Math.round(pictureBox.height * ratio))
    }

    contentItem: RowLayout {
        spacing: Theme.s4
        // The list.
        ColumnLayout {
            Layout.preferredWidth: Math.round(sheet.width * .3)
            Layout.fillHeight: true
            spacing: Theme.s2
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("EFFECTS")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
                }
                ToolButton {
                    id: addButton
                    objectName: "addVideoEffect"
                    iconName: "plus"; showLabel: true
                    text: qsTr("Add effect")
                    tip: qsTr("Add an effect at the bottom of the list")
                    onClicked: addMenu.popup(addButton, 0, addButton.height)
                }
            }
            ContextMenu {
                id: addMenu
                objectName: "videoEffectAddMenu"
                Instantiator {
                    model: sheet.groups
                    delegate: ContextMenu {
                        id: groupMenu
                        required property var modelData
                        title: modelData.name
                        Instantiator {
                            model: groupMenu.modelData.items
                            delegate: MenuAction {
                                required property var modelData
                                objectName: "addVideoEffect:" + modelData.id
                                text: modelData.name
                                onTriggered: sheet.edited("add", "", {id: modelData.id})
                            }
                            onObjectAdded: function(index, object) { groupMenu.insertItem(index, object) }
                            onObjectRemoved: function(index, object) { groupMenu.removeItem(object) }
                        }
                    }
                    onObjectAdded: function(index, object) { addMenu.insertMenu(index, object) }
                    onObjectRemoved: function(index, object) { addMenu.removeMenu(object) }
                }
            }
            ListView {
                id: list
                objectName: "videoEffectList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: sheet.effects
                currentIndex: sheet.selectedIndex
                keyNavigationEnabled: false
                C.ScrollBar.vertical: ScrollBar { atRest: true }
                Keys.onDeletePressed: if (sheet.selected) sheet.edited("remove", sheet.selectedUid, {})
                Keys.onUpPressed: function(event) {
                    const i = sheet.selectedIndex
                    if (i <= 0) return
                    if (event.modifiers & Qt.ControlModifier) sheet.move(i, i - 1)
                    else sheet.selectedUid = sheet.effects[i - 1].uid
                }
                Keys.onDownPressed: function(event) {
                    const i = sheet.selectedIndex
                    if (i < 0 || i + 1 >= sheet.effects.length) return
                    if (event.modifiers & Qt.ControlModifier) sheet.move(i, i + 1)
                    else sheet.selectedUid = sheet.effects[i + 1].uid
                }
                // Where a dragged row would land: just above or below row `dropTarget`.
                property int dropTarget: -1
                property bool dropBelow: false
                function dropAt(from, contentY) {
                    const i = indexAt(Theme.s4, contentY)
                    if (i < 0 || i === from) { dropTarget = -1; return }
                    const item = itemAtIndex(i)
                    dropTarget = i
                    dropBelow = contentY > item.y + item.height / 2
                    dropLine.y = (dropBelow ? item.y + item.height : item.y) - contentY - dropLine.height / 2
                }
                Rectangle {
                    id: dropLine
                    z: 10
                    visible: list.dropTarget >= 0
                    x: Theme.s2
                    width: list.width - Theme.s4
                    height: Theme.selectionRing
                    color: Theme.accent
                }
                delegate: Rectangle {
                    id: row
                    required property int index
                    required property var modelData
                    objectName: "videoEffectRow:" + index
                    width: ListView.view.width
                    height: Theme.szIconHit + Theme.s1
                    readonly property bool chosen: sheet.selectedIndex === index
                    color: chosen ? Theme.panelRaised : "transparent"
                    border.width: chosen ? Theme.hairline : 0
                    border.color: Theme.accent
                    radius: Theme.rControl
                    opacity: dragger.active ? .6 : 1
                    HoverHandler { id: hover }
                    TapHandler { onTapped: { sheet.selectedUid = row.modelData.uid; list.forceActiveFocus() } }
                    DragHandler {
                        id: dragger
                        target: null
                        xAxis.enabled: false
                        onActiveChanged: {
                            if (active) { sheet.selectedUid = row.modelData.uid; return }
                            // Cleared first: the drop rebuilds this row.
                            const target = list.dropTarget, below = list.dropBelow, from = row.index
                            list.dropTarget = -1
                            if (target < 0) return
                            let to = target + (below ? 1 : 0)
                            if (from < to) --to
                            if (to !== from) sheet.move(from, to)
                        }
                        onCentroidChanged: if (active) list.dropAt(row.index, row.y + centroid.position.y)
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s1; anchors.rightMargin: Theme.s1
                        spacing: Theme.s1
                        IconButton {
                            objectName: "switchVideoEffect:" + row.index
                            iconName: row.modelData.bypass ? "eye-off" : "eye"
                            text: row.modelData.bypass ? qsTr("Turn on") : qsTr("Turn off (keeps its settings)")
                            onClicked: sheet.edited("update", row.modelData.uid, {bypass: !row.modelData.bypass})
                        }
                        Text {
                            Layout.fillWidth: true
                            text: sheet.nameOf(row.modelData)
                            elide: Text.ElideRight
                            color: row.modelData.bypass ? Theme.textMuted : Theme.textPrimary
                            font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                        }
                        IconButton {
                            objectName: "removeVideoEffect:" + row.index
                            iconName: "trash-2"
                            text: qsTr("Remove")
                            // Shown on the row under the pointer and the selected one.
                            opacity: hover.hovered || row.chosen ? 1 : 0
                            onClicked: sheet.edited("remove", row.modelData.uid, {})
                        }
                    }
                }
                Text {
                    anchors.fill: parent
                    anchors.margins: Theme.s4
                    visible: list.count === 0
                    text: qsTr("No effects yet. Press Add effect to add one; they run from the top of the list down.")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
                }
            }
            Text {
                Layout.fillWidth: true
                visible: sheet.effects.length > 1
                text: qsTr("Drag an effect up or down to change the order.")
                color: Theme.textMuted
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
            }
        }
        // The picture and the selected effect's settings.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s2
            Rectangle {
                id: pictureBox
                objectName: "videoEffectsPreviewBox"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(sheet.height * .42)
                color: "black"
                radius: Theme.rControl
                clip: true
                onWidthChanged: settle.restart()
                onHeightChanged: settle.restart()
                VideoView {
                    id: picture
                    objectName: "videoEffectsPreview"
                    anchors.fill: parent
                    source: backend
                    channel: sheet.inTimeline ? VideoView.Program : VideoView.EffectPreview
                    colorOptions: ({})
                    overlayPalette: Theme.viewerOverlayPalette
                }
                MouseArea {
                    objectName: "videoEffectsPickArea"
                    anchors.fill: parent
                    enabled: sheet.picking
                    cursorShape: sheet.picking ? Qt.CrossCursor : Qt.ArrowCursor
                    onClicked: function(mouse) {
                        // The picture letterboxed in the box, as the view fits it.
                        const aspect = sheet.entry && backend.videoHeight > 0 ? backend.videoWidth / backend.videoHeight : 16 / 9
                        const w = Math.min(width, height * aspect), h = w / aspect
                        const x = (mouse.x - (width - w) / 2) / w, y = (mouse.y - (height - h) / 2) / h
                        sheet.picking = false
                        if (x < 0 || y < 0 || x > 1 || y > 1 || !sheet.selected) return
                        backend.editVideoEffect(sheet.entry.track, sheet.entry.index, "pickBackdrop", sheet.selectedUid, {clipFrame: sheet.clipFrame, x: x, y: y})
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s3
                Toggle {
                    objectName: "videoEffectsInTimeline"
                    label: qsTr("In the timeline")
                    tip: qsTr("Show the timeline with this clip over what is behind it, where Light Wrap can be seen. Off shows the clip alone")
                    checked: sheet.inTimeline
                    onClicked: sheet.inTimeline = !sheet.inTimeline
                }
                Toggle {
                    objectName: "videoEffectsShowMatte"
                    visible: sheet.canMatte && !sheet.inTimeline
                    label: qsTr("Show matte")
                    tip: qsTr("See only what is kept (white) and taken out (black), here in this preview; the program is not touched")
                    checked: sheet.showMatte
                    onClicked: sheet.showMatte = !sheet.showMatte
                }
                Text {
                    Layout.fillWidth: true
                    visible: sheet.picking
                    text: qsTr("Click the backdrop in the preview")
                    color: Theme.accent
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
                }
            }
            C.ScrollView {
                C.ScrollBar.vertical: ScrollBar { atRest: true }
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
                VideoEffectStack {
                    objectName: "videoEffectsWindowSettings"
                    width: parent.width
                    reorderable: false
                    effects: sheet.selected ? [sheet.selected] : []
                    catalogue: sheet.catalogue
                    pickingUid: sheet.picking ? sheet.selectedUid : ""
                    animationFrame: sheet.entry ? sheet.clipFrame : -1
                    onEditRequested: function(action, uid, values) {
                        if (action === "pickBackdrop") {
                            sheet.picking = !sheet.picking
                            if (sheet.picking) { sheet.showMatte = false; sheet.inTimeline = false }
                            return
                        }
                        sheet.edited(action, uid, values)
                    }
                    onGestureStarted: sheet.gestureStarted()
                    onGestureFinished: sheet.gestureFinished()
                }
            }
        }
    }
}
