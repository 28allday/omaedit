pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaEdit.Ui

GridLayout {
    id: root
    columns: 2
    columnSpacing: Theme.s3
    rowSpacing: Theme.s2
    property var preserved: ({})
    function reset(r) {
        preserved = Object.assign({}, r)
        words.text = r.text || ""; tag.text = r.tag || ""
        kind.currentIndex = ["", "video", "audio", "image"].indexOf(r.kind || "")
        rating.currentIndex = Number(r.ratingMin || 0)
        widthMin.text = r.widthMin || ""; widthMax.text = r.widthMax || ""
        fpsMin.text = r.fpsMin || ""; fpsMax.text = r.fpsMax || ""
        proxy.currentIndex = ["", "yes", "no"].indexOf(r.proxy || "")
        offline.currentIndex = ["", "yes", "no"].indexOf(r.offline || "")
        dateFrom.text = r.dateFrom || ""; dateTo.text = r.dateTo || ""
    }
    function rules() {
        return Object.assign({}, preserved, {text: words.text.trim(), tag: tag.text.trim(), kind: ["", "video", "audio", "image"][kind.currentIndex],
            ratingMin: rating.currentIndex, widthMin: Number(widthMin.text), widthMax: Number(widthMax.text),
            fpsMin: Number(fpsMin.text), fpsMax: Number(fpsMax.text), proxy: ["", "yes", "no"][proxy.currentIndex],
            offline: ["", "yes", "no"][offline.currentIndex], dateFrom: dateFrom.text, dateTo: dateTo.text})
    }
    component Label: Text { color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel }
    Label { text: qsTr("Words / transcript") }
    LineEdit { id: words; objectName: "ruleWords"; tip: qsTr("Words matched in names, logging, custom fields and transcripts"); Layout.fillWidth: true; Accessible.name: qsTr("Search words") }
    Label { text: qsTr("Tag") }
    LineEdit { id: tag; tip: qsTr("Keep only clips carrying this tag"); Layout.fillWidth: true; Accessible.name: qsTr("Tag") }
    Label { text: qsTr("Media kind") }
    ComboField { id: kind; tip: qsTr("Limit results to video, audio or still images"); Layout.fillWidth: true; model: [qsTr("Any"), qsTr("Video"), qsTr("Audio"), qsTr("Image")] }
    Label { text: qsTr("Minimum rating") }
    ComboField { id: rating; tip: qsTr("Hide clips rated below this many stars"); Layout.fillWidth: true; model: [qsTr("Any"), "1", "2", "3", "4", "5"] }
    Label { text: qsTr("Width in pixels") }
    RowLayout {
        LineEdit { id: widthMin; tip: qsTr("Smallest frame width in pixels a clip may have"); Layout.fillWidth: true; hint: qsTr("Minimum"); validator: IntValidator { bottom: 0; top: 65536 } }
        LineEdit { id: widthMax; tip: qsTr("Largest frame width in pixels a clip may have"); Layout.fillWidth: true; hint: qsTr("Maximum"); validator: IntValidator { bottom: 0; top: 65536 } }
    }
    Label { text: qsTr("Frames per second") }
    RowLayout {
        LineEdit { id: fpsMin; tip: qsTr("Lowest frame rate a clip may have"); Layout.fillWidth: true; hint: qsTr("Minimum"); validator: DoubleValidator { bottom: 0; top: 1000; locale: "C" } }
        LineEdit { id: fpsMax; tip: qsTr("Highest frame rate a clip may have"); Layout.fillWidth: true; hint: qsTr("Maximum"); validator: DoubleValidator { bottom: 0; top: 1000; locale: "C" } }
    }
    Label { text: qsTr("Proxy") }
    ComboField { id: proxy; tip: qsTr("Keep only clips that have, or lack, an attached proxy"); Layout.fillWidth: true; model: [qsTr("Any"), qsTr("Attached"), qsTr("No proxy")] }
    Label { text: qsTr("Source status") }
    ComboField { id: offline; tip: qsTr("Keep only clips whose source file is missing, or present"); Layout.fillWidth: true; model: [qsTr("Any"), qsTr("Offline"), qsTr("Online")] }
    Label { text: qsTr("Created between") }
    RowLayout {
        LineEdit { id: dateFrom; tip: qsTr("Earliest creation date to include, as YYYY-MM-DD"); Layout.fillWidth: true; hint: "YYYY-MM-DD"; Accessible.name: qsTr("From date") }
        LineEdit { id: dateTo; tip: qsTr("Latest creation date to include, as YYYY-MM-DD"); Layout.fillWidth: true; hint: "YYYY-MM-DD"; Accessible.name: qsTr("To date") }
    }
}
