import QtQuick
import QtQuick.Controls as C
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs as D
import OmaEdit.Ui as Ui

Ui.ModalDialog {
    id: root
    objectName: "viewerPlaybackDialog"
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 48)
    height: Math.min(690, parent.height - 48)
    modal: true
    font.family: Ui.Theme.fontFamily
    font.pixelSize: Ui.Theme.fsControl
    Material.theme: Material.Dark
    Material.foreground: Ui.Theme.textPrimary
    background: Rectangle { color: Ui.Theme.panelBg; radius: Ui.Theme.rMenu; border.color: Ui.Theme.border; border.width: Ui.Theme.hairline }
    title: qsTr("Viewer and Playback")
    standardButtons: C.Dialog.Close
    readonly property var view: backend.viewerSettings
    readonly property var monitor: backend.monitorSettings
    readonly property var stats: backend.playbackStats
    readonly property var zooms: [25, 50, 100, 200]
    readonly property int zoomIndex: view.zoomMode === "fit" ? 0 : view.zoomMode === "fill" ? 1 : zooms.indexOf(view.zoomPercent) >= 0 ? zooms.indexOf(view.zoomPercent) + 2 : 6
    function setZoom(index) {
        backend.setViewerOption("zoomMode", index === 0 ? "fit" : index === 1 ? "fill" : "custom")
        if (index >= 2 && index <= 5) backend.setViewerOption("zoomPercent", zooms[index - 2])
    }
    contentItem: C.ScrollView {
        C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: Ui.Theme.s2
            C.Label { text: qsTr("Shared across all workspaces"); font.bold: true; color: Ui.Theme.textPrimary }
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                C.Label { text: qsTr("Viewer zoom") }
                RowLayout {
                    Layout.fillWidth: true
                    Ui.ComboField { objectName: "viewerZoom"; tip: qsTr("How the frame is scaled in the viewer: fit, fill or a set percentage"); Layout.fillWidth: true; model: [qsTr("Fit"), qsTr("Fill"), "25%", "50%", "100%", "200%", qsTr("Custom")]; currentIndex: root.zoomIndex; onActivated: function(index) { root.setZoom(index) } Accessible.name: qsTr("Viewer zoom") }
                    C.SpinBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Exact viewer magnification from 1 to 800 per cent, used when zoom is Custom"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerZoomPercent"; Layout.preferredWidth: 140; from: 1; to: 800; editable: true; value: root.view.zoomPercent; enabled: root.view.zoomMode === "custom"; onValueModified: { backend.setViewerOption("zoomPercent", value) } Accessible.name: qsTr("Custom zoom percent") }
                    C.Label { text: "%" }
                }
                C.Label { text: qsTr("Pan horizontal") }
                C.Slider { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Slide the zoomed image left or right inside the viewer"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerPanX"; Layout.fillWidth: true; from: -1; to: 1; value: root.view.panX; enabled: root.view.zoomMode !== "fit"; onMoved: backend.setViewerOption("panX", value); Accessible.name: qsTr("Pan horizontal") }
                C.Label { text: qsTr("Pan vertical") }
                C.Slider { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Slide the zoomed image up or down inside the viewer"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; Layout.fillWidth: true; from: -1; to: 1; value: root.view.panY; enabled: root.view.zoomMode !== "fit"; onMoved: backend.setViewerOption("panY", value); Accessible.name: qsTr("Pan vertical") }
                C.Label { text: qsTr("Canvas colour") }
                RowLayout {
                    Layout.fillWidth: true
                    // The presets, then Custom: picking Custom opens the colour picker.
                    Ui.ComboField { objectName: "viewerCanvas"; tip: qsTr("The colour around the picture, so a tall or narrow frame shows where its edges are"); Layout.fillWidth: true
                        model: Ui.CanvasColours.presets.map(function (p) { return p.name }).concat([qsTr("Custom…")])
                        currentIndex: Ui.CanvasColours.indexOf(root.view.canvas) >= 0 ? Ui.CanvasColours.indexOf(root.view.canvas) : Ui.CanvasColours.presets.length
                        onActivated: function(index) {
                            if (index < Ui.CanvasColours.presets.length) backend.setViewerOption("canvas", Ui.CanvasColours.presets[index].colour)
                            else { canvasPicker.selectedColor = root.view.canvas; canvasPicker.open() }
                            currentIndex = Qt.binding(function () { return Ui.CanvasColours.indexOf(root.view.canvas) >= 0 ? Ui.CanvasColours.indexOf(root.view.canvas) : Ui.CanvasColours.presets.length })
                        }
                        Accessible.name: qsTr("Canvas colour") }
                    Rectangle { objectName: "viewerCanvasSwatch"; implicitWidth: Ui.Theme.hControl; implicitHeight: Ui.Theme.hControl; radius: Ui.Theme.rControl; color: root.view.canvas; border.width: Ui.Theme.hairline; border.color: Ui.Theme.border }
                    D.ColorDialog { id: canvasPicker; objectName: "viewerCanvasPicker"; title: qsTr("Canvas Colour"); options: D.ColorDialog.DontUseNativeDialog; onAccepted: backend.setViewerOption("canvas", String(selectedColor)) }
                }
                C.Label { text: qsTr("Playback resolution") }
                Ui.ComboField { objectName: "viewerResolution"; tip: qsTr("Play at half or quarter of the timeline's resolution so a heavy timeline keeps its frame rate; scopes and exports stay full resolution"); Layout.fillWidth: true; model: [qsTr("Full"), qsTr("Half"), qsTr("Quarter")]; currentIndex: [1,2,4].indexOf(root.view.previewDivisor); onActivated: function(index) { backend.setViewerOption("previewDivisor", [1,2,4][index]) } Accessible.name: qsTr("Preview resolution") }
                C.Label { text: qsTr("Playback media") }
                Ui.ComboField { objectName: "viewerMedia"; tip: qsTr("Whether playback reads the original files or the lighter proxies"); Layout.fillWidth: true; model: [qsTr("Proxy preferred"), qsTr("Original"), qsTr("Proxy")]; currentIndex: ["preferred","original","proxy"].indexOf(backend.mediaPlayback); onActivated: function(index) { backend.mediaPlayback = ["preferred","original","proxy"][index] } Accessible.name: qsTr("Playback media") }
            }
            C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Playback resolution affects the viewer only, while playing: paused, the picture is full resolution. Snapshots, scopes and exports keep full-resolution frames. Zoom and overlays are viewing aids."); color: Ui.Theme.textMuted; font.pixelSize: Ui.Theme.fsLabel }
            RowLayout {
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Overlay title and action safe guides on the viewer image"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerSafeAreas"; text: qsTr("Safe areas"); checked: root.view.safeAreas; onClicked: backend.setViewerOption("safeAreas", checked) }
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Show a chequerboard behind the image so transparent areas are visible"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerChecker"; text: qsTr("Transparency"); checked: root.view.checker; onClicked: backend.setViewerOption("checker", checked) }
                Ui.ComboField { objectName: "viewerGuides"; tip: qsTr("Composition overlay drawn on the image: thirds, cross or grid"); Layout.fillWidth: true; model: [qsTr("No guides"), qsTr("Thirds"), qsTr("Cross"), qsTr("Grid")]; currentIndex: ["none","thirds","cross","grid"].indexOf(root.view.guides); onActivated: function(index) { backend.setViewerOption("guides", ["none","thirds","cross","grid"][index]) } Accessible.name: qsTr("Viewer guides") }
            }
            C.Label { text: qsTr("Monitoring"); font.bold: true }
            RowLayout {
                C.Label { text: qsTr("Volume") }
                C.Slider { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Listening level for the monitor; it does not change the mix"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerVolume"; Layout.fillWidth: true; from: 0; to: 1; value: backend.monitorVolume; onMoved: backend.monitorVolume = value; Accessible.name: qsTr("Monitor volume") }
                C.Label { text: Math.round(backend.monitorVolume * 100) + "%" }
            }
            RowLayout {
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Silence monitoring without touching the volume setting"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerMute"; text: qsTr("Mute"); checked: root.monitor.muted === true; onClicked: backend.setMonitorOptions(checked, root.monitor.dim === true, root.monitor.mono === true) }
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Drop monitoring by 20 dB for a quick quiet check"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Dim −20 dB"); checked: root.monitor.dim === true; onClicked: backend.setMonitorOptions(root.monitor.muted === true, checked, root.monitor.mono === true) }
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Fold the monitor feed to mono to check phase and balance"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Mono"); checked: root.monitor.mono === true; onClicked: backend.setMonitorOptions(root.monitor.muted === true, root.monitor.dim === true, checked) }
                C.CheckBox { implicitHeight: Ui.Theme.hControl; C.ToolTip.text: qsTr("Replay the In to Out range over and over instead of stopping"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; objectName: "viewerLoop"; text: qsTr("Loop In/Out"); checked: backend.loop; onClicked: backend.loop = checked }
            }
            RowLayout {
                C.Button { implicitHeight: Ui.Theme.hControl + Ui.Theme.s2; objectName: "viewerPlay"; C.ToolTip.text: qsTr("Start or halt playback of whatever the viewer is showing"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: backend.playing ? qsTr("Pause") : qsTr("Play"); enabled: backend.hasVideo || backend.sourceDuration > 0; onClicked: win.layoutTransport("play", 0) }
                C.Label { objectName: "viewerStats"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("%1 · %2 displayed fps · %3 dropped display frames").arg(root.stats.quality).arg(Math.round(Number(root.stats.displayedFps))).arg(root.stats.dropped); font.pixelSize: Ui.Theme.fsLabel; color: root.stats.dropped ? Ui.Theme.warning : Ui.Theme.textSecondary }
                C.Button { implicitHeight: Ui.Theme.hControl + Ui.Theme.s2; C.ToolTip.text: qsTr("Zero the dropped-frame and frame-rate counters"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Reset"); onClicked: backend.resetPlaybackStats() }
            }
            C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Counts frames replaced before the viewer paints them. Rates reset when playback starts; source and timeline use their own In/Out range."); font.pixelSize: Ui.Theme.fsLabel; color: Ui.Theme.textMuted }
            RowLayout {
                C.Button { implicitHeight: Ui.Theme.hControl + Ui.Theme.s2; objectName: "viewerExportFrame"; C.ToolTip.text: qsTr("Save the frame on screen as a full-resolution still image"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Export Current Frame…"); enabled: backend.hasVideo || backend.sourceDuration > 0; onClicked: { root.close(); backend.exportFrameDialog() } }
                C.Button { implicitHeight: Ui.Theme.hControl + Ui.Theme.s2; C.ToolTip.text: qsTr("Return pan to the middle, leaving the zoom level alone"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; text: qsTr("Centre image"); onClicked: { backend.setViewerOption("panX", 0); backend.setViewerOption("panY", 0) } }
            }
            C.Label { text: qsTr("External video monitoring"); font.bold: true }
            C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Dedicated video-output devices are not configured in this version. Use Full Screen Viewer or detach the viewer to a desktop display."); font.pixelSize: Ui.Theme.fsLabel; color: Ui.Theme.textMuted }
        }
    }
}
