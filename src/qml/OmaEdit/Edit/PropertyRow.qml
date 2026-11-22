pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// One Inspector line: label, up to two value fields, and a keyframe diamond.
// The diamond is always present on an animatable property — a property whose
// keyframe affordance appears only on hover is a property nobody animates.
Item {
    id: root

    property string label: ""
    property bool animatable: true
    property bool keyed: false
    property bool hasAnimation: false
    property string keyObjectName: ""
    // Up to two fields, as "X 960.0  Y 540.0".
    property var fields: []          // [{ prefix, value, suffix, decimals, fps }]; fps makes it a length in time

    signal keyToggled()
    signal fieldEdited(int index, real value)
    signal editStarted()
    signal editFinished()

    implicitHeight: Theme.hControl + Theme.s1
    implicitWidth: 260

    // Anchored against the field group rather than given a fixed width, so a
    // long label elides instead of colliding with its own value at narrow
    // Inspector widths.
    Text {
        id: caption
        text: root.label
        anchors.left: parent.left
        anchors.right: values.left
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
        elide: Text.ElideRight
    }

    // Anchored right rather than laid out right-to-left: RightToLeft would
    // also reverse the fields, and "Y then X" is a field somebody types the
    // wrong number into.
    Row {
        id: values
        anchors.right: key.left
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        Repeater {
            // Updated values must not destroy a field that owns a pointer grab.
            model: root.fields.length
            delegate: Row {
                id: field
                required property int index
                readonly property var modelData: root.fields[index]
                spacing: Theme.s1
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: field.modelData.prefix !== undefined ? field.modelData.prefix : ""
                    visible: text !== ""
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                    anchors.verticalCenter: parent.verticalCenter
                }

                ValueField {
                    id: numericField
                    objectName: "propertyValue" + field.index
                    width: 62
                    tip: qsTr("Value for %1; drag across it or type to change it").arg(root.label)
                    label: root.label
                    value: field.modelData.value
                    decimals: field.modelData.decimals !== undefined ? field.modelData.decimals : 1
                    suffix: field.modelData.suffix !== undefined ? field.modelData.suffix : ""
                    fps: field.modelData.fps !== undefined ? field.modelData.fps : 0
                    anchors.verticalCenter: parent.verticalCenter
                    onEdited: function (v) { root.fieldEdited(field.index, v) }
                    onEditStarted: root.editStarted()
                    onEditFinished: root.editFinished()
                    // ValueField also changes its own value while scrubbing.
                    // Apply later worker echoes and undo without replacing it.
                    Connections {
                        target: field
                        function onModelDataChanged() { numericField.value = field.modelData.value }
                    }
                }
            }
        }
    }

    IconButton {
        id: key
        objectName: root.keyObjectName
        iconName: "diamond"
        text: root.keyed ? qsTr("Remove %1 keyframe at playhead").arg(root.label)
                         : qsTr("Add %1 keyframe at playhead").arg(root.label)
        visible: root.animatable
        checked: root.keyed
        iconColor: root.keyed ? Theme.accent : root.hasAnimation ? Theme.textPrimary : Theme.textMuted
        implicitWidth: Theme.szIcon + Theme.s1
        implicitHeight: Theme.szIcon + Theme.s1
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        onClicked: root.keyToggled()
    }
}
