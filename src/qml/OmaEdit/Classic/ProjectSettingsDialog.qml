import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// The active timeline's frame, rate, colour, audio and starting timecode.
// Frame rate stays fixed while this timeline holds clips. Import and proxy
// defaults at the end of the form belong to the project container.
Ui.ModalDialog {
    id: root
    objectName: "projectSettings"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(580, parent ? parent.width - Ui.Theme.s5 * 2 : 580)
    modal: true
    title: qsTr("Timeline Settings")
    standardButtons: Dialog.Ok | Dialog.Cancel
    background: Rectangle { color: Ui.Theme.panelBg; radius: Ui.Theme.rMenu; border.color: Ui.Theme.border; border.width: 1 }

    // The working copy; Ok hands it to the engine as one change.
    property var draft: ({})
    readonly property bool textActive: activeFocus || startField.active || proxyFolderField.activeFocus || importLutField.activeFocus

    readonly property var formats: [{ key: "auto", name: qsTr("Automatic (from the first clip)"), w: 0, h: 0 }]
        .concat(Ui.FrameSizes.sizes.map(function (f) { return { key: f.w + "x" + f.h, name: Ui.FrameSizes.label(f), w: f.w, h: f.h } }))
        .concat([{ key: "custom", name: qsTr("Custom…"), w: 0, h: 0 }])
    readonly property var rates: [
        { num: 24000, den: 1001, label: "23.976" }, { num: 24, den: 1, label: "24" },
        { num: 25, den: 1, label: "25" }, { num: 30000, den: 1001, label: "29.97" },
        { num: 30, den: 1, label: "30" }, { num: 50, den: 1, label: "50" },
        { num: 60000, den: 1001, label: "59.94" }, { num: 60, den: 1, label: "60" }
    ]
    readonly property var colorspaces: [
        { value: 601, label: qsTr("Rec.601 (SD)") }, { value: 709, label: qsTr("Rec.709 (HD)") },
        { value: 2020, label: qsTr("Rec.2020 (UHD / wide gamut)") }
    ]
    readonly property var sampleRates: [44100, 48000, 96000]
    readonly property var channelOptions: [
        { n: 1, label: qsTr("Mono") }, { n: 2, label: qsTr("Stereo") }, { n: 6, label: qsTr("5.1 Surround") }
    ]

    readonly property bool custom: draft.custom === true
    readonly property real draftFps: draft.fpsNum > 0 && draft.fpsDen > 0 ? draft.fpsNum / draft.fpsDen : 25
    // The rate the timecode field counts in: the timeline's while clips hold
    // it there, else the one chosen here.
    readonly property real tcFps: backend.projectRateLocked && backend.fps > 0 ? backend.fps : draftFps

    function formatIndex() {
        if (!custom) return 0
        for (var i = 1; i < formats.length - 1; ++i)
            if (formats[i].w === draft.width && formats[i].h === draft.height) return i
        return formats.length - 1
    }
    function rateIndex() {
        for (var i = 0; i < rates.length; ++i)
            if (rates[i].num === draft.fpsNum && rates[i].den === draft.fpsDen) return i
        // A clip-dictated rate that is not on the list (a 12 fps GIF…):
        // the nearest listed one, so the combo still reads something true.
        var best = 0, bestD = 1e9
        for (var j = 0; j < rates.length; ++j) {
            var d = Math.abs(rates[j].num / rates[j].den - draftFps)
            if (d < bestD) { bestD = d; best = j }
        }
        return best
    }
    function set(key, value) {
        var d = {}
        for (var k in draft) d[k] = draft[k]
        d[key] = value
        draft = d
    }
    function openFor() {
        var s = backend.projectSettings
        var d = {}
        for (var k in s) d[k] = s[k]
        // An automatic project shows the timeline's real numbers, so
        // switching to Custom starts from what is on screen.
        if (!d.custom && backend.hasVideo) {
            d.width = backend.videoWidth
            d.height = backend.videoHeight
            var r = nearestRate(backend.fps)
            d.fpsNum = r.num
            d.fpsDen = r.den
        }
        // A start with no rate written was counted at the timeline's own: say
        // so, so it stays the same timecode if another rate is chosen here.
        if (d.startFrame > 0 && !(d.startRate > 0) && (backend.fps > 0 || d.custom))
            d.startRate = Math.round(backend.fps > 0 ? backend.fps : d.fpsNum / d.fpsDen)
        draft = d
        startField.clearRefused()
        open()
    }
    function nearestRate(fps) {
        var best = rates[2], bestD = 1e9
        for (var j = 0; j < rates.length; ++j) {
            var d = Math.abs(rates[j].num / rates[j].den - fps)
            if (d < bestD) { bestD = d; best = rates[j] }
        }
        return best
    }
    onAccepted: {
        // A start that could not be read brings the box back, its warning showing.
        if (startField.commitHolds()) { Qt.callLater(root.open); return }
        backend.setProjectSettings(draft)
    }

    // With the defaults section the body outgrows a 720-pixel window: it scrolls.
    contentItem: ScrollView {
        ScrollBar.vertical: Ui.ScrollBar { atRest: true }
        id: scroll
        clip: true
        contentWidth: availableWidth
        implicitWidth: 520 + Ui.Theme.s3
        implicitHeight: Math.min(column.implicitHeight + Ui.Theme.s2, (root.parent ? root.parent.height : 720) - 170)
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ColumnLayout {
        id: column
        spacing: Ui.Theme.s3
        width: scroll.availableWidth

        // ── Project ───────────────────────────────────────────────────────
        GridLayout {
            columns: 2
            columnSpacing: Ui.Theme.s3
            rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true

            Label { text: qsTr("Name"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Label {
                    objectName: "project:name"
                    text: backend.sequences[backend.activeSequence] ? backend.sequences[backend.activeSequence].name : ""
                    color: Ui.Theme.textPrimary
                    font.pixelSize: Ui.Theme.fsControl
                }
                Label {
                    text: qsTr("in %1").arg(backend.projectName)
                    color: Ui.Theme.textMuted
                    font.pixelSize: Ui.Theme.fsLabel
                }
            }

            Label { text: qsTr("Starting timecode"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.TimecodeField {
                id: startField
                objectName: "project:startTimecode"
                start: true
                refusalInForm: true
                // The count is kept with the rate it was typed at, so the same
                // timecode stands when the timeline's rate is changed.
                frame: backend.startFrameAtRate(root.draft.startFrame || 0, root.draft.startRate || 0, fps)
                fps: root.tcFps
                tip: qsTr("Timecode the timeline starts counting from, such as 01:00:00:00")
                onEdited: function (f) {
                    var d = {}
                    for (var k in root.draft) d[k] = root.draft[k]
                    d.startFrame = Math.max(0, f)
                    d.startRate = Math.round(fps)
                    root.draft = d
                }
            }
            Item { visible: startRefused.visible; Layout.preferredWidth: 1; Layout.preferredHeight: 1 }
            Label {
                id: startRefused
                objectName: "project:startRefused"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.WordWrap
                text: startField.refusedText
                color: Ui.Theme.warning
                font.pixelSize: Ui.Theme.fsLabel
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Ui.Theme.hairline; color: Ui.Theme.border }

        // ── Video ─────────────────────────────────────────────────────────
        Label { text: qsTr("Video"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        GridLayout {
            columns: 2
            columnSpacing: Ui.Theme.s3
            rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true

            Label { text: qsTr("Format"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ComboField {
                objectName: "project:format"
                Layout.fillWidth: true
                caption: qsTr("Format")
                tip: qsTr("Frame size this timeline renders at; Automatic follows the first clip")
                model: root.formats.map(function (f) { return f.name })
                currentIndex: root.formatIndex()
                onActivated: function (i) {
                    var f = root.formats[i]
                    var d = {}
                    for (var k in root.draft) d[k] = root.draft[k]
                    d.custom = backend.projectRateLocked || f.key !== "auto"
                    if (f.w > 0) { d.width = f.w; d.height = f.h }
                    root.draft = d
                }
            }

            Label { text: qsTr("Resolution"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.ValueField {
                    objectName: "project:width"
                    Layout.preferredWidth: 84
                    tip: qsTr("Frame width in pixels for a custom format (even numbers, 16–8192)")
                    editable: root.custom
                    decimals: 0; step: 2; from: 16; to: 8192
                    value: root.draft.width !== undefined ? root.draft.width : 1920
                    onEdited: function (v) { root.set("width", Math.max(16, Math.round(v / 2) * 2)) }
                }
                Label { text: "×"; color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsControl }
                Ui.ValueField {
                    objectName: "project:height"
                    Layout.preferredWidth: 84
                    tip: qsTr("Frame height in pixels for a custom format (even numbers, 16–8192)")
                    editable: root.custom
                    decimals: 0; step: 2; from: 16; to: 8192
                    value: root.draft.height !== undefined ? root.draft.height : 1080
                    onEdited: function (v) { root.set("height", Math.max(16, Math.round(v / 2) * 2)) }
                }
                Label {
                    text: root.custom ? qsTr("square pixels, progressive") : qsTr("follows the first clip")
                    color: Ui.Theme.textMuted
                    font.pixelSize: Ui.Theme.fsLabel
                }
            }

            Label { text: qsTr("Frame rate"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.ComboField {
                    objectName: "project:rate"
                    Layout.preferredWidth: 120
                    caption: qsTr("Frame rate")
                    tip: qsTr("Frames per second of this timeline; locked once it holds clips")
                    enabled: root.custom && !backend.projectRateLocked
                    model: root.rates.map(function (r) { return r.label + " fps" })
                    currentIndex: root.rateIndex()
                    onActivated: function (i) {
                        var d = {}
                        for (var k in root.draft) d[k] = root.draft[k]
                        d.fpsNum = root.rates[i].num
                        d.fpsDen = root.rates[i].den
                        root.draft = d
                    }
                }
                Ui.Icon {
                    visible: backend.projectRateLocked
                    name: "lock"
                    size: 13
                    color: Ui.Theme.warning
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: backend.projectRateLocked
                          ? qsTr("This timeline contains clips. Create a new timeline to choose a different frame rate.")
                          : root.custom ? "" : qsTr("follows the first clip")
                    color: backend.projectRateLocked ? Ui.Theme.warning : Ui.Theme.textMuted
                    font.pixelSize: Ui.Theme.fsLabel
                }
            }
        }

        // ── Rendering ─────────────────────────────────────────────────────
        Label { text: qsTr("Rendering"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        GridLayout {
            columns: 2
            columnSpacing: Ui.Theme.s3
            rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true
            Label { text: qsTr("Colour space"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ComboField {
                objectName: "project:colorspace"
                Layout.fillWidth: true
                caption: qsTr("Colour space")
                tip: qsTr("Colour space for the scopes, the Colour page and every export")
                model: root.colorspaces.map(function (c) { return c.label })
                currentIndex: Math.max(0, root.colorspaces.map(function (c) { return c.value }).indexOf(root.draft.colorspace))
                onActivated: function (i) { root.set("colorspace", root.colorspaces[i].value) }
            }
            Label { text: qsTr("Scaling"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ComboField {
                objectName: "project:scaling"
                readonly property var keys: ["fit", "fill", "stretch", "original"]
                Layout.fillWidth: true
                caption: qsTr("Scaling")
                tip: qsTr("How clips of another size or shape fill this timeline's frame; a clip can choose its own in the Inspector")
                model: [qsTr("Fit — the whole picture, with bars"), qsTr("Fill — the whole frame, edges cropped"),
                        qsTr("Stretch — to the frame's corners"), qsTr("Original size — pixel for pixel")]
                currentIndex: Math.max(0, keys.indexOf(root.draft.scaling || "fit"))
                onActivated: function (i) { root.set("scaling", keys[i]) }
            }
        }

        // ── Defaults for this project ─────────────────────────────────────
        Label { text: qsTr("Project defaults"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        GridLayout {
            columns: 2
            columnSpacing: Ui.Theme.s3
            rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true
            Label { text: qsTr("Proxy folder"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.LineEdit {
                    id: proxyFolderField
                    objectName: "project:proxyFolder"
                    Layout.fillWidth: true
                    text: root.draft.proxyFolder || ""
                    hint: backend.storageActivePaths.proxies || qsTr("Beside the project (asked while untitled)")
                    tip: qsTr("Folder Create Proxies offers for this project; empty uses the storage default")
                    onTextEdited: root.set("proxyFolder", text)
                }
                Ui.ToolButton { text: qsTr("Browse…"); onClicked: backend.mediaTasks.browse("project-proxy-folder") }
            }
            Label { text: qsTr("Import input space"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ComboField {
                objectName: "project:importSpace"
                Layout.preferredWidth: 200
                caption: qsTr("Import input space")
                tip: qsTr("Input colour space Import Media proposes for new clips; existing clips are unchanged")
                readonly property var values: [0, 601, 709, 2020]
                model: [qsTr("Use file metadata"), "Rec.601", "Rec.709", "Rec.2020"]
                currentIndex: Math.max(0, values.indexOf(Number(root.draft.importSpace || 0)))
                onActivated: function (i) { root.set("importSpace", values[i]) }
            }
            Label { text: qsTr("Import LUT"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.LineEdit {
                    id: importLutField
                    objectName: "project:importLut"
                    Layout.fillWidth: true
                    text: root.draft.importLut || ""
                    hint: qsTr("None")
                    tip: qsTr("LUT file Import Media proposes applying to newly imported clips")
                    onTextEdited: root.set("importLut", text)
                }
                Ui.ToolButton { text: qsTr("Browse…"); onClicked: backend.mediaTasks.browse("project-import-lut", "file") }
            }
            Item { Layout.preferredWidth: 1; Layout.preferredHeight: 1 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Create Proxies offers the folder; Import Media proposes the input space and LUT. Existing clips are not changed.")
                color: Ui.Theme.textMuted
                font.pixelSize: Ui.Theme.fsLabel
            }
        }
        Connections {
            target: backend.mediaTasks
            function onPathsChosen(purpose, paths) {
                if (!paths.length || !root.visible) return
                if (purpose === "project-proxy-folder") root.set("proxyFolder", paths[0])
                else if (purpose === "project-import-lut") root.set("importLut", paths[0])
            }
        }

        // ── Audio ─────────────────────────────────────────────────────────
        Label { text: qsTr("Audio"); color: Ui.Theme.textPrimary; font.pixelSize: Ui.Theme.fsHeading; font.weight: Ui.Theme.wHeading }
        GridLayout {
            columns: 2
            columnSpacing: Ui.Theme.s3
            rowSpacing: Ui.Theme.s2
            Layout.fillWidth: true
            Label { text: qsTr("Channels"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            Ui.ComboField {
                objectName: "project:channels"
                Layout.preferredWidth: 160
                caption: qsTr("Channels")
                tip: qsTr("Channel layout of this timeline’s mix and of exports")
                model: root.channelOptions.map(function (c) { return c.label })
                currentIndex: Math.max(0, root.channelOptions.map(function (c) { return c.n }).indexOf(root.draft.channels))
                onActivated: function (i) { root.set("channels", root.channelOptions[i].n) }
            }
            Label { text: qsTr("Sample rate"); color: Ui.Theme.textSecondary; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                spacing: Ui.Theme.s2
                Ui.ComboField {
                    objectName: "project:sampleRate"
                    Layout.preferredWidth: 120
                    caption: qsTr("Sample rate")
                    tip: qsTr("Sample rate exports are made at unless a preset pins its own")
                    model: root.sampleRates.map(function (r) { return (r / 1000) + " kHz" })
                    currentIndex: Math.max(0, root.sampleRates.indexOf(root.draft.sampleRate))
                    onActivated: function (i) { root.set("sampleRate", root.sampleRates[i]) }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("What exports are made at unless a preset pins its own; playback always monitors at 48 kHz stereo.")
                    color: Ui.Theme.textMuted
                    font.pixelSize: Ui.Theme.fsLabel
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: Ui.Theme.hairline; color: Ui.Theme.border }

        // What OK will do — the consequences, stated before they happen.
        Label {
            objectName: "project:consequence"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Ui.Theme.fsLabel
            color: Ui.Theme.textSecondary
            text: {
                var s = backend.projectSettings
                var lines = []
                if (root.custom && backend.hasVideo
                        && (root.draft.width !== backend.videoWidth || root.draft.height !== backend.videoHeight))
                    lines.push(qsTr("The frame changes from %1 × %2 to %3 × %4: every clip is re-fitted to it. Check transforms and crops afterwards.")
                               .arg(backend.videoWidth).arg(backend.videoHeight).arg(root.draft.width).arg(root.draft.height))
                if (!root.custom && s.custom === true)
                    lines.push(qsTr("Back to automatic: the frame and rate follow the first clip again."))
                if (root.draft.colorspace !== s.colorspace)
                    lines.push(qsTr("Colour space changes affect the scopes, the Colour page and every export."))
                if (root.draft.sampleRate !== s.sampleRate || root.draft.channels !== s.channels)
                    lines.push(qsTr("Audio output settings change this timeline’s mix and exports. Check channel routing afterwards."))
                if (lines.length === 0)
                    lines.push(backend.hasVideo
                               ? qsTr("Timeline now: %1 × %2 at %3, %4.").arg(backend.videoWidth).arg(backend.videoHeight)
                                     .arg(win.fpsLabel(backend.fps)).arg(backend.colorSpace)
                               : qsTr("The timeline is empty — an automatic timeline takes its frame and rate from the first clip you add."))
                return lines.join("\n")
            }
        }
    }
    }
}
