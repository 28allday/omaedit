import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

AppMenu {
    id: libraryContextMenu // a library row (right-click selects it)
    objectName: "libraryContextMenu"
    KeyMenuItem {
        text: qsTr("View in Source")
        onTriggered: backend.previewClip(win.selectedLibraryIndex)
    }
    MenuSeparator {}
    AppMenu {
        title: qsTr("Add to Timeline")
        KeyMenuItem {
            text: qsTr("Append to Timeline")
            kbd: "E"
            onTriggered: backend.appendClipAt(win.selectedLibraryIndex)
        }
        KeyMenuItem {
            text: qsTr("Insert at Playhead")
            kbd: "W"
            enabled: backend.hasVideo
            onTriggered: backend.insertClipAt(win.selectedLibraryIndex)
        }
        KeyMenuItem {
            text: qsTr("Overwrite at Playhead")
            kbd: "D"
            enabled: backend.hasVideo
            onTriggered: backend.overwriteClipAt(win.selectedLibraryIndex)
        }
        KeyMenuItem {
            text: qsTr("Place on Top")
            kbd: "Q"
            enabled: backend.hasVideo
            onTriggered: backend.connectClipAt(win.selectedLibraryIndex)
        }
        KeyMenuItem {
            text: qsTr("Place Audio on Top")
            kbd: "Shift+Q"
            enabled: backend.hasVideo
            onTriggered: backend.connectAudioClipAt(win.selectedLibraryIndex)
        }
    }
    MenuSeparator {}
    KeyMenuItem {
        text: qsTr("New Multicam Clip…")
        enabled: backend.library.length >= 2
        onTriggered: win.openMulticamDialog([win.selectedLibraryIndex])
    }
    KeyMenuItem {
        text: qsTr("Sync Clips…")
        enabled: backend.library.length >= 2
        onTriggered: win.openSyncDialog([win.selectedLibraryIndex])
    }
    // A multicam clip's sound source, for when it is placed from now on.
    SoundFromMenu {
        objectName: "librarySoundFromMenu"
        // (backend.library named so the rows follow a change to the item.)
        rows: libraryContextMenu.visible && backend.library.length > win.selectedLibraryIndex && win.selectedLibraryIndex >= 0
              ? backend.libraryMulticamSoundSources(win.selectedLibraryIndex) : []
        showInParent: rows.length > 0
        onChosen: function (source) { backend.setLibraryMulticamSound(win.selectedLibraryIndex, source) }
    }
    MenuSeparator {}
    KeyMenuItem {
        objectName: "libraryMapBeats"
        readonly property var entry:
            win.selectedLibraryIndex >= 0
            && win.selectedLibraryIndex < backend.library.length
            ? backend.library[win.selectedLibraryIndex] : null
        text: qsTr("Map Beats")
        enabled: entry !== null && entry.multicam !== true && entry.hasAudio !== false && entry.offline !== true
        onTriggered: backend.mapBeats(win.selectedLibraryIndex)
    }
    KeyMenuItem {
        objectName: "libraryClearBeatMap"
        readonly property var entry:
            win.selectedLibraryIndex >= 0
            && win.selectedLibraryIndex < backend.library.length
            ? backend.library[win.selectedLibraryIndex] : null
        text: qsTr("Clear Beat Map")
        enabled: entry !== null && entry.beatMap !== undefined
        onTriggered: backend.clearBeatMap(win.selectedLibraryIndex)
    }
    KeyMenuItem {
        readonly property var entry:
            win.selectedLibraryIndex >= 0
            && win.selectedLibraryIndex < backend.library.length
            ? backend.library[win.selectedLibraryIndex] : null
        text: qsTr("Clear Range")
        kbd: "Alt+X"
        enabled: entry !== null && entry.rangeIn !== undefined
                 && (entry.rangeIn >= 0 || entry.rangeOut >= 0)
        onTriggered: backend.clearRange()
    }
}
