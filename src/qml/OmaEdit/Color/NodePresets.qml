pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "nodePresetsDialog"
    title: qsTr("Layer Presets")
    width: Math.min(540,parent?parent.width-32:540)
    property var presets: []
    property bool editable: false
    property bool hasNode: false
    property string selected: ""
    signal saveRequested(string name)
    signal applyRequested(string id)
    signal removeRequested(string id)
    signal assetRequested(string action,string id)
    readonly property bool hasSelection: presets.some(function(p){return p.id===root.selected})
    standardButtons: C.Dialog.Close
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:qsTr("Save a correction or layer, then insert an independent copy on another clip.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textSecondary }
        RowLayout {
            Layout.fillWidth:true
            C.TextField { id:nameField;objectName:"nodePresetName";Layout.fillWidth:true;placeholderText:qsTr("Preset name");maximumLength:120;hoverEnabled:true;C.ToolTip.text:qsTr("Name the saved layer is listed under in this preset list");C.ToolTip.visible:hovered&&!activeFocus;C.ToolTip.delay:450 }
            ToolButton { objectName:"saveNodePreset";text:qsTr("Save Layer");enabled:root.hasNode&&nameField.text.trim()!=="";onClicked:{root.saveRequested(nameField.text);nameField.clear()} }
        }
        ListView {
            id:list;objectName:"nodePresetList";Layout.fillWidth:true;Layout.preferredHeight:220;clip:true;model:root.presets
            C.ScrollBar.vertical: ScrollBar { atRest: true }
            delegate: Rectangle {
                required property var modelData
                width:list.width;height:Theme.hRow+8;color:root.selected===modelData.id?Theme.controlBg:Theme.panelBg
                Text { anchors.fill:parent;anchors.margins:Theme.s2;text:parent.modelData.name;elide:Text.ElideRight;font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textPrimary }
                TapHandler { onTapped:root.selected=parent.modelData.id }
            }
        }
        RowLayout {
            Layout.fillWidth:true
            ToolButton { text:qsTr("Import…");onClicked:root.assetRequested("importPreset","") }
            ToolButton { text:qsTr("Export…");enabled:root.hasSelection;onClicked:root.assetRequested("exportPreset",root.selected) }
            ToolButton { text:qsTr("Delete");enabled:root.hasSelection;onClicked:root.removeRequested(root.selected) }
            Item { Layout.fillWidth:true }
            ToolButton { objectName:"applyNodePreset";text:qsTr("Insert Layer");enabled:root.editable&&root.hasSelection;onClicked:{root.applyRequested(root.selected);root.accept()} }
        }
    }
}
