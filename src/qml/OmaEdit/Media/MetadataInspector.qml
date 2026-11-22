pragma ComponentBehavior: Bound
import QtQuick
import "../Ui/MediaTime.js" as MediaTime
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Edit as Edit

// The right-hand metadata inspector. Six collapsible groups over the primary
// selection; rating and tags apply to every selected clip. Fields the file
// does not carry read "—" — never a made-up value.
Rectangle {
    id: root

    property var entry: ({})           // primary selection
    property int selectedCount: 0
    property var schema: []
    signal customEdited(string key, string value)
    property real fpsFallback: 25

    signal inputSettingsRequested()
    signal attributesRequested()
    signal ratingEdited(int rating)
    // One tag on or off: each selected clip applies it to its own tags.
    signal tagEdited(string tag, bool on)
    signal notesEdited(string notes)
    signal renameRequested(string name)
    signal fieldEdited(string key, string value)

    readonly property bool hasEntry: entry.path !== undefined
    readonly property bool textActive: tagInput.activeFocus || rowsActive()

    function rowsActive() {
        for (var i = 0; i < editableRows.length; ++i)
            if (editableRows[i] && editableRows[i].textActive) return true
        for (var j = 0; j < customRows.count; ++j) if (customRows.itemAt(j) && customRows.itemAt(j).textActive) return true
        return false
    }
    property var editableRows: [nameRow, notesRow, reelRow, sceneRow, shotRow, takeRow, angleRow, commentsRow]

    // Every tag the file's container and streams carry, as imported.
    readonly property var sourceTags: entry.containerTags !== undefined && entry.containerTags !== null ? entry.containerTags : ({})
    readonly property var sourceKeys: Object.keys(sourceTags).sort()

    function v(key) { return entry[key] !== undefined && entry[key] !== null ? String(entry[key]) : "" }
    function dateLabel(key) { return v(key).replace("T", " ").substring(0, 19) }
    function timecode(frames) {
        var fps = entry.fps !== undefined && entry.fps > 0 ? entry.fps : fpsFallback
        var s = Math.floor(frames / fps)
        var ff = Math.round(frames - s * fps)
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff)
    }
    function sizeLabel() { return MediaTime.sizeText(entry.size) }
    // The rate it plays at; an interpreted clip also says what it was recorded at.
    function fpsLabel() {
        var rate = MediaTime.rateText(entry)
        if (rate === "") return ""
        if (entry.forceFps > 0 && entry.sourceFps > 0)
            return qsTr("%1 fps, interpreted (recorded at %2)").arg(rate).arg(String(Number(Number(entry.sourceFps).toFixed(3))))
        return rate + " fps"
    }
    function channelsLabel() {
        var c = entry.channels !== undefined ? entry.channels : 0
        if (c <= 0) return entry.hasAudio !== undefined && !entry.hasAudio ? qsTr("None") : ""
        return c + (c === 1 ? qsTr(" (Mono)") : c === 2 ? qsTr(" (Stereo)") : "")
    }
    function rateLabel() {
        var r = entry.sampleRate !== undefined ? entry.sampleRate : 0
        return r > 0 ? (r / 1000) + " kHz" : ""
    }
    function tagColor(tag) {
        var h = 0
        for (var i = 0; i < tag.length; ++i) h = (h * 31 + tag.charCodeAt(i)) | 0
        return Theme.labelNames[Math.abs(h) % Theme.labelNames.length]
    }

    color: Theme.panelBg
    implicitWidth: 340

    Rectangle { anchors.left: parent.left; width: Theme.hairline; height: parent.height; color: Theme.border; z: 2 }

    C.ScrollView {
        anchors.fill: parent
        anchors.leftMargin: Theme.hairline
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        C.ScrollBar.vertical: ScrollBar {}

        Column {
            width: parent.width
            spacing: 0

            EmptyState {
                width: parent.width
                height: 200
                visible: !root.hasEntry
                iconName: "sliders-horizontal"
                title: qsTr("No clip selected")
                description: qsTr("Select a clip to see its metadata.")
            }

            Rectangle {
                width: parent.width
                height: Theme.hRow + Theme.s2
                visible: root.hasEntry && root.selectedCount > 1
                color: Theme.panelRaised
                Text {
                    anchors.centerIn: parent
                    text: qsTr("%1 clips selected — production fields, notes, rating and tags apply to all").arg(root.selectedCount)
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textSecondary
                }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry
                title: qsTr("Clip Metadata")
                expanded: true
                resettable: false

                MetaRow { id: nameRow; label: qsTr("File Name"); value: root.v("name"); editable: true; onEdited: function (t) { root.renameRequested(t) } }
                MetaRow {
                    label: qsTr("Location")
                    value: root.entry.multicam !== undefined
                           ? qsTr("Multicam clip · %1 angles").arg(root.entry.angles !== undefined ? root.entry.angles.length : 0)
                           : root.v("path").replace(/\/[^\/]*$/, "")
                }
                MetaRow { label: qsTr("Created"); value: root.dateLabel("created") }
                MetaRow { label: qsTr("Modified"); value: root.dateLabel("modified") }
                MetaRow { label: qsTr("Duration"); objectName: "metaDuration"; value: MediaTime.clipFrames(root.entry) > 0 ? MediaTime.lengthText(root.entry) : "" }
                MetaRow {
                    label: qsTr("Start Timecode"); objectName: "metaStartTimecode"
                    value: root.entry.startTimecodeFrames !== undefined ? root.timecode(root.entry.startTimecodeFrames) + qsTr(" (set here)")
                         : root.entry.timecode !== undefined && root.entry.timecode !== "" ? String(root.entry.timecode) : ""
                }
                MetaRow { label: qsTr("Size"); objectName: "metaSize"; value: root.sizeLabel() }
                MetaRow {
                    label: qsTr("Rating"); custom: true
                    StarRating {
                        objectName: "ratingStars"
                        tip: qsTr("Rate this clip from one to five stars; click again to clear")
                        value: root.entry.rating !== undefined ? root.entry.rating : 0
                        onEdited: function (n) { root.ratingEdited(n) }
                    }
                }
                MetaRow {
                    label: qsTr("Tags"); custom: true
                    Flow {
                        width: parent.width
                        spacing: Theme.s1
                        Repeater {
                            model: root.entry.tags !== undefined ? root.entry.tags : []
                            delegate: Chip {
                                required property string modelData
                                tip: qsTr("A tag on this clip; the cross takes it off")
                                label: modelData
                                tint: root.tagColor(modelData)
                                removable: true
                                onRemoveRequested: root.tagEdited(modelData, false)
                            }
                        }
                        Rectangle {
                            width: Math.max(60, tagInput.contentWidth + Theme.s3 + 16)
                            height: 18
                            radius: Theme.rControl
                            color: Theme.controlBg
                            border.width: tagInput.activeFocus ? Theme.focusRing : Theme.hairline
                            border.color: tagInput.activeFocus ? Theme.accent : Theme.border
                            Icon { anchors.left: parent.left; anchors.leftMargin: 3; anchors.verticalCenter: parent.verticalCenter; name: "plus"; size: 10; color: Theme.textMuted }
                            TextInput {
                                id: tagInput
                                objectName: "tagInput"
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: Theme.s1
                                verticalAlignment: TextInput.AlignVCenter
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fsLabel - 1
                                color: Theme.textPrimary
                                selectionColor: Theme.accent
                                clip: true
                                Accessible.name: qsTr("Add tag")
                                Text {
                                    anchors.fill: parent
                                    visible: !tagInput.text && !tagInput.activeFocus
                                    text: qsTr("Add tag")
                                    verticalAlignment: Text.AlignVCenter
                                    font: tagInput.font
                                    color: Theme.textMuted
                                }
                                onAccepted: {
                                    var t = text.trim()
                                    if (t !== "")
                                        root.tagEdited(t, true)
                                    text = ""
                                }
                                Keys.onEscapePressed: { text = ""; focus = false }
                            }
                        }
                    }
                }
                MetaRow { id: notesRow; label: qsTr("Notes"); value: root.v("notes"); editable: true; multiline: true; placeholder: qsTr("Add a note"); onEdited: function (t) { root.notesEdited(t) } }
                // The clip's own attributes belong with the clip, not under LUT.
                ToolButton { objectName: "metaClipAttributes"; text: qsTr("Clip Attributes…"); enabled: root.entry.multicam === undefined; onClicked: root.attributesRequested() }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry
                title: qsTr("Camera")
                expanded: true
                resettable: false

                MetaRow { label: qsTr("Make and Model"); value: [root.v("make"), root.v("model")].filter(function (s) { return s !== "" }).join(" ") }
                MetaRow { label: qsTr("Lens"); value: root.v("lens") }
                MetaRow { label: qsTr("Focal Length"); value: root.v("focalLength") }
                MetaRow { label: qsTr("Focus Distance"); value: root.v("focusDistance") }
                MetaRow { label: qsTr("Aperture"); value: root.v("aperture") }
                MetaRow { label: qsTr("ISO"); value: root.v("iso") }
                MetaRow { label: qsTr("Shutter"); value: root.v("shutter") }
                MetaRow { label: qsTr("White Balance"); value: root.v("whiteBalance") }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry
                title: qsTr("Reel / Scene")
                expanded: true
                resettable: false

                MetaRow { id: reelRow; label: qsTr("Reel"); value: root.v("reel"); editable: true; placeholder: qsTr("—"); onEdited: function (t) { root.fieldEdited("reel", t) } }
                MetaRow { id: sceneRow; label: qsTr("Scene"); value: root.v("scene"); editable: true; onEdited: function (t) { root.fieldEdited("scene", t) } }
                MetaRow { id: shotRow; label: qsTr("Shot"); value: root.v("shot"); editable: true; onEdited: function (t) { root.fieldEdited("shot", t) } }
                MetaRow { id: takeRow; label: qsTr("Number"); value: root.v("take"); editable: true; onEdited: function (t) { root.fieldEdited("take", t) } }
                MetaRow { id: angleRow; label: qsTr("Angle"); value: root.v("angle"); editable: true; onEdited: function (t) { root.fieldEdited("angle", t) } }
                MetaRow {
                    label: qsTr("Keeper"); custom: true
                    Toggle {
                        tip: qsTr("Mark this clip as one worth using, for sorting and filtering")
                        checked: root.entry.goodTake !== undefined && root.entry.goodTake
                        onToggled: root.fieldEdited("goodTake", checked ? "1" : "")
                    }
                }
                MetaRow { id: commentsRow; label: qsTr("Comments"); value: root.v("comments"); editable: true; multiline: true; placeholder: qsTr("Add a comment"); onEdited: function (t) { root.fieldEdited("comments", t) } }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry && MediaTime.hasPicture(root.entry)
                title: qsTr("Video")
                expanded: true
                resettable: false

                MetaRow { label: qsTr("Codec"); value: root.v("videoCodec").toUpperCase() }
                MetaRow { label: qsTr("Codec Name"); value: root.v("videoCodecLong"); multiline: true; visible: value !== "" }
                MetaRow { label: qsTr("Resolution"); objectName: "metaResolution"; value: root.entry.width !== undefined && root.entry.width > 0 ? root.entry.width + " × " + root.entry.height : "" }
                MetaRow { label: qsTr("Frame Rate"); objectName: "metaFrameRate"; value: root.fpsLabel(); multiline: true; visible: !MediaTime.isStill(root.entry) }
                MetaRow { label: qsTr("Bit Depth"); value: root.entry.bitDepth !== undefined && root.entry.bitDepth > 0 ? root.entry.bitDepth + qsTr(" bit") : "" }
                MetaRow { label: qsTr("Pixel Aspect"); value: root.v("pixelAspect") }
                MetaRow { label: qsTr("Pixel Format"); value: root.v("pixelFormat") }
                MetaRow { label: qsTr("Colour Space"); value: root.v("colorSpace") }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry
                title: qsTr("Audio")
                expanded: true
                resettable: false

                MetaRow { label: qsTr("Channels"); value: root.channelsLabel() }
                MetaRow { label: qsTr("Sample Rate"); value: root.rateLabel() }
                MetaRow { label: qsTr("Codec"); value: root.v("audioCodec").toUpperCase() }
                MetaRow { label: qsTr("Bit Depth"); value: root.entry.audioBitDepth !== undefined && root.entry.audioBitDepth > 0 ? root.entry.audioBitDepth + qsTr(" bit") : "" }
                // Once its beats are mapped (Map Beats).
                MetaRow {
                    objectName: "metaTempo"
                    label: qsTr("Tempo")
                    visible: value !== ""
                    value: root.entry.beatMap !== undefined && root.entry.beatMap.bpm > 0
                           ? qsTr("%1 BPM, %2 beats").arg(Number(root.entry.beatMap.bpm).toFixed(1)).arg(root.entry.beatMap.beats.length) : ""
                }
            }

            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry && root.sourceKeys.length > 0
                title: qsTr("All Metadata")
                expanded: false
                resettable: false

                Repeater {
                    model: root.sourceKeys.length
                    delegate: MetaRow {
                        required property int index
                        labelWidth: 132
                        label: root.sourceKeys[index] !== undefined ? root.sourceKeys[index] : ""
                        value: root.sourceKeys[index] !== undefined ? String(root.sourceTags[root.sourceKeys[index]]) : ""
                        multiline: true
                    }
                }
            }

            Edit.InspectorSection {
                width: parent.width; visible: root.hasEntry && root.schema.length > 0
                title: qsTr("Custom Metadata"); expanded: true; resettable: false
                Repeater {
                    id: customRows
                    model: root.schema
                    delegate: MetaRow {
                        required property var modelData
                        label: modelData.label
                        value: (root.entry.customMetadata || {})[modelData.key] !== undefined ? String(root.entry.customMetadata[modelData.key]) : ""
                        editable: true
                        onEdited: function (text) { root.customEdited(modelData.key, text) }
                    }
                }
            }
            Edit.InspectorSection {
                width: parent.width
                visible: root.hasEntry
                title: qsTr("LUT")
                expanded: false
                resettable: false

                MetaRow { label: qsTr("Input LUT"); value: root.v("inputLut").split("/").pop(); placeholder: qsTr("None") }
                MetaRow { label: qsTr("Input Matrix"); value: root.entry.inputSpace ? "Rec. " + root.entry.inputSpace : qsTr("Source metadata") }
                ToolButton { text: qsTr("Input Settings…"); onClicked: root.inputSettingsRequested() }
            }

            Item { width: 1; height: Theme.s4 }
        }
    }
}
