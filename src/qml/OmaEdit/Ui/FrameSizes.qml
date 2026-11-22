pragma Singleton
import QtQuick

// Standard frame sizes a timeline can be set to, all with square pixels.
// New Timeline and Timeline Settings offer the same list.
QtObject {
    readonly property var sizes: [
        { name: qsTr("HD 1080p 16:9"), w: 1920, h: 1080 },
        { name: qsTr("HD 720p 16:9"), w: 1280, h: 720 },
        { name: qsTr("QHD 1440p 16:9"), w: 2560, h: 1440 },
        { name: qsTr("UHD 4K 16:9"), w: 3840, h: 2160 },
        { name: qsTr("UHD 8K 16:9"), w: 7680, h: 4320 },
        { name: qsTr("HD Widescreen 2.39:1"), w: 1920, h: 804 },
        { name: qsTr("UHD Widescreen 2.39:1"), w: 3840, h: 1608 },
        { name: qsTr("DCI 2K Full"), w: 2048, h: 1080 },
        { name: qsTr("DCI 2K Flat 1.85:1"), w: 1998, h: 1080 },
        { name: qsTr("DCI 2K Scope 2.39:1"), w: 2048, h: 858 },
        { name: qsTr("DCI 4K Full"), w: 4096, h: 2160 },
        { name: qsTr("DCI 4K Flat 1.85:1"), w: 3996, h: 2160 },
        { name: qsTr("DCI 4K Scope 2.39:1"), w: 4096, h: 1716 },
        { name: qsTr("Vertical HD 9:16"), w: 1080, h: 1920 },
        { name: qsTr("Vertical 720p 9:16"), w: 720, h: 1280 },
        { name: qsTr("Vertical 4K 9:16"), w: 2160, h: 3840 },
        { name: qsTr("Portrait 4:5"), w: 1080, h: 1350 },
        { name: qsTr("Square 1:1"), w: 1080, h: 1080 }
    ]
    function label(size) { return size.name + " · " + size.w + " × " + size.h }
}
