import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import omaedit

// "Switch Angle ▸" for a multicam clip's context menu: one item per angle,
// the shown one ticked. The whole clip switches (audio too unless the
// multicam viewer's audio option says otherwise).
AppMenu {
    id: angleMenu
    property var entry: null   // the timeline/connected entry
    property int track: 0
    property int index: -1
    readonly property bool isMulticam: entry !== null && entry !== undefined
                                       && entry.mcam !== undefined && entry.mcam !== ""
    readonly property var angles: isMulticam ? JSON.parse(entry.mcam).angles : []
    title: isMulticam ? qsTr("Switch Angle (") + JSON.parse(entry.mcam).name + ")" : qsTr("Switch Angle")
    enabled: isMulticam
    Instantiator {
        model: angleMenu.angles.length
        delegate: KeyMenuItem {
            required property int index
            text: (index + 1) + "  " + angleMenu.angles[index].name
            kbd: angleMenu.isMulticam && angleMenu.entry.mcamAngle === index ? "●" : ""
            onTriggered: backend.switchAngle(angleMenu.track, angleMenu.index, index)
        }
        onObjectAdded: function (i, object) { angleMenu.insertItem(i, object) }
        onObjectRemoved: function (i, object) { angleMenu.removeItem(object) }
    }
}
