pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "maskProviderDialog"
    title: qsTr("Face / Object Mask (AI Provider)")
    width: Math.min(640,parent?parent.width-32:640)
    property var provider: ({})
    property var job: ({})
    property bool editable: false
    readonly property bool busy: job.percent!==undefined
    signal configureRequested(var settings)
    signal generateRequested(string kind,string prompt)
    signal cancelRequested()
    standardButtons: C.Dialog.Close
    onOpened: {command.text=provider.command || "";argumentField.text=provider.arguments || "--request {request} --output {output}"}
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:qsTr("Generate a still matte for the current frame with your chosen provider. It is attached as an editable external matte, with one Undo. The provider controls its own model and any network use.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textSecondary }
        C.TextField { id:command;objectName:"maskProviderCommand";Layout.fillWidth:true;placeholderText:qsTr("Provider executable (absolute path)");enabled:!root.busy;hoverEnabled:true;C.ToolTip.text:qsTr("Absolute path of the provider program that generates the matte");C.ToolTip.visible:hovered&&!activeFocus;C.ToolTip.delay:450 }
        C.TextField { id:argumentField;objectName:"maskProviderArguments";Layout.fillWidth:true;placeholderText:"--request {request} --output {output}";enabled:!root.busy;hoverEnabled:true;C.ToolTip.text:qsTr("Arguments passed to the provider; {request} and {output} are substituted");C.ToolTip.visible:hovered&&!activeFocus;C.ToolTip.delay:450 }
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:qsTr("Provider protocol: read the JSON at {request}, write a greyscale PNG to {output} at the supplied dimensions. {input} is the source PNG. Arguments are passed directly without a shell.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsLabel;color:Theme.textMuted }
        ComboField { id:kind;objectName:"maskKind";Layout.fillWidth:true;model:[qsTr("Face"),qsTr("Object")];enabled:!root.busy;tip:qsTr("Whether the provider is asked for a face or an object matte") }
        C.TextField { id:prompt;objectName:"maskPrompt";Layout.fillWidth:true;placeholderText:qsTr("Describe the face or object to select");maximumLength:4096;enabled:!root.busy;hoverEnabled:true;C.ToolTip.text:qsTr("Text description the provider uses to find the subject to matte");C.ToolTip.visible:hovered&&!activeFocus;C.ToolTip.delay:450 }
        Text { Layout.fillWidth:true;wrapMode:Text.WordWrap;text:root.busy?(root.job.message || qsTr("Working…")):qsTr("This matte is static across the clip. Review it in Highlight view before using it on moving subjects.");font.family:Theme.fontFamily;font.pixelSize:Theme.fsControl;color:Theme.textSecondary }
        RowLayout {
            ToolButton { objectName:"saveMaskProvider";showLabel:true;text:qsTr("Save Provider");enabled:!root.busy;onClicked:root.configureRequested({command:command.text,arguments:argumentField.text}) }
            Item { Layout.fillWidth:true }
            ToolButton { objectName:"cancelMask";showLabel:true;text:qsTr("Cancel Request");enabled:root.busy;onClicked:root.cancelRequested() }
            ToolButton { objectName:"generateMask";showLabel:true;iconName:"wand-sparkles";text:qsTr("Generate Mask (AI)");enabled:root.editable&&!root.busy&&command.text.trim()!=="";onClicked:{root.configureRequested({command:command.text,arguments:argumentField.text});root.generateRequested(kind.currentIndex===0?"face":"object",prompt.text)} }
        }
    }
}
