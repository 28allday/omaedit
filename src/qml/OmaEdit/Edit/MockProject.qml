pragma Singleton

import QtQuick

// Stand-in for the real project model while the Edit workspace layout is being
// built. Shapes match what Backend already exposes (or will), so swapping this
// for the engine is a source change in one place per panel, not a rewrite.
QtObject {
    readonly property string name: "Northern Trails"
    readonly property string timecode: "00:01:26:14"
    readonly property string duration: "00:03:42:07"

    readonly property var bins: [
        { name: "01 Sequences", count: 6 },
        { name: "02 Footage", count: 28 },
        { name: "03 Audio", count: 12 },
        { name: "04 Graphics", count: 9 },
        { name: "05 B-Roll", count: 15 },
        { name: "06 Stills", count: 8 }
    ]

    // Backend library shape: duration in frames, fps, height; thumb empty so
    // the tonal plate shows. `tone` only feeds the plate.
    readonly property var clips: [
        { name: "NT_001_A001_C005", duration: 17688, fps: 23.976, height: 2160, thumb: "", tone: 0.42 },
        { name: "NT_001_A002_C006", duration: 13103, fps: 23.976, height: 2160, thumb: "", tone: 0.66 },
        { name: "NT_002_A003_C012", duration: 10390, fps: 23.976, height: 2160, thumb: "", tone: 0.30, rangeIn: 2400, rangeOut: 6100 },
        { name: "NT_002_A004_C013", duration: 20668, fps: 23.976, height: 2160, thumb: "", tone: 0.54 },
        { name: "NT_003_A003_C018", duration: 11772, fps: 23.976, height: 2160, thumb: "", tone: 0.72 },
        { name: "NT_002_A006_C019", duration: 8703,  fps: 23.976, height: 2160, thumb: "", tone: 0.38 }
    ]

    // Timeline: video tracks descend to the storyline, audio hangs below.
    readonly property var tracks: [
        { name: "V3", kind: "video", height: 44 },
        { name: "V2", kind: "video", height: 44 },
        { name: "V1", kind: "video", height: 58 },
        { name: "A1", kind: "audio", height: 48 },
        { name: "A2", kind: "audio", height: 48 },
        { name: "A3", kind: "audio", height: 48 }
    ]

    // start/span are fractions of the visible timeline width.
    readonly property var clipsByTrack: [
        [ { label: "Trail Title",       start: 0.19, span: 0.11, fx: true, kind: "title" },
          { label: "Map Overlay",       start: 0.60, span: 0.09, fx: true, kind: "title" } ],
        [ { label: "NT_003_A005_C018",  start: 0.11, span: 0.20, fx: true, kind: "video" },
          { label: "NT_001_A005_C006",  start: 0.35, span: 0.19, fx: true, kind: "video" },
          { label: "NT_002_A004_C013",  start: 0.64, span: 0.22, fx: true, kind: "video" } ],
        [ { label: "NT_001_A001_C005",  start: 0.00, span: 0.30, fx: true, kind: "film" },
          { label: "NT_002_A003_C012",  start: 0.30, span: 0.31, fx: true, kind: "film" },
          { label: "NT_003_A006_C019",  start: 0.61, span: 0.39, fx: false, kind: "film" } ],
        [ { label: "NT_001_A001_C005.wav", start: 0.00, span: 1.00, fx: true, kind: "audio" } ],
        [ { label: "NT_002_A003_C012.wav", start: 0.00, span: 1.00, fx: true, kind: "audio" } ],
        [ { label: "Ambience_Forest_Wind.wav", start: 0.00, span: 1.00, fx: true, kind: "audio" } ]
    ]

    // The selected clip as the Inspector sees it.
    readonly property var selectedClip: ({
        name: "NT_001_A001_C005", kind: "video", connected: true,
        speed: 1, volume: -2.5, pan: 0, eq: "", opacity: 1,
        rectX: 0, rectY: 0, rectW: 1, rectH: 1, colorB: 0, colorC: 1, colorS: 1,
        fadeIn: 0, fadeOut: 12, mixOut: 0, disabled: false
    })

    readonly property var markers: [0.055, 0.145, 0.235, 0.325, 0.415, 0.505, 0.60, 0.69]
    readonly property real playhead: 0.445
    // Sequence length in frames at 24 fps (≈ 3 min 42 s) for the ruler.
    readonly property int frames: 5335
}
