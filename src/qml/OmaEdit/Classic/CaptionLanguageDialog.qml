import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// A caption track's language, from the caption track's menu: set it on the
// track, or (duplicating) make a copy of the track's captions and style in
// another language to be translated.
Ui.FormDialog {
    id: root
    objectName: "captionLanguageDialog"
    width: Math.min(440, parent ? parent.width - Ui.Theme.s5 * 2 : 440)
    title: duplicating ? qsTr("Duplicate Caption Track as a New Language") : qsTr("Caption Track Language")
    standardButtons: Dialog.Ok | Dialog.Cancel

    property int track: -1
    property bool duplicating: false
    // "Not set", the languages the editor knows by name, then "Other".
    readonly property var languages: backend.captionLanguages()
    readonly property var choices: [qsTr("Not set")].concat(languages.map(function (l) { return l.name + " (" + l.code + ")" }))
                                   .concat([qsTr("Other…")])
    readonly property bool other: languageBox.currentIndex === choices.length - 1

    function openFor(t, duplicate) {
        track = t
        duplicating = duplicate === true
        const now = backend.captionTrackLanguage(t)
        let at = 0
        for (let i = 0; i < languages.length; ++i)
            if (languages[i].code === now.code) at = i + 1
        // Duplicating: the copy is another language, so nothing is chosen yet.
        if (duplicating) at = 0
        else if (at === 0 && now.code && now.code !== "und") at = choices.length - 1
        languageBox.currentIndex = at
        codeField.text = at === choices.length - 1 ? now.code : ""
        nameField.text = at === choices.length - 1 ? now.name : ""
        open()
    }
    function chosen() {
        if (other) return { code: codeField.text.trim(), name: nameField.text.trim() }
        if (languageBox.currentIndex <= 0) return { code: "und", name: "" }
        const l = languages[languageBox.currentIndex - 1]
        return { code: l.code, name: l.name }
    }
    onAccepted: {
        landTyped()
        const l = chosen()
        if (duplicating) backend.duplicateCaptionTrack(track, l.code, l.name)
        else backend.setCaptionTrackLanguage(track, l.code, l.name)
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Ui.Theme.textMuted
            text: root.duplicating
                  ? qsTr("A new caption track gets a copy of every caption on this one and its style, in the language chosen here, ready to be translated. It is not burnt into the picture until you choose it in its menu.")
                  : qsTr("The language the captions on this track are written in. It names the track's captions when they are delivered.")
        }
        Ui.ComboField {
            id: languageBox
            objectName: "captionLanguage"
            Accessible.name: tip
            tip: qsTr("Language of the captions on this track")
            model: root.choices
            Layout.fillWidth: true
        }
        RowLayout {
            spacing: 10
            visible: root.other
            Label { text: qsTr("Code"); color: Ui.Theme.textMuted }
            Ui.LineEdit {
                id: codeField
                objectName: "captionLanguageCode"
                hoverEnabled: true
                tip: qsTr("Language code, such as en, fr or pt-BR")
                hint: "pt-BR"
                Layout.preferredWidth: 100
                validator: RegularExpressionValidator { regularExpression: /[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*/ }
            }
            Label { text: qsTr("Name"); color: Ui.Theme.textMuted }
            Ui.LineEdit {
                id: nameField
                objectName: "captionLanguageName"
                hoverEnabled: true
                tip: qsTr("Name shown for the language")
                Layout.fillWidth: true
            }
        }
    }
}
