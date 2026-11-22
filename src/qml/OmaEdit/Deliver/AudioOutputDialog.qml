pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

// What sound the render carries and what is made beside it: which tracks go
// into the main file and on which channels, whether stems are made (one WAV
// a track, or one a category), whether the range is cut into parts, and what
// happens when the hardware encoder is not there. A draft of the render's
// settings, handed back whole on OK; its keys are the render spec's own.
FormDialog {
    id: root
    objectName: "deliverAudioOutputs"
    title: qsTr("Audio outputs and file splitting")
    width: Math.min(640, parent ? parent.width - Theme.s5 * 2 : 640)
    property var spec: ({})
    property var tracks: []
    property var draft: ({})
    // The stems a draft would make: function(draft) → [{name, file}]. The
    // owner knows the timeline and the file's name; without one, no list.
    property var stemLister: null
    readonly property var stems: stemLister && draft.stems === true ? stemLister(draft) : []
    readonly property int stemsChoice: draft.stems !== true ? 0 : draft.stemsBy === "category" ? 2 : 1
    signal edited(var changes)
    function openFor(settings) { draft = Object.assign({}, settings); open() }
    function set(key, value) { var next = Object.assign({}, draft); next[key] = value; draft = next }
    function selected(index) { return draft.audioSelection !== "custom" || (draft.audioTracks || []).indexOf(index) >= 0 }
    function select(index, on) {
        var next = draft.audioSelection === "custom" ? (draft.audioTracks || []).slice() : tracks.map(function (t) { return t.index })
        next = next.filter(function (i) { return i !== index }); if (on) next.push(index)
        var changed = Object.assign({}, draft, {audioSelection: "custom", audioTracks: next}); draft = changed
    }
    function chooseStems(choice) {
        var next = Object.assign({}, draft)
        next.stems = choice > 0
        if (choice > 0) next.stemsBy = choice === 2 ? "category" : "track"
        draft = next
    }
    onAccepted: { landTyped(); edited(draft) }

    // The caption that heads a group, as on the pages.
    component Heading: Text {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel - 1
        font.weight: Theme.wHeading
        font.letterSpacing: 0.6
        font.capitalization: Font.AllUppercase
        color: Theme.accent
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }
    // What a choice does, said under it: no bubble has to cover a neighbour.
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }

    Heading { text: qsTr("The main file's sound"); Layout.topMargin: 0 }
    ComboField {
        objectName: "deliverAudioSource"
        Layout.fillWidth: true
        caption: qsTr("Audio source")
        tip: qsTr("Mix every track into the output, or pick the tracks to include below")
        showsTip: false
        model: [qsTr("Full timeline mix"), qsTr("Selected tracks")]
        currentIndex: root.draft.audioSelection === "custom" ? 1 : 0
        onActivated: function (i) { var next = Object.assign({}, root.draft); next.audioSelection = i === 0 ? "all" : "custom"; next.audioTracks = i === 0 ? [] : root.tracks.map(function (t) { return t.index }); root.draft = next }
    }
    Note { text: root.tracks.length > 0 ? qsTr("Each track with sound, and the channel of the file it goes to:") : qsTr("This timeline has no track with sound.") }
    Repeater {
        model: root.tracks.length
        delegate: RowLayout {
            id: trackRow
            required property int index
            readonly property var track: root.tracks[index]
            objectName: "deliverTrackRow:" + track.index
            Layout.fillWidth: true
            spacing: Theme.s2
            Toggle {
                objectName: "deliverTrack:" + trackRow.track.index
                tip: qsTr("Include %1 in the main file's sound").arg(trackRow.track.name)
                Accessible.name: trackRow.track.name
                checked: root.selected(trackRow.track.index)
                // (A click writes `checked`: bind it again to the draft.)
                onClicked: { root.select(trackRow.track.index, checked); checked = Qt.binding(function () { return root.selected(trackRow.track.index) }) }
            }
            Text {
                Layout.fillWidth: true
                text: trackRow.track.name
                textFormat: Text.PlainText // an editor's name for a track
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textPrimary
                opacity: root.selected(trackRow.track.index) ? 1.0 : Theme.disabledOpacity
            }
            ComboField {
                objectName: "deliverChannel:" + trackRow.track.index
                Layout.preferredWidth: Math.round(200 * Theme.densityScale)
                enabled: root.selected(trackRow.track.index)
                tip: qsTr("Output channel this track is routed to, or keep its own channel placement")
                readonly property var options: (root.draft.channels || 2) >= 6 ? ["stereo", "left", "right", "center", "lfe", "surroundLeft", "surroundRight"] : (root.draft.channels || 2) >= 2 ? ["stereo", "left", "right"] : ["stereo"]
                model: options.map(function (v) { return {stereo: qsTr("Original channels"), left: qsTr("Left"), right: qsTr("Right"), center: qsTr("Centre"), lfe: qsTr("LFE"), surroundLeft: qsTr("Surround left"), surroundRight: qsTr("Surround right")}[v] })
                currentIndex: Math.max(0, options.indexOf((root.draft.audioTrackMap || {})[String(trackRow.track.index)] || "stereo"))
                onActivated: function (i) { var next = Object.assign({}, root.draft.audioTrackMap || {}); next[String(trackRow.track.index)] = options[i]; root.set("audioTrackMap", next) }
            }
        }
    }
    Toggle {
        objectName: "deliverMaster"
        label: qsTr("Include master processing in the main file")
        tip: qsTr("Run the master bus processing on the mix that goes into the main file")
        checked: root.draft.includeMaster !== false
        onClicked: { root.set("includeMaster", checked); checked = Qt.binding(function () { return root.draft.includeMaster !== false }) }
    }

    Heading { text: qsTr("Stems") }
    ComboField {
        objectName: "deliverStems"
        Layout.fillWidth: true
        caption: qsTr("Stems")
        tip: qsTr("Separate 24-bit WAV files made beside the main file: one for each track, or one for each category of sound")
        showsTip: false
        model: [qsTr("None"), qsTr("One per track"), qsTr("One per category")]
        currentIndex: root.stemsChoice
        onActivated: function (i) {
            root.chooseStems(i)
            currentIndex = Qt.binding(function () { return root.stemsChoice })
        }
    }
    Note {
        objectName: "deliverStemsSummary"
        readonly property var names: root.stems.map(function (s) { return s.name })
        readonly property var files: root.stems.map(function (s) { return s.file })
        text: root.stemsChoice === 0
              ? qsTr("Only the main file is made. A stem is a 24-bit WAV of one part of the mix, the length of the render, silent where that part is.")
              : root.stemLister === null ? ""
              : root.stems.length === 0
                ? (root.stemsChoice === 2 ? qsTr("No stem would be made: no clip with sound is in use on this timeline.")
                                         : qsTr("No stem would be made: none of the chosen tracks has sound."))
                : (root.stemsChoice === 2 ? qsTr("One stem for each category in use (%1): %2.").arg(root.stems.length).arg(names.join(", "))
                                         : qsTr("One stem for each chosen track with sound (%1): %2.").arg(root.stems.length).arg(names.join(", ")))
                  + " " + qsTr("On disk: %1.").arg(files.join(", "))
                  + ((root.draft.splitSeconds || 0) > 0 ? " " + qsTr("Each part of a split render gets its own.") : "")
    }
    Note {
        visible: root.stemsChoice === 2
        text: qsTr("A clip's category is set under Modify ▸ Category. A sound with no category goes to a stem of its own, Uncategorised.")
    }
    Toggle {
        objectName: "deliverStemsMaster"
        label: qsTr("Apply master processing to stems")
        tip: qsTr("Run the master bus processing on each stem as well as its clip, track and bus processing")
        enabled: root.stemsChoice > 0
        checked: root.draft.stemsMaster === true
        onClicked: { root.set("stemsMaster", checked); checked = Qt.binding(function () { return root.draft.stemsMaster === true }) }
    }
    Note { text: qsTr("A stem always has its clips', tracks' and buses' processing. Without master processing, the stems added together are the main mix without it.") }

    Heading { text: qsTr("Splitting") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.s2
        Text { text: qsTr("Split into parts of"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase; color: Theme.textPrimary }
        ValueField {
            objectName: "deliverSplitSeconds"
            Layout.preferredWidth: Math.round(88 * Theme.densityScale)
            label: qsTr("Length of each part in seconds")
            tip: qsTr("Length of each part in seconds; every part becomes its own queue job, 0 renders one file")
            showsTip: false
            from: 0; to: 86400; step: 1; decimals: 0
            suffix: qsTr("s")
            value: root.draft.splitSeconds || 0
            onEdited: function (v) { root.set("splitSeconds", Math.round(v)) }
        }
        Text { Layout.fillWidth: true; text: qsTr("(0: one file)"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary; elide: Text.ElideRight }
    }

    Heading { text: qsTr("Encoder fallback") }
    Toggle {
        objectName: "deliverSoftwareFallback"
        label: qsTr("Use software if hardware encoding is unavailable or fails")
        tip: qsTr("Retry once with the software encoder when the hardware encoder is missing or fails")
        checked: root.draft.softwareFallback !== false
        onClicked: { root.set("softwareFallback", checked); checked = Qt.binding(function () { return root.draft.softwareFallback !== false }) }
    }
    Note { text: qsTr("Each part and stem becomes a separate queue job. The queue saves the project state used by each job.") }
}
