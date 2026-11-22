pragma Singleton

import QtQuick

// Stand-in for the engine while the Color page is laid out and for the
// headless preview scene. Shapes are Backend's: gradeClip, colorClips,
// StillsGallery.items, luts, scope keys.
QtObject {
    readonly property string projectName: "Northern Trails"
    readonly property real fps: 25
    readonly property int duration: 25 * 60 * 4
    readonly property int position: 25 * 86 + 14

    function node(label, kind, primaries) {
        var p = { lift: [0, 0, 0, 0], gamma: [0, 0, 0, 0], gain: [1, 0, 0, 0], offset: [0, 0, 0, 0],
                  temp: 6500, tint: 0, exposure: 0, contrast: 1, pivot: 0.435, saturation: 1, hue: 0,
                  highlights: 0, shadows: 0, whites: 0, blacks: 0, midDetail: 0 }
        if (primaries) for (var k in primaries) p[k] = primaries[k]
        return { id: label.toLowerCase(), label: label, kind: kind || "serial", enabled: true, primaries: p,
                 curves: { master: [[0, 0], [1, 1]], red: [[0, 0], [1, 1]], green: [[0, 0], [1, 1]], blue: [[0, 0], [1, 1]],
                           hueHue: [[0, 0.5], [1, 0.5]], hueSat: [[0, 0.5], [1, 0.5]], hueLum: [[0, 0.5], [1, 0.5]], lumaLuma: [[0, 0], [1, 1]], satSat: [[0, 0], [1, 1]] },
                 qualifier: { enabled: false, color: "#ff0000", deltaH: 0.2, deltaS: 0.3, deltaL: 0.3, softness: 0.1, invert: false, subspace: "hsl" },
                 window: { enabled: false, shape: "circle", x: 0.5, y: 0.5, w: 0.4, h: 0.4, rotation: 0, softness: 0.1, invert: false, track: [] },
                 lut: { path: "", intensity: 1 }, keyframes: {} }
    }
    readonly property var grade: ({
        version: 1, input: "auto", output: "rec709", bypass: false,
        nodes: [ node("Input"), node("Balance", "serial", { temp: 5600, tint: 0 }), node("Contrast", "serial", { contrast: 1.12, pivot: 0.435 }),
                 node("Look", "serial", { saturation: 1.08, gain: [1.0, 0.02, 0.0, -0.03] }), node("Output") ]
    })
    readonly property var gradeClip: ({ track: 0, index: 5, name: "NT_001_A006_C019", start: 25 * 85, frames: 25 * 6 + 3,
                                        grade: grade, gradeVersions: [ { name: "Version 1", grade: grade }, { name: "Warm", grade: grade } ], gradeVersion: 0, bypass: false, label: "" })
    function clip(i, name, start, frames, graded, label) {
        return { track: 0, index: i, name: name, start: start, frames: frames, label: label || "", markers: i % 3 === 0 ? 1 : 0,
                 graded: graded, versions: graded ? 2 : 0, thumb: "" }
    }
    readonly property var colorClips: [
        clip(0, "NT_001_A001_C005", 0, 25 * 12, true, "blue"), clip(1, "NT_001_A002_C006", 25 * 12, 25 * 9, true),
        clip(2, "NT_002_A003_C012", 25 * 21, 25 * 7, false), clip(3, "NT_002_A004_C013", 25 * 28, 25 * 14, false),
        clip(4, "NT_003_A003_C018", 25 * 42, 25 * 8, true, "orange"), clip(5, "NT_001_A006_C019", 25 * 85, 25 * 6, true),
        clip(6, "NT_003_A005_C018", 25 * 91, 25 * 11, false), clip(7, "NT_003_A005_C018", 25 * 102, 25 * 9, false),
        clip(8, "NT_002_A007_C020", 25 * 111, 25 * 10, true)
    ]
    function still(id, name, album, favorite, seconds) {
        return { id: id, name: name, album: album, path: "", grade: grade, clipName: name, frame: seconds * 25, favorite: favorite, created: "2026-09-05T10:00:00", width: 1920, height: 1080 }
    }
    readonly property var stills: [
        still("s1", "NT_001_A001_C005", "Northern Trails", true, 738), still("s2", "NT_001_A002_C006", "Northern Trails", false, 547),
        still("s3", "NT_002_A003_C012", "Northern Trails", false, 434), still("s4", "NT_002_A004_C013", "Mountain Biking", false, 862),
        still("s5", "NT_003_A003_C018", "Nature", false, 491), still("s6", "NT_002_A006_C019", "Trails", false, 363)
    ]
    readonly property var albums: ["Northern Trails", "Mountain Biking", "Nature", "Trails", "Cinematic"]
    readonly property var luts: [ { path: "/luts/Kodak 2383.cube", name: "Kodak 2383", folder: "Film" }, { path: "/luts/Teal Orange.cube", name: "Teal Orange", folder: "Looks" },
                                  { path: "/luts/Rec709 to sLog3.cube", name: "Rec709 to sLog3", folder: "Technical" } ]
    readonly property var keyframes: [
        { param: "Master", frame: 25 * 86 + 14, value: 1.0 }, { param: "Corrector 1", frame: 25 * 86 + 14, value: 1.0 },
        { param: "Corrector 2", frame: 25 * 86 + 14, value: 1.0 }, { param: "Corrector 3", frame: 25 * 86 + 14, value: 1.0 },
        { param: "Corrector 4", frame: 25 * 86 + 14, value: 1.0 }, { param: "Look", frame: 25 * 86 + 14, value: 1.0 }
    ]
}
