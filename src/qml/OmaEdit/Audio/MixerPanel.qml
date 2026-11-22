pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Edit as Edit
import OmaEdit.Ui

// MIXER / EFFECTS / ADR / LOUDNESS tabs. The mixer tab is a horizontally
// scrolling bank of channel strips — one per audio track, then the buses,
// then the master — with a compact/expanded toggle.
Rectangle {
    id: root

    property var tracks: []
    property var trackLevels: []
    property var routingLevels: []
    property var master: ({})
    property real masterLeft: -100
    property real masterRight: -100
    property bool masterMuted: false
    property var buses: []            // [{id, name, mix}]
    property var catalogue: []
    property var inputs: []
    property var outputs: []
    property int selectedTrack: -1    // -2 master, bus = -10 - busIndex
    property bool compact: false
    property int currentTab: 0
    property alias tabsItem: header

    signal trackSelected(int track)
    signal trackMixEdited(int track, string key, var value)
    signal mixFinished(int track, string key)
    signal mixStarted(int track, string key)
    signal trackFlagEdited(int track, string flag, bool on)
    signal masterEdited(string key, var value)
    signal masterMuteToggled(bool on)
    signal busEdited(string busId, string key, var value)
    signal insertOpened(int track, int fxIndex)
    signal insertAddRequested(int track)

    color: Theme.panelBg

    readonly property var strips: {
        var out = []
        for (var i = 0; i < tracks.length; ++i)
            if (tracks[i].kind === "audio")
                out.push({ kind: "track", index: tracks[i].index, name: tracks[i].name, mix: tracks[i].mix, muted: tracks[i].muted, solo: tracks[i].solo, locked: tracks[i].locked })
        for (var b = 0; b < buses.length; ++b)
            out.push({ kind: "bus", index: -10 - b, busId: buses[b].id, name: buses[b].name, mix: buses[b].mix !== undefined ? buses[b].mix : ({}), muted: buses[b].mix !== undefined && buses[b].mix.muted === true, solo: false })
        out.push({ kind: "master", index: -2, name: qsTr("Master"), mix: master, muted: masterMuted, solo: false })
        return out
    }
    readonly property var busOptions: buses.map(function (b) { return { id: b.id, name: b.name } })

    Edit.PanelHeader {
        id: header
        width: parent.width
        tabs: [qsTr("Mixer"), qsTr("Effects"), qsTr("ADR"), qsTr("Loudness")]
        tabPrefix: "mixerTab:"
        currentIndex: root.currentTab
        onActivated: index => root.currentTab = index
        Connections { target: root; function onCurrentTabChanged() { header.currentIndex = root.currentTab } }
        IconButton {
            objectName: "mixerStripSize"
            visible: root.currentTab === 0
            iconName: root.compact ? "maximize" : "minimize"
            text: root.compact ? qsTr("Expand mixer strips") : qsTr("Compact mixer strips")
            onClicked: root.compact = !root.compact
        }
    }

    // The mixer bank.
    Flickable {
        id: bank
        objectName: "mixerBank"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s1
        visible: root.currentTab === 0
        contentWidth: row.width
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // More strips than fit: the bar stays in view to say so (at 1280 wide
        // the fourth strip was cut off with nothing to show there was more).
        C.ScrollBar.horizontal: ScrollBar {
            objectName: "mixerScrollX"
            policy: bank.contentWidth > bank.width + 0.5 ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded
        }
        Row {
            id: row
            height: bank.height - Theme.s3
            spacing: Theme.s1
            Repeater {
                // A COUNT model: strips re-bind in place on every fader move
                // instead of being rebuilt under the pointer.
                model: root.strips.length
                delegate: ChannelStrip {
                    id: strip
                    required property int index
                    readonly property var entry: root.strips[index] !== undefined ? root.strips[index] : ({ kind: "track", index: -99, name: "", mix: {} })
                    objectName: "strip:" + entry.index
                    height: row.height
                    strip: entry
                    compact: root.compact
                    catalogue: root.catalogue
                    inputs: root.inputs
                    outputs: root.outputs
                    buses: root.busOptions
                    selected: root.selectedTrack === entry.index
                    levels: entry.kind === "master" ? root.routingLevels.find(function(m){return m.id === "master"}) || ({ out: [root.masterLeft, root.masterRight] })
                          : entry.kind === "track" && root.trackLevels[entry.index] !== undefined ? root.trackLevels[entry.index]
                          : entry.kind === "bus" ? root.routingLevels.find(function (m) { return m.id === strip.entry.busId }) || {} : ({})
                    onSelectedRequested: root.trackSelected(entry.index)
                    onMixFinished: function (key) { root.mixFinished(entry.index, key) }
                    onMixStarted: function (key) { root.mixStarted(entry.index, key) }
                    onMixEdited: function (key, value) {
                        if (entry.kind === "master") root.masterEdited(key, value)
                        else if (entry.kind === "bus") root.busEdited(entry.busId, key, value)
                        else root.trackMixEdited(entry.index, key, value)
                    }
                    onFlagToggled: function (flag, on) {
                        if (entry.kind === "master") { if (flag === "muted") root.masterMuteToggled(on) }
                        else if (entry.kind === "bus") root.busEdited(entry.busId, flag, on)
                        else root.trackFlagEdited(entry.index, flag, on)
                    }
                    onInsertOpened: function (i) { root.insertOpened(entry.index, i) }
                    onInsertBypassToggled: function (i, b) {
                        var fx = (entry.mix.fx !== undefined ? entry.mix.fx : []).map(function (e) { var c = {}; for (var k in e) c[k] = e[k]; return c })
                        if (i < fx.length) { fx[i].bypass = b; strip.mixEdited("fx", fx) }
                    }
                    onInsertAddRequested: root.insertAddRequested(entry.index)
                }
            }
        }
        EmptyState {
            anchors.centerIn: parent
            visible: root.strips.length <= 1
            iconName: "sliders-horizontal"
            title: qsTr("No audio tracks to mix")
            description: qsTr("Drop a sound on the timeline or add a track.")
        }
    }
}
