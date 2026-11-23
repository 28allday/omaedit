import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// The caption list (Window ▸ Panels ▸ Caption List): one row for each
// caption of the timeline, in time order — its start, end and length, its
// track's language, its speaker in their colour, its words to edit in place
// (Enter keeps them, Shift+Enter starts a new line) and anything the caption
// checks find, with the reason. Filters by track and by speaker, a search
// and Replace; a row clicked takes the playhead to the caption and selects
// it (Ctrl adds it to the selection). The menu holds the timing tools and
// the speakers. Rows follow every edit, made here or anywhere else.
Item {
    id: root
    objectName: "captionList"
    clip: true

    // The filters: a caption track (-1 every one), a speaker ("*" every one,
    // "" none), problems only.
    property int trackFilter: -1
    property string speakerFilter: "*"
    property bool problemsOnly: false
    readonly property var trackChoices: {
        const choices = [{ track: -1, label: qsTr("Every caption track") }]
        for (let t = 0; t < backend.tracks.length; ++t) {
            const info = backend.tracks[t]
            if (!info.mix || info.mix.editKind !== "caption") continue
            const language = backend.captionTrackLanguage(t)
            choices.push({ track: t, label: language.code && language.code !== "und"
                                            ? qsTr("%1 — %2").arg(info.name).arg(language.shown) : info.name })
        }
        return choices
    }
    readonly property var speakers: {
        const settings = backend.captionSettings, entries = backend.connected
        return backend.captionSpeakers()
    }
    readonly property var speakerChoices: [{ key: "*", label: qsTr("Every speaker") }, { key: "", label: qsTr("No speaker") }]
                                          .concat(speakers.map(function (s) { return { key: s.name, label: s.name } }))
    readonly property var filter: {
        const f = { track: trackFilter, search: searchField.text, problems: problemsOnly }
        if (speakerFilter !== "*") f.speaker = speakerFilter
        return f
    }
    readonly property var rows: {
        const settings = backend.captionSettings, entries = backend.connected, tracks = backend.tracks, rate = backend.fps
        return backend.captionList(filter)
    }
    readonly property int problemCount: rows.filter(function (r) { return r.problems.length > 0 }).length
    // The captions the timing tools and speakers work on: those selected,
    // else the rows shown.
    function chosenIndices() {
        const selected = win.clipCommands.selectedCaptionIndices()
        return selected.length > 0 ? selected : rows.map(function (r) { return r.index })
    }
    function isSelected(row) {
        if (row.index === win.selectedConnectedIndex) return true
        const entry = backend.connected[row.index]
        return entry ? win.selection.isSelectedAt(entry.track, entry.index) : false
    }
    function choose(row, add) {
        const entry = backend.connected[row.index]
        if (!entry) return
        if (add && win.selectedConnectedIndex >= 0) {
            win.selection.toggleSelect(entry.track, entry.index)
            return
        }
        backend.viewTimeline()
        backend.seek(row.start)
        win.selectedEdge = -1
        win.selection.selectOnly(entry.track, entry.index, true)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Ui.Theme.s2
        spacing: Ui.Theme.s2

        RowLayout {
            Layout.fillWidth: true
            spacing: Ui.Theme.s2
            Ui.ComboField {
                id: trackBox
                objectName: "captionListTrack"
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                implicitWidth: 120
                tip: qsTr("Show the captions of one caption track (one language), or of every one")
                model: root.trackChoices.map(function (c) { return c.label })
                onActivated: function (i) { root.trackFilter = root.trackChoices[i].track }
                // A new list of tracks keeps the one chosen (every one when it went).
                onModelChanged: {
                    const at = root.trackChoices.findIndex(function (c) { return c.track === root.trackFilter })
                    if (at < 0) root.trackFilter = -1
                    currentIndex = Math.max(0, at)
                }
            }
            Ui.ComboField {
                id: speakerBox
                objectName: "captionListSpeaker"
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                implicitWidth: 120
                tip: qsTr("Show the captions of one speaker, or those with none")
                model: root.speakerChoices.map(function (c) { return c.label })
                onActivated: function (i) { root.speakerFilter = root.speakerChoices[i].key }
                onModelChanged: {
                    const at = root.speakerChoices.findIndex(function (c) { return c.key === root.speakerFilter })
                    if (at < 0) root.speakerFilter = "*"
                    currentIndex = Math.max(0, at)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Ui.Theme.s2
            Ui.LineEdit {
                id: searchField
                objectName: "captionListSearch"
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                implicitWidth: 120
                hint: qsTr("Find")
                tip: qsTr("Narrows the list to captions with these words; Replace changes them")
            }
            FlatButton {
                objectName: "captionListProblems"
                text: qsTr("Problems")
                tip: qsTr("Show only the captions the checks find a problem with (Caption Style ▸ Rules sets the limits)")
                quiet: true
                implicitHeight: 28
                implicitWidth: implicitContentWidth + 16
                highlighted: root.problemsOnly
                onClicked: root.problemsOnly = !root.problemsOnly
            }
            FlatButton {
                objectName: "captionListMenuButton"
                text: qsTr("Timing ▾")
                tip: qsTr("Timing, speakers and rules for the selected captions, or for those listed when none is selected")
                quiet: true
                implicitHeight: 28
                implicitWidth: implicitContentWidth + 16
                onClicked: listMenu.popup(this, 0, height)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Ui.Theme.s2
            Ui.LineEdit {
                id: replaceField
                objectName: "captionListReplaceWith"
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                implicitWidth: 120
                hint: qsTr("Replace with")
                tip: qsTr("The words Replace and Replace All put in place of those found")
            }
            FlatButton {
                objectName: "captionListReplace"
                text: qsTr("Replace")
                tip: qsTr("Replace the words found in the selected caption (or the first listed). One undo step.")
                quiet: true
                implicitHeight: 28
                implicitWidth: implicitContentWidth + 16
                enabled: searchField.text !== "" && root.rows.length > 0
                onClicked: {
                    let target = -1
                    for (let i = 0; i < root.rows.length && target < 0; ++i)
                        if (root.isSelected(root.rows[i])) target = root.rows[i].index
                    if (target < 0) target = root.rows[0].index
                    backend.replaceCaptionText(searchField.text, replaceField.text, [target], false)
                }
            }
            FlatButton {
                objectName: "captionListReplaceAll"
                text: qsTr("Replace All")
                tip: qsTr("Replace the words found in every caption listed. One undo step.")
                quiet: true
                implicitHeight: 28
                implicitWidth: implicitContentWidth + 16
                enabled: searchField.text !== "" && root.rows.length > 0
                onClicked: backend.replaceCaptionText(searchField.text, replaceField.text,
                                                      root.rows.map(function (r) { return r.index }), false)
            }
        }

        Label {
            objectName: "captionListSummary"
            Layout.fillWidth: true
            color: Ui.Theme.textMuted
            font.pixelSize: Ui.Theme.fsLabel
            elide: Text.ElideRight
            text: root.problemCount > 0
                  ? qsTr("%n caption(s) listed, %1 with a problem", "", root.rows.length).arg(root.problemCount)
                  : qsTr("%n caption(s) listed", "", root.rows.length)
        }

        ListView {
            id: list
            objectName: "captionListRows"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Ui.ScrollBar {}
            // A new list (any edit) keeps the place it was scrolled to.
            property real keptY: 0
            onContentYChanged: if (!moving && count === 0) keptY = 0
            onModelChanged: { const y = keptY; Qt.callLater(function () { list.contentY = Math.max(0, Math.min(y, list.contentHeight - list.height)) }) }
            onMovementEnded: keptY = contentY

            delegate: Rectangle {
                id: rowItem
                required property var modelData
                required property int index
                readonly property bool chosen: root.isSelected(modelData)
                readonly property bool underPlayhead: backend.position >= modelData.start && backend.position < modelData.end
                objectName: "captionRow:" + modelData.index
                width: list.width - 8
                height: rowColumn.implicitHeight + Ui.Theme.s2 * 2
                radius: Ui.Theme.rControl
                color: chosen ? Qt.rgba(Ui.Theme.accent.r, Ui.Theme.accent.g, Ui.Theme.accent.b, 0.22)
                              : underPlayhead ? Qt.rgba(Ui.Theme.accent.r, Ui.Theme.accent.g, Ui.Theme.accent.b, 0.10)
                                              : Ui.Theme.panelRaised
                border.width: modelData.problems.length > 0 ? 1 : 0
                border.color: Ui.Theme.warning

                Rectangle { // the speaker's colour down the left edge
                    visible: rowItem.modelData.colour !== ""
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    width: 4
                    radius: 2
                    color: rowItem.modelData.colour || "transparent"
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: function (mouse) { root.choose(rowItem.modelData, (mouse.modifiers & Qt.ControlModifier) !== 0) }
                }

                ColumnLayout {
                    id: rowColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: Ui.Theme.s3
                    anchors.rightMargin: Ui.Theme.s2
                    anchors.topMargin: Ui.Theme.s2
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: backend.startFrame >= 0 && backend.fps >= 0
                              ? qsTr("%1 → %2").arg(backend.timecode(rowItem.modelData.start)).arg(backend.timecode(rowItem.modelData.end)) : ""
                        color: Ui.Theme.textSecondary
                        font.family: Ui.Theme.monoFamily
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Ui.Theme.s2
                        Label {
                            text: qsTr("%1 s").arg(Number(rowItem.modelData.seconds).toFixed(2))
                            color: Ui.Theme.textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            visible: rowItem.modelData.language.code !== "und"
                            text: rowItem.modelData.language.shown
                            color: Ui.Theme.textMuted
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.maximumWidth: 110
                        }
                        Label {
                            objectName: "captionRowSpeaker:" + rowItem.modelData.index
                            visible: rowItem.modelData.speaker !== ""
                            text: rowItem.modelData.speaker
                            color: rowItem.modelData.colour || Ui.Theme.textPrimary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Item { Layout.fillWidth: rowItem.modelData.speaker === "" }
                        Label {
                            objectName: "captionRowMark:" + rowItem.modelData.index
                            visible: rowItem.modelData.problems.length > 0
                            text: "⚠ " + rowItem.modelData.problems.length
                            color: Ui.Theme.warning
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }

                    TextArea {
                        id: words
                        objectName: "captionRowText:" + rowItem.modelData.index
                        Layout.fillWidth: true
                        Layout.preferredWidth: rowColumn.width
                        text: rowItem.modelData.text
                        wrapMode: TextEdit.Wrap
                        color: Ui.Theme.textPrimary
                        font.pixelSize: Ui.Theme.fsBase
                        selectByMouse: true
                        padding: 4
                        topPadding: 4
                        bottomPadding: 4
                        leftPadding: 4
                        rightPadding: 4
                        topInset: 0
                        bottomInset: 0
                        leftInset: 0
                        rightInset: 0
                        background: Rectangle {
                            color: words.activeFocus ? Ui.Theme.controlBg : "transparent"
                            radius: Ui.Theme.rControl
                            border.width: words.activeFocus ? 1 : 0
                            border.color: Ui.Theme.accent
                        }
                        Accessible.name: qsTr("Caption words")
                        // Enter keeps the words (one undo step); Shift+Enter
                        // a new line; Esc puts them back as they were.
                        function commit(event) {
                            if (event.modifiers & Qt.ShiftModifier) { event.accepted = false; return }
                            if (text !== rowItem.modelData.text) backend.setCaptionText(rowItem.modelData.index, text)
                            words.focus = false
                        }
                        Keys.onReturnPressed: function (event) { commit(event) }
                        Keys.onEnterPressed: function (event) { commit(event) }
                        Keys.onEscapePressed: { text = rowItem.modelData.text; words.focus = false }
                        onActiveFocusChanged: if (activeFocus && !rowItem.chosen) root.choose(rowItem.modelData, false)
                    }

                    Label {
                        objectName: "captionRowProblems:" + rowItem.modelData.index
                        visible: rowItem.modelData.problems.length > 0
                        Layout.fillWidth: true
                        Layout.preferredWidth: rowColumn.width
                        wrapMode: Text.WordWrap
                        color: Ui.Theme.warning
                        font.pixelSize: 11
                        text: rowItem.modelData.problems.map(function (p) { return p.reason }).join(" · ")
                    }
                }
            }

            Label {
                visible: list.count === 0
                anchors.centerIn: parent
                width: parent.width - Ui.Theme.s5 * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Ui.Theme.textMuted
                font.pixelSize: Ui.Theme.fsLabel
                text: backend.captionCount > 0 ? qsTr("No caption matches")
                                               : qsTr("No captions yet: Edit ▸ Captions adds, imports or generates them")
            }
        }
    }

    Ui.ContextMenu {
        id: listMenu
        objectName: "captionListMenu"
        Ui.MenuAction {
            text: qsTr("Snap Edges to Cuts…")
            onTriggered: captionTimingDialog.openFor("snap", root.chosenIndices())
        }
        Ui.MenuAction {
            text: qsTr("Extend to the Next Caption")
            onTriggered: backend.extendCaptionsToNext(root.chosenIndices())
        }
        Ui.MenuAction {
            text: qsTr("Close Small Gaps…")
            onTriggered: captionTimingDialog.openFor("gaps", root.chosenIndices())
        }
        Ui.MenuAction {
            text: qsTr("Shift Captions…")
            onTriggered: captionTimingDialog.openFor("shift", root.chosenIndices())
        }
        MenuSeparator {}
        Ui.MenuAction {
            text: qsTr("Speakers…")
            onTriggered: captionSpeakerDialog.openFor(win.clipCommands.selectedCaptionIndices())
        }
        Ui.MenuAction {
            text: qsTr("Caption Rules…")
            onTriggered: captionStyleDialog.openRules()
        }
    }
}
