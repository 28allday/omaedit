pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

Column {
    id: root
    property var clipData: null
    property real amount: 15
    property real duration: .5
    readonly property bool isTitle: clipData && clipData.kind === "title"
    readonly property int frames: clipData && clipData.frames ? clipData.frames : 0
    readonly property var presets: (isTitle ? [
        {key: "title-fade", label: qsTr("Title Fade")},
        {key: "title-slide-left", label: qsTr("Title Slide Left")},
        {key: "title-slide-right", label: qsTr("Title Slide Right")},
        {key: "title-rise", label: qsTr("Title Rise")},
        {key: "title-scale", label: qsTr("Title Scale")}
    ] : []).concat([
        {key: "zoom-in", label: qsTr("Zoom In")}, {key: "zoom-out", label: qsTr("Zoom Out")},
        {key: "pan-left", label: qsTr("Pan Left")}, {key: "pan-right", label: qsTr("Pan Right")},
        {key: "pan-up", label: qsTr("Pan Up")}, {key: "pan-down", label: qsTr("Pan Down")}
    ])
    readonly property string preset: (presets[presetBox.currentIndex] || presets[0]).key
    readonly property bool titleMotion: preset.indexOf("title-") === 0
    signal applyRequested(string preset, var options)
    signal frameRequested(int frame)
    onIsTitleChanged: presetBox.currentIndex = 0
    spacing: Theme.s2

    ComboField {
        id: presetBox
        objectName: "motionPreset"
        width: parent.width
        caption: qsTr("Motion")
        tip: qsTr("Choose pan, zoom, or a title entrance and exit")
        model: root.presets.map(function(p) { return p.label })
        currentIndex: 0
    }
    SliderField {
        objectName: "motionAmount"
        width: parent.width
        visible: !root.titleMotion
        label: qsTr("Amount")
        tip: qsTr("How far to zoom or pan over the clip")
        from: 1; to: 100; stepSize: 1; decimals: 0; suffix: "%"
        value: root.amount
        onEditingFinished: function(v) { root.amount = v }
    }
    ComboField {
        id: easing
        objectName: "motionEasing"
        width: parent.width
        visible: !root.titleMotion
        caption: qsTr("Easing")
        tip: qsTr("Smooth eases the start and end; Linear keeps a steady speed")
        model: [qsTr("Smooth"), qsTr("Linear")]
    }
    SliderField {
        objectName: "motionDuration"
        width: parent.width
        visible: root.titleMotion
        label: qsTr("In / out")
        tip: qsTr("Duration of each title entrance and exit, shortened to fit a short clip")
        from: .04; to: 10; stepSize: .04; decimals: 2; suffix: qsTr(" s")
        value: root.duration
        onEditingFinished: function(v) { root.duration = v }
    }
    Flow {
        width: parent.width
        spacing: Theme.s1
        ToolButton {
            objectName: "applyMotionPreset"
            text: qsTr("Apply Motion")
            tip: qsTr("Replace position and size animation; title presets also replace opacity animation")
            iconName: "wand-sparkles"; showLabel: true
            enabled: root.frames >= (root.titleMotion ? 3 : 2)
            onClicked: root.applyRequested(root.preset, root.titleMotion
                ? {duration: root.duration} : {amount: root.amount, interpolation: easing.currentIndex === 1 ? "linear" : "smooth"})
        }
        ToolButton {
            objectName: "motionStart"
            text: qsTr("Start")
            tip: qsTr("Go to the clip's first frame to adjust its starting position and size")
            enabled: root.frames > 0
            onClicked: root.frameRequested(0)
        }
        ToolButton {
            objectName: "motionEnd"
            text: qsTr("End")
            tip: qsTr("Go to the clip's last frame to adjust its final position and size")
            enabled: root.frames > 0
            onClicked: root.frameRequested(root.frames - 1)
        }
    }
    Text {
        width: parent.width
        text: qsTr("Apply across the clip, then use Start and End to refine framing in the viewer. Each preset is one undo step.")
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textMuted
    }
}
