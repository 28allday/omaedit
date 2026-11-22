pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Six overlapping luminance zones, with exposure and RGB pushes.
Item {
    id: root

    property var node: ({})
    signal hdrEdited(var hdr)
    signal committed()
    signal keyToggled(string key)

    readonly property var hdr: node.hdr !== undefined ? node.hdr : ({})
    readonly property var zones: [
        { key: "black", title: qsTr("Black"), lo: 0.0, hi: 0.08 }, { key: "dark", title: qsTr("Dark"), lo: 0.08, hi: 0.2 },
        { key: "shadow", title: qsTr("Shadow"), lo: 0.2, hi: 0.4 }, { key: "light", title: qsTr("Light"), lo: 0.4, hi: 0.65 },
        { key: "highlight", title: qsTr("Highlight"), lo: 0.65, hi: 0.85 }, { key: "specular", title: qsTr("Specular"), lo: 0.85, hi: 1.0 }
    ]
    function put(key, value) {
        var m = {}
        for (var k in hdr) m[k] = hdr[k]
        m[key] = value
        root.hdrEdited(m)
    }

    // The six wheels share the width and grow with the panel height, above
    // the info line and the zone graph.
    // On a short panel the note and then the zone graph step aside so the
    // wheels fit whole (a wheel is never shorter than its four value boxes).
    readonly property int graphHeight: 30
    readonly property real usable: width - 2 * Theme.s2
    readonly property int chrome: Theme.fsHeading + Theme.s2 + Theme.s3 + Theme.hControl + Theme.s2
    readonly property int fieldsHeight: 4 * (Theme.hControl - 2) + 3 * 2
    readonly property int noteHeight: Theme.fsLabel + 4 + Theme.s2
    readonly property bool showNote: height - 2 * Theme.s2 - chrome - fieldsHeight >= noteHeight
    readonly property int wheelsTop: 2 * Theme.s2 + (showNote ? noteHeight : 0)
    readonly property bool showGraph: height - wheelsTop - chrome - graphHeight - Theme.s2 >= Math.max(80, fieldsHeight)
    readonly property int ringSize: Math.max(64, Math.min(height - wheelsTop - chrome - (showGraph ? graphHeight + Theme.s2 : 0),
                                                          (usable - 5 * Theme.s3) / 6 - 74, 200))
    readonly property real minimumHeight: 2 * Theme.s2 + chrome + fieldsHeight

    Column {
        id: hdrColumn
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s2
        Text {
            visible: root.showNote
            width: parent.width
            elide: Text.ElideRight
            text: qsTr("Adjust exposure and colour within each luminance zone. Neighbouring zones blend smoothly.")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
        Row {
            spacing: Math.max(Theme.s2, Math.min(Theme.s5 * 2, (root.usable - 6 * (root.ringSize + 74)) / 5))
            Repeater {
                model: root.zones.length
                delegate: ColorWheel {
                    id: wheel
                    required property int index
                    readonly property var zone: root.zones[index]
                    objectName: "hdr:" + zone.key
                    width: root.ringSize + 74
                    maxWheel: root.ringSize
                    title: zone.title
                    resettable: true
                    keyed: ((root.node.keyframes || {})["hdr." + zone.key + ".y"] || []).length > 0
                    onKeyToggled: root.keyToggled("hdr." + zone.key + ".y")
                    value: root.hdr[zone.key] !== undefined ? root.hdr[zone.key] : [0, 0, 0, 0]
                    onEdited: function (v) { root.put(zone.key, v) }
                    onEditingFinished: root.committed()
                    onResetRequested: { root.put(zone.key, [0, 0, 0, 0]); root.committed() }
                }
            }
        }
        // Zone graph: the six luminance bands and where each wheel pushes.
        Rectangle {
            objectName: "hdrZoneGraph"
            visible: root.showGraph
            width: parent.width
            height: root.graphHeight
            color: Theme.meterWell
            border.color: Theme.border
            border.width: Theme.hairline
            Row {
                anchors.fill: parent
                anchors.margins: Theme.hairline
                Repeater {
                    model: root.zones.length
                    delegate: Rectangle {
                        id: band
                        required property int index
                        readonly property var zone: root.zones[index]
                        readonly property real push: root.hdr[zone.key] !== undefined ? root.hdr[zone.key][0] : 0
                        width: parent.width * (zone.hi - zone.lo)
                        height: parent.height
                        color: Qt.rgba(zone.lo + 0.1, zone.lo + 0.1, zone.lo + 0.1, 1)
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Math.max(2, parent.height * Math.abs(band.push)); color: band.push >= 0 ? Theme.accent : Theme.danger; opacity: 0.7 }
                        Text { anchors.centerIn: parent; text: band.zone.title; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: band.zone.lo > 0.5 ? Theme.windowBg : Theme.textSecondary }
                    }
                }
            }
        }
    }
}
