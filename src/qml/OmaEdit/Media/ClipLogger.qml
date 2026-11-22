pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui
import "../Ui/MediaTime.js" as MediaTime

// Logging for the previewed clip: In / Out, duration, markers, keywords,
// notes, and the pick / reject / subclip / insert / overwrite actions.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property var entry: ({})
    property bool hasMedia: false
    property int position: 0
    property real fps: 25
    property int frames: 0

    signal inRequested(int frame)
    signal outRequested(int frame)
    signal rangeCleared()
    signal markerAdded(int frame, string type, string text)
    signal markerRemoved(int frame)
    signal markerEdited(int oldFrame, int frame, string type, string text, string notes)
    signal markerActivated(int frame)
    signal keywordsEdited(var keywords)
    signal notesEdited(string notes)
    signal favoriteToggled(bool on)
    signal rejectedToggled(bool on)
    signal subclipRequested()
    signal insertRequested()
    signal overwriteRequested()

    readonly property bool textActive: markerPrompt.visible || keywordInput.activeFocus || notes.activeFocus
                                       || inField.active || outField.active
    readonly property int rIn: entry.rangeIn !== undefined ? entry.rangeIn : -1
    readonly property int rOut: entry.rangeOut !== undefined ? entry.rangeOut : -1
    readonly property int effIn: rIn >= 0 ? rIn : 0
    readonly property int effOut: rOut >= 0 ? rOut : Math.max(0, frames - 1)
    readonly property var markers: entry.clipMarkers !== undefined ? entry.clipMarkers : []
    readonly property var keywords: entry.keywords !== undefined ? entry.keywords : []
    property string markerType: "standard"

    function timecode(f) {
        var rate = fps > 0 ? fps : 25
        var s = Math.floor(f / rate)
        var ff = Math.round(f - s * rate)
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff)
    }

    function editMarker(marker) {
        markerPrompt.sourceId = entry.libraryId || entry.path
        markerPrompt.oldFrame = marker.frame
        markerFrame.frame = marker.frame
        markerText.text = marker.text || ""
        markerNotes.text = marker.notes || ""
        markerKind.currentIndex = Math.max(0, ["standard", "chapter", "todo", "done"].indexOf(marker.type))
        markerPrompt.open()
    }
    function stepMarker(direction) {
        if (!markers.length) return
        var choices = markers.filter(function (m) { return direction > 0 ? m.frame > root.position : m.frame < root.position })
        var marker = direction > 0 ? (choices[0] || markers[0]) : (choices[choices.length - 1] || markers[markers.length - 1])
        markerActivated(marker.frame)
    }
    color: Theme.panelBg

    Rectangle { anchors.left: parent.left; width: Theme.hairline; height: parent.height; color: Theme.border }
    Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: Theme.border }

    SectionTitle {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        height: logActions.inBar ? Theme.s1 : implicitHeight
        text: root.dockBar === null ? qsTr("Clip Log") : ""
    }

    DockBarActions {
        id: logActions
        dockBar: root.dockBar
        fallback: title
        fallbackMargin: 0

        IconButton {
            objectName: "favoriteButton"
            iconName: "heart"; text: qsTr("Favourite")
            enabled: root.hasMedia
            checkable: true
            checked: root.entry.favorite !== undefined && root.entry.favorite
            iconColor: checked ? Theme.audio : Theme.textSecondary
            onClicked: root.favoriteToggled(!(root.entry.favorite !== undefined && root.entry.favorite))
        }
        IconButton {
            objectName: "rejectButton"
            iconName: "thumbs-down"; text: qsTr("Reject")
            enabled: root.hasMedia
            checkable: true
            checked: root.entry.rejected !== undefined && root.entry.rejected
            iconColor: checked ? Theme.danger : Theme.textSecondary
            onClicked: root.rejectedToggled(!(root.entry.rejected !== undefined && root.entry.rejected))
        }
    }

    C.ScrollView {
        anchors.top: title.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        C.ScrollBar.vertical: ScrollBar {}
        enabled: root.hasMedia
        opacity: root.hasMedia ? 1 : Theme.disabledOpacity

        Column {
            width: parent.width
            spacing: Theme.s2

            // In / Out: two label+field pairs that wrap to two lines when the
            // panel is narrow, rather than truncating a timecode.
            Flow {
                width: parent.width
                spacing: Theme.s2

                Row {
                    spacing: Theme.s2
                    Text { width: 56; text: qsTr("In"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    TimecodeField {
                        id: inField
                        objectName: "inField"
                        tip: qsTr("First frame of the source range sent to the timeline")
                        width: 98
                        frame: root.effIn
                        fps: root.fps
                        label: qsTr("In point")
                        onEdited: function (f) { root.inRequested(root.frames > 0 ? Math.max(0, Math.min(root.frames - 1, f)) : f) }
                    }
                }
                Row {
                    spacing: Theme.s2
                    Text { width: 24; text: qsTr("Out"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                    TimecodeField {
                        id: outField
                        objectName: "outField"
                        tip: qsTr("Last frame of the source range sent to the timeline")
                        width: 98
                        frame: root.effOut
                        fps: root.fps
                        label: qsTr("Out point")
                        onEdited: function (f) { root.outRequested(root.frames > 0 ? Math.max(0, Math.min(root.frames - 1, f)) : f) }
                    }
                    IconButton {
                        iconName: "rotate-ccw"; text: qsTr("Clear range")
                        enabled: root.rIn >= 0 || root.rOut >= 0
                        onClicked: root.rangeCleared()
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Row {
                spacing: Theme.s2
                Text { width: 56; text: qsTr("Duration"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    objectName: "rangeDuration"
                    text: root.frames > 0 ? MediaTime.durationText(root.effOut - root.effIn + 1, root.fps) : "–"
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fsControl; color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    visible: (root.rIn >= 0 || root.rOut >= 0) && root.width >= 380
                    text: qsTr("(marked — I / O at the playhead)")
                    font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: Theme.s2
                width: parent.width
                Text { width: 56; text: qsTr("Markers"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Flow {
                    width: Math.max(40, parent.width - 56 - 94 - Theme.szIconHit - Theme.s2 * 3)
                    spacing: Theme.s1
                    anchors.verticalCenter: parent.verticalCenter
                    Repeater {
                        model: root.markers
                        delegate: Rectangle {
                            required property var modelData
                            objectName: "clipMarker:" + modelData.frame
                            width: 14; height: 14
                            radius: 2
                            color: Theme.markerTint(modelData)
                            border.width: Math.abs(root.position - modelData.frame) < 1 ? 2 : 0
                            border.color: Theme.textPrimary
                            HoverHandler { id: markerHover }
                            TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.markerActivated(modelData.frame); onDoubleTapped: root.editMarker(modelData) }
                            TapHandler { acceptedButtons: Qt.RightButton; onTapped: root.markerRemoved(modelData.frame) }
                            Tooltip {
                                text: root.timecode(modelData.frame) + (modelData.text ? " — " + modelData.text : "")
                                shortcut: qsTr("double-click edits · right-click removes")
                                visible: markerHover.hovered
                            }
                        }
                    }
                    Text {
                        visible: root.markers.length === 0
                        text: qsTr("none")
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted
                    }
                }
                ComboField {
                    objectName: "markerType"
                    tip: qsTr("Kind, and so colour, of the next marker you add")
                    width: 94
                    model: [qsTr("Standard"), qsTr("Chapter"), qsTr("To Do"), qsTr("Done")]
                    currentIndex: ["standard", "chapter", "todo", "done"].indexOf(root.markerType)
                    onActivated: function (i) { root.markerType = ["standard", "chapter", "todo", "done"][i] }
                    anchors.verticalCenter: parent.verticalCenter
                }
                IconButton {
                    objectName: "addMarkerButton"
                    iconName: "plus"; text: qsTr("Add marker at the playhead")
                    onClicked: root.markerAdded(root.position, root.markerType, "")
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: Theme.s2
                IconButton { iconName: "chevron-left"; text: qsTr("Previous marker"); enabled: root.markers.length > 0; onClicked: root.stepMarker(-1) }
                IconButton { iconName: "chevron-right"; text: qsTr("Next marker"); enabled: root.markers.length > 0; onClicked: root.stepMarker(1) }
                ToolButton { text: qsTr("Edit Marker…"); enabled: root.markers.some(function (m) { return m.frame === root.position }); onClicked: root.editMarker(root.markers.find(function (m) { return m.frame === root.position })) }
            }
            Row {
                spacing: Theme.s2
                width: parent.width
                Text { width: 56; text: qsTr("Keywords"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Flow {
                    width: parent.width - 56 - Theme.s2
                    spacing: Theme.s1
                    Repeater {
                        model: root.keywords
                        delegate: Chip {
                            required property string modelData
                            tip: qsTr("A keyword logged on this clip; the cross removes it")
                            label: modelData
                            removable: true
                            onRemoveRequested: root.keywordsEdited(root.keywords.filter(function (k) { return k !== modelData }))
                        }
                    }
                    Rectangle {
                        width: Math.max(80, keywordInput.contentWidth + Theme.s3 + 16)
                        height: 18
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: keywordInput.activeFocus ? Theme.focusRing : Theme.hairline
                        border.color: keywordInput.activeFocus ? Theme.accent : Theme.border
                        Icon { anchors.left: parent.left; anchors.leftMargin: 3; anchors.verticalCenter: parent.verticalCenter; name: "plus"; size: 10; color: Theme.textMuted }
                        TextInput {
                            id: keywordInput
                            objectName: "keywordInput"
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: Theme.s1
                            verticalAlignment: TextInput.AlignVCenter
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsLabel - 1
                            color: Theme.textPrimary
                            selectionColor: Theme.accent
                            clip: true
                            Accessible.name: qsTr("Add keyword")
                            Text {
                                anchors.fill: parent
                                visible: !keywordInput.text && !keywordInput.activeFocus
                                text: qsTr("Add keyword")
                                verticalAlignment: Text.AlignVCenter
                                font: keywordInput.font
                                color: Theme.textMuted
                            }
                            onAccepted: {
                                var t = text.trim()
                                if (t !== "") {
                                    var next = root.keywords.slice()
                                    if (next.indexOf(t) < 0) next.push(t)
                                    root.keywordsEdited(next)
                                }
                                text = ""
                            }
                            Keys.onEscapePressed: { text = ""; focus = false }
                        }
                    }
                }
            }

            Row {
                spacing: Theme.s2
                width: parent.width
                Text { width: 56; text: qsTr("Notes"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted; y: Theme.s1 }
                Rectangle {
                    width: parent.width - 56 - Theme.s2
                    height: Math.max(Theme.hControl * 2, notes.implicitHeight + Theme.s1)
                    radius: Theme.rControl
                    color: Theme.controlBg
                    border.width: notes.activeFocus ? Theme.focusRing : Theme.hairline
                    border.color: notes.activeFocus ? Theme.accent : Theme.border
                    C.TextArea {
                        id: notes
                        objectName: "notesField"
                        anchors.fill: parent
                        text: root.entry.notes !== undefined ? root.entry.notes : ""
                        placeholderText: qsTr("Describe the shot…")
                        placeholderTextColor: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsLabel
                        color: Theme.textPrimary
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentText
                        wrapMode: TextEdit.Wrap
                        leftPadding: Theme.s1; rightPadding: Theme.s1; topPadding: 3; bottomPadding: 3
                        background: null
                        Accessible.name: qsTr("Notes")
                        onActiveFocusChanged: if (!activeFocus) commit()
                        Keys.onEscapePressed: { text = Qt.binding(() => root.entry.notes !== undefined ? root.entry.notes : ""); focus = false }
                        function commit() {
                            var current = root.entry.notes !== undefined ? root.entry.notes : ""
                            if (text !== current) root.notesEdited(text)
                        }
                    }
                }
            }

            Row {
                spacing: Theme.s2
                ToolButton { objectName: "subclipButton"; showLabel: true; iconName: "scissors"; text: qsTr("Subclip"); onClicked: root.subclipRequested() }
                ToolButton { objectName: "insertButton"; showLabel: true; iconName: "arrow-left-right"; text: qsTr("Insert"); shortcut: "W"; onClicked: root.insertRequested() }
                ToolButton { objectName: "overwriteButton"; showLabel: true; iconName: "square"; text: qsTr("Overwrite"); shortcut: "D"; onClicked: root.overwriteRequested() }
            }
        }
    }
    ModalDialog {
        id: markerPrompt
        title: qsTr("Edit Clip Marker")
        property string sourceId: ""
        property int oldFrame: -1
        onAccepted: if (sourceId === (root.entry.libraryId || root.entry.path))
            root.markerEdited(oldFrame, markerFrame.frame, ["standard", "chapter", "todo", "done"][markerKind.currentIndex], markerText.text, markerNotes.text)
        ColumnLayout {
            width: parent.width
            TimecodeField { id: markerFrame; tip: qsTr("Frame in the source this marker sits on"); fps: root.fps; label: qsTr("Marker position"); onEdited: function (f) { frame = Math.max(0, Math.min(root.frames - 1, f)) } }
            ComboField { id: markerKind; tip: qsTr("Kind of marker, which sets the colour it is drawn in"); Layout.fillWidth: true; model: [qsTr("Standard"), qsTr("Chapter"), qsTr("To Do"), qsTr("Done")] }
            LineEdit { id: markerText; tip: qsTr("Short name shown beside this marker"); Layout.fillWidth: true; hint: qsTr("Marker name") }
            LineEdit { id: markerNotes; tip: qsTr("Longer notes kept with this marker"); Layout.fillWidth: true; hint: qsTr("Notes") }
        }
    }
}
