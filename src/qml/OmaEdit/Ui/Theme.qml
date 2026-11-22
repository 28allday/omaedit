pragma Singleton

import QtQuick
import QtCore
import OmaEdit.Desktop

// The single source of every colour, dimension, radius, duration and type
// size in OmaEdit. Nothing outside this file may contain a colour literal or
// a magic dimension — that is an acceptance criterion of the UI brief, not a
// style preference.
//
// Palette is FIXED by design and stays fixed: an editing surface has to be
// colour-stable, because a surround that changes hue changes how you read the
// picture and the grade in front of it — which is why every editor worth the
// name pins neutral greys, and why this app has scopes, LUTs and a GPU/CPU
// parity gate to protect.
//
// The ONE exception is the ACCENT, which follows the Omarchy theme: the
// chrome may belong to the desktop, the surround around a picture may not.
// Selection, focus, drop targets and the AI marks travel with it; everything
// nearer the viewer — wells, panels, clip tints, meters, markers, the
// playhead — stays exactly as it was.
QtObject {
    id: theme

    property alias highContrast: accessibility.highContrast
    property alias reducedMotion: accessibility.reducedMotion
    property QtObject accessibilitySettings: Settings {
        id: accessibility
        category: "accessibility"
        property bool highContrast: false
        property bool reducedMotion: false
    }

    // ── Identity ────────────────────────────────────────────────────────────
    readonly property string appName: "OmaEdit"
    readonly property int wFileBrowser: 860
    readonly property int hFileBrowser: 560
    readonly property int wFilePlaces: 164

    // ── Colour ──────────────────────────────────────────────────────────────
    readonly property color windowBg: "#0B0E13"
    readonly property color panelBg: "#111722"
    readonly property color panelRaised: "#171E2A"
    readonly property color controlBg: "#1B2431"
    readonly property color border: highContrast ? "#8994A6" : "#293241"
    readonly property color borderStrong: highContrast ? "#C6CEDA" : "#3A4658"

    readonly property color textPrimary: "#E7EDF7"
    readonly property color textSecondary: highContrast ? "#E7EDF7" : "#A7B1C1"
    readonly property color textMuted: highContrast ? "#C6CEDA" : "#8994A6"

    // The desktop's accent when Omarchy is there, the Oma accent when it is
    // not (plain Arch, a container, CI) — and the app must look finished
    // either way, so the fallback is the colour this always used.
    readonly property color accent: OmarchyTheme.accent
    // ⚠️ Derived, not hand-picked: the old value was a shade of the old cyan
    // and means nothing against an arbitrary accent.
    readonly property color accentPressed: Qt.darker(accent, 1.35)
    readonly property color aiAccent: "#8B5CF6"
    readonly property color audio: "#34D399"
    readonly property color warning: "#FBBF24"
    readonly property color danger: "#EF4444"
    // Lighter tints for TEXT in those roles: the fills above read 3.7–4.8:1
    // against the panels, below AA; these hold 5.6:1 or better everywhere.
    readonly property color dangerText: "#F87171"
    readonly property color aiText: "#A78BFA"
    readonly property color playhead: "#FF3B45"

    // Text that stays legible on top of a filled accent control.
    // ⚠️ Derived, not fixed: an accent can be pale gold or deep violet and the
    // label on top has to stay legible on both. Rec.709 luminance of the
    // accent decides whether that text is near-black or near-white.
    function luminance(c) {
        function linear(v) { return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
        return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b)
    }
    function foregroundOn(c) {
        const light = Qt.rgba(1, 1, 1, 1)
        const dark = Qt.rgba(0, 0, 0, 1)
        const l = luminance(c)
        return (l + 0.05) / 0.05 >= 1.05 / (l + 0.05) ? dark : light
    }
    readonly property color accentText: foregroundOn(accent)

    // Derived interaction states. Every control derives from these rather
    // than inventing its own hover or pressed colour.
    function hovered(base) { return Qt.lighter(base, 1.06) }
    function pressedOn(base) { return Qt.darker(base, 1.08) }
    readonly property real disabledOpacity: 0.38

    // Drop targets and invalid drops, per the brief's drag language.
    readonly property color dropValid: accent
    readonly property color dropInvalid: danger

    // ── Timeline surfaces ───────────────────────────────────────────────────
    // Clip tints by kind. Video is a deep blue so filmstrip frames read on
    // top of it; audio is the meter green; titles are the AI violet's quieter
    // cousin; a gap is barely-there chrome.
    readonly property color clipVideo: "#1E4F86"
    readonly property color clipAudio: "#1E7A5A"
    readonly property color clipTitle: "#5B3FA8"
    readonly property color clipGap: "#161C27"
    readonly property color clipLabelOnMedia: "#FFFFFF"
    // Translucent scrims laid over media: label backings, fade wedges, the
    // disabled wash. Alpha in the token, never at the call site.
    readonly property color scrim: Qt.rgba(0, 0, 0, 0.55)
    readonly property color scrimStrong: Qt.rgba(0, 0, 0, 0.75)
    readonly property color scrimLight: Qt.rgba(0, 0, 0, 0.35)
    // The line round a clip on a track: black, so that its edges and the cut
    // between two clips read on any picture.
    readonly property color clipOutline: "#000000"
    // A dissolve's overlap: an accent wash under a diagonal hairline. Takes
    // the accent's own rgb — a hand-written cyan here would leave a gold
    // theme with cyan transitions.
    readonly property color transitionWash: Qt.rgba(accent.r, accent.g, accent.b, 0.28)
    // Timeline range selection (I/O on the program side).
    readonly property color rangeFill: Qt.rgba(0.984, 0.749, 0.141, 0.13)
    readonly property color rangeStroke: warning
    // Marker types: standard is the brief's amber flag; the other three
    // keep the conventional chapter / to-do / done colours.
    readonly property color markerStandard: warning
    readonly property color markerChapter: "#FF8A3D"
    // The beats of mapped music, as lines down the tracks: the markers' tone,
    // lighter across the other tracks than on the music's own clip; the bars
    // above the finest lines shown are solid all the way and a little wider.
    readonly property color beatLine: markerStandard
    readonly property real beatLineAcross: 0.7
    // One pixel a beat, two a bar: each made whole device pixels at the
    // screen's scale (BeatLinesItem), so they stay sharp at a fractional one.
    readonly property real beatLineWidth: 1
    readonly property real barLineWidth: 2
    readonly property real beatLineSpacing: 8
    readonly property color markerTodo: danger
    readonly property color markerDone: audio
    function markerColor(type) {
        if (type === "chapter") return markerChapter
        if (type === "todo") return markerTodo
        if (type === "done") return markerDone
        return markerStandard
    }
    // Colours a marker can be given instead of its type's (marker.colour).
    readonly property var markerColours: ({
        red: "#F0444F", orange: "#FF8A3D", yellow: "#F5C93A", green: "#3DCB6B", cyan: "#2FD0E0",
        blue: "#4C8DF6", purple: "#9B6CF0", pink: "#F06FB7", white: "#E8E8E8"
    })
    readonly property var markerColourNames: ({
        red: qsTr("Red"), orange: qsTr("Orange"), yellow: qsTr("Yellow"), green: qsTr("Green"), cyan: qsTr("Cyan"),
        blue: qsTr("Blue"), purple: qsTr("Purple"), pink: qsTr("Pink"), white: qsTr("White")
    })
    // A marker's colour: the one it was given, else its type's.
    function markerTint(marker) {
        if (!marker) return markerStandard
        const own = marker.colour !== undefined ? markerColours[marker.colour] : undefined
        return own !== undefined ? own : markerColor(marker.type)
    }
    // Clip colour labels (context menu ▸ Label): six tints a block can wear
    // instead of its kind colour. Muted so filmstrips still read on top.
    readonly property var labelNames: ["red", "orange", "yellow", "green", "blue", "purple"]
    readonly property color labelRed: "#8A2E3A"
    readonly property color labelOrange: "#8C4A1E"
    readonly property color labelYellow: "#7A6A1C"
    readonly property color labelGreen: "#2C6B3F"
    readonly property color labelBlue: "#2A4F9A"
    readonly property color labelPurple: "#5E3A8C"
    function labelColor(name) {
        if (name === "red") return labelRed
        if (name === "orange") return labelOrange
        if (name === "yellow") return labelYellow
        if (name === "green") return labelGreen
        if (name === "blue") return labelBlue
        if (name === "purple") return labelPurple
        return "transparent"
    }
    function labelTitle(name) {
        if (name === "red") return qsTr("Red")
        if (name === "orange") return qsTr("Orange")
        if (name === "yellow") return qsTr("Yellow")
        if (name === "green") return qsTr("Green")
        if (name === "blue") return qsTr("Blue")
        if (name === "purple") return qsTr("Purple")
        return qsTr("None")
    }
    // ── Audio page ──────────────────────────────────────────────────────────
    // Track roles carry a colour each — group headers, clip bodies, strip
    // name plates and the mixer's identity all read from these.
    readonly property color roleDialogue: "#2E7D5B"
    readonly property color roleAmbience: "#2A8A85"
    readonly property color roleMusic: "#2F6BB5"
    readonly property color roleEffects: "#6D4FC2"
    readonly property color roleFoley: "#B0752A"
    readonly property color roleRoomtone: "#3C8F9B"
    readonly property color roleSubmix: "#8A4A7A"
    readonly property color roleBus: "#B23A6E"
    readonly property color roleMaster: "#3A4658"
    readonly property var roleNames: ["dialogue", "ambience", "music", "effects", "foley", "roomtone", "submix"]
    function roleColor(role) {
        if (role === "dialogue") return roleDialogue
        if (role === "ambience") return roleAmbience
        if (role === "music") return roleMusic
        if (role === "effects") return roleEffects
        if (role === "foley") return roleFoley
        if (role === "roomtone") return roleRoomtone
        if (role === "submix") return roleSubmix
        if (role === "bus") return roleBus
        if (role === "master") return roleMaster
        return clipAudio
    }
    function roleTitle(role) {
        if (role === "dialogue") return qsTr("Dialogue")
        if (role === "ambience") return qsTr("Ambience")
        if (role === "music") return qsTr("Music")
        if (role === "effects") return qsTr("Effects")
        if (role === "foley") return qsTr("Foley")
        if (role === "roomtone") return qsTr("Room Tone")
        if (role === "submix") return qsTr("Submix")
        if (role === "bus") return qsTr("Bus")
        if (role === "master") return qsTr("Master")
        return qsTr("Audio")
    }
    function roleShort(role) {
        if (role === "dialogue") return qsTr("DIAL")
        if (role === "ambience") return qsTr("AMB")
        if (role === "music") return qsTr("MUSIC")
        if (role === "effects") return qsTr("SFX")
        if (role === "foley") return qsTr("FOLEY")
        if (role === "roomtone") return qsTr("RT")
        if (role === "submix") return qsTr("SUB")
        if (role === "bus") return qsTr("BUS")
        if (role === "master") return qsTr("MASTER")
        return qsTr("AUD")
    }
    // Meters: green to −18, amber to −6, red above; the well behind them.
    readonly property color meterLow: audio
    readonly property color meterMid: warning
    readonly property color meterHigh: danger
    readonly property color meterWell: "#0E141C"
    readonly property color meterScale: textMuted
    readonly property color automationLine: "#FFFFFF"
    readonly property color automationPoint: accent
    readonly property color eqCurve: audio
    readonly property color eqFill: Qt.rgba(0.204, 0.827, 0.6, 0.18)
    readonly property color spectrumFill: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    readonly property color recordArm: danger
    readonly property color busPlate: roleBus
    readonly property int wMixerStrip: 72
    readonly property int wMixerStripCompact: 46
    readonly property int wAudioTrackControl: 176
    readonly property int hAudioTrack: 56
    readonly property int hAudioTrackTall: 92
    readonly property int hAudioGroupHeader: 22
    readonly property int hMasterRow: 30
    readonly property int szKnob: 34
    readonly property int szKnobSmall: 26
    readonly property int hFader: 220
    readonly property int wFaderCap: 18
    readonly property int hFaderCap: 30
    readonly property int hHMeter: 14
    readonly property int hSoundRow: 26
    readonly property int wSoundWave: 40
    readonly property int hEqGraph: 96
    readonly property int hSpectrum: 64
    readonly property int hLoudnessGraph: 72
    readonly property int wInspectorAudio: 232

    // The fade handle knob on a selected block's top corners.
    readonly property int szFadeKnob: 8
    // Rubber bands drawn over clips: volume in white, opacity in amber.
    readonly property color bandVolume: "#FFFFFF"
    readonly property color bandOpacity: warning

    // ── Spacing ─────────────────────────────────────────────────────────────
    // The brief's scale: 4, 8, 12, 16, 24. Named by step so call sites read
    // as intent rather than arithmetic.
    readonly property int s1: Math.round(4 * densityScale)
    readonly property int s2: Math.round(8 * densityScale)
    readonly property int s3: Math.round(12 * densityScale)
    readonly property int s4: Math.round(16 * densityScale)
    readonly property int s5: Math.round(24 * densityScale)

    // ── Radius ──────────────────────────────────────────────────────────────
    // Tiny, always. Controls 3, menus and dialogs 4, major panels square.
    // Never a pill — a pill reads as consumer software.
    readonly property int rControl: 3
    readonly property int rMenu: 4
    readonly property int rPanel: 0

    // ── Lines ───────────────────────────────────────────────────────────────
    readonly property int hairline: 1
    readonly property int focusRing: 1
    readonly property int selectionRing: 2
    readonly property int clipOutlineWidth: 1 // round a clip on a track
    readonly property int clipPictureInset: 2 // of the clip's colour before and after its pictures
    readonly property int activeUnderline: 2

    // ── Density ─────────────────────────────────────────────────────────────
    // compact (default) | normal | comfortable. Scales spacing and control
    // heights only — never type size, which stays legible at every density.
    property string density: "compact"
    readonly property real densityScale: density === "comfortable" ? 1.25
                                       : density === "normal" ? 1.12
                                       : 1.0

    // ── Type ────────────────────────────────────────────────────────────────
    // One face through the whole app: JetBrains Mono, under whichever name
    // the installed package gives it. Without it, Inter or Noto Sans; the
    // fallback chain never reaches a serif.
    readonly property string fontFamily: {
        const families = Qt.fontFamilies()
        const wanted = ["JetBrains Mono", "JetBrainsMono Nerd Font", "JetBrainsMono NF",
                        "Inter", "Noto Sans"]
        for (let i = 0; i < wanted.length; ++i)
            if (families.indexOf(wanted[i]) >= 0) return wanted[i]
        return "sans-serif"
    }
    // Tabular numerals matter for timecode: digits must not jitter as they
    // count. Use a monospaced family so every fallback keeps equal digit widths.
    readonly property string monoFamily: {
        const families = Qt.fontFamilies()
        const wanted = ["JetBrains Mono", "JetBrainsMono Nerd Font", "JetBrainsMono NF",
                        "Noto Sans Mono"]
        for (let i = 0; i < wanted.length; ++i)
            if (families.indexOf(wanted[i]) >= 0) return wanted[i]
        return "monospace"
    }

    readonly property int fsBase: 12
    readonly property int fsLabel: 11
    readonly property int fsControl: 12
    readonly property int fsHeading: 12
    readonly property int fsTimecode: 14
    readonly property int fsTitle: 15
    readonly property int wHeading: Font.DemiBold
    readonly property int wNormal: Font.Normal

    // ── Control metrics ─────────────────────────────────────────────────────
    readonly property int hTitleBar: 44
    readonly property int hStatusBar: 24
    readonly property int hEditToolbar: 38
    readonly property int hControl: Math.round(24 * densityScale)
    readonly property int hRow: Math.round(22 * densityScale)
    readonly property int hTab: Math.round(32 * densityScale)
    readonly property int szIcon: 16
    readonly property int szIconHit: Math.round(24 * densityScale)
    readonly property int wTrackHeader: 140
    readonly property int hMediaFilmstrip: 140
    readonly property int wMediaCompare: 1000
    readonly property int hMediaForm: 460
    readonly property int hMediaTasks: 360
    readonly property int wMediaDialog: 620
    readonly property int wNameDialog: 360
    readonly property int hTimeRuler: 22
    readonly property int hLaneRow: 44    // one video/audio track row
    readonly property int hLaneClip: 40   // the block inside that row
    readonly property int wTrimHandle: 10
    readonly property int wSplitter: 4
    // How wide a divider is to the pointer: the strip itself stays a seam.
    readonly property int wSplitterGrab: 12
    // Travel before a press becomes a drag. Qt's own styleHints are not
    // reachable from QML here, so the app carries its own value.
    readonly property int dragThreshold: 4

    // ── Motion ──────────────────────────────────────────────────────────────
    // Panels, menus and state transitions only. Nothing decorative.
    readonly property int dFast: reducedMotion ? 0 : 100
    readonly property int dNormal: reducedMotion ? 0 : 130
    readonly property int dSlow: reducedMotion ? 0 : 160
    readonly property int easing: Easing.OutCubic
    readonly property color viewerCheckerDark: "#5a5a5a"
    readonly property color viewerCheckerLight: "#878787"
    readonly property color viewerGuide: "#beebebeb"
    readonly property var viewerOverlayPalette: ({checkerDark: viewerCheckerDark, checkerLight: viewerCheckerLight, guides: viewerGuide})
    readonly property int tooltipDelay: 450
}
