pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "colorManagement"
    title: qsTr("Colour Management")
    width: Math.min(720, parent ? parent.width - 32 : 720)
    property var grade: ({})
    property var display: ({})
    property var draft: ({})
    property var monitor: ({})
    property var colourConfig: ({})
    property bool editable: false
    signal managementApplied(var management)
    signal displayEdited(var options)
    // The source and grading space given to more clips than this one.
    signal sourceAppliedTo(string scope, var fields)
    readonly property var gamuts: ["rec709", "srgb", "rec2020", "p3", "rec601"]
    readonly property var names: ["Rec.709", "sRGB", "Rec.2020", "Display P3", "Rec.601 (525)"]
    readonly property var gammas: ["rec709", "srgb", "linear", "2.2", "2.4", "2.6", "pq", "hlg"]
    readonly property var gammaNames: ["Rec.709", "sRGB", "Linear", "Gamma 2.2", "Gamma 2.4", "Gamma 2.6", "PQ (ST 2084)", "HLG"]
    // Camera formats: a gamut and curve the camera records together.
    readonly property var formats: [
        { gamut: "rec709", gamma: "rec709", label: qsTr("Standard video (phone, camera, screen recording)"), short: qsTr("Standard video") },
        { gamut: "apple", gamma: "log", label: "Apple Log" },
        { gamut: "arri", gamma: "logc3", label: "ARRI LogC3 (EI 800), Wide Gamut 3" },
        { gamut: "arri", gamma: "logc4", label: "ARRI LogC4, Wide Gamut 4" },
        { gamut: "blackmagic", gamma: "gen5", label: "Blackmagic Film Gen 5, Wide Gamut" },
        { gamut: "canon", gamma: "log", label: "Canon Log, Cinema Gamut" },
        { gamut: "canon", gamma: "log2", label: "Canon Log 2, Cinema Gamut" },
        { gamut: "canon", gamma: "log3", label: "Canon Log 3, Cinema Gamut" },
        { gamut: "dji", gamma: "dlog", label: "DJI D-Log, D-Gamut" },
        { gamut: "fuji", gamma: "flog", label: "Fujifilm F-Log, F-Gamut" },
        { gamut: "fuji", gamma: "flog2", label: "Fujifilm F-Log2, F-Gamut" },
        { gamut: "panasonic", gamma: "vlog", label: "Panasonic V-Log, V-Gamut" },
        { gamut: "red", gamma: "log3g10", label: "RED Log3G10, RED Wide Gamut" },
        { gamut: "red", gamma: "logfilm", label: "RED LogFilm, RED Wide Gamut" },
        { gamut: "sony", gamma: "slog3-sgamut3", label: "Sony S-Log3, S-Gamut3" },
        { gamut: "sony", gamma: "slog3-sgamut3cine", label: "Sony S-Log3, S-Gamut3.Cine" },
        { gamut: "sony", gamma: "slog3-venice-sgamut3", label: "Sony S-Log3, S-Gamut3 (Venice)" },
        { gamut: "sony", gamma: "slog3-venice-sgamut3cine", label: "Sony S-Log3, S-Gamut3.Cine (Venice)" },
        { gamut: "aces", gamma: "2065-1", label: "ACES2065-1" },
        { gamut: "aces", gamma: "cg", label: "ACEScg" },
        { gamut: "aces", gamma: "cct", label: "ACEScct" },
        { gamut: "aces", gamma: "cc", label: "ACEScc" },
        { gamut: "", gamma: "", label: qsTr("Other (choose primaries and curve)"), short: qsTr("Custom") }
    ]
    readonly property int otherFormat: formats.length - 1
    // Other was chosen, though the pair may still match a named format.
    property bool otherChosen: false
    function formatOf(m) {
        for (var i = 0; i < otherFormat; ++i)
            if (formats[i].gamut === m.input && formats[i].gamma === m.inputGamma) return i
        return otherFormat
    }
    readonly property int formatIndex: otherChosen ? otherFormat : formatOf(draft)
    // A clip's source in a few words, for the viewer's button.
    function sourceName(m) {
        if (!m || m.input === undefined) return formats[0].short
        const f = formats[formatOf(m)]
        return f.short !== undefined ? f.short : f.label.split(",")[0]
    }
    // The spaces a grade can work in; the nodes run there, everything else in linear light.
    readonly property var gradeSpaces: [
        { key: "", label: qsTr("Linear Rec.709"), help: qsTr("The default, and fine for standard video. Controls act on light itself, so shadows respond strongly.") },
        { key: "aces/cct", label: "ACEScct", help: qsTr("The usual choice for log footage. The wheels and contrast feel like grading film.") },
        { key: "aces/cc", label: "ACEScc", help: qsTr("Like ACEScct, but the shadows push harder.") },
        { key: "aces/cg", label: "ACEScg", help: qsTr("Wide colour and light-based, for very saturated footage and compositing.") },
        { key: "rec709/2.4", label: qsTr("Rec.709 Gamma 2.4"), help: qsTr("Controls act on the picture as a TV shows it. Simple for standard video.") }
    ]
    readonly property int gradeIndex: Math.max(0, gradeSpaces.findIndex(function (s) { return s.key === (draft.gradeSpace || "") }))
    readonly property var scopes: ["clip", "file", "timeline"]
    property int scope: 0
    // The screens people grade on, or a pair chosen by hand.
    readonly property var screens: [
        { gamut: "srgb", gamma: "srgb", label: qsTr("Computer monitor (sRGB)"), short: qsTr("computer monitor") },
        { gamut: "p3", gamma: "srgb", label: qsTr("Wide-colour monitor (Display P3)"), short: qsTr("wide-colour monitor") },
        { gamut: "rec709", gamma: "2.4", label: qsTr("Video monitor or TV (Rec.709, Gamma 2.4)"), short: qsTr("video monitor") },
        { gamut: "rec2020", gamma: "pq", label: qsTr("HDR screen (Rec.2020, PQ)"), short: qsTr("HDR screen") },
        { gamut: "rec2020", gamma: "hlg", label: qsTr("HDR screen (Rec.2020, HLG)"), short: qsTr("HDR screen") },
        { gamut: "", gamma: "", label: qsTr("Other (choose primaries and curve)"), short: qsTr("your screen") }
    ]
    readonly property int otherScreen: screens.length - 1
    property bool otherScreenChosen: false
    readonly property int screenIndex: {
        if (otherScreenChosen) return otherScreen
        for (var i = 0; i < otherScreen; ++i)
            if (screens[i].gamut === monitor.gamut && screens[i].gamma === monitor.gamma) return i
        return otherScreen
    }
    // Tone maps exist for every display but a linear one, and HLG in Rec.2020 alone.
    readonly property bool toneMaps: monitor.gamma !== "linear" && (monitor.gamma !== "hlg" || monitor.gamut === "rec2020")
    readonly property var views: toneMaps ? ["Standard", "ACES 1.3", "ACES 2.0", "Linear diagnostic"] : ["Standard", "Linear diagnostic"]
    readonly property var viewNames: toneMaps ? [qsTr("None"), "ACES 1.3", "ACES 2.0", qsTr("Linear diagnostic")] : [qsTr("None"), qsTr("Linear diagnostic")]
    readonly property string view: monitor.view || "Standard"
    readonly property bool logSetup: (draft.gradeSpace || "") === "aces/cct" && view === "ACES 2.0"
    readonly property bool standardSetup: formatIndex === 0 && (draft.gradeSpace || "") === "" && view === "Standard"
    // The whole path in words, so the choices below read as one thing.
    readonly property string chain: {
        var steps = [formats[formatIndex].short !== undefined ? formats[formatIndex].short : formats[formatIndex].label.split(",")[0],
                     qsTr("graded in %1").arg(gradeSpaces[gradeIndex].label)]
        if (view !== "Standard") steps.push(view)
        steps.push(screens[screenIndex].short)
        return steps.join("  →  ")
    }
    function put(key, value) { var m = Object.assign({}, draft); m[key] = value; draft = m }
    function putDisplay(key, value) { var m = Object.assign({}, monitor); m[key] = value; monitor = m }
    function chooseFormat(i) {
        otherChosen = i === otherFormat
        var m = Object.assign({}, draft)
        if (!otherChosen) { m.input = formats[i].gamut; m.inputGamma = formats[i].gamma }
        else if (gamuts.indexOf(m.input) < 0) { m.input = "rec709"; m.inputGamma = "rec709" }
        draft = m
    }
    function chooseScreen(i) {
        otherScreenChosen = i === otherScreen
        if (!otherScreenChosen) {
            var m = Object.assign({}, monitor)
            m.gamut = screens[i].gamut; m.gamma = screens[i].gamma
            monitor = m
        }
    }
    function useStandard() {
        chooseFormat(0)
        put("gradeSpace", "")
        putDisplay("view", "Standard")
    }
    // The format stays the user's to pick: only they know what the camera was set to.
    function useLog() {
        put("gradeSpace", "aces/cct")
        putDisplay("view", "ACES 2.0")
    }
    function chooseEngine(ocio) {
        var m = Object.assign({}, draft)
        m.engine = ocio ? "ocio" : "legacy"
        if (ocio) { m.config = colourConfig.config; m.configHash = colourConfig.configHash; m.working = "rec709"; m.workingGamma = "linear" }
        else { delete m.config; delete m.configHash }
        draft = m
    }
    onOpened: {
        scope = 0
        otherChosen = false
        otherScreenChosen = false
        // Every clip is colour-managed through OpenColorIO: only its input is chosen here.
        draft = Object.assign({ input: "rec709", inputGamma: "rec709", working: "rec709", workingGamma: "linear", output: "rec709", outputGamma: "rec709" }, grade.management || {})
        // A grade from the earlier configuration means the same in this one.
        if (draft.engine !== "ocio" || draft.enabled !== true || draft.config === "oma-video-v1") { draft.enabled = true; chooseEngine(true) }
        monitor = Object.assign({ enabled: true, gamut: "srgb", gamma: "srgb" }, display)
        monitor.engine = "ocio"  // the viewer's display transform is OpenColorIO's
    }
    onAccepted: {
        if (editable) {
            root.managementApplied(draft)
            if (scope > 0) root.sourceAppliedTo(scopes[scope], { input: draft.input, inputGamma: draft.inputGamma, gradeSpace: draft.gradeSpace || "" })
        }
        root.displayEdited(monitor)
    }
    component Heading: Text {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s1
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; font.weight: Theme.wHeading
        color: Theme.textPrimary
    }
    component Help: Text {
        Layout.fillWidth: true
        Layout.leftMargin: 90
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
    component RowName: Text {
        Layout.preferredWidth: 82
        font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl
        color: Theme.textSecondary
    }
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { objectName: "colourChain"; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: root.chain; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; color: Theme.accent }
        Text { Layout.fillWidth: true; visible: !root.editable; wrapMode: Text.WordWrap; text: qsTr("Select a clip to set its footage and grading. Your screen can be set now."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            RowName { text: qsTr("Start from") }
            Chip { objectName: "colourStandardSetup"; Layout.preferredHeight: Theme.hControl; label: qsTr("Standard video"); selected: root.standardSetup; enabled: root.editable; tip: qsTr("Phone, camera or screen recording footage: nothing to convert, no tone map"); onClicked: root.useStandard() }
            Chip { objectName: "colourLogSetup"; Layout.preferredHeight: Theme.hControl; label: qsTr("Log camera footage"); selected: root.logSetup; enabled: root.editable; tip: qsTr("Flat-looking footage from a camera set to log: grade in ACEScct and view through ACES 2.0"); onClicked: root.useLog() }
            Item { Layout.fillWidth: true }
        }

        Heading { text: qsTr("1. The footage") }
        RowLayout {
            Layout.fillWidth: true
            RowName { text: qsTr("Shot as") }
            ComboField { objectName: "colorFormat"; Layout.fillWidth: true; model: root.formats.map(function (f) { return f.label }); enabled: root.editable; currentIndex: root.formatIndex; tip: qsTr("What the camera recorded. Log footage looks flat until this is set"); onActivated: function (i) { root.chooseFormat(i) } }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.formatIndex === root.otherFormat
            RowName { text: "" }
            ComboField { objectName: "colorGamut:input"; Layout.fillWidth: true; model: root.names; enabled: root.editable; currentIndex: root.gamuts.indexOf(root.draft.input); tip: qsTr("Colour primaries of this clip"); onActivated: function (i) { root.put("input", root.gamuts[i]) } }
            ComboField { objectName: "colorGamma:input"; Layout.fillWidth: true; model: root.gammaNames; enabled: root.editable; currentIndex: root.gammas.indexOf(root.draft.inputGamma); tip: qsTr("Transfer curve of this clip"); onActivated: function (i) { root.put("inputGamma", root.gammas[i]) } }
        }
        Help {
            objectName: "colourFormatHelp"
            text: root.formatIndex === 0
                  ? (root.logSetup ? qsTr("Now choose what your camera was set to. The camera's picture profile or the clip's notes will say.")
                                   : qsTr("Keep this unless the camera was set to a log profile and the picture looks flat and grey."))
                  : root.formatIndex === root.otherFormat ? qsTr("Only for a file whose primaries and curve you know.")
                  : qsTr("The flat look goes once this matches what the camera was set to.")
            color: root.formatIndex === 0 && root.logSetup ? Theme.textPrimary : Theme.textMuted
        }

        Heading { text: qsTr("2. How you grade it") }
        RowLayout {
            Layout.fillWidth: true
            RowName { text: qsTr("Grade in") }
            ComboField { objectName: "colorGradeSpace"; Layout.fillWidth: true; model: root.gradeSpaces.map(function (s) { return s.label }); enabled: root.editable; currentIndex: root.gradeIndex; tip: qsTr("The space this clip's colour layers work in. Tracks and effects stay in linear light"); onActivated: function (i) { root.put("gradeSpace", root.gradeSpaces[i].key) } }
        }
        Help { objectName: "colourGradeHelp"; text: root.gradeSpaces[root.gradeIndex].help }
        RowLayout {
            Layout.fillWidth: true
            RowName { text: qsTr("Use on") }
            ComboField { objectName: "colorApplyScope"; Layout.fillWidth: true; model: [qsTr("This clip"), qsTr("Every clip from this file"), qsTr("Every clip on the timeline")]; enabled: root.editable; currentIndex: root.scope; tip: qsTr("Which clips take these two settings"); onActivated: function (i) { root.scope = i } }
        }

        Heading { text: qsTr("3. Your screen") }
        RowLayout {
            Layout.fillWidth: true
            RowName { text: qsTr("Screen") }
            ComboField { objectName: "colorScreen"; Layout.fillWidth: true; model: root.screens.map(function (s) { return s.label }); currentIndex: root.screenIndex; tip: qsTr("The screen you are looking at the viewer on"); onActivated: function (i) { root.chooseScreen(i) } }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.screenIndex === root.otherScreen
            RowName { text: "" }
            ComboField { objectName: "colorGamut:display"; Layout.fillWidth: true; model: root.names; currentIndex: root.gamuts.indexOf(root.monitor.gamut); tip: qsTr("Colour primaries of the monitor you are grading on"); onActivated: function(i) { root.putDisplay("gamut", root.gamuts[i]) } }
            ComboField { objectName: "colorGamma:display"; Layout.fillWidth: true; model: root.gammaNames; currentIndex: root.gammas.indexOf(root.monitor.gamma); tip: qsTr("Transfer curve of the monitor you are grading on"); onActivated: function(i) { root.putDisplay("gamma", root.gammas[i]) } }
        }
        RowLayout {
            Layout.fillWidth: true
            RowName { text: qsTr("Tone map") }
            ComboField { objectName: "ocioDisplayView"; Layout.fillWidth: true; model: root.viewNames; currentIndex: Math.max(0, root.views.indexOf(root.view)); tip: qsTr("How the viewer brings bright light down to the screen"); onActivated: function(i) { root.putDisplay("view", root.views[i]) } }
        }
        Help {
            objectName: "colourToneHelp"
            text: root.view === "Standard" ? qsTr("Anything brighter than white clips. Right for standard video.")
                  : root.view === "Linear diagnostic" ? qsTr("Shows the raw light values, for checking. Not for judging a picture.")
                  : qsTr("Rolls bright highlights off as film does, and changes the look. Exports follow it unless you change it on the Export page.")
        }
        Toggle { objectName: "displayTransformEnabled"; label: qsTr("Convert the viewer for this screen"); checked: root.monitor.enabled !== false; tip: qsTr("Off shows the picture unconverted. Files are not affected"); onToggled: root.putDisplay("enabled", checked) }
        Text { Layout.fillWidth: true; visible: !!root.colourConfig.error || root.draft.configHash !== root.colourConfig.configHash; wrapMode: Text.WordWrap; text: root.colourConfig.error || qsTr("This grade was made with another colour configuration. Choosing OK moves it to this one."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
    }
}
