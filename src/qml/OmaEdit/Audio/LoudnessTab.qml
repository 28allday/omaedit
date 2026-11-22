pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// Loudness: live EBU R128 readings (integrated, short-term, momentary,
// true peak, LRA), a target preset with pass/fail against it, the history
// graph, and the offline Analyse / Normalize pass.
Rectangle {
    id: root

    property var live: ({})          // LoudnessMeter snapshot
    property var history: []
    property var analysis: ({})      // {running, percent, values, history}
    property string targetId: "ebu"
    property bool playing: false

    signal resetRequested()
    property bool hasRange: false
    property bool analyzeRange: false
    signal analyzeRequested(bool selection)
    signal cancelRequested()
    signal normalizeRequested(real target, real peak)
    signal targetEdited(string id)

    color: Theme.panelBg

    readonly property var targets: [
        { id: "ebu", name: qsTr("EBU R128 (broadcast)"), lufs: -23, tolerance: 1, peak: -1, lra: 20 },
        { id: "atsc", name: qsTr("ATSC A/85 (US TV)"), lufs: -24, tolerance: 2, peak: -2, lra: 0 },
        { id: "youtube", name: qsTr("Web / streaming (−14 LUFS)"), lufs: -14, tolerance: 1, peak: -1, lra: 0 },
        { id: "spotify", name: qsTr("Music reference (−14 LUFS)"), lufs: -14, tolerance: 1, peak: -1, lra: 0 },
        { id: "podcast", name: qsTr("Podcast (stereo)"), lufs: -16, tolerance: 1, peak: -1, lra: 0 },
        { id: "netflix", name: qsTr("Dialogue reference (−27 LUFS)"), lufs: -27, tolerance: 2, peak: -2, lra: 0 },
        { id: "cinema", name: qsTr("Programme reference (−24 LUFS)"), lufs: -24, tolerance: 2, peak: -1, lra: 0 }
    ]
    readonly property var target: { for (var i = 0; i < targets.length; ++i) if (targets[i].id === targetId) return targets[i]; return targets[0] }
    // The measured programme: the analysis when it exists, else the live meter.
    readonly property bool measured: analysis.stale !== true && analysis.values !== undefined && analysis.values.integrated !== undefined && analysis.values.integrated > -99
    readonly property var values: analysis.stale === true ? ({}) : measured ? analysis.values : live
    function v(key) { return values !== undefined && values[key] !== undefined ? values[key] : -100 }
    function fmt(x, unit) { return x <= -99 ? "—" : x.toFixed(1) + (unit !== undefined ? " " + unit : "") }
    readonly property bool integratedOk: v("integrated") > -99 && Math.abs(v("integrated") - target.lufs) <= target.tolerance
    readonly property bool peakOk: v("truePeak") > -99 && v("truePeak") <= target.peak
    readonly property bool lraOk: target.lra === 0 || v("range") <= target.lra
    readonly property bool passes: integratedOk && peakOk && lraOk

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.s2
        contentHeight: column.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}
        Column {
            id: column
            width: parent.width
            spacing: Theme.s2

            Row {
                width: parent.width
                spacing: Theme.s2
                Text { anchors.verticalCenter: parent.verticalCenter; width: parent.width - resetBtn.width - Theme.s2; text: qsTr("Loudness"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading; color: Theme.textPrimary }
                ToolButton { id: resetBtn; iconName: "rotate-ccw"; text: qsTr("Reset"); showLabel: true; onClicked: root.resetRequested() }
            }
            Flow {
                width: parent.width
                spacing: Theme.s2
                ToolButton {
                    objectName: "loudnessAnalyze"
                    iconName: root.analysis.running === true ? "loader" : "gauge"
                    text: root.analysis.running === true ? qsTr("Cancel analysis (%1%)").arg(root.analysis.percent !== undefined ? root.analysis.percent : 0) : root.analyzeRange && root.hasRange ? qsTr("Analyse Selection") : qsTr("Analyse Timeline")
                    showLabel: true
                    onClicked: root.analysis.running === true ? root.cancelRequested() : root.analyzeRequested(root.analyzeRange && root.hasRange)
                }
                ToolButton {
                    objectName: "loudnessNormalize"
                    iconName: "wand-sparkles"
                    text: qsTr("Normalise to %1 LUFS").arg(root.target.lufs)
                    showLabel: true
                    enabled: root.measured && root.analysis.running !== true
                    onClicked: root.normalizeRequested(root.target.lufs, root.target.peak)
                }
            }
            ComboField {
                objectName: "loudnessTarget"
                width: parent.width
                caption: qsTr("Target")
                tip: qsTr("Delivery standard the readings are judged against and matched to")
                model: root.targets.map(function (t) { return t.name })
                currentIndex: root.targets.indexOf(root.target)
                onActivated: function (i) { root.targetEdited(root.targets[i].id) }
            }

            ComboField {
                width: parent.width; caption: qsTr("Measure")
                tip: qsTr("Whether analysis covers the whole timeline or just the In/Out range")
                model: root.hasRange ? [qsTr("Whole timeline"), qsTr("In/Out selection")] : [qsTr("Whole timeline")]
                currentIndex: root.analyzeRange && root.hasRange ? 1 : 0
                onActivated: function (i) { root.analyzeRange = i === 1 }
            }
            // Readings grid.
            Grid {
                width: parent.width
                columns: 2
                columnSpacing: Theme.s2
                rowSpacing: Theme.s1
                component Reading: Rectangle {
                    property string label: ""
                    property string value: ""
                    property bool ok: true
                    property bool judged: false
                    property string name: ""
                    objectName: name
                    width: (column.width - Theme.s2) / 2
                    height: Theme.hControl * 2
                    radius: Theme.rControl
                    color: Theme.controlBg
                    border.color: judged ? (ok ? Theme.audio : Theme.danger) : Theme.border
                    border.width: Theme.hairline
                    Text { anchors.left: parent.left; anchors.top: parent.top; anchors.margins: Theme.s1; text: parent.label; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                    Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.s1; text: parent.value; font.family: Theme.monoFamily; font.pixelSize: Theme.fsTimecode; color: parent.judged && !parent.ok ? Theme.danger : Theme.textPrimary }
                }
                Reading { name: "loudnessIntegrated"; label: qsTr("Integrated"); value: root.fmt(root.v("integrated"), "LUFS"); judged: root.v("integrated") > -99; ok: root.integratedOk }
                Reading { name: "loudnessShortTerm"; label: qsTr("Short-term"); value: root.fmt(root.v("shortTerm"), "LUFS") }
                Reading { name: "loudnessMomentary"; label: qsTr("Momentary"); value: root.fmt(root.v("momentary"), "LUFS") }
                Reading { name: "loudnessTruePeak"; label: qsTr("True peak"); value: root.fmt(root.v("truePeak"), "dBTP"); judged: root.v("truePeak") > -99; ok: root.peakOk }
                Reading { name: "loudnessRange"; label: qsTr("Loudness range"); value: root.v("range") <= -99 ? "—" : root.v("range").toFixed(1) + " LU"; judged: root.target.lra > 0 && root.v("integrated") > -99; ok: root.lraOk }
                Reading { label: qsTr("Max momentary"); value: root.fmt(root.v("momentaryMax"), "LUFS") }
            }

            // Compliance.
            Rectangle {
                objectName: "loudnessVerdict"
                width: parent.width
                height: Math.max(Theme.hControl + Theme.s2, verdictText.implicitHeight + Theme.s2 * 2)
                radius: Theme.rControl
                color: root.v("integrated") <= -99 ? Theme.controlBg : root.passes ? Qt.rgba(Theme.audio.r, Theme.audio.g, Theme.audio.b, 0.18) : Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.18)
                border.color: root.v("integrated") <= -99 ? Theme.border : root.passes ? Theme.audio : Theme.danger
                border.width: Theme.hairline
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.s2
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.s2
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.s2
                    Icon { id: verdictIcon; anchors.verticalCenter: parent.verticalCenter; name: root.v("integrated") <= -99 ? "circle" : root.passes ? "circle-check" : "circle-alert"; color: root.v("integrated") <= -99 ? Theme.textMuted : root.passes ? Theme.audio : Theme.danger }
                    Text {
                        id: verdictText
                        width: parent.width - verdictIcon.width - Theme.s2
                        wrapMode: Text.WordWrap
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.analysis.stale === true ? qsTr("Mix changed. Analyse again to measure the current sound.") : root.v("integrated") <= -99 ? qsTr("Play or analyse to measure against %1 LUFS ±%2, %3 dBTP").arg(root.target.lufs).arg(root.target.tolerance).arg(root.target.peak)
                            : root.passes ? qsTr("Passes %1").arg(root.target.name)
                            : !root.integratedOk ? qsTr("%1 LU %2 target").arg(Math.abs(root.v("integrated") - root.target.lufs).toFixed(1)).arg(root.v("integrated") > root.target.lufs ? qsTr("over") : qsTr("under"))
                            : !root.peakOk ? qsTr("True peak above %1 dBTP").arg(root.target.peak) : qsTr("Loudness range above %1 LU").arg(root.target.lra)
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsBase
                        color: Theme.textPrimary
                    }
                }
            }

            LoudnessGraph {
                width: parent.width
                history: root.measured && root.analysis.history !== undefined ? root.analysis.history : root.history
                target: root.target.lufs
                tolerance: root.target.tolerance
            }
            Text {
                width: parent.width
                text: root.analysis.stale === true ? qsTr("Previous measurement is out of date") : root.measured ? qsTr("Offline measurement (%1 s)").arg(Math.round(root.analysis.values.seconds !== undefined ? root.analysis.values.seconds : 0))
                    : qsTr("Live from the master while playing (%1 s measured)").arg(Math.round(root.live.seconds !== undefined ? root.live.seconds : 0))
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Normalise adjusts the measured range in one undoable edit. Analyse again to verify loudness and true peak; add a master limiter if needed.")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }
}
