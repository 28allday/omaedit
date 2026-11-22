pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "gradeSharingDialog"
    title: qsTr("Shared Grades and Remote Versions")
    width: Math.min(590,parent?parent.width-32:590)
    property var groups: []
    property var link: ({})
    property int selectedCount: 1
    property bool editable: false
    signal shareRequested(string action,string name)
    standardButtons: C.Dialog.Close
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:root.link.id ? qsTr("Linked to %1 (%2). Edits and named versions update every linked clip, including members on locked tracks.").arg(root.link.name).arg(root.link.kind) : qsTr("This clip has independent local grades.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textPrimary }
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:qsTr("A group shares one look and its versions across your selected clips. Joining an existing name adopts that group's look. Each action can be undone.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textSecondary }
        RowLayout {
            Layout.fillWidth:true
            C.TextField { id:nameField;objectName:"colorGroupName";Layout.fillWidth:true;placeholderText:qsTr("New or existing group name");maximumLength:120;hoverEnabled:true;C.ToolTip.text:qsTr("Name of the shared grade group to create or join");C.ToolTip.visible:hovered&&!activeFocus;C.ToolTip.delay:450 }
            ToolButton { objectName:"joinColorGroup";showLabel:true;text:qsTr("Link %1 clips").arg(root.selectedCount);enabled:root.editable&&nameField.text.trim()!=="";onClicked:root.shareRequested("group",nameField.text) }
        }
        ComboField { Layout.fillWidth:true;model:[qsTr("Existing groups…")].concat(root.groups.filter(function(g){return g.kind==="group"}).map(function(g){return g.name}));tip:qsTr("Pick an existing group name to fill the field beside it");onActivated:function(i){if(i>0)nameField.text=model[i]} }
        Rectangle { Layout.fillWidth:true; Layout.preferredHeight:Theme.hairline;color:Theme.border }
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:qsTr("Remote versions link every current use of this source across all project sequences. They stay in this project. Use this again after adding new instances.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textSecondary }
        ToolButton { objectName:"useRemoteGrades";showLabel:true;iconName:"link";text:qsTr("Use Remote Grades for This Source");enabled:root.editable;onClicked:root.shareRequested("remote","") }
        ToolButton { objectName:"detachSharedGrade";showLabel:true;text:qsTr("Make This Clip Local");enabled:root.editable&&!!root.link.id;onClicked:root.shareRequested("detach","") }
    }
}
