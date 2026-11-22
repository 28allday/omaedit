import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

AppMenu {
    id: rulerContextMenu // the skim ruler (right-click parks first)
    KeyMenuItem {text:qsTr("Show frame numbers");checkable:true;checked:win.rulerFrames;onTriggered:win.rulerFrames=!win.rulerFrames}
    MenuSeparator {}
    KeyMenuItem {
        text: win.clipMarkers.timelineMarkerHere() !== null ? qsTr("Remove Marker")
                                              : qsTr("Add Marker")
        kbd: "M"
        onTriggered: {
            backend.viewTimeline()
            backend.toggleMarker()
        }
    }
    KeyMenuItem {
        text: qsTr("Modify Marker…")
        kbd: "Shift+M"
        enabled: win.clipMarkers.timelineMarkerHere() !== null
        onTriggered: markerDialog.openFor(win.clipMarkers.timelineMarkerHere())
    }
    MenuSeparator {}
    KeyMenuItem {
        text: qsTr("Insert Gap")
        kbd: "Alt+W"
        onTriggered: backend.insertGapAtPlayhead()
    }
    KeyMenuItem {
        text: qsTr("Blade at Playhead")
        commandId: "edit.blade"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
    KeyMenuItem {
        text: qsTr("Blade All at Playhead")
        commandId: "edit.blade-all"
        kbd: Ui.Commands.hint(commandId)
        enabled: registeredCommand && registeredCommand.available
        onTriggered: Ui.Commands.trigger(commandId)
    }
}
