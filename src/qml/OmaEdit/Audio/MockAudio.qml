pragma Singleton

import QtQuick

// Stand-in for the engine while the Audio page is laid out and for the
// headless preview scene. Shapes are Backend's: tracks (with `mix`),
// connected clip entries, trackLevels, master, loudness,
// spectrum, SoundLibrary.items, Recorder.inputs, adrCues.
QtObject {
    readonly property string projectName: "Northern Trails"
    readonly property real fps: 25
    readonly property int duration: 25 * 60 * 4  // 4 minutes
    readonly property int position: 25 * 86 + 14

    function track(index, name, role, volume, pan, fx, extra) {
        var mix = { role: role, volume: volume, pan: pan, fx: fx || [], output: "master",
                    sends: [{ bus: "bus1", level: -12, pre: false }], input: "" }
        if (extra) for (var k in extra) mix[k] = extra[k]
        return { index: index, name: name, kind: index === 0 ? "video" : "audio", locked: false, disabled: false,
                 muted: false, solo: false, autoSelect: true, clips: 2, mix: mix }
    }
    readonly property var tracks: [
        track(0, "V1", "", 0, 0, []),
        track(1, "D1", "dialogue", -3.2, 0, [ { id: "eq", params: { bands: [{f: 100, g: 0, q: 0.7}, {f: 350, g: -2.6, q: 1}, {f: 1000, g: -1.8, q: 1}, {f: 3500, g: 2.4, q: 1}, {f: 10000, g: 0, q: 0.7}] } },
                                                { id: "comp", params: { threshold: -20, ratio: 3, attack: 10, release: 120, makeup: 2 } },
                                                { id: "deesser", params: { frequency: 6500, intensity: 0.5, max: 0.5 } } ], { armed: true }),
        track(2, "A1", "ambience", -8.0, -0.2, [ { id: "eq" }, { id: "gate" } ]),
        track(3, "M1", "music", -5.0, 0, [ { id: "eq" }, { id: "comp" } ]),
        track(4, "E1", "effects", -7.4, 0.1, [ { id: "limiter" } ]),
        track(5, "E2", "effects", -6.0, 0, []),
        track(6, "E3", "effects", -3.0, -0.4, []),
        track(7, "E4", "effects", -9.2, 0.3, []),
        track(8, "R1", "roomtone", -18.0, 0, [ { id: "eq" } ])
    ]

    function clip(index, track, name, start, frames, volume, kf, shape, seed) {
        return { index: index, track: track, lane: track, name: name, path: "/sounds/" + name, start: start, frames: frames,
                 in: 0, out: frames - 1, length: frames, volume: volume, volumeKf: kf || [], pan: 0, fadeIn: 12, fadeOut: 18,
                 audioOnly: true, markers: [], label: "", disabled: false, eq: "" }
    }
    readonly property var clips: [
        clip(0, 1, "DIA_001_Take1.wav", 0, 25 * 84, -8.6, [{frame: 0, db: -8.6}, {frame: 600, db: -6}, {frame: 1100, db: -12}, {frame: 1800, db: -7}], "speech", 7),
        clip(1, 2, "AMB_Forest_Wind.wav", 0, 25 * 135, -12.0, [{frame: 0, db: -18}, {frame: 900, db: -10}, {frame: 2200, db: -14}, {frame: 3300, db: -9}], "bed", 11),
        clip(2, 3, "MUS_Adventure_Main.wav", 0, 25 * 195, -14.3, [], "bed", 13),
        clip(3, 4, "SFX_Bike_Gear_Shift.wav", 25 * 40, 25 * 3, -3.0, [], "hit", 17),
        clip(4, 5, "SFX_Tire_Roll_Gravel.wav", 25 * 44, 25 * 130, -6.0, [{frame: 0, db: -12}, {frame: 1500, db: -4}, {frame: 3200, db: -10}], "bed", 19),
        clip(5, 6, "SFX_Brake_Squeal.wav", 25 * 112, 25 * 2, -3.0, [], "hit", 23),
        clip(6, 7, "SFX_Chain_Rattle.wav", 25 * 60, 25 * 4, -9.2, [], "hit", 29),
        clip(7, 8, "RT_Trail_RoomTone.wav", 0, 25 * 200, -18.0, [], "quiet", 31)
    ]

    readonly property var markers: [
        { frame: 25 * 15, type: "standard", text: "Trail Start" },
        { frame: 25 * 48, type: "standard", text: "Climb" },
        { frame: 25 * 112, type: "standard", text: "Descent" },
        { frame: 25 * 163, type: "chapter", text: "Jump" },
        { frame: 25 * 196, type: "done", text: "Finish" }
    ]

    function levels(l, r) { return { in: [l + 6, r + 6], post: [l + 3, r + 3], out: [l, r] } }
    readonly property var trackLevels: [
        levels(-100, -100), levels(-3.2, -4.0), levels(-8.0, -8.6), levels(-5.0, -5.4), levels(-7.4, -7.0),
        levels(-20, -22), levels(-24, -24), levels(-30, -31), levels(-18, -18.5)
    ]
    readonly property var masterLevels: [-2.1, -2.6]
    readonly property var master: ({ volume: -6.0, pan: 0, fx: [ { id: "limiter", params: { ceiling: -1 } } ] })
    readonly property var buses: [ { id: "bus1", name: "Bus 1", volume: -2.1, pan: 0, fx: [ { id: "comp" }, { id: "limiter" } ] } ]

    readonly property var loudness: ({ momentary: -15.2, shortTerm: -17.2, integrated: -16.0, range: 7.4, truePeak: -1.2, seconds: 86 })
    readonly property var loudnessHistory: {
        var h = []
        for (var i = 0; i < 400; ++i) h.push(-19 + 4 * Math.sin(i / 23) + 2 * Math.sin(i / 7))
        return h
    }
    readonly property var spectrum: {
        var s = []
        for (var i = 0; i < 48; ++i) s.push(-18 - i * 0.7 + 6 * Math.sin(i / 3.1))
        return s
    }
    readonly property var mixEffects: [
        { id: "eq", name: "Equaliser", mixable: true, params: { bands: [{f: 100, g: 0, q: 0.7}, {f: 350, g: 0, q: 1}, {f: 1000, g: 0, q: 1}, {f: 3500, g: 0, q: 1}, {f: 10000, g: 0, q: 0.7}] } },
        { id: "comp", name: "Compressor", mixable: true, params: { threshold: -20, ratio: 3, attack: 10, release: 120, knee: 6, makeup: 2 } },
        { id: "gate", name: "Gate / Expander", mixable: false, params: { threshold: -40, ratio: 2, attack: 20, release: 250, range: -60, knee: 6 } },
        { id: "limiter", name: "Limiter", mixable: false, params: { ceiling: -1, attack: 5, release: 50 } },
        { id: "deesser", name: "De-Esser", mixable: false, params: { frequency: 6500, intensity: 0.5, max: 0.5 } },
        { id: "nr", name: "Noise Reduction", mixable: false, params: { reduction: 12, floor: -50, track: true } },
        { id: "highpass", name: "High-pass", mixable: false, params: { frequency: 80 } },
        { id: "lowpass", name: "Low-pass", mixable: false, params: { frequency: 12000 } }
    ]

    function sound(name, category, seconds, rate, ch, seed, favorite) {
        var peaks = []
        var x = seed
        for (var i = 0; i < 48; ++i) { x = (x * 9301 + 49297) % 233280; peaks.push(0.15 + 0.85 * (x / 233280) * (category === "roomtone" ? 0.3 : 1)) }
        return { path: "~/Music/" + name, name: name, folder: "~/Music", category: category, duration: seconds,
                 sampleRate: rate, channels: ch, size: seconds * rate * ch * 3, favorite: favorite === true, tags: [], peaks: peaks, probed: true, missing: false }
    }
    readonly property var sounds: [
        sound("DIA_001_Take1.wav", "dialogue", 84, 48000, 2, 3, true),
        sound("DIA_002_Take2.wav", "dialogue", 58, 48000, 2, 5),
        sound("DIA_003_Adlib.wav", "dialogue", 26, 48000, 2, 8),
        sound("AMB_Forest_Wind.wav", "ambience", 135, 48000, 4, 9),
        sound("AMB_Trail_Birds.wav", "ambience", 97, 48000, 4, 12),
        sound("MUS_Tension_Build.wav", "music", 128, 48000, 2, 15),
        sound("MUS_Adventure_Main.wav", "music", 195, 48000, 2, 18, true),
        sound("SFX_Bike_Gear_Shift.wav", "effects", 3, 48000, 2, 21),
        sound("SFX_Tire_Roll_Gravel.wav", "effects", 6, 48000, 2, 24),
        sound("SFX_Brake_Squeal.wav", "effects", 2, 48000, 2, 27),
        sound("SFX_Chain_Rattle.wav", "effects", 4, 48000, 2, 30),
        sound("FOL_Boots_Gravel.wav", "foley", 9, 48000, 2, 33),
        sound("RT_Trail_RoomTone.wav", "roomtone", 60, 48000, 4, 36),
        sound("RT_Woods_RoomTone.wav", "roomtone", 60, 48000, 4, 39)
    ]
    readonly property var soundFolders: ["~/Music"]
    readonly property var collections: [ { name: "Bike Pass", query: "bike", category: "" } ]
    readonly property var inputs: [ { id: "in1", name: "Scarlett 2i2 (Input 1)", default: true }, { id: "in2", name: "Built-in Microphone", default: false } ]
    readonly property var outputs: [ { id: "out1", name: "Scarlett 2i2", default: true } ]
    readonly property var adrCues: [
        { id: "c1", character: "MAYA", line: "We should have turned back at the ridge.", notes: "Breathier, over the shoulder", in: 25 * 20, out: 25 * 24, preRoll: 50, postRoll: 25, track: 1, status: "approved",
          takes: [ { path: "/takes/c1_t1.wav", rating: 2, promoted: false }, { path: "/takes/c1_t2.wav", rating: 4, promoted: true } ] },
        { id: "c2", character: "MAYA", line: "Hold on — the chain's slipping.", notes: "", in: 25 * 58, out: 25 * 61, preRoll: 50, postRoll: 25, track: 1, status: "todo", takes: [] },
        { id: "c3", character: "JONAS", line: "Left! Left, left!", notes: "Shouted, off-mic", in: 25 * 110, out: 25 * 112, preRoll: 50, postRoll: 25, track: 1, status: "recorded",
          takes: [ { path: "/takes/c3_t1.wav", rating: 3, promoted: false } ] }
    ]
}
