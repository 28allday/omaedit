pragma Singleton

import QtQuick

// Stand-in for the engine while the Deliver page is laid out and for the
// headless preview scene. The shapes are Backend's: RenderPresets.presets
// and RenderQueue.jobs, ExportSpec keys inside `spec`.
QtObject {
    readonly property string projectName: "Northern Trails"

    function preset(id, name, group, builtin, spec) {
        return { id: id, name: name, group: group, builtin: builtin, favorite: id === "web-youtube-2160p", spec: spec }
    }

    readonly property var presets: [
        preset("master-prores-hq", "ProRes 422 HQ", "master", true, { container: "mov", vcodec: "prores", vprofile: "hq", acodec: "pcm24" }),
        preset("master-h265-10bit", "H.265 10-bit Master", "master", true, { container: "mp4", vcodec: "hevc", bitDepth: 10, quality: 16, acodec: "aac", abitrate: 320 }),
        preset("web-youtube-2160p", "YouTube 2160p 4K", "web", true, { container: "mp4", vcodec: "h264", width: 3840, height: 2160, quality: 18, acodec: "aac", abitrate: 320 }),
        preset("web-youtube-1080p", "YouTube 1080p", "web", true, { container: "mp4", vcodec: "h264", width: 1920, height: 1080, quality: 18, acodec: "aac", abitrate: 192 }),
        preset("web-vimeo-1080p", "Vimeo 1080p", "web", true, { container: "mp4", vcodec: "h264", width: 1920, height: 1080, rateMode: "vbr", bitrate: 20000, acodec: "aac", abitrate: 320 }),
        preset("broadcast-prores-1080i", "ProRes 422 HQ 1080i", "broadcast", true, { container: "mov", vcodec: "prores", vprofile: "hq", width: 1920, height: 1080, scan: "tff", acodec: "pcm24" }),
        preset("social-instagram-reels", "Instagram Reels 1080×1920", "social", true, { container: "mp4", vcodec: "h264", width: 1080, height: 1920, quality: 20, acodec: "aac", abitrate: 128 }),
        preset("audio-aac-320", "AAC 320 kbps", "audio", true, { container: "m4a", video: false, acodec: "aac", abitrate: 320 }),
        preset("custom-1", "YouTube 2160p 4K", "custom", false, { container: "mp4", vcodec: "h264", width: 3840, height: 2160, quality: 18, acodec: "aac", abitrate: 320 }),
        preset("custom-2", "Vimeo 1080p", "custom", false, { container: "mp4", vcodec: "h264", width: 1920, height: 1080, quality: 18, acodec: "aac", abitrate: 320 }),
        preset("custom-3", "Instagram 1080x1920", "custom", false, { container: "mp4", vcodec: "h264", width: 1080, height: 1920, quality: 20, acodec: "aac", abitrate: 128 }),
        preset("custom-4", "H.265 10bit Master", "custom", false, { container: "mp4", vcodec: "hevc", width: 3840, height: 2160, bitDepth: 10, quality: 16, acodec: "aac", abitrate: 320 }),
        preset("custom-5", "ProRes 422 HQ", "custom", false, { container: "mov", vcodec: "prores", width: 3840, height: 2160, vprofile: "hq", acodec: "pcm24" }),
        preset("custom-6", "Archive 1080p", "custom", false, { container: "mkv", vcodec: "ffv1", width: 1920, height: 1080, acodec: "flac" })
    ]

    function job(id, name, preset, status, percent, elapsed, path) {
        return { id: id, name: name, preset: preset, status: status, percent: percent, elapsed: elapsed,
                 remaining: status === "rendering" ? 74 : 0, path: path, stage: status === "rendering" ? "render" : "",
                 warnings: [], log: [], error: "", spec: {} }
    }

    readonly property var jobs: [
        job(1, "Northern Trails_Master", "Master – 4K", "completed", 100, 225, "~/Videos/Exports/Northern Trails_Master.mp4"),
        job(2, "Northern Trails_YouTube", "YouTube 2160p 4K", "completed", 100, 192, "~/Videos/Exports/Northern Trails_YouTube.mp4"),
        job(3, "Northern Trails_Vimeo", "Vimeo 1080p", "rendering", 62, 116, "~/Videos/Exports/Northern Trails_Vimeo.mp4"),
        job(4, "Northern Trails_IG", "Instagram 1080x1920", "queued", 0, 0, "~/Videos/Exports/Northern Trails_IG.mp4"),
        job(5, "Northern Trails_Audio", "Audio Only (AAC)", "queued", 0, 0, "~/Videos/Exports/Northern Trails_Audio.m4a")
    ]

    readonly property var validation: [
        { level: "ok", text: "Format is valid" },
        { level: "ok", text: "Video settings are valid" },
        { level: "ok", text: "Audio settings are valid" },
        { level: "ok", text: "In/Out range is valid" },
        { level: "warning", text: "Insufficient disk space for 1 file" }
    ]

    readonly property var estimate: ({ bytes: 12.48e9, freeBytes: 85.21e9, seconds: 165, bytesText: "12.48 GB", freeText: "85.21 GB" })

    function timecode(frame) {
        var fps = 23.976
        var s = Math.floor(frame / fps)
        var ff = Math.round(frame - s * fps)
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff)
    }
}
