pragma Singleton

import QtQuick

// Stand-in for the engine while the Media page is laid out and for the
// headless preview scene. Every key here is one Backend's library entries
// really carry (see PlayerWorker::importClip and Backend's logging ops), so
// the seam in MediaPage.qml is a binding swap, not a translation.
QtObject {
    readonly property string projectName: "Northern Trails"

    readonly property var drives: [
        { name: "NVMe System", path: "/", total: 1.8e12, free: 1.2e12, totalLabel: "1.8 TB", freeLabel: "1.2 TB", fraction: 0.33, removable: false, network: false },
        { name: "Media RAID", path: "/mnt/media", total: 12e12, free: 7.6e12, totalLabel: "12 TB", freeLabel: "7.6 TB", fraction: 0.37, removable: false, network: false },
        { name: "Archive HDD", path: "/run/media/editor/archive", total: 18e12, free: 11.3e12, totalLabel: "18 TB", freeLabel: "11.3 TB", fraction: 0.37, removable: true, network: false }
    ]

    readonly property var recentProjects: [
        "/mnt/media/Northern Trails/Northern Trails.omaedit",
        "/mnt/media/Adventure Series/Adventure Series.omaedit",
        "/mnt/media/Brand Film Summer/Brand Film Summer.omaedit",
        "/mnt/media/Documentary Package/Documentary Package.omaedit",
        "/mnt/media/Trailer Cutdowns/Trailer Cutdowns.omaedit"
    ]

    function clip(i, name, frames, tone, extra) {
        var e = {
            name: name, path: "/mnt/media/Northern Trails/" + name + ".mov",
            duration: frames, fps: 23.976, width: 3840, height: 2160, thumb: "", tone: tone,
            hasVideo: true, hasAudio: true, videoCodec: "xavc", videoCodecLong: "XAVC-I",
            pixelFormat: "yuv422p10le", bitDepth: 10, colorSpace: "Rec. 709",
            audioCodec: "pcm_s24le", channels: 2, sampleRate: 48000, audioBitDepth: 24,
            size: 2.1e9, created: "2024-05-14T10:23:18", modified: "2024-05-14T10:23:18",
            imported: "2024-06-01T09:00:00", make: "Sony", model: "FX6", rating: 4,
            tags: ["Trail"], keywords: ["Descent", "Forest"], notes: ""
        }
        for (var k in extra) e[k] = extra[k]
        return e
    }

    readonly property var clips: [
        clip(0, "NT_001_A001_C005", 17688, 0.42, { notes: "Mountain bike descent on rocky trail.", tags: ["Trail"], rangeIn: 1800, rangeOut: 4100, favorite: true }),
        clip(1, "NT_001_A002_C006", 13103, 0.66, { tags: ["Action"], rating: 5, proxy: true }),
        clip(2, "NT_001_A003_C007", 21970, 0.30, { tags: ["Landscape"], rating: 4, hasAudio: false }),
        clip(3, "NT_001_A004_C008", 11826, 0.54, { tags: ["People"], rating: 3 }),
        clip(4, "NT_001_A005_C009", 15926, 0.72, { tags: ["Trail"], rating: 3 }),
        clip(5, "NT_002_A001_C010", 19855, 0.38, { tags: ["Scenic"], rating: 3 }),
        clip(6, "NT_002_A002_C011", 10390, 0.48, { tags: ["Water"], rating: 4 }),
        clip(7, "NT_002_A003_C012", 20668, 0.60, { tags: ["Action"], rating: 4, proxy: true }),
        clip(8, "NT_002_A004_C013", 13160, 0.35, { tags: ["Trail"], rating: 2 }),
        clip(9, "NT_002_A005_C014", 15188, 0.58, { tags: ["Action"], rating: 4 }),
        clip(10, "NT_003_A001_C015", 12574, 0.44, { rating: 0, width: 1920, height: 1080 }),
        clip(11, "NT_003_A002_C016", 17400, 0.62, { rating: 0 }),
        clip(12, "NT_003_A003_C017", 9526, 0.28, { rating: 0, offline: true }),
        clip(13, "NT_003_A004_C018", 17008, 0.50, { rating: 0, proxy: true }),
        clip(14, "NT_003_A005_C019", 10768, 0.66, { rating: 0 })
    ]
}
