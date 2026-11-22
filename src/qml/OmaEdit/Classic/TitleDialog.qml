import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.FormDialog {
    id: titleDialog
    objectName: "titleDialog"
    width: Math.min(560, parent ? parent.width - Ui.Theme.s5 * 2 : 560)
    // editIndex >= 0 = editing that connected clip's text in place.
    property int editIndex: -1
    readonly property var placeKeys: ["centre", "lower", "bottom", "top"]
    // A caption's distance from its edge and wrapping width (Caption Style),
    // kept as they were: this dialog does not set them.
    property var keptMargin: undefined
    property var keptWidth: undefined
    readonly property var sizeKeys: ["small", "medium", "large"]
    // White first = the default; stored as "" so default titles carry no
    // colour property at all.
    readonly property var colorKeys: ["#ffffff", "#f5c518", "#ff9500",
                                      "#e5493a", "#4fac4f", "#3f8ae0",
                                      "#101010"]
    property int colorIndex: 0
    readonly property var alignKeys: ["left", "centre", "right"]
    // Background boxes: none, a dark wash, a light wash, solid black.
    readonly property var bgKeys: ["", "#99000000", "#ccffffff", "#ff000000"]
    title: editIndex >= 0 ? qsTr("Edit Title") : qsTr("Add Title")
    standardButtons: Dialog.Ok | Dialog.Cancel

    // `extra` (optional): {align, weight, italic, bg, outline} of a placed
    // title — win.catalogue.titleExtras(entry).
    function openFor(index, text, place, size, font, color, extra) {
        editIndex = index
        titleField.text = text
        placeBox.currentIndex =
            Math.max(0, placeKeys.indexOf(place || "centre"))
        sizeBox.currentIndex =
            Math.max(0, sizeKeys.indexOf(size || "medium"))
        var families = fontBox.model
        var fontAt = font ? families.indexOf(font) : 0
        fontBox.currentIndex = fontAt > 0 ? fontAt : 0
        colorIndex = Math.max(0, colorKeys.indexOf(
                                     (color || "#ffffff").toLowerCase()))
        var x = extra || {}
        var bold = x.weight === "bold"
        var italic = x.italic === true
        styleBox.currentIndex = bold && italic ? 3 : italic ? 2 : bold ? 1 : 0
        alignBox.currentIndex = Math.max(0, alignKeys.indexOf(x.align || "centre"))
        bgBox.currentIndex = Math.max(0, bgKeys.indexOf(x.bg || ""))
        outlineCheck.checked = x.outline === undefined ? true : x.outline !== false
        trackingField.value = x.tracking !== undefined ? x.tracking : 0
        spacingField.value = x.lineSpacing !== undefined ? Math.round(x.lineSpacing*100) : 100
        pixelField.value = x.pixelSize !== undefined ? x.pixelSize : 0
        customColor.text = color || "#ffffff"
        keptMargin = x.margin
        keptWidth = x.width
        open()
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        TextArea {
            id: titleField
            objectName: "titleTextField"
            color: Ui.Theme.textPrimary
            font.family: Ui.Theme.fontFamily
            font.pixelSize: Ui.Theme.fsBase
            textFormat: TextEdit.PlainText
            padding: Ui.Theme.s2
            topPadding: Ui.Theme.s2; bottomPadding: Ui.Theme.s2
            leftPadding: Ui.Theme.s2; rightPadding: Ui.Theme.s2
            topInset: 0; bottomInset: 0
            background: Rectangle {
                color: Ui.Theme.controlBg; radius: Ui.Theme.rControl
                border.width: Ui.Theme.hairline
                border.color: titleField.activeFocus ? Ui.Theme.accent : Ui.Theme.border
            }
            Layout.fillWidth: true
            Layout.minimumWidth: 400
            placeholderText: ""
            Accessible.name: qsTr("Title text")
            Text {
                x: titleField.leftPadding; y: titleField.topPadding
                text: qsTr("Title text")
                visible: titleField.text.length === 0
                color: Ui.Theme.textMuted
                font: titleField.font
            }
            Layout.preferredHeight: 80
            wrapMode: TextEdit.Wrap
        }

        RowLayout {
            spacing: 10
            Ui.ComboField {
                Accessible.name: tip
                id: placeBox
                tip: qsTr("Where the text sits in the frame")
                model: [qsTr("Centre"), qsTr("Lower Third"), qsTr("Bottom"), qsTr("Top")]
                Layout.preferredWidth: 150
            }
            Ui.ComboField {
                Accessible.name: tip
                id: sizeBox
                tip: qsTr("Preset text size, unless a pixel size is set below")
                model: ["Small", "Medium", "Large"]
                currentIndex: 1
                Layout.preferredWidth: 120
            }
            Item { Layout.fillWidth: true }
        }

        Ui.ComboField {
                Accessible.name: tip
            id: fontBox
            tip: qsTr("Typeface the title is drawn in")
            model: ["Default"].concat(Qt.fontFamilies())
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: 10
            Ui.ComboField {
                Accessible.name: tip
                id: styleBox
                objectName: "titleStyleBox"
                tip: qsTr("Bold and italic weighting of the title text")
                model: ["Regular", "Bold", "Italic", "Bold Italic"]
                Layout.preferredWidth: 130
            }
            Ui.ComboField {
                Accessible.name: tip
                id: alignBox
                objectName: "titleAlignBox"
                tip: qsTr("How the lines of text line up with each other")
                model: ["Left", "Centre", "Right"]
                currentIndex: 1
                Layout.preferredWidth: 110
            }
            Ui.ComboField {
                Accessible.name: tip
                id: bgBox
                objectName: "titleBgBox"
                tip: qsTr("Box drawn behind the text so it reads over the picture")
                model: ["No background", "Dark box", "Light box", "Black box"]
                Layout.preferredWidth: 150
            }
            Ui.Toggle {
                id: outlineCheck
                objectName: "titleOutlineCheck"
                label: qsTr("Outline")
                tip: qsTr("Draw a dark edge round the letters for legibility")
                checked: true
            }
        }

        RowLayout {
            Label { text: qsTr("Tracking (px)") }
            Ui.ValueField { Accessible.name: tip; decimals: 0; id: trackingField; objectName:"titleTracking"; tip: qsTr("Extra space between letters, in pixels"); from:-50; to:200; editable:true }
            Label { text: qsTr("Line spacing %") }
            Ui.ValueField { Accessible.name: tip; decimals: 0; id: spacingField; objectName:"titleLineSpacing"; tip: qsTr("Height of each line as a percentage of normal"); from:50; to:400; value:100; editable:true }
        }
        RowLayout {
            Label { text: qsTr("Font size (px, 0 = preset)") }
            Ui.ValueField { Accessible.name: tip; decimals: 0; id: pixelField; objectName:"titlePixelSize"; tip: qsTr("Exact text height in pixels; 0 keeps the preset size"); from:0; to:2000; editable:true }
            Ui.LineEdit { id:customColor; objectName:"titleCustomColor"; hoverEnabled: true; tip: qsTr("Custom text colour as a six-digit hex code"); Layout.preferredWidth:110; hint:"#ffffff"; validator:RegularExpressionValidator {regularExpression:/#[0-9a-fA-F]{6}/} }
        }

        RowLayout {
            spacing: 8
            Repeater {
                model: titleDialog.colorKeys
                Rectangle {
                    required property var modelData
                    required property int index
                    width: 24
                    height: 24
                    radius: 12
                    color: modelData
                    border.color: index === titleDialog.colorIndex
                                  ? win.accent : "#5a5a62"
                    border.width: index === titleDialog.colorIndex ? 2 : 1
                    MouseArea {
                        anchors.fill: parent
                        onClicked: { titleDialog.colorIndex = parent.index; customColor.text=parent.modelData }
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    onAboutToShow: titleField.forceActiveFocus()
    onClosed: editIndex = -1
    onAccepted: {
        // OK does not take keyboard focus. Commit a numeric field still being
        // typed before reading the title's settings.
        titleField.forceActiveFocus()
        if (titleField.text.trim().length === 0)
            return
        var style = {
            "place": placeKeys[placeBox.currentIndex],
            "size": sizeKeys[sizeBox.currentIndex],
            "font": fontBox.currentIndex > 0 ? fontBox.currentText : "",
            "color": customColor.acceptableInput ? customColor.text : colorKeys[colorIndex],
            "tracking":trackingField.value, "lineSpacing":spacingField.value/100,
            "align": alignKeys[alignBox.currentIndex],
            "weight": styleBox.currentIndex === 1 || styleBox.currentIndex === 3 ? "bold" : "",
            "italic": styleBox.currentIndex >= 2,
            "bg": bgKeys[bgBox.currentIndex],
            "outline": outlineCheck.checked
        }
        if(pixelField.value>0)style.pixelSize=pixelField.value
        if (keptMargin !== undefined && keptMargin !== null) style.margin = keptMargin
        if (keptWidth !== undefined && keptWidth !== null) style.width = keptWidth
        if (editIndex >= 0)
            backend.updateTitle(editIndex, titleField.text, style)
        else
            backend.addTitle(titleField.text, style)
    }
}
