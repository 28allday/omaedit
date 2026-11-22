import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import OmaEdit.Ui

// The right column's scopes: one, two or four ScopeViews with a shared
// scale / channels / intensity / graticule / clamp / pause bar.
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property bool floating: false
    // Whether the panel can pop out into its own window (the Color page's can).
    property bool detachable: true
    signal detachRequested(bool detached)
    property int tick: 0
    property string layout: "dual"          // single | dual | quad
    property var kinds: ["parade", "waveform", "vectorscope", "histogram"]
    property string scopeScale: "ire"
    property string channels: "rgb"
    property real intensity: 1.0
    property bool graticule: true
    property bool clamp: false
    property bool paused: false
    property string bitDepth: "Float"

    signal layoutEdited(string layout)
    signal kindEdited(int slot, string kind)
    signal scaleEdited(string scale)
    signal channelsEdited(string channels)
    signal intensityEdited(real intensity)
    signal graticuleToggled(bool on)
    signal clampToggled(bool on)
    signal pausedToggled(bool on)
    signal bitDepthEdited(string depth)

    color: Theme.panelBg
    readonly property int count: layout === "single" ? 1 : layout === "quad" ? 4 : 2

    Item {
        id: header
        width: parent.width
        height: scopeActions.inBar ? 0 : Theme.hTab
        visible: !scopeActions.inBar
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            visible: root.dockBar === null // a dock bar names the panel
            text: qsTr("Scopes")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        DockBarActions {
            id: scopeActions
            dockBar: root.dockBar
            fallback: header
            spacing: Theme.s1
            SegmentedControl {
                anchors.verticalCenter: parent.verticalCenter
                icons: ["square", "rows-2", "grid-2x2"]
                tip: qsTr("How many scopes are shown at once: one, two or four")
                names: ["single", "dual", "quad"]
                currentIndex: root.layout === "single" ? 0 : root.layout === "quad" ? 2 : 1
                onActivated: function (i) { root.layoutEdited(["single", "dual", "quad"][i]) }
            }
            IconButton { visible: root.detachable; objectName: "scopeDetach"; iconName: root.floating ? "panel-left" : "picture-in-picture-2"; text: root.floating ? qsTr("Dock scopes") : qsTr("Pop out scopes"); onClicked: root.detachRequested(!root.floating) }
            IconButton { objectName: "scopePause"; iconName: root.paused ? "play" : "pause"; checked: root.paused; Accessible.name: root.paused ? qsTr("Resume scopes") : qsTr("Pause scopes"); onClicked: root.pausedToggled(!root.paused) }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }

    Grid {
        id: grid
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        columns: root.count === 4 || (root.count === 2 && root.height < 280 && root.width >= 300) ? 2 : 1
        Repeater {
            model: root.count
            delegate: ScopeView {
                required property int index
                objectName: "scope:" + index
                width: grid.width / grid.columns
                height: grid.height / Math.ceil(root.count / grid.columns)
                kind: root.kinds[index] !== undefined ? root.kinds[index] : "waveform"
                tick: root.tick
                scopeScale: root.scopeScale
                clamp: root.clamp
                live: !root.paused
                onKindEdited: function (k) { root.kindEdited(index, k) }
                onSettingsRequested: settings.open()
            }
        }
    }

    Rectangle {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.hControl + Theme.s2
        color: Theme.panelRaised
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s2
            ComboField {
                anchors.verticalCenter: parent.verticalCenter
                width: 108
                Accessible.name: qsTr("Scope scale")
                tip: qsTr("Signal level units: IRE, full-range RGB code values or normalised float. This does not change the measured precision.")
                model: ["IRE", "8-bit RGB", "10-bit RGB", "12-bit RGB", "Float"]
                currentIndex: ["ire", "8bit", "10bit", "12bit", "float"].indexOf(root.scopeScale)
                onActivated: function (i) { root.scaleEdited(["ire", "8bit", "10bit", "12bit", "float"][i]) }
            }
            Toggle { anchors.verticalCenter: parent.verticalCenter; label: qsTr("0–100%"); tip: qsTr("Limit waveform, parade and histogram to 0–100%. Turn off to show −10–110%. Values beyond the plotted range appear in amber."); checked: root.clamp; onToggled: root.clampToggled(checked) }
        }
    }

    FormDialog {
        width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
        id: settings
        objectName: "scopeSettings"
        title: qsTr("Scope Settings")
        modal: true
        standardButtons: C.Dialog.Close
        Column {
            spacing: Theme.s2
            Layout.fillWidth: true
            SliderField { width: parent.width; label: qsTr("Intensity"); tip: qsTr("How brightly the scope trace is drawn"); from: 0.2; to: 4; stepSize: 0.1; value: root.intensity; onEdited: function (v) { root.intensityEdited(v) } }
            Row {
                spacing: Theme.s2
                Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Channels"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                SegmentedControl {
                    labels: ["RGB", "R", "G", "B", "Y"]
                    tip: qsTr("Waveform and histogram channels. RGB waveform shows luma; the parade always compares R, G and B, and vectorscope always shows chroma.")
                    names: ["rgb", "r", "g", "b", "y"]
                    currentIndex: Math.max(0, ["rgb", "r", "g", "b", "y"].indexOf(root.channels))
                    onActivated: function (i) { root.channelsEdited(["rgb", "r", "g", "b", "y"][i]) }
                }
            }
            Toggle { label: qsTr("Graticule"); tip: qsTr("Draw the reference grid and level markings over the scope"); checked: root.graticule; onToggled: root.graticuleToggled(checked) }
            Row {
                spacing: Theme.s2
                Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Precision"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
                ComboField {
                    width: 90
                    Accessible.name: qsTr("Scope precision")
                    tip: qsTr("Float preserves the incoming signal precision. Other modes quantise RGB before measurement; they do not add precision to the source.")
                    model: ["Float", "8 bit", "10 bit", "12 bit"]
                    currentIndex: Math.max(0, ["Float", "8 bit", "10 bit", "12 bit"].indexOf(root.bitDepth))
                    onActivated: function (i) { root.bitDepthEdited(["Float", "8 bit", "10 bit", "12 bit"][i]) }
                }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("Scopes measure all pixels of the current preview in project output colour space, at up to 25 Hz. Density stays fixed between frames; histogram height uses a fixed logarithmic pixel count. Pause holds the last reading."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        }
    }
}
