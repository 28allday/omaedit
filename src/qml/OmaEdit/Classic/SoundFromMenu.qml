import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import omaedit

// "Sound From ▸" for a multicam clip: where its sound comes from, chosen
// after the clip is made. "Follows the picture", then every source lined up
// when it was made that has sound (each angle, and a recording that is no
// angle), the one in use marked. `rows` come from the backend
// (multicamSoundSources for a placed clip, libraryMulticamSoundSources for
// the clip in the library); the menu's owner makes the change on `chosen`.
AppMenu {
    id: soundMenu
    property var rows: []   // each {source, text, current}
    signal chosen(int source)
    title: qsTr("Sound From")
    enabled: rows.length > 0
    Instantiator {
        model: soundMenu.rows.length
        delegate: KeyMenuItem {
            required property int index
            text: soundMenu.rows[index].text
            kbd: soundMenu.rows[index].current ? "●" : ""
            onTriggered: soundMenu.chosen(soundMenu.rows[index].source)
        }
        onObjectAdded: function (i, object) { soundMenu.insertItem(i, object) }
        onObjectRemoved: function (i, object) { soundMenu.removeItem(object) }
    }
}
