import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Edit ▸ Captions ▸ Caption Style…: the look of a caption track's captions
// (the selected caption's track, or the one whose menu opened it), or of
// every caption track's, and the lengths Generate Captions makes them. OK
// restyles the captions that still follow the track's look (or all of them)
// in one undo step; Update Captions makes them again from the transcripts.
// Its Rules are the timeline's: the limits the caption checks keep to (the
// caption list and the Export page show what they find), and whether
// Generate Captions names a speaker for each channel of a stereo clip.
Ui.FormDialog {
    id: root
    objectName: "captionStyleDialog"
    width: Math.min(640, parent ? parent.width - Ui.Theme.s5 * 2 : 640)
    title: qsTr("Caption Style")
    standardButtons: Dialog.Ok | Dialog.Cancel

    readonly property var sizeKeys: ["small", "medium", "large"]
    readonly property var styleKeys: [["", false], ["bold", false], ["", true], ["bold", true]]
    readonly property var alignKeys: ["left", "centre", "right"]
    readonly property var placeKeys: ["bottom", "top", "centre"]
    readonly property int captions: backend.captionCount
    // A stored font the list does not hold is kept while the list is left
    // as it opened.
    property string keptFont: ""
    // The caption tracks it can style ({track, label}), then every one (-1).
    property var trackChoices: []
    readonly property int chosenTrack: trackBox.currentIndex >= 0 && trackBox.currentIndex < trackChoices.length
                                       ? trackChoices[trackBox.currentIndex].track : -1
    property bool loading: false

    function two(n) { const h = Math.round(Math.max(0, Math.min(255, n))).toString(16); return h.length < 2 ? "0" + h : h }
    // The track to open on: the selected caption's, else the first caption track.
    function defaultTrack() {
        const i = typeof win !== "undefined" ? win.selectedConnectedIndex : -1
        if (i >= 0 && i < backend.connected.length && backend.connected[i].caption === true)
            return backend.connected[i].track
        for (let t = 0; t < backend.tracks.length; ++t)
            if (backend.tracks[t].mix && backend.tracks[t].mix.editKind === "caption") return t
        return -1
    }
    function openDialog(track) {
        if (track === undefined || track === null || track < -1) track = defaultTrack()
        const choices = []
        for (let t = 0; t < backend.tracks.length; ++t) {
            const info = backend.tracks[t]
            if (!info.mix || info.mix.editKind !== "caption") continue
            const language = backend.captionTrackLanguage(t)
            choices.push({ track: t, label: language.code && language.code !== "und"
                                            ? qsTr("%1 — %2").arg(info.name).arg(language.shown) : info.name })
        }
        choices.push({ track: -1, label: qsTr("Every caption track") })
        trackChoices = choices
        let at = choices.length - 1
        for (let i = 0; i < choices.length; ++i) if (choices[i].track === track) at = i
        trackBox.currentIndex = at
        load(track)
        loadRules()
        everyBox.currentIndex = 0
        open()
    }
    // Open at the Rules.
    function openRules() {
        openDialog()
        rulesScroll.restart()
    }
    // Once the window has its height: the Rules at its top.
    Timer {
        id: rulesScroll
        interval: 120
        onTriggered: {
            const flick = root.bodyScroll.contentItem
            flick.contentY = Math.max(0, Math.min(rulesHeading.mapToItem(flick.contentItem, 0, 0).y - Ui.Theme.s2,
                                                  flick.contentHeight - flick.height))
        }
    }
    function loadRules() {
        const r = backend.captionRules()
        cpsField.value = r.charsPerSecond
        lineLettersField.value = r.lineLetters
        linesRuleField.value = r.lines
        shortestField.value = r.shortestSeconds
        longestField.value = r.longestSeconds
        gapField.value = r.gapFrames
        overlapCheck.checked = r.noOverlap
        twoSpeakersCheck.checked = r.twoSpeakers
        leftField.text = r.leftSpeaker
        rightField.text = r.rightSpeaker
    }
    function rules() {
        const r = {
            "charsPerSecond": cpsField.value,
            "lineLetters": lineLettersField.value,
            "lines": linesRuleField.value,
            "shortestSeconds": shortestField.value,
            "longestSeconds": longestField.value,
            "gapFrames": gapField.value,
            "noOverlap": overlapCheck.checked,
            "twoSpeakers": twoSpeakersCheck.checked
        }
        if (leftField.text.trim() !== "") r.leftSpeaker = leftField.text.trim()
        if (rightField.text.trim() !== "") r.rightSpeaker = rightField.text.trim()
        return r
    }
    // The fields from a caption track's style (-1: the first track's).
    function load(track) {
        loading = true
        const s = backend.trackCaptionStyle(track)
        const families = fontBox.model
        const fontAt = s.font ? families.indexOf(s.font) : 0
        fontBox.currentIndex = fontAt > 0 ? fontAt : 0
        keptFont = s.font && fontAt <= 0 ? s.font : ""
        sizeBox.currentIndex = Math.max(0, sizeKeys.indexOf(s.size))
        pixelField.value = s.pixelSize || 0
        colorField.text = s.color || "#ffffff"
        styleBox.currentIndex = (s.weight === "bold" ? 1 : 0) + (s.italic ? 2 : 0)
        outlineCheck.checked = s.outline !== false
        outlineColorField.text = s.outlineColor || "#000000"
        outlineWidthField.value = s.outlineWidth !== undefined ? s.outlineWidth : 2
        shadowCheck.checked = s.shadow === true
        shadowColorField.text = s.shadowColor || "#000000"
        shadowOffsetField.value = s.shadowOffset !== undefined ? s.shadowOffset : 3
        shadowSoftnessField.value = s.shadowSoftness !== undefined ? s.shadowSoftness : 4
        const bg = s.bg || ""
        boxCheck.checked = bg !== "" && (s.boxOpacity || 0) > 0
        boxColorField.text = bg.length === 9 ? "#" + bg.substring(3) : "#000000"
        boxOpacityField.value = bg !== "" ? s.boxOpacity : 60
        paddingAuto.checked = !(s.boxPadding >= 0)
        paddingField.value = s.boxPadding >= 0 ? s.boxPadding : 0
        radiusField.value = s.boxRadius || 0
        placeBox.currentIndex = Math.max(0, placeKeys.indexOf(s.place))
        marginField.value = s.margin
        widthField.value = s.width
        alignBox.currentIndex = Math.max(0, alignKeys.indexOf(s.align))
        charsField.value = s.maxChars
        linesField.value = s.maxLines
        secondsField.value = s.minSeconds
        loading = false
    }
    function settings() {
        const style = {
            "font": fontBox.currentIndex > 0 ? fontBox.currentText : keptFont,
            "size": sizeKeys[sizeBox.currentIndex],
            "pixelSize": pixelField.value,
            "color": colorField.acceptableInput ? colorField.text.toLowerCase() : "#ffffff",
            "weight": styleKeys[styleBox.currentIndex][0],
            "italic": styleKeys[styleBox.currentIndex][1],
            "outline": outlineCheck.checked,
            "outlineColor": outlineColorField.acceptableInput ? outlineColorField.text.toLowerCase() : "#000000",
            "outlineWidth": outlineWidthField.value,
            "shadow": shadowCheck.checked,
            "shadowColor": shadowColorField.acceptableInput ? shadowColorField.text.toLowerCase() : "#000000",
            "shadowOffset": shadowOffsetField.value,
            "shadowSoftness": shadowSoftnessField.value,
            "bg": boxCheck.checked
                  ? "#" + two(boxOpacityField.value * 255 / 100) + (boxColorField.acceptableInput ? boxColorField.text.substring(1).toLowerCase() : "000000")
                  : "",
            "boxPadding": paddingAuto.checked ? -1 : paddingField.value,
            "boxRadius": radiusField.value,
            "place": placeKeys[placeBox.currentIndex],
            "margin": marginField.value,
            "width": widthField.value,
            "align": alignKeys[alignBox.currentIndex],
            "maxChars": charsField.value,
            "maxLines": linesField.value,
            "minSeconds": secondsField.value
        }
        return style
    }

    // A number still being typed counts: it is landed before reading.
    onAccepted: {
        landTyped()
        backend.setTrackCaptionStyle(chosenTrack, settings(), everyBox.currentIndex === 1)
        backend.setCaptionRules(rules())
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        RowLayout {
            spacing: 10
            Label { text: qsTr("Captions on"); color: Ui.Theme.textMuted }
            Ui.ComboField {
                id: trackBox
                objectName: "captionStyleTrack"
                Accessible.name: tip
                tip: qsTr("The caption track this style is for, or every caption track")
                model: root.trackChoices.map(function (c) { return c.label })
                Layout.fillWidth: true
                onActivated: root.load(root.chosenTrack)
            }
        }

        Label { text: qsTr("Look"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        Ui.ComboField {
            id: fontBox
            objectName: "captionFont"
            onActivated: root.keptFont = ""
            Accessible.name: tip
            tip: qsTr("Typeface every caption is drawn in")
            model: [qsTr("Default")].concat(Qt.fontFamilies())
            Layout.fillWidth: true
        }
        RowLayout {
            spacing: 10
            Ui.ComboField {
                id: sizeBox
                objectName: "captionSize"
                Accessible.name: tip
                tip: qsTr("Preset text size, unless a pixel size is set")
                model: [qsTr("Small"), qsTr("Medium"), qsTr("Large")]
                Layout.preferredWidth: 120
            }
            Label { text: qsTr("Pixels"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: pixelField
                objectName: "captionPixelSize"
                label: qsTr("Pixels")
                Accessible.name: tip
                tip: qsTr("Exact text height in pixels; 0 keeps the preset size")
                decimals: 0; from: 0; to: 2000; editable: true
            }
            Ui.ComboField {
                id: styleBox
                objectName: "captionWeight"
                Accessible.name: tip
                tip: qsTr("Bold and italic weighting of the captions")
                model: [qsTr("Regular"), qsTr("Bold"), qsTr("Italic"), qsTr("Bold Italic")]
                Layout.preferredWidth: 130
            }
            Ui.LineEdit {
                id: colorField
                objectName: "captionColor"
                hoverEnabled: true
                tip: qsTr("Text colour as a six-digit hex code")
                hint: "#ffffff"
                Layout.preferredWidth: 100
                validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            Ui.Toggle {
                id: outlineCheck
                objectName: "captionOutline"
                label: qsTr("Outline")
                tip: qsTr("Draw an edge round the letters for legibility")
            }
            Ui.LineEdit {
                id: outlineColorField
                objectName: "captionOutlineColor"
                enabled: outlineCheck.checked
                hoverEnabled: true
                tip: qsTr("Colour of the edge round the letters, as a six-digit hex code")
                hint: "#000000"
                Layout.preferredWidth: 100
                validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            }
            Label { text: qsTr("Width"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: outlineWidthField
                objectName: "captionOutlineWidth"
                enabled: outlineCheck.checked
                label: qsTr("Outline width")
                Accessible.name: tip
                tip: qsTr("Thickness of the edge round the letters, in pixels of the frame")
                decimals: 1; from: 0; to: 20; step: 0.5; editable: true
                suffix: " px"
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            Ui.Toggle {
                id: shadowCheck
                objectName: "captionShadow"
                label: qsTr("Shadow")
                tip: qsTr("Draw a soft shadow under the letters")
            }
            Ui.LineEdit {
                id: shadowColorField
                objectName: "captionShadowColor"
                enabled: shadowCheck.checked
                hoverEnabled: true
                tip: qsTr("Shadow colour as a hex code: six digits, or eight with its opacity first")
                hint: "#000000"
                Layout.preferredWidth: 110
                validator: RegularExpressionValidator { regularExpression: /#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})/ }
            }
            Label { text: qsTr("Offset"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: shadowOffsetField
                objectName: "captionShadowOffset"
                enabled: shadowCheck.checked
                label: qsTr("Shadow offset")
                Accessible.name: tip
                tip: qsTr("How far the shadow falls down and to the right, in pixels of the frame")
                decimals: 0; from: -50; to: 50; editable: true
                suffix: " px"
            }
            Label { text: qsTr("Softness"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: shadowSoftnessField
                objectName: "captionShadowSoftness"
                enabled: shadowCheck.checked
                label: qsTr("Shadow softness")
                Accessible.name: tip
                tip: qsTr("How far the shadow's edge spreads, in pixels of the frame; 0 is sharp")
                decimals: 0; from: 0; to: 50; editable: true
                suffix: " px"
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            Ui.Toggle {
                id: boxCheck
                objectName: "captionBox"
                label: qsTr("Box")
                tip: qsTr("Draw a box behind the words so they read over the picture")
            }
            Ui.LineEdit {
                id: boxColorField
                objectName: "captionBoxColor"
                enabled: boxCheck.checked
                hoverEnabled: true
                tip: qsTr("Box colour as a six-digit hex code")
                hint: "#000000"
                Layout.preferredWidth: 100
                validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            }
            Label { text: qsTr("Opacity"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: boxOpacityField
                objectName: "captionBoxOpacity"
                enabled: boxCheck.checked
                label: qsTr("Box opacity")
                Accessible.name: tip
                tip: qsTr("How solid the box is: 100 hides the picture behind it, lower lets it show through")
                decimals: 0; from: 0; to: 100; editable: true
                suffix: "%"
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            enabled: boxCheck.checked
            Label { text: qsTr("Padding"); color: Ui.Theme.textMuted }
            Ui.Toggle {
                id: paddingAuto
                objectName: "captionBoxPaddingAuto"
                label: qsTr("Automatic")
                tip: qsTr("Room round the words of a quarter of the text height")
            }
            Ui.ValueField {
                id: paddingField
                objectName: "captionBoxPadding"
                enabled: !paddingAuto.checked
                label: qsTr("Box padding")
                Accessible.name: tip
                tip: qsTr("Room between the words and the box's sides, in pixels of the frame (half that above and below)")
                decimals: 0; from: 0; to: 200; editable: true
                suffix: " px"
            }
            Label { text: qsTr("Corners"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: radiusField
                objectName: "captionBoxRadius"
                label: qsTr("Corner radius")
                Accessible.name: tip
                tip: qsTr("How round the box's corners are, in pixels of the frame; 0 is square")
                decimals: 0; from: 0; to: 200; editable: true
                suffix: " px"
            }
            Item { Layout.fillWidth: true }
        }

        Label { text: qsTr("Position"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        RowLayout {
            spacing: 10
            Ui.ComboField {
                id: placeBox
                objectName: "captionPlace"
                Accessible.name: tip
                tip: qsTr("Which part of the frame the captions sit in")
                model: [qsTr("Bottom"), qsTr("Top"), qsTr("Centre")]
                Layout.preferredWidth: 120
            }
            Label { text: qsTr("Margin"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: marginField
                objectName: "captionMargin"
                label: qsTr("Margin")
                Accessible.name: tip
                tip: qsTr("Distance from the top or bottom edge, in percent of the frame's height")
                enabled: placeBox.currentIndex !== 2
                decimals: 0; from: 0; to: 45; editable: true
                suffix: "%"
            }
            Label { text: qsTr("Width"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: widthField
                objectName: "captionWidth"
                label: qsTr("Width")
                Accessible.name: tip
                tip: qsTr("Width the words wrap in, in percent of the frame's width")
                decimals: 0; from: 20; to: 100; editable: true
                suffix: "%"
            }
            Ui.ComboField {
                id: alignBox
                objectName: "captionAlign"
                Accessible.name: tip
                tip: qsTr("How the lines of a caption line up with each other")
                model: [qsTr("Left"), qsTr("Centre"), qsTr("Right")]
                Layout.preferredWidth: 110
            }
            Item { Layout.fillWidth: true }
        }

        Label { text: qsTr("Generated captions"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        RowLayout {
            spacing: 10
            Label { text: qsTr("Letters a line"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: charsField
                objectName: "captionMaxChars"
                label: qsTr("Letters a line")
                Accessible.name: tip
                tip: qsTr("Most letters on one line of a generated caption")
                decimals: 0; from: 16; to: 80; editable: true
            }
            Label { text: qsTr("Lines"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: linesField
                objectName: "captionMaxLines"
                label: qsTr("Lines")
                Accessible.name: tip
                tip: qsTr("Most lines in one generated caption")
                decimals: 0; from: 1; to: 3; editable: true
            }
            Label { text: qsTr("Seconds"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: secondsField
                objectName: "captionMinSeconds"
                label: qsTr("Seconds on screen")
                Accessible.name: tip
                tip: qsTr("Shortest time a generated caption stays on screen, in seconds, where the next one leaves room")
                decimals: 1; from: 0.3; to: 10; step: 0.1; editable: true
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            Item { Layout.fillWidth: true }
            FlatButton {
                objectName: "captionRegenerate"
                text: qsTr("Update Captions")
                tip: qsTr("Make the captions again from the timeline's speech at these lengths, in this style. Words typed into captions are lost; Undo brings them back.")
                enabled: backend.hasVideo
                onClicked: {
                    root.landTyped()
                    if (backend.setTrackCaptionStyle(root.chosenTrack, root.settings(), everyBox.currentIndex === 1)
                            && backend.setCaptionRules(root.rules()) && backend.regenerateCaptions())
                        root.close()
                }
            }
        }

        RowLayout {
            spacing: 10
            Ui.ComboField {
                id: everyBox
                objectName: "captionEvery"
                Accessible.name: tip
                tip: qsTr("Whether captions given a look of their own, or dragged elsewhere, take this style too")
                model: [qsTr("Captions with their own look keep it"), qsTr("Every caption takes this style")]
                Layout.fillWidth: true
            }
        }
        Label {
            objectName: "captionStyleNote"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.captions > 0
                  ? (root.chosenTrack >= 0
                     ? qsTr("Applies to the captions on this track that follow its style, and to new ones made on it. One undo step.")
                     : qsTr("Applies to the %n caption(s) on the timeline, and to new and generated ones. One undo step.", "", root.captions))
                  : qsTr("New, imported and generated captions take this style.")
        }

        Label {
            id: rulesHeading
            objectName: "captionRulesHeading"
            text: qsTr("Rules")
            color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: qsTr("The limits a caption is checked against, kept with this timeline. The caption list marks a caption past one, with the reason, and the Export page says so when the captions go out with the file.")
        }
        GridLayout {
            columns: 4
            columnSpacing: 10
            rowSpacing: 6
            Label { text: qsTr("Characters a second"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: cpsField
                objectName: "captionRuleCharsPerSecond"
                label: qsTr("Characters a second")
                Accessible.name: tip
                tip: qsTr("Most characters a caption may ask to be read in a second, spaces counted")
                decimals: 1; from: 1; to: 60; step: 0.5; editable: true
            }
            Label { text: qsTr("Letters a line"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: lineLettersField
                objectName: "captionRuleLineLetters"
                label: qsTr("Letters a line")
                Accessible.name: tip
                tip: qsTr("Most letters on one line of a caption, spaces counted")
                decimals: 0; from: 10; to: 120; editable: true
            }
            Label { text: qsTr("Lines"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: linesRuleField
                objectName: "captionRuleLines"
                label: qsTr("Lines")
                Accessible.name: tip
                tip: qsTr("Most lines in one caption")
                decimals: 0; from: 1; to: 6; editable: true
            }
            Label { text: qsTr("Gap in frames"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: gapField
                objectName: "captionRuleGapFrames"
                label: qsTr("Gap in frames")
                Accessible.name: tip
                tip: qsTr("Fewest frames between a caption and the next on its track; none at all is fine too")
                decimals: 0; from: 0; to: 100; editable: true
            }
            Label { text: qsTr("Shortest seconds"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: shortestField
                objectName: "captionRuleShortest"
                label: qsTr("Shortest seconds")
                Accessible.name: tip
                tip: qsTr("Least time a caption stays on screen, in seconds")
                decimals: 2; from: 0; to: 10; step: 0.01; editable: true
            }
            Label { text: qsTr("Longest seconds"); color: Ui.Theme.textMuted }
            Ui.ValueField {
                id: longestField
                objectName: "captionRuleLongest"
                label: qsTr("Longest seconds")
                Accessible.name: tip
                tip: qsTr("Most time a caption stays on screen, in seconds")
                decimals: 1; from: 0.5; to: 120; step: 0.5; editable: true
            }
        }
        CheckBox {
            id: overlapCheck
            objectName: "captionRuleNoOverlap"
            text: qsTr("Mark captions that overlap another on their track")
            ToolTip.text: qsTr("Show a problem for a caption that runs into the next one on the same caption track")
            ToolTip.visible: hovered
        }
        Label { text: qsTr("Speakers"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsLabel; font.weight: Ui.Theme.wHeading }
        CheckBox {
            id: twoSpeakersCheck
            objectName: "captionRuleTwoSpeakers"
            text: qsTr("Two speakers on left and right channels")
            ToolTip.text: qsTr("Make Captions names a speaker for each channel of a stereo recording made with one microphone per person")
            ToolTip.visible: hovered
        }
        Label {
            objectName: "captionRuleTwoSpeakersNote"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: qsTr("Only for a stereo recording made with one microphone per person, each on its own channel: Make Captions then names the speaker of each caption by the channel the voice is louder on. It cannot tell two voices apart on one microphone or in a mixed recording.")
        }
        RowLayout {
            enabled: twoSpeakersCheck.checked
            spacing: 10
            Label { text: qsTr("Left"); color: Ui.Theme.textMuted }
            Ui.LineEdit {
                id: leftField
                objectName: "captionRuleLeftSpeaker"
                Layout.fillWidth: true
                hint: qsTr("Speaker 1")
                tip: qsTr("The name given to the voice on the left channel")
            }
            Label { text: qsTr("Right"); color: Ui.Theme.textMuted }
            Ui.LineEdit {
                id: rightField
                objectName: "captionRuleRightSpeaker"
                Layout.fillWidth: true
                hint: qsTr("Speaker 2")
                tip: qsTr("The name given to the voice on the right channel")
            }
        }
    }
}
