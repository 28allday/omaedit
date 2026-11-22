import QtQuick
import OmaEdit.Ui as Ui
import QtQuick.Layouts
import omaedit
import OmaEdit.Edit as Edit

// The Edit-page toolbar above the timeline, bound to the engine. This is the
// seam: the toolbar knows nothing of the backend or the window.
Edit.EditToolbar {
    Layout.fillWidth: true
    actionMenus: timelineActions
    TimelineActionMenus { id: timelineActions }

    tool: win.tool
    zoom: win.navigation.zoomLevel
    canZoom: backend.hasVideo && backend.duration > 1
    snapping: win.snapEnabled
    linked: win.linkedSelection
    beatMap: win.showBeatMap
    canBeatMap: win.hasBeatLines
    indexVisible: win.showTimelineIndex
    effectsVisible: classicLayout.browserTab === 1
    canExport: backend.hasVideo
    exporting: backend.exporting
    exportPercent: backend.exportPercent

    onToolSelected: function(name) { Ui.Commands.trigger("tool."+name) }
    onZoomEdited: function (level) { win.navigation.setZoomLevel(level) }
    onFitRequested: Ui.Commands.trigger("view.zoom-fit")
    onSnappingToggled: function(on) { Ui.Commands.trigger("view.snapping") }
    onLinkedToggled: function(on) { Ui.Commands.trigger("edit.linked-selection") }
    onBeatMapToggled: function(on) { Ui.Commands.trigger("view.beat-map") }
    onSpeedRequested: Ui.Commands.trigger("edit.speed-duration")
    onMarkerRequested: Ui.Commands.trigger("marker.toggle")
    onTitleRequested: Ui.Commands.trigger("edit.title")
    onIndexToggled: function(on) { Ui.Commands.trigger("view.timeline-index") }
    onEffectsToggled: function(on) { Ui.Commands.trigger("view.effects") }
    onExportRequested: Ui.Commands.trigger("deliver.export")
}
