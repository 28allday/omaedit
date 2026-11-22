import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Render Settings ▸ Quality ▸ ⚙: the encoder's finer points — rate control
// and bitrates, profile and level, keyframe interval, bit depth, scan type,
// pixel aspect, alpha, data levels, HDR metadata. Only rows the chosen
// codec understands are offered.
Ui.FormDialog {
    id: root
    objectName: "advancedVideoDialog"
    width: Math.min(620, parent ? parent.width - Ui.Theme.s5 * 2 : 620)
    title: qsTr("Advanced Video Settings")
    standardButtons: Dialog.Ok | Dialog.Cancel

    component DraftValue: Ui.ValueField {
        property real modelValue: 0
        value: modelValue
        onModelValueChanged: value = modelValue
    }
    property var spec: ({})
    property real timelineFps: 0
    // Keyframe spacing is counted in frames of the export; it is shown as time.
    readonly property real exportFps: spec.fps > 0 ? spec.fps : timelineFps > 0 ? timelineFps : 25
    property var draft: ({})
    signal edited(var changes)

    readonly property string vcodec: draft.vcodec !== undefined ? draft.vcodec : "h264"
    readonly property bool fixedRate: vcodec === "prores" || vcodec === "dnxhr" || vcodec === "ffv1"
    readonly property bool hasProfile: vcodec === "h264" || vcodec === "hevc" || vcodec === "prores" || vcodec === "dnxhr" || vcodec === "apv" || vcodec === "mpeg2"
    readonly property bool isHdr: draft.colorSpace === "pq" || draft.colorSpace === "hlg"
    readonly property bool masteringMetadata: isHdr && vcodec === "hevc" && (!draft.hw || draft.hw === "software")

    function openFor(s) {
        spec = s
        var d = {}
        for (var k in s) d[k] = s[k]
        draft = d
        open()
    }
    function set(key, value) {
        var d = {}
        for (var k in draft) d[k] = draft[k]
        d[key] = value
        draft = d
    }
    function setHdr(key, value) {
        var h = {}
        var cur = draft.hdr || {}
        for (var k in cur) h[k] = cur[k]
        h[key] = value
        set("hdr", h)
    }
    onAccepted: { landTyped(); edited(draft) }

    readonly property var profileOptions: {
        switch (vcodec) {
        case "h264": return [{ key: "", label: qsTr("Auto") }, { key: "baseline", label: "Baseline" }, { key: "main", label: "Main" }, { key: "high", label: "High" }, { key: "high10", label: "High 10" }]
        case "hevc": return [{ key: "", label: qsTr("Auto") }, { key: "main", label: "Main" }, { key: "main10", label: "Main 10" }]
        case "prores": return [{ key: "proxy", label: "Proxy" }, { key: "lt", label: "LT" }, { key: "standard", label: "422" }, { key: "hq", label: "422 HQ" }, { key: "4444", label: "4444" }, { key: "4444xq", label: "4444 XQ" }]
        case "mpeg2": return [{ key: "", label: qsTr("Main (4:2:0)") }, { key: "422", label: qsTr("4:2:2") }]
        case "dnxhr": return [{ key: "lb", label: "LB" }, { key: "sq", label: "SQ" }, { key: "hq", label: "HQ" }, { key: "hqx", label: "HQX" }, { key: "444", label: "444" }]
        case "apv": return [{ key: "422-10", label: qsTr("4:2:2 10-bit") }, { key: "422-12", label: qsTr("4:2:2 12-bit") },
                            { key: "444-10", label: qsTr("4:4:4 10-bit") }, { key: "444-12", label: qsTr("4:4:4 12-bit") },
                            { key: "4444-10", label: qsTr("4:4:4:4 10-bit (alpha)") }, { key: "4444-12", label: qsTr("4:4:4:4 12-bit (alpha)") }]
        }
        return []
    }
    function indexOfKey(list, key) {
        for (var i = 0; i < list.length; ++i) if (list[i].key === key) return i
        return 0
    }
    readonly property var levelOptions: ["", "3.0", "3.1", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "6.0", "6.1"]

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Ui.Theme.s3
        rowSpacing: Ui.Theme.s2

        Label { text: qsTr("Rate control"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: !root.fixedRate }
        Ui.ComboField {
            objectName: "advRateMode"
            tip: qsTr("How the encoder spends bits: a fixed quality level, or a variable or constant target bitrate")
            visible: !root.fixedRate
            Layout.fillWidth: true
            Layout.minimumWidth: 180
            model: [qsTr("Constant quality"), qsTr("Variable bitrate"), qsTr("Constant bitrate")]
            currentIndex: root.draft.rateMode === "vbr" ? 1 : root.draft.rateMode === "cbr" ? 2 : 0
            onActivated: function (i) { root.set("rateMode", i === 1 ? "vbr" : i === 2 ? "cbr" : "quality") }
        }
        Label { text: root.vcodec === "h264" || root.vcodec === "hevc" ? qsTr("Quality (CRF)") : qsTr("Quality (0–51)"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: !root.fixedRate && root.draft.rateMode !== "vbr" && root.draft.rateMode !== "cbr" }
        DraftValue {
            objectName: "advQuality"
            tip: qsTr("Quality level from 0 to 51; lower means a better picture and a larger file. VP9, AV1 and MPEG-2 map this scale to their encoder's quality range.")
            visible: !root.fixedRate && root.draft.rateMode !== "vbr" && root.draft.rateMode !== "cbr"
            modelValue: root.draft.quality !== undefined ? root.draft.quality : 18
            from: 0; to: 51; decimals: 0; step: 0.2
            onEdited: function (v) { root.set("quality", Math.round(v)) }
        }
        Label { text: qsTr("Bitrate"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: !root.fixedRate && (root.draft.rateMode === "vbr" || root.draft.rateMode === "cbr") }
        DraftValue {
            objectName: "advBitrate"
            tip: qsTr("Target video bitrate in kb/s; the main control of file size in the bitrate modes")
            visible: !root.fixedRate && (root.draft.rateMode === "vbr" || root.draft.rateMode === "cbr")
            modelValue: root.draft.bitrate !== undefined ? root.draft.bitrate : 0
            from: 0; to: 400000; decimals: 0; step: 50; suffix: "kb/s"
            implicitWidth: 120
            onEdited: function (v) { root.set("bitrate", Math.round(v)) }
        }
        Label { text: qsTr("Max bitrate"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: !root.fixedRate && root.draft.rateMode === "vbr" }
        DraftValue {
            objectName: "advMaxrate"
            tip: qsTr("Peak bitrate the variable stream may reach in kb/s; 0 leaves it uncapped")
            visible: !root.fixedRate && root.draft.rateMode === "vbr"
            modelValue: root.draft.maxrate !== undefined ? root.draft.maxrate : 0
            from: 0; to: 400000; decimals: 0; step: 50; suffix: "kb/s"
            implicitWidth: 120
            onEdited: function (v) { root.set("maxrate", Math.round(v)) }
        }
        Label { text: qsTr("Profile"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: root.hasProfile }
        Ui.ComboField {
            objectName: "advProfile"
            tip: qsTr("Encoder profile that decides which players can decode it, or the ProRes / DNxHR flavour")
            visible: root.hasProfile
            Layout.fillWidth: true
            model: root.profileOptions.map(function (o) { return o.label })
            currentIndex: root.indexOfKey(root.profileOptions, root.draft.vprofile !== undefined ? root.draft.vprofile : "")
            onActivated: function (i) { root.set("vprofile", root.profileOptions[i].key) }
        }
        Label { text: qsTr("Level"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: root.vcodec === "h264" || root.vcodec === "hevc" }
        Ui.ComboField {
            objectName: "advLevel"
            tip: qsTr("H.264 / HEVC level cap on resolution, frame rate and bitrate; Auto lets the encoder choose")
            visible: root.vcodec === "h264" || root.vcodec === "hevc"
            Layout.fillWidth: true
            model: root.levelOptions.map(function (l) { return l === "" ? qsTr("Auto") : l })
            currentIndex: Math.max(0, root.levelOptions.indexOf(root.draft.level !== undefined ? root.draft.level : ""))
            onActivated: function (i) { root.set("level", root.levelOptions[i]) }
        }
        Label { text: qsTr("Keyframe interval"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: !root.fixedRate && root.vcodec !== "apv" }
        DraftValue {
            objectName: "advGop"
            tip: qsTr("Time between keyframes; shorter seeks finer, longer compresses better, 0 means two seconds")
            visible: !root.fixedRate && root.vcodec !== "apv"
            modelValue: root.draft.gop !== undefined ? root.draft.gop : 0
            fps: root.exportFps
            from: 0; to: Math.round(root.exportFps * 20)
            implicitWidth: 160
            onEdited: function (v) { root.set("gop", Math.round(v)) }
        }
        Label { text: qsTr("Bit depth"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: root.vcodec !== "prores" && root.vcodec !== "dnxhr" && root.vcodec !== "mpeg2" && root.vcodec !== "apv" }
        Ui.ComboField {
            objectName: "advBitDepth"
            tip: qsTr("Bits per colour sample; 10-bit avoids banding in gradients and is required for HDR")
            visible: root.vcodec !== "prores" && root.vcodec !== "dnxhr" && root.vcodec !== "mpeg2" && root.vcodec !== "apv"
            Layout.fillWidth: true
            model: [qsTr("8-bit"), qsTr("10-bit")]
            currentIndex: root.draft.bitDepth === 10 ? 1 : 0
            onActivated: function (i) { root.set("bitDepth", i === 1 ? 10 : 8) }
        }
        Label { text: qsTr("Scan type"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
        Ui.ComboField {
            objectName: "advScan"
            tip: qsTr("Progressive frames or interlaced fields, and which field comes first, for broadcast delivery")
            Layout.fillWidth: true
            model: [qsTr("Progressive"), qsTr("Interlaced, top field first"), qsTr("Interlaced, bottom field first")]
            currentIndex: root.draft.scan === "tff" ? 1 : root.draft.scan === "bff" ? 2 : 0
            onActivated: function (i) { root.set("scan", i === 1 ? "tff" : i === 2 ? "bff" : "progressive") }
        }
        Label { text: qsTr("Pixel aspect"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
        Ui.ComboField {
            objectName: "advPar"
            tip: qsTr("Pixel aspect ratio written into the file; non-square values suit SD and HDV broadcast formats")
            Layout.fillWidth: true
            model: [qsTr("Square (1.0)"), qsTr("PAL widescreen (1.4222)"), qsTr("NTSC widescreen (1.2121)"), qsTr("HDV / DVCPRO HD (1.3333)")]
            currentIndex: {
                var p = root.draft.par !== undefined ? root.draft.par : 1
                return Math.abs(p - 1.4222) < 0.01 ? 1 : Math.abs(p - 1.2121) < 0.01 ? 2 : Math.abs(p - 1.3333) < 0.01 ? 3 : 0
            }
            onActivated: function (i) { root.set("par", [1.0, 1.4222, 1.2121, 1.3333][i]) }
        }
        Label { text: qsTr("Data levels"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
        Ui.ComboField {
            objectName: "advRange"
            tip: qsTr("Whether the file uses video (16–235) or full (0–255) levels; players expect video levels")
            Layout.fillWidth: true
            model: [qsTr("Video (16–235)"), qsTr("Full (0–255)")]
            currentIndex: root.draft.range === "pc" ? 1 : 0
            onActivated: function (i) { root.set("range", i === 1 ? "pc" : "tv") }
        }
        Label { text: qsTr("Alpha channel"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
        Ui.Toggle {
            objectName: "advAlpha"
            tip: qsTr("Keep transparency in the file; only ProRes 4444, APV 4:4:4:4, FFV1 and VP9 can carry an alpha channel")
            checked: root.draft.alpha === true
            enabled: root.vcodec === "prores" || root.vcodec === "ffv1" || root.vcodec === "vp9" || root.vcodec === "apv"
            onToggled: root.set("alpha", checked)
        }
        Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.WordWrap; visible: root.isHdr && !root.masteringMetadata; color: Ui.Theme.textSecondary; text: qsTr("HDR mastering metadata is available with software H.265. Colour tags are written for every HDR export.") }
        Label { text: qsTr("HDR mastering display"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: root.masteringMetadata }
        TextField {
            visible: root.masteringMetadata
            hoverEnabled: true
            objectName: "advMasterDisplay"
            ToolTip.text: qsTr("Mastering display primaries and luminance written as HDR metadata, in the format shown"); ToolTip.visible: hovered && !activeFocus; ToolTip.delay: 450
            Layout.fillWidth: true
            text: root.draft.hdr !== undefined && root.draft.hdr.masterDisplay !== undefined ? root.draft.hdr.masterDisplay : ""
            placeholderText: qsTr("G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)")
            color: Ui.Theme.textPrimary
            font.pixelSize: Ui.Theme.fsLabel
            onEditingFinished: root.setHdr("masterDisplay", text.trim())
        }
        Label { text: qsTr("HDR MaxCLL / MaxFALL"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel; visible: root.masteringMetadata }
        RowLayout {
            visible: root.masteringMetadata
            spacing: Ui.Theme.s2
            DraftValue {
                objectName: "advMaxCll"
                tip: qsTr("Maximum content light level in cd/m² written into the HDR metadata")
                modelValue: root.draft.hdr !== undefined && root.draft.hdr.maxCll !== undefined ? root.draft.hdr.maxCll : 1000
                from: 0; to: 10000; decimals: 0; step: 10; suffix: "cd/m²"
                implicitWidth: 120
                onEdited: function (v) { root.setHdr("maxCll", Math.round(v)) }
            }
            DraftValue {
                objectName: "advMaxFall"
                tip: qsTr("Maximum frame-average light level in cd/m² written into the HDR metadata")
                modelValue: root.draft.hdr !== undefined && root.draft.hdr.maxFall !== undefined ? root.draft.hdr.maxFall : 400
                from: 0; to: 10000; decimals: 0; step: 10; suffix: "cd/m²"
                implicitWidth: 120
                onEdited: function (v) { root.setHdr("maxFall", Math.round(v)) }
            }
        }
    }
}
