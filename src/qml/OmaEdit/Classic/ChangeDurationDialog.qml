import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaEdit.Ui as Ui

// Change Duration: type how long the selected clip should be. Its end moves;
// on the storyline the clips after it follow. A clip can grow only as far as
// its media goes; stills, titles and generators have plenty of room.
Ui.FormDialog {
    id: root
    objectName: "changeDurationDialog"
    width: Math.min(380, parent ? parent.width - Ui.Theme.s5 * 2 : 380)
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Change Duration")
    standardButtons: Dialog.Ok | Dialog.Cancel
    property bool connected: false
    property int index: -1
    property int current: 0
    property int limit: 0
    property int frames: 0
    readonly property real fps: backend.fps > 0 ? backend.fps : 25

    // The selected clip: a connected one if any, else the storyline's.
    function openForSelection() {
        connected = win.selectedConnectedIndex >= 0
        index = connected ? win.selectedConnectedIndex : win.selectedTimelineIndex
        const list = connected ? backend.connected : backend.timeline
        if (index < 0 || index >= list.length)
            return
        current = backend.durationNow(connected, index)
        limit = backend.durationLimit(connected, index)
        frames = current
        open()
    }

    ColumnLayout {
        spacing: Ui.Theme.s2
        width: parent ? parent.width : implicitWidth
        Ui.TimecodeField {
            id: field
            objectName: "durationField"
            Layout.fillWidth: true
            fps: root.fps
            length: true
            frame: root.frames
            tip: qsTr("How long the clip should be, in seconds (2.5) or minutes and seconds (1:30)")
            onEdited: function (f) { root.frames = f }
            onAccepted: root.accept()
        }
        Label {
            objectName: "durationLimit"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Ui.Theme.textMuted
            font.pixelSize: Ui.Theme.fsLabel
            text: root.limit > root.current
                  ? qsTr("Now %1. Its media allows up to %2.").arg(field.format(root.current)).arg(field.format(root.limit))
                  : qsTr("Now %1. It can only get shorter: its media ends at the clip's end.").arg(field.format(root.current))
        }
    }
    onOpened: field.focusInput()
    onAccepted: {
        field.commit()
        if (root.frames >= 1)
            backend.changeDuration(root.connected, root.index, root.frames)
    }
}
