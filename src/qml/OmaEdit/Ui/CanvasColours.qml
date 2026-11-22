pragma Singleton
import QtQuick

// Colours the viewer can show around the picture. The picture's own frame
// stays black inside, so its edges show against any of these but Black.
// The viewer toolbar and Viewer and Playback offer the same list.
QtObject {
    readonly property var presets: [
        { name: qsTr("Black"), colour: "#000000" },
        { name: qsTr("Dark grey"), colour: "#333333" },
        { name: qsTr("Mid grey"), colour: "#808080" },
        { name: qsTr("Light grey"), colour: "#c0c0c0" },
        { name: qsTr("White"), colour: "#ffffff" }
    ]
    // The preset a stored colour matches, or -1 for a custom colour.
    function indexOf(colour) {
        const wanted = String(colour).toLowerCase()
        for (let i = 0; i < presets.length; ++i)
            if (presets[i].colour === wanted) return i
        return -1
    }
}
