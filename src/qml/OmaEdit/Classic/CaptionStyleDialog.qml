import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Edit ▸ Captions ▸ Caption Style…: one look for every caption on the
// timeline, and the lengths Generate Captions makes them. OK restyles the
// captions that still have the timeline's look (or all of them) in one undo
// step; Regenerate makes the captions again from the transcripts.
Ui.FormDialog {
    id: root
    objectName: "captionStyleDialog"
    width: Math.min(560, parent ? parent.width - Ui.Theme.s5 * 2 : 560)
    title: qsTr("Caption Style")
    standardButtons: Dialog.Ok | Dialog.Cancel

    readonly property var sizeKeys: ["small", "medium", "large"]
    readonly property var styleKeys: [["", false], ["bold", false], ["", true], ["bold", true]]
    readonly property var alignKeys: ["left", "centre", "right"]
    readonly property var placeKeys: ["bottom", "top", "centre"]
    // Background boxes: none, a dark wash, a light wash, solid black.
    readonly property var bgKeys: ["", "#99000000", "#ccffffff", "#ff000000"]
    readonly property int captions: backend.captionCount
    // A stored box colour or font the lists do not hold is kept while that
    // list is left as it opened.
    property string keptBg: ""
    property string keptFont: ""

    function openDialog() {
        const s = backend.captionStyleSettings()
        const families = fontBox.model
        const fontAt = s.font ? families.indexOf(s.font) : 0
        fontBox.currentIndex = fontAt > 0 ? fontAt : 0
        keptFont = s.font && fontAt <= 0 ? s.font : ""
        sizeBox.currentIndex = Math.max(0, sizeKeys.indexOf(s.size))
        pixelField.value = s.pixelSize || 0
        colorField.text = s.color || "#ffffff"
        styleBox.currentIndex = (s.weight === "bold" ? 1 : 0) + (s.italic ? 2 : 0)
        outlineCheck.checked = s.outline !== false
        const bgAt = bgKeys.indexOf(s.bg || "")
        bgBox.currentIndex = bgAt >= 0 ? bgAt : 1
        keptBg = bgAt >= 0 ? "" : (s.bg || "")
        placeBox.currentIndex = Math.max(0, placeKeys.indexOf(s.place))
        marginField.value = s.margin
        widthField.value = s.width
        alignBox.currentIndex = Math.max(0, alignKeys.indexOf(s.align))
        charsField.value = s.maxChars
        linesField.value = s.maxLines
        secondsField.value = s.minSeconds
        everyBox.currentIndex = 0
        open()
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
            "bg": keptBg !== "" && bgBox.currentIndex === 1 ? keptBg : bgKeys[bgBox.currentIndex],
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
        backend.setCaptionStyle(settings(), everyBox.currentIndex === 1)
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

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
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 10
            Ui.LineEdit {
                id: colorField
                objectName: "captionColor"
                hoverEnabled: true
                tip: qsTr("Text colour as a six-digit hex code")
                hint: "#ffffff"
                Layout.preferredWidth: 110
                validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            }
            Ui.ComboField {
                id: bgBox
                objectName: "captionBox"
                onActivated: root.keptBg = ""
                Accessible.name: tip
                tip: qsTr("Box drawn behind the words so they read over the picture")
                model: [qsTr("No background"), qsTr("Dark box"), qsTr("Light box"), qsTr("Black box")]
                Layout.preferredWidth: 150
            }
            Ui.Toggle {
                id: outlineCheck
                objectName: "captionOutline"
                label: qsTr("Outline")
                tip: qsTr("Draw a dark edge round the letters for legibility")
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
                text: qsTr("Regenerate Captions")
                tip: qsTr("Make the captions again from the timeline's speech at these lengths, in this style. Words typed into captions are lost; Undo brings them back.")
                enabled: backend.hasVideo
                onClicked: {
                    root.landTyped()
                    if (backend.setCaptionStyle(root.settings(), everyBox.currentIndex === 1) && backend.regenerateCaptions())
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
                  ? qsTr("Applies to the %n caption(s) on the timeline, and to new and generated ones. One undo step.", "", root.captions)
                  : qsTr("New, imported and generated captions take this style.")
        }
    }
}
