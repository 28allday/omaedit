pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The Render Settings column. Every row edits one ExportSpec key through
// `specEdited`; the owner keeps the spec and hands it back. Rows the
// current format cannot use fold away rather than sitting disabled.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property var spec: ({})
    property string fileName: ""
    property string saveDir: ""
    property string rangeMode: "full"   // full | inout | selection | clips
    property int handles: 0
    property var encoders: ({})
    property int timelineWidth: 0
    property int timelineHeight: 0
    property real timelineFps: 0
    property bool hasRange: false
    property int selectionCount: 0
    property bool hasCaptions: false
    property int captionCount: 0
    property string presetName: ""

    signal specEdited(string key, var value)
    signal fileNameEdited(string name)
    signal saveDirEdited(string dir)
    signal folderRequested()
    signal rangeModeEdited(string mode)
    signal handlesEdited(int frames)
    signal metadataRequested()
    signal audioOutputsRequested()
    signal advancedRequested()
    signal resetRequested()

    readonly property bool textActive: nameField.activeFocus || dirField.activeFocus || customW.activeFocus || customH.activeFocus

    color: Theme.panelBg
    implicitWidth: 460

    component SettingValue: ValueField {
        property real modelValue: 0
        value: modelValue
        onModelValueChanged: value = modelValue
    }
    function v(key, fallback) { return spec[key] !== undefined ? spec[key] : fallback }
    readonly property string container: v("container", "mp4")
    readonly property bool audioOnlyContainer: container === "m4a" || container === "wav" || container === "mp3" || container === "flac"
    // A numbered image sequence: a folder of pictures, no sound, the timeline's own rate.
    readonly property bool sequenceContainer: container === "png" || container === "tiff" || container === "exr" || container === "jpg"
    readonly property bool video: !audioOnlyContainer && v("video", true)
    readonly property bool audio: !sequenceContainer && v("audio", true)
    readonly property string vcodec: v("vcodec", "h264")
    readonly property string acodec: v("acodec", "aac")
    readonly property bool fixedQuality: vcodec === "prores" || vcodec === "dnxhr" || vcodec === "ffv1"
    readonly property bool lossyAudio: acodec === "aac" || acodec === "opus" || acodec === "mp3" || acodec === "vorbis"

    // ── Option tables ────────────────────────────────────────────────────
    readonly property var containers: [
        { key: "mp4", label: "MP4" }, { key: "mov", label: "MOV (QuickTime)" }, { key: "mkv", label: "MKV (Matroska)" }, { key: "webm", label: "WebM" },
        { key: "m4a", label: qsTr("M4A (audio)") }, { key: "wav", label: qsTr("WAV (audio)") }, { key: "mp3", label: qsTr("MP3 (audio)") }, { key: "flac", label: qsTr("FLAC (audio)") },
        { key: "png", label: qsTr("PNG (image sequence)") }, { key: "tiff", label: qsTr("TIFF (image sequence)") },
        { key: "exr", label: qsTr("OpenEXR (image sequence)") }, { key: "jpg", label: qsTr("JPEG (image sequence)") }
    ]
    readonly property var codecs: {
        var all = [
            { key: "h264", label: "H.264", ok: ["mp4", "mov", "mkv"] }, { key: "hevc", label: "H.265 / HEVC", ok: ["mp4", "mov", "mkv"] },
            { key: "prores", label: "ProRes", ok: ["mov", "mkv"] }, { key: "dnxhr", label: "DNxHR", ok: ["mov", "mkv"] },
            { key: "vp9", label: "VP9", ok: ["webm", "mkv", "mp4"] }, { key: "av1", label: "AV1", ok: ["mp4", "mkv", "webm"] },
            { key: "mpeg2", label: "MPEG-2", ok: ["mov", "mkv", "mp4"] }, { key: "ffv1", label: qsTr("FFV1 (lossless)"), ok: ["mkv"] },
            { key: "apv", label: "APV", ok: ["mov", "mp4", "mkv"] },
            { key: "png16", label: qsTr("16-bit"), ok: ["png"] }, { key: "png8", label: qsTr("8-bit"), ok: ["png"] },
            { key: "tiff16", label: qsTr("16-bit"), ok: ["tiff"] }, { key: "exr", label: qsTr("Scene-linear, half float"), ok: ["exr"] },
            { key: "jpeg", label: "JPEG", ok: ["jpg"] }
        ]
        return all.filter(function (c) { return c.ok.indexOf(root.container) >= 0 })
    }
    readonly property var resolutions: [
        { w: 0, h: 0, label: root.timelineWidth > 0 ? qsTr("Timeline (%1×%2)").arg(root.timelineWidth).arg(root.timelineHeight) : qsTr("Timeline size") },
        { w: 3840, h: 2160, label: "3840×2160 (4K UHD)" }, { w: 4096, h: 2160, label: "4096×2160 (DCI 4K)" },
        { w: 2560, h: 1440, label: "2560×1440 (QHD)" }, { w: 1920, h: 1080, label: "1920×1080 (HD)" },
        { w: 1280, h: 720, label: "1280×720" }, { w: 1080, h: 1920, label: qsTr("1080×1920 (vertical)") },
        { w: 1080, h: 1080, label: qsTr("1080×1080 (square)") }, { w: 854, h: 480, label: "854×480" },
        { w: -1, h: -1, label: qsTr("Custom…") }
    ]
    readonly property var frameRates: [
        { fps: 0, label: root.timelineFps > 0 ? qsTr("Timeline (%1 fps)").arg(fpsText(root.timelineFps)) : qsTr("Timeline rate") },
        { fps: 23.976, label: "23.976 fps" }, { fps: 24, label: "24 fps" }, { fps: 25, label: "25 fps" }, { fps: 29.97, label: "29.97 fps" },
        { fps: 30, label: "30 fps" }, { fps: 50, label: "50 fps" }, { fps: 59.94, label: "59.94 fps" }, { fps: 60, label: "60 fps" }
    ]
    readonly property var qualities: [
        { key: "best", label: qsTr("Best"), crf: 14 }, { key: "high", label: qsTr("High"), crf: 18 },
        { key: "medium", label: qsTr("Medium"), crf: 23 }, { key: "low", label: qsTr("Low"), crf: 28 },
        { key: "bitrate", label: qsTr("Bitrate…") }
    ]
    readonly property var colorSpaces: [
        { key: "rec709", label: "Rec.709" }, { key: "rec601", label: "Rec.601" }, { key: "srgb", label: "sRGB" },
        { key: "rec2020", label: "Rec.2020" }, { key: "pq", label: qsTr("HDR PQ (ST 2084)") }, { key: "hlg", label: qsTr("HDR HLG") }
    ]
    readonly property var audioCodecs: {
        var all = [
            { key: "aac", label: "AAC", ok: ["mp4", "mov", "mkv", "m4a"] }, { key: "opus", label: "Opus", ok: ["webm", "mkv", "mp4"] },
            { key: "mp3", label: "MP3", ok: ["mp4", "mov", "mkv", "mp3"] }, { key: "pcm16", label: qsTr("PCM 16-bit"), ok: ["mov", "mkv", "wav", "mp4"] },
            { key: "pcm24", label: qsTr("PCM 24-bit"), ok: ["mov", "mkv", "wav", "mp4"] }, { key: "flac", label: "FLAC", ok: ["mkv", "flac", "mp4"] },
            { key: "vorbis", label: "Vorbis", ok: ["webm", "mkv"] }
        ]
        return all.filter(function (c) { return c.ok.indexOf(root.container) >= 0 })
    }
    readonly property var audioBitrates: [96, 128, 160, 192, 256, 320]
    readonly property var sampleRates: acodec === "opus" ? [48000] : acodec === "mp3" || acodec === "vorbis" ? [44100, 48000] : [44100, 48000, 96000]
    readonly property var channelOptions: [{ n: 1, label: qsTr("Mono") }, { n: 2, label: qsTr("Stereo") }].concat(acodec === "mp3" ? [] : [{ n: 6, label: qsTr("5.1 Surround") }])
    readonly property var subtitleOptions: [
        { key: "none", label: qsTr("None") }, { key: "burn", label: qsTr("Burn in") },
        { key: "embed", label: qsTr("Embed as a track") }, { key: "sidecar", label: qsTr("Sidecar files") }
    ]
    // The sidecar files' format: a file for each caption track, named by its language.
    readonly property var sidecarFormats: [
        { key: "srt", label: qsTr("SubRip (.srt)") }, { key: "vtt", label: qsTr("WebVTT (.vtt)") },
        { key: "itt", label: qsTr("iTT, timed text for video stores (.itt)") }, { key: "scc", label: qsTr("SCC, broadcast captions (.scc)") }
    ]
    // The ways of encoding found on this machine, not a fixed list of
    // makers: one that did not open here is left out, unless the settings
    // already name it (a preset made elsewhere), when it is shown as not
    // available so the choice can be seen and changed.
    readonly property string chosenHw: v("hw", "software")
    readonly property var hwOptions: [
        { key: "software", label: qsTr("Software") },
        { key: "nvenc", label: "NVIDIA NVENC" + hwSuffix("nvenc") },
        { key: "vaapi", label: "VA-API" + hwSuffix("vaapi") },
        { key: "vulkan", label: "Vulkan ProRes" + hwSuffix("vulkan") }
    ].filter(function (h) {
        var found = encoders[h.key]
        return h.key === "software" || h.key === chosenHw || found === undefined || found === "yes" || found === "probing"
    })
    function hwSuffix(key) {
        var s = encoders[key]
        if (s === undefined) return ""
        if (s === "probing") return qsTr(" (checking…)")
        return s === "yes" ? "" : qsTr(" (not available)")
    }
    function fpsText(f) { return Math.abs(f - Math.round(f)) < 0.001 ? String(Math.round(f)) : f.toFixed(3) }
    function indexOfKey(list, key, field) {
        for (var i = 0; i < list.length; ++i) if (list[i][field || "key"] === key) return i
        return 0
    }
    readonly property int resolutionIndex: {
        var w = v("width", 0), h = v("height", 0)
        if (w <= 0 || h <= 0) return 0
        for (var i = 1; i < resolutions.length - 1; ++i) if (resolutions[i].w === w && resolutions[i].h === h) return i
        return resolutions.length - 1
    }
    property bool customResolution: false
    readonly property int frameRateIndex: {
        var f = v("fps", 0)
        if (f <= 0) return 0
        for (var i = 1; i < frameRates.length; ++i) if (Math.abs(frameRates[i].fps - f) < 0.01) return i
        return 0
    }
    readonly property int qualityIndex: {
        if (v("rateMode", "quality") !== "quality") return 4
        var q = v("quality", 18)
        var best = 1, dist = 99
        for (var i = 0; i < 4; ++i) { var d = Math.abs(qualities[i].crf - q); if (d < dist) { dist = d; best = i } }
        return best
    }
    readonly property string qualityDetail: {
        if (fixedQuality) return v("vprofile", "") !== "" ? String(v("vprofile", "")).toUpperCase() : ""
        if (v("rateMode", "quality") !== "quality") return (v("rateMode", "") === "cbr" ? qsTr("%1 Mb/s CBR") : qsTr("%1 Mb/s VBR")).arg((v("bitrate", 0) / 1000).toFixed(1))
        var q = v("quality", 18)
        // APV encodes at a quantiser: quality + 12, capped (75 at 12 bits, else 63).
        if (vcodec === "apv") return "QP " + Math.min(q + 12, String(v("vprofile", "")).endsWith("12") ? 75 : 63)
        return vcodec === "mpeg2" ? "Q " + (2 + Math.floor(q * 29 / 51))
             : "CRF " + (vcodec === "vp9" || vcodec === "av1" ? Math.floor(q * 63 / 51) : q)
    }

    Rectangle { anchors.left: parent.left; width: Theme.hairline; height: parent.height; color: Theme.border; z: 2 }

    Media.SectionTitle {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3 + Theme.hairline
        anchors.rightMargin: Theme.s3
        height: settingsActions.inBar ? Theme.s1 : implicitHeight
        text: root.dockBar === null ? qsTr("Render Settings") : ""
    }

    DockBarActions {
        id: settingsActions
        dockBar: root.dockBar
        fallback: title
        fallbackMargin: 0
        spacing: Theme.s1

        Text {
            visible: root.presetName !== "" && !settingsActions.inBar // docked, the bar shows it
            text: root.presetName
            elide: Text.ElideRight
            width: Math.min(implicitWidth, title.width * 0.5)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
            anchors.verticalCenter: parent.verticalCenter
        }
        IconButton {
            iconName: "rotate-ccw"
            text: qsTr("Reset to the preset")
            implicitWidth: Theme.szIcon + Theme.s1
            implicitHeight: Theme.szIcon + Theme.s1
            onClicked: root.resetRequested()
        }
    }

    C.ScrollView {
        id: settingsScroll
        objectName: "renderSettingsScroll"
        anchors.top: title.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.hairline
        contentWidth: availableWidth
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        // Shown whenever the form runs past the panel, so the rows below
        // never look like the end of the settings.
        C.ScrollBar.vertical: ScrollBar {
            objectName: "renderSettingsScrollY"
            policy: settingsScroll.contentHeight > settingsScroll.height + 0.5 ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded
        }

        Column {
            id: form
            width: parent.width
            leftPadding: Theme.s3
            rightPadding: Theme.s3
            spacing: 0
            readonly property int w: width - leftPadding - rightPadding

            // ── File ───────────────────────────────────────────────────────
            SettingRow {
                label: qsTr("File Name")
                width: form.w
                Row {
                    width: parent.width
                    spacing: Theme.s1
                    Rectangle {
                        width: parent.width - tokenButton.width - parent.spacing
                        height: Theme.hControl
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: Theme.hairline
                        border.color: nameField.activeFocus ? Theme.accent : Theme.border
                        C.TextField {
                            id: nameField
                            objectName: "setting:fileName"
                            hoverEnabled: true
                            C.ToolTip.text: qsTr("Name of the rendered file without its extension; tokens such as {date} fill in at render time"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s2
                            anchors.rightMargin: Theme.s2
                            leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
                            text: root.fileName
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsControl
                            color: Theme.textPrimary
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentText
                            background: null
                            verticalAlignment: TextInput.AlignVCenter
                            onEditingFinished: if (text !== root.fileName) root.fileNameEdited(text)
                            Keys.onEscapePressed: { text = Qt.binding(() => root.fileName); focus = false }
                        }
                    }
                    IconButton {
                        id: tokenButton
                        iconName: "chevrons-up-down"
                        text: qsTr("Insert a name token")
                        flat: false
                        implicitWidth: Theme.hControl
                        implicitHeight: Theme.hControl
                        onClicked: tokenMenu.popup()
                    }
                }
            }
            SettingRow {
                label: qsTr("Save Location")
                width: form.w
                Row {
                    width: parent.width
                    spacing: Theme.s1
                    Rectangle {
                        width: parent.width - folderButton.width - parent.spacing
                        height: Theme.hControl
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: Theme.hairline
                        border.color: dirField.activeFocus ? Theme.accent : Theme.border
                        C.TextField {
                            id: dirField
                            objectName: "setting:saveDir"
                            hoverEnabled: true
                            C.ToolTip.text: qsTr("Folder the rendered file is written to; type a path or pick one with the folder button"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s2
                            anchors.rightMargin: Theme.s2
                            leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
                            text: root.saveDir
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fsControl
                            color: Theme.textPrimary
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentText
                            background: null
                            verticalAlignment: TextInput.AlignVCenter
                            onEditingFinished: if (text !== root.saveDir) root.saveDirEdited(text)
                            Keys.onEscapePressed: { text = Qt.binding(() => root.saveDir); focus = false }
                        }
                    }
                    IconButton {
                        id: folderButton
                        objectName: "chooseFolder"
                        iconName: "folder"
                        text: qsTr("Choose a folder")
                        flat: false
                        implicitWidth: Theme.hControl
                        implicitHeight: Theme.hControl
                        onClicked: root.folderRequested()
                    }
                }
            }

            // ── Format ─────────────────────────────────────────────────────
            SettingRow {
                label: qsTr("Format")
                width: form.w
                ComboField {
                    objectName: "setting:container"
                    tip: qsTr("File type of the export; it decides which codecs are offered and whether video is included")
                    width: parent.width
                    model: root.containers.map(function (c) { return c.label })
                    currentIndex: root.indexOfKey(root.containers, root.container)
                    onActivated: function (i) { root.specEdited("container", root.containers[i].key) }
                }
            }
            SettingRow {
                label: qsTr("Codec")
                width: form.w
                visible: root.video
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:vcodec"
                    tip: qsTr("Video codec the export is encoded with; ProRes, DNxHR and FFV1 keep quality for grading")
                    width: parent.width
                    model: root.codecs.map(function (c) { return c.label })
                    currentIndex: root.indexOfKey(root.codecs, root.vcodec)
                    onActivated: function (i) { root.specEdited("vcodec", root.codecs[i].key) }
                }
            }
            SettingRow {
                label: qsTr("Resolution")
                width: form.w
                visible: root.video
                height: visible ? implicitHeight : 0
                Column {
                    width: parent.width
                    spacing: Theme.s1
                    ComboField {
                        objectName: "setting:resolution"
                        tip: qsTr("Frame size of the export: the timeline's own size, a standard size, or a custom one")
                        width: parent.width
                        model: root.resolutions.map(function (r) { return r.label })
                        currentIndex: root.customResolution ? root.resolutions.length - 1 : root.resolutionIndex
                        onActivated: function (i) {
                            var r = root.resolutions[i]
                            if (r.w < 0) { root.customResolution = true; return }
                            root.customResolution = false
                            root.specEdited("size", { width: r.w, height: r.h })
                        }
                    }
                    Row {
                        visible: root.customResolution || root.resolutionIndex === root.resolutions.length - 1
                        spacing: Theme.s2
                        SettingValue {
                            id: customW
                            objectName: "setting:width"
                            tip: qsTr("Custom frame width in pixels, rounded to an even number")
                            label: qsTr("Width")
                            modelValue: root.v("width", 0) > 0 ? root.v("width", 0) : root.timelineWidth
                            from: 16; to: 16384; decimals: 0; step: 2; suffix: "px"
                            implicitWidth: 96
                            onEdited: function (val) { root.specEdited("size", { width: Math.round(val / 2) * 2, height: root.v("height", 0) > 0 ? root.v("height", 0) : root.timelineHeight }) }
                        }
                        Text { text: "×"; color: Theme.textMuted; font.pixelSize: Theme.fsBase; anchors.verticalCenter: parent.verticalCenter }
                        SettingValue {
                            id: customH
                            objectName: "setting:height"
                            tip: qsTr("Custom frame height in pixels, rounded to an even number")
                            label: qsTr("Height")
                            modelValue: root.v("height", 0) > 0 ? root.v("height", 0) : root.timelineHeight
                            from: 16; to: 16384; decimals: 0; step: 2; suffix: "px"
                            implicitWidth: 96
                            onEdited: function (val) { root.specEdited("size", { width: root.v("width", 0) > 0 ? root.v("width", 0) : root.timelineWidth, height: Math.round(val / 2) * 2 }) }
                        }
                    }
                }
            }
            SettingRow {
                label: qsTr("Frame Rate")
                width: form.w
                visible: root.video && !root.sequenceContainer
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:fps"
                    tip: qsTr("Frame rate of the export; a rate other than the timeline's re-times the render afterwards")
                    width: parent.width
                    model: root.frameRates.map(function (r) { return r.label })
                    currentIndex: root.frameRateIndex
                    onActivated: function (i) { root.specEdited("fps", root.frameRates[i].fps) }
                }
            }
            SettingRow {
                label: qsTr("Quality")
                width: form.w
                visible: root.video && !root.sequenceContainer
                height: visible ? implicitHeight : 0
                Row {
                    width: parent.width
                    spacing: Theme.s1
                    ComboField {
                        objectName: "setting:quality"
                        tip: qsTr("Picture quality against file size; Bitrate… switches to a target bitrate in the advanced settings")
                        width: parent.width - gear.width - parent.spacing
                        enabled: !root.fixedQuality
                        caption: root.qualityDetail
                        model: root.qualities.map(function (q) { return q.label })
                        currentIndex: root.qualityIndex
                        onActivated: function (i) {
                            if (i === 4) { root.specEdited("rateMode", root.v("rateMode", "quality") === "quality" ? "vbr" : root.v("rateMode", "vbr")); root.advancedRequested(); return }
                            root.specEdited("qualityPreset", root.qualities[i].crf)
                        }
                    }
                    IconButton {
                        id: gear
                        objectName: "advancedSettings"
                        iconName: "settings"
                        text: qsTr("Advanced video settings")
                        flat: false
                        implicitWidth: Theme.hControl
                        implicitHeight: Theme.hControl
                        onClicked: root.advancedRequested()
                    }
                }
            }
            SettingRow {
                label: qsTr("Colour space")
                width: form.w
                visible: root.video
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:colorSpace"
                    tip: qsTr("Output colour space and transfer; sRGB, Rec.2020 and HDR require OpenColorIO colour conversion in the Colour workspace")
                    width: parent.width
                    model: root.colorSpaces.map(function (c) { return c.label })
                    currentIndex: root.indexOfKey(root.colorSpaces, root.v("colorSpace", "rec709"))
                    onActivated: function (i) { root.specEdited("colorSpace", root.colorSpaces[i].key) }
                }
            }
            SettingRow {
                label: qsTr("Tone mapping")
                width: form.w
                visible: root.video
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:toneMap"
                    tip: qsTr("How bright light is brought down to the file. As in the viewer exports what you see while grading; None clips what the file cannot hold")
                    width: parent.width
                    readonly property var maps: ["", "none", "ACES 1.3", "ACES 2.0"]
                    model: [qsTr("As in the viewer"), qsTr("None"), "ACES 1.3", "ACES 2.0"]
                    currentIndex: Math.max(0, maps.indexOf(root.v("toneMap", "")))
                    onActivated: function (i) { root.specEdited("toneMap", maps[i]) }
                }
            }

            // ── Audio ──────────────────────────────────────────────────────
            SettingRow {
                label: qsTr("Audio Codec")
                width: form.w
                visible: root.audio
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:acodec"
                    tip: qsTr("Audio codec; AAC and Opus keep files small, PCM and FLAC are lossless")
                    width: parent.width
                    model: root.audioCodecs.map(function (c) { return c.label })
                    currentIndex: root.indexOfKey(root.audioCodecs, root.acodec)
                    onActivated: function (i) { root.specEdited("acodec", root.audioCodecs[i].key) }
                }
            }
            SettingRow {
                label: qsTr("Audio Bitrate")
                width: form.w
                visible: root.audio && root.lossyAudio
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:abitrate"
                    tip: qsTr("Bitrate of the compressed audio in kbps; higher keeps more detail at a larger size")
                    width: parent.width
                    model: root.audioBitrates.map(function (b) { return qsTr("%1 kbps").arg(b) })
                    currentIndex: Math.max(0, root.audioBitrates.indexOf(root.v("abitrate", 192)))
                    onActivated: function (i) { root.specEdited("abitrate", root.audioBitrates[i]) }
                }
            }
            SettingRow {
                label: qsTr("Sample Rate")
                width: form.w
                visible: root.audio
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:arate"
                    tip: qsTr("Sample rate of the exported audio; 48 kHz is the video standard")
                    width: parent.width
                    model: root.sampleRates.map(function (r) { return qsTr("%1 kHz").arg(r / 1000) })
                    currentIndex: Math.max(0, root.sampleRates.indexOf(root.v("arate", 48000)))
                    onActivated: function (i) { root.specEdited("arate", root.sampleRates[i]) }
                }
            }
            SettingRow {
                label: qsTr("Channels")
                width: form.w
                visible: root.audio
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:channels"
                    tip: qsTr("Channel layout of the exported audio: mono, stereo or 5.1 surround")
                    width: parent.width
                    model: root.channelOptions.map(function (c) { return c.label })
                    currentIndex: root.indexOfKey(root.channelOptions, root.v("channels", 2), "n")
                    onActivated: function (i) { root.specEdited("channels", root.channelOptions[i].n) }
                }
            }
            SettingRow {
                label: qsTr("Outputs")
                width: form.w
                ToolButton { objectName: "setting:audioOutputs"; width: parent.width; text: qsTr("Tracks, stems and file splitting…"); showLabel: true; iconName: "settings-2"; onClicked: root.audioOutputsRequested() }
            }
            SettingRow {
                label: qsTr("Subtitles")
                width: form.w
                visible: root.video && !root.sequenceContainer
                height: visible ? implicitHeight : 0
                Row {
                    width: parent.width
                    spacing: Theme.s1
                    ComboField {
                        objectName: "setting:subtitles"
                        tip: qsTr("What happens to timeline captions: burnt into the picture, embedded as a track for each caption track with its language, or written beside the video as a file for each caption track")
                        width: parent.width - subInfo.width - parent.spacing
                        model: root.subtitleOptions.map(function (s) { return s.label })
                        currentIndex: root.indexOfKey(root.subtitleOptions, root.v("subtitles", "none"))
                        onActivated: function (i) { root.specEdited("subtitles", root.subtitleOptions[i].key) }
                    }
                    Rectangle {
                        id: subInfo
                        width: Theme.hControl + Theme.s2
                        height: Theme.hControl
                        radius: Theme.rControl
                        color: Theme.controlBg
                        border.width: Theme.hairline
                        border.color: Theme.border
                        Text {
                            anchors.centerIn: parent
                            text: root.hasCaptions ? root.captionCount : "…"
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fsLabel
                            color: root.hasCaptions ? Theme.textPrimary : Theme.textMuted
                        }
                        HoverHandler { id: subHover }
                        Tooltip { visible: subHover.hovered; text: root.hasCaptions ? qsTr("%1 caption cues on the timeline").arg(root.captionCount) : qsTr("No captions on the timeline (File ▸ Import Captions…)") }
                    }
                }
            }
            SettingRow {
                label: qsTr("Caption files")
                width: form.w
                visible: root.video && !root.sequenceContainer && root.v("subtitles", "none") === "sidecar"
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:sidecarFormat"
                    tip: qsTr("The format of the caption files written beside the video: one for each caption track, named by its language, such as Film.fr.srt")
                    width: parent.width
                    model: root.sidecarFormats.map(function (f) { return f.label })
                    currentIndex: root.indexOfKey(root.sidecarFormats, root.v("sidecarFormat", "srt"))
                    onActivated: function (i) { root.specEdited("sidecarFormat", root.sidecarFormats[i].key) }
                }
            }
            SettingRow {
                label: qsTr("Metadata")
                width: form.w
                C.AbstractButton {
                    id: metaButton
                    objectName: "setting:metadata"
                    width: parent.width
                    height: Theme.hControl
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.name: qsTr("Edit metadata")
                    background: Rectangle {
                        radius: Theme.rControl
                        color: metaButton.down ? Theme.pressedOn(Theme.controlBg) : metaButton.hovered ? Theme.hovered(Theme.controlBg) : Theme.controlBg
                        border.width: Theme.hairline
                        border.color: Theme.border
                    }
                    contentItem: Text {
                        text: {
                            var m = root.v("metadata", {})
                            var n = 0
                            for (var k in m) if (m[k] !== "") ++n
                            return n > 0 ? qsTr("Edit… (%1 set)").arg(n) : qsTr("Edit…")
                        }
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsControl
                        color: Theme.textPrimary
                    }
                    onClicked: root.metadataRequested()
                }
            }
            SettingRow {
                label: qsTr("Hardware Acceleration")
                width: form.w
                visible: root.video && !root.sequenceContainer
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:hw"
                    tip: qsTr("Use the CPU, NVIDIA/VA-API for H.264, HEVC and AV1, or Vulkan for progressive ProRes 422")
                    width: parent.width
                    model: root.hwOptions.map(function (h) { return h.label })
                    currentIndex: root.indexOfKey(root.hwOptions, root.chosenHw)
                    onActivated: function (i) { root.specEdited("hw", root.hwOptions[i].key) }
                }
            }

            SettingRow {
                label: qsTr("VA-API Device")
                width: form.w
                visible: root.video && root.v("hw", "software") === "vaapi"
                height: visible ? implicitHeight : 0
                ComboField {
                    objectName: "setting:vaapiDevice"
                    tip: qsTr("Render node the VA-API encoder runs on; Automatic picks a device tested for this codec")
                    width: parent.width
                    readonly property var nodes: [""].concat(root.encoders.renderNodes || [])
                    readonly property string selected: root.v("vaapiDevice", "")
                    readonly property var choices: selected && nodes.indexOf(selected) < 0 ? nodes.concat([selected]) : nodes
                    model: choices.map(function (n) { return n || qsTr("Automatic · tested device for this codec") })
                    currentIndex: Math.max(0, choices.indexOf(selected))
                    onActivated: function (i) { root.specEdited("vaapiDevice", choices[i]) }
                }
            }

            Item { width: 1; height: Theme.s2 }
            Rectangle { width: form.w; height: Theme.hairline; color: Theme.border }
            Item { width: 1; height: Theme.s2 }

            // ── Render ─────────────────────────────────────────────────────
            Text {
                text: qsTr("Render")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                font.weight: Theme.wHeading
                color: Theme.textPrimary
                bottomPadding: Theme.s1
            }
            Grid {
                columns: 2
                columnSpacing: Theme.s4
                rowSpacing: Theme.s1
                width: form.w
                Repeater {
                    // A constant model: the delegates must not rebuild when the
                    // range or selection changes (a click mid-change would die).
                    model: [
                        { key: "full", label: qsTr("Full Timeline") },
                        { key: "inout", label: qsTr("In/Out Range") },
                        { key: "selection", label: qsTr("Selected Clips") },
                        { key: "clips", label: qsTr("Individual Clips") }
                    ]
                    delegate: C.AbstractButton {
                        id: radio
                        required property var modelData
                        objectName: "rangeMode:" + modelData.key
                        readonly property bool on: root.rangeMode === modelData.key
                        enabled: modelData.key === "full" ? true
                               : modelData.key === "inout" ? root.hasRange : root.selectionCount > 0
                        opacity: enabled ? 1 : Theme.disabledOpacity
                        implicitHeight: Theme.hRow
                        implicitWidth: radioLabel.implicitWidth + 24
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.label
                        Accessible.checked: on
                        background: null
                        contentItem: Row {
                            spacing: Theme.s2
                            Rectangle {
                                width: 14; height: 14; radius: 7
                                anchors.verticalCenter: parent.verticalCenter
                                color: "transparent"
                                border.width: radio.on ? 2 : Theme.hairline
                                border.color: radio.on ? Theme.accent : radio.hovered ? Theme.borderStrong : Theme.border
                                Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3; color: Theme.accent; visible: radio.on }
                            }
                            Text {
                                id: radioLabel
                                text: radio.modelData.label
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fsBase
                                color: radio.on ? Theme.textPrimary : Theme.textSecondary
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        onClicked: root.rangeModeEdited(modelData.key)
                    }
                }
            }
            SettingRow {
                label: qsTr("Handles")
                width: form.w
                visible: root.rangeMode === "clips"
                height: visible ? implicitHeight : 0
                SettingValue {
                    objectName: "setting:handles"
                    tip: qsTr("Extra media kept either side of each clip when rendering clips individually")
                    modelValue: root.handles
                    fps: root.timelineFps > 0 ? root.timelineFps : 25
                    from: 0; to: Math.round(fps * 10)
                    implicitWidth: 120
                    onEdited: function (val) { root.handlesEdited(Math.round(val)) }
                }
            }
            Flow {
                width: form.w
                spacing: Theme.s2
                topPadding: Theme.s1
                Toggle {
                    objectName: "setting:video"
                    tip: qsTr("Include the picture in the export; off renders the sound alone")
                    label: qsTr("Render video")
                    visible: !root.audioOnlyContainer && !root.sequenceContainer
                    checked: root.video
                    enabled: root.audio || !checked
                    onToggled: root.specEdited("video", checked)
                }
                Toggle {
                    objectName: "setting:audio"
                    tip: qsTr("Include the sound in the export; off renders the picture alone")
                    label: qsTr("Render audio")
                    visible: !root.sequenceContainer
                    checked: root.audio
                    enabled: root.video || !checked
                    onToggled: root.specEdited("audio", checked)
                }
                Toggle {
                    objectName: "setting:loudness"
                    tip: qsTr("Measure the finished sound and level it to %1 LUFS with true peaks under %2 dBTP, as streaming and social sites play it. Off keeps the mix exactly as it is. The project's own mix never changes.")
                         .arg(root.v("loudnessTarget", -14)).arg(root.v("truePeak", -1))
                    label: qsTr("Level sound to %1 LUFS").arg(root.v("loudnessTarget", -14))
                    visible: root.audio
                    checked: root.v("loudness", false)
                    onToggled: root.specEdited("loudness", checked)
                }
                Toggle {
                    objectName: "setting:alpha"
                    tip: qsTr("Keep transparency in the file; offered only where the codec can carry an alpha channel")
                    label: qsTr("Export alpha")
                    // VP9's alpha rides Matroska side data: WebM and MKV only.
                    visible: root.video && (root.vcodec === "prores" || root.vcodec === "ffv1" || root.vcodec === "apv"
                                            || (root.vcodec === "vp9" && root.container !== "mp4")
                                            || (root.sequenceContainer && root.container !== "jpg"))
                    checked: root.v("alpha", false)
                    onToggled: root.specEdited("alpha", checked)
                }
            }
            Item { width: 1; height: Theme.s3 }
        }
    }

    ContextMenu {
        id: tokenMenu
        parent: tokenButton
        y: tokenButton.height
        x: tokenButton.width - width
        Repeater {
            model: [
                { token: "{project}", label: qsTr("Project name") }, { token: "{sequence}", label: qsTr("Sequence name") },
                { token: "{preset}", label: qsTr("Preset name") }, { token: "{resolution}", label: qsTr("Resolution") },
                { token: "{fps}", label: qsTr("Frame rate") }, { token: "{date}", label: qsTr("Date") },
                { token: "{time}", label: qsTr("Time") }, { token: "{version}", label: qsTr("Version (v1, v2…)") }
            ]
            delegate: MenuAction {
                required property var modelData
                text: modelData.label
                shortcut: modelData.token
                onTriggered: root.fileNameEdited(root.fileName + (root.fileName !== "" ? "_" : "") + modelData.token)
            }
        }
    }
}
