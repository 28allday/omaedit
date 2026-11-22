import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The node's LUT: a searchable list of every .cube the app knows (grouped
// by folder), a folder button, and the intensity slider under it.
Column {
    id: root

    property var luts: []                  // [{path, name, folder}]
    property var lut: ({ path: "", intensity: 1 })
    signal lutEdited(var lut)
    signal committed()
    signal folderRequested()
    signal browseRequested()

    spacing: Theme.s1
    readonly property var paths: [""].concat(luts.map(function (l) { return l.path }))
    function put(key, value) {
        var m = {}
        for (var k in lut) m[k] = lut[k]
        m[key] = value
        root.lutEdited(m)
    }

    Row {
        width: parent.width
        spacing: Theme.s1
        Text { width: 60; anchors.verticalCenter: parent.verticalCenter; text: qsTr("LUT"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
        C.Button {
            objectName: "lutCombo"
            width: parent.width - 60 - Theme.szIconHit - Theme.s1 * 2
            anchors.verticalCenter: parent.verticalCenter
            implicitHeight: Theme.hControl
            text: root.lut.path ? root.lut.path.split("/").pop() : qsTr("None")
            contentItem: Text { text: parent.text; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsControl; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: Theme.controlBg; radius: Theme.rControl; border.width: Theme.hairline; border.color: Theme.border }
            C.ToolTip.text: qsTr("Choose the .cube or .3dl LUT this layer applies")
            C.ToolTip.visible: hovered
            C.ToolTip.delay: 450
            onClicked: root.browseRequested()
        }
        IconButton { anchors.verticalCenter: parent.verticalCenter; iconName: "folder"; Accessible.name: qsTr("LUT folder"); onClicked: root.folderRequested() }
    }
    SliderField {
        objectName: "lutIntensity"
        liveEdit: true
        width: parent.width
        label: qsTr("Intensity")
        labelWidth: 60
        from: 0; to: 100; stepSize: 1
        decimals: 0
        suffix: "%"
        enabled: root.lut.path !== undefined && root.lut.path !== ""
        value: (root.lut.intensity !== undefined ? root.lut.intensity : 1) * 100
        tip: qsTr("How strongly the LUT is blended with the ungraded image")
        onEdited: function (v) { root.put("intensity", v / 100) }
        onEditingFinished: root.committed()
    }
}
