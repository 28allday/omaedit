import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

ColumnLayout {
    id: root
    spacing: Ui.Theme.s2
    property var info: backend.hardware.info
    readonly property var gpu: info.gpu || ({})
    readonly property var decoders: info.decoderDetails || ({})
    readonly property bool busy: !!info.encoderBusy || !!info.gpuBusy
    property var system: JSON.parse(backend.systemInformation())
    property bool copied: false
    Connections {
        target: backend.hardware
        function onInfoChanged() { root.system = JSON.parse(backend.systemInformation()) }
    }
    function size(bytes) { return (Number(bytes) / 1073741824).toFixed(1) + qsTr(" GiB") }
    component Note: C.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Ui.Theme.textSecondary
        font.pixelSize: Ui.Theme.fsLabel
    }
    RowLayout {
        Layout.fillWidth: true
        Ui.ToolButton {
            objectName: "hardwareRefresh"
            text: root.busy ? qsTr("Checking…") : qsTr("Refresh Hardware")
            enabled: !root.busy
            onClicked: { root.copied = false; backend.hardware.probe(true) }
        }
        Ui.ToolButton {
            objectName: "hardwareCopy"
            text: root.copied ? qsTr("Copied") : qsTr("Copy Diagnostics")
            onClicked: root.copied = backend.copySystemInformation()
        }
    }
    Note { text: qsTr("Effects and compositing: CPU · Viewer and cache colour can use Vulkan") }
    Ui.ComboField {
        id: engineGpu
        objectName: "engineGpuPreference"
        tip: qsTr("Which graphics processor draws the picture; takes effect on the next launch")
        Layout.fillWidth: true
        caption: qsTr("Graphics processor")
        readonly property var choices: backend.hardware.engineGpus
        model: choices.map(choice => choice.name)
        currentIndex: Math.max(0, choices.findIndex(choice => choice.id === backend.hardware.engineGpu))
        onActivated: backend.hardware.setEngineGpu(choices[currentIndex].id)
    }
    Note { text: qsTr("Automatic takes the best graphics processor found. The choice is yours alone: it is never changed for you, on battery or otherwise. CPU draws everything without one.") }
    Note {
        visible: backend.hardware.engineGpuRestartRequired
        text: qsTr("Restart OmaEdit to draw with the graphics processor chosen.")
    }
    Ui.ComboField {
        objectName: "cacheProcessingPreference"
        tip: qsTr("Use the GPU for supported cache colour conversions when a local comparison shows a speed gain")
        Layout.fillWidth: true
        caption: qsTr("Playback cache")
        model: [qsTr("Auto — measured Vulkan"), qsTr("CPU")]
        currentIndex: backend.backgroundRenderProcessing === "cpu" ? 1 : 0
        onActivated: backend.setBackgroundRenderProcessing(currentIndex === 1 ? "cpu" : "auto")
    }
    Note { text: qsTr("Auto detects Vulkan and checks colour accuracy and speed before using it. Video decoding follows the decoder preference below. Effects, compositing and lossless cache compression run on the CPU.") }
    Note {
        readonly property var processing: backend.backgroundRender.processing || ({})
        text: processing.preference ? qsTr("Cache colour: %1 · %2 GPU / %3 CPU conversions")
            .arg((processing.backend || "cpu").toUpperCase()).arg(processing.gpuConversions || 0).arg(processing.cpuConversions || 0) : qsTr("Cache colour: idle")
    }
    Note {
        readonly property var processing: backend.backgroundRender.processing || ({})
        visible: !!processing.reason
        text: processing.reason || ""
    }
    Note {
        readonly property var conversion: (backend.backgroundRender.processing || {}).conversion || ({})
        visible: !!conversion.device
        text: qsTr("Cache GPU: %1").arg(conversion.device || "")
    }
    Ui.ComboField {
        objectName: "viewerProcessingPreference"
        tip: qsTr("Whether viewer colour conversion may use Vulkan or stays on the CPU")
        Layout.fillWidth: true
        caption: qsTr("Viewer")
        model: [qsTr("Auto — measured Vulkan"), qsTr("CPU")]
        currentIndex: backend.hardware.viewerProcessingPreference === "cpu" ? 1 : 0
        onActivated: backend.hardware.setViewerProcessingPreference(currentIndex === 1 ? "cpu" : "auto")
    }
    Note { text: qsTr("Auto compares supported colour transforms locally and uses Vulkan only when it is consistently faster. CPU remains available for every transform.") }
    Note {
        readonly property var processing: root.system.viewerProcessing || ({})
        text: processing.workers ? qsTr("Viewer conversion: %1 · %2 queued · %3 reused results")
             .arg((processing.backend || "cpu").toUpperCase()).arg(processing.pendingClients || 0).arg(processing.cacheHits || 0) : qsTr("Viewer conversion: idle")
    }
    Note {
        readonly property var conversion: (root.system.viewerProcessing || {}).conversion || ({})
        visible: !!conversion.reason
        text: conversion.reason || ""
    }
    Note {
        readonly property var conversion: (root.system.viewerProcessing || {}).conversion || ({})
        visible: !!conversion.device
        text: qsTr("Viewer GPU: %1").arg(conversion.device || "")
    }
    Note { text: qsTr("Qt Quick interface: %1").arg(root.system.interfaceRenderer || qsTr("Unknown")) }
    Ui.ComboField {
        objectName: "interfaceRenderingPreference"
        tip: qsTr("How the interface is drawn; takes effect on the next launch")
        Layout.fillWidth: true
        caption: qsTr("Interface")
        model: [qsTr("Auto — prefer Vulkan"), qsTr("Vulkan with software fallback"), qsTr("Software")]
        currentIndex: backend.hardware.interfaceRenderingPreference === "software" ? 2 : backend.hardware.interfaceRenderingPreference === "vulkan" ? 1 : 0
        onActivated: backend.hardware.setInterfaceRenderingPreference(currentIndex === 2 ? "software" : currentIndex === 1 ? "vulkan" : "auto")
    }
    Note { text: backend.hardware.interfaceRenderingRestartRequired ? qsTr("Restart OmaEdit to apply the interface preference. Save your project before closing.") : qsTr("Interface preferences apply on the next launch. Vulkan is checked before startup; software is available if the check fails.") }
    Note { visible: !!(root.system.interfaceStartup || {}).reason; text: (root.system.interfaceStartup || {}).reason || "" }
    Note {
        visible: !!root.system.systemMemoryBytes
        text: qsTr("System RAM: %1 available / %2 total")
            .arg(root.size((root.system.systemMemoryBytes || {}).MemAvailable || 0))
            .arg(root.size((root.system.systemMemoryBytes || {}).MemTotal || 0))
    }
    Note {
        objectName: "hardwareVulkanStatus"
        text: root.info.gpuBusy ? qsTr("Detecting Vulkan devices…")
              : root.gpu.available ? qsTr("Vulkan %1 · %2 device(s)").arg(root.gpu.loaderVersion).arg((root.gpu.devices || []).length)
              : qsTr("Vulkan unavailable. CPU processing remains available.")
    }
    Repeater {
        model: root.gpu.devices || []
        delegate: ColumnLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: Ui.Theme.s1
            Note { text: modelData.name; color: Ui.Theme.textPrimary; font.bold: true }
            Note {
                text: qsTr("%1 · API %2 · %3 device-local memory")
                    .arg(modelData.type).arg(modelData.apiVersion).arg(root.size(modelData.deviceLocalBytes))
            }
            Note {
                text: [modelData.compute ? qsTr("Compute queue") : qsTr("No compute queue"),
                       modelData.graphics ? qsTr("Graphics queue") : qsTr("No graphics queue")].join(" · ")
            }
            Repeater {
                model: modelData.memoryBudgetAvailable ? modelData.heaps : []
                delegate: Note {
                    required property var modelData
                    visible: modelData.deviceLocal
                    text: qsTr("Driver estimate: %1 budget · %2 process usage")
                        .arg(root.size(modelData.budgetBytes)).arg(root.size(modelData.usageBytes))
                }
            }
        }
    }
    Note { text: qsTr("Integrated and CPU devices may share system RAM. Memory estimates describe the probe process at the last refresh.") }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Ui.Theme.border }
    Note { text: qsTr("Hardware encoding · two-frame test per codec and device"); color: Ui.Theme.textPrimary; font.bold: true }
    Note { text: root.info.ffmpegVersion || (root.info.encoderBusy ? qsTr("Finding FFmpeg…") : qsTr("FFmpeg version unavailable")) }
    Note { visible: !!root.info.encoderError; text: root.info.encoderError || ""; color: Ui.Theme.dangerText }
    Repeater {
        model: root.info.encoderDetails || []
        delegate: Note {
            required property var modelData
            text: modelData.encoder + (modelData.device ? " · " + modelData.device : "") + " — "
                  + (modelData.available ? qsTr("Test passed") : qsTr("Unavailable"))
        }
    }
    Note { text: qsTr("A small probe does not guarantee every format, profile or resolution. Render failures can use the existing software fallback.") }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Ui.Theme.border }
    Note { text: qsTr("Decoding"); color: Ui.Theme.textPrimary; font.bold: true }
    Ui.ComboField {
        objectName: "videoDecoderPreference"
        tip: qsTr("Which decoder plays video files; takes effect on the next launch")
        Layout.fillWidth: true
        caption: qsTr("Decoder")
        model: [qsTr("Auto — measured"), qsTr("Software"), qsTr("Vulkan"), qsTr("VA-API"), qsTr("NVDEC")]
        currentIndex: ["auto", "software", "vulkan", "vaapi", "nvdec"].indexOf(backend.hardware.videoDecoderPreference)
        onActivated: backend.hardware.setVideoDecoderPreference(["auto", "software", "vulkan", "vaapi", "nvdec"][currentIndex])
    }
    Ui.ComboField {
        objectName: "videoDecoderDevice"
        tip: qsTr("Which hardware device the chosen decoder runs on")
        Layout.fillWidth: true
        visible: backend.hardware.videoDecoderPreference !== "auto" && backend.hardware.videoDecoderPreference !== "software"
        caption: qsTr("Device")
        model: [qsTr("Automatic")].concat(backend.hardware.videoDecoderDevices)
        currentIndex: backend.hardware.videoDecoderDevice ? backend.hardware.videoDecoderDevices.indexOf(backend.hardware.videoDecoderDevice) + 1 : 0
        onActivated: backend.hardware.setVideoDecoderDevice(currentIndex ? backend.hardware.videoDecoderDevices[currentIndex - 1] : "")
    }
    Note { text: backend.hardware.videoDecoderRestartRequired ? qsTr("Restart OmaEdit to apply decoder changes. Save your project before closing.") : qsTr("Decoder preferences apply on the next launch.") }
    Note { text: qsTr("Playback decoder policy: %1").arg((root.system.playbackDecoder || "software").toUpperCase()) }
    Note { text: (root.system.decodeStartup || {}).reason || qsTr("Startup decoder check has not run.") }
    Ui.ToolButton {
        objectName: "videoDecoderRetest"
        text: qsTr("Retest decoder on next launch")
        onClicked: backend.hardware.retestVideoDecoder()
    }
    Note { text: qsTr("Auto compares software and hardware on a short H.264 sample, including setup and frame downloads. Other formats may use software. Audio stays on the existing playback clock.") }
    Note {
        readonly property var nativeDecode: root.system.nativeDecoding || ({})
        text: qsTr("Hardware frames: %1 · CPU replays: %2")
            .arg(Number(nativeDecode.vaapiDownloads || 0) + Number(nativeDecode.nvdecDownloads || 0))
            .arg(Number(nativeDecode.softwareReplays || 0))
    }
    Note { text: qsTr("The capability checks below are separate from playback.") }
    Repeater {
        model: root.decoders.runtimeTests || []
        delegate: Note {
            required property var modelData
            text: modelData.backend.toUpperCase() + " · H.264" + (modelData.device ? " · " + modelData.device : "")
                  + " — " + (modelData.available ? qsTr("Test passed") : qsTr("Unavailable"))
        }
    }
    Note { visible: !!root.decoders.fixtureError; text: root.decoders.fixtureError || "" }
    Note { text: qsTr("FFmpeg compiled methods: %1").arg((root.decoders.compiledMethods || []).join(", ") || qsTr("Unknown")) }
    Note { text: qsTr("NVDEC decoder wrappers: %1").arg((root.decoders.nvdecCompiled || []).join(", ") || qsTr("None reported")) }
    Note { text: qsTr("Vulkan compute, hardware video decoding and the Qt Quick interface are separate capabilities. CUDA is optional.") }
}
