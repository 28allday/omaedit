import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui

Ui.FormDialog {
    id: root
    objectName: "sequenceCreationDialog"
    title: qsTr("New Timeline")
    width: Math.min(660,parent ? parent.width-Ui.Theme.s5*2 : 660)
    standardButtons: C.Dialog.NoButton
    closePolicy: busy ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property var context: ({})
    property var draft: ({})
    property bool awaiting: false
    property int pending: 0
    readonly property bool busy: awaiting || pending>0
    property string problem: ""
    property string error: ""
    property bool completed: false
    onClosed: if (!completed) backend.cancelTimelinePlacement()
    readonly property var formats: [{label:qsTr("Match first clip"),w:0,h:0}]
        .concat(Ui.FrameSizes.sizes.map(function(f){return {label:Ui.FrameSizes.label(f),w:f.w,h:f.h}}))
        .concat([{label:qsTr("Custom dimensions"),w:-1,h:-1}])
    readonly property var rates: [
        {label:"23.976",n:24000,d:1001},{label:"24",n:24,d:1},{label:"25",n:25,d:1},
        {label:"29.97",n:30000,d:1001},{label:"30",n:30,d:1},{label:"50",n:50,d:1},
        {label:"59.94",n:60000,d:1001},{label:"60",n:60,d:1},{label:"120",n:120,d:1}]
    function set(key,value) { var s=Object.assign({},draft); s[key]=value; draft=s }
    function refreshProblem() { problem=backend.sequenceCreationProblem(context,draft.title || "",draft) }
    function openFor() {
        context=backend.creationContext(); awaiting=false; pending=0; error=""; completed=false
        var settings=backend.newTimelineDefaults(); settings.title=backend.nextSequenceName(); draft=settings; startField.clearRefused(); refreshProblem(); open()
    }
    function submit() {
        // A start that could not be read keeps the box open, its warning showing.
        if (startField.commitHolds()) return
        refreshProblem()
        if (busy || problem) return
        error=""; pending=backend.createSequence(context,draft.title,draft)
    }
    function formatIndex() {
        if (!draft.custom) return 0
        for (var i=1;i<formats.length-1;++i) if (formats[i].w===draft.width && formats[i].h===draft.height) return i
        return formats.length-1
    }
    function rateIndex() {
        for (var i=0;i<rates.length;++i) if (Math.abs(rates[i].n/rates[i].d-draft.fpsNum/draft.fpsDen)<0.000001) return i
        return -1
    }
    onBusyChanged: if (!busy && visible) refreshProblem()
    onDraftChanged: { error=""; refreshProblem() }
    onOpened: { projectName.forceActiveFocus(); projectName.selectAll() }
    Connections {
        target: backend
        function onProjectBusyChanged() { if (root.visible && !root.busy) root.refreshProblem() }
        function onSequencesChanged() { if (root.visible && !root.busy) root.refreshProblem() }
        function onProjectPlaybackReset() { if (!root.busy) root.close() }
        function onSequenceCreateFinished(request,created,message) {
            if (request!==root.pending) return
            root.pending=0
            if (created) { root.completed=true; root.close() }
            else { root.error=message; root.refreshProblem() }
        }
    }
    component Caption: C.Label {
        color: Ui.Theme.textSecondary; font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel
        wrapMode: Text.Wrap
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Ui.Theme.s3
        enabled: !root.busy
        Caption { Layout.fillWidth: true; text: qsTr("Add a timeline to %1. Each timeline has its own format and edits.").arg(root.context.projectName || qsTr("this project")) }
        GridLayout {
            Layout.fillWidth: true
            columns: 2; columnSpacing: Ui.Theme.s3; rowSpacing: Ui.Theme.s2
            Caption { text: qsTr("Timeline name") }
            Ui.LineEdit {
                id: projectName; objectName: "newSequenceName"
                Layout.fillWidth: true; hint: qsTr("Timeline name")
                tip: qsTr("Name of this timeline, shown in its tab")
                text: root.draft.title || ""
                onTextChanged: if (text !== root.draft.title) root.set("title",text)
                onAccepted: root.submit()
            }
            Caption { text: qsTr("Video format") }
            Ui.ComboField {
                objectName: "newSequenceFormat"; Layout.fillWidth: true; caption: ""
                tip: qsTr("Frame size for this timeline; Match first clip uses the first clip you add")
                model: root.formats.map(function(f){return f.label}); currentIndex: root.formatIndex()
                onActivated: function(i) {
                    var s=Object.assign({},root.draft); s.custom=i!==0
                    if (root.formats[i].w>0) {s.width=root.formats[i].w; s.height=root.formats[i].h}
                    root.draft=s
                }
            }
            Caption { text: qsTr("Dimensions") }
            RowLayout {
                Ui.LineEdit { objectName:"newSequenceWidth"; Layout.fillWidth:true; enabled:root.draft.custom===true; hint:qsTr("Width in pixels"); tip:qsTr("Frame width in pixels for a custom format"); text:String(root.draft.width || ""); onTextEdited:root.set("width",Number(text)) }
                Caption { text:"×" }
                Ui.LineEdit { objectName:"newSequenceHeight"; Layout.fillWidth:true; enabled:root.draft.custom===true; hint:qsTr("Height in pixels"); tip:qsTr("Frame height in pixels for a custom format"); text:String(root.draft.height || ""); onTextEdited:root.set("height",Number(text)) }
            }
            Caption { text: qsTr("Frame rate") }
            Ui.ComboField {
                objectName: "newSequenceRate"; Layout.fillWidth:true; caption:""
                tip:qsTr("Frames per second of this timeline; fixed once it contains clips")
                enabled:root.draft.custom===true
                model:root.rates.map(function(r){return r.label+" fps"}); currentIndex:root.rateIndex()
                onActivated:function(i) {var s=Object.assign({},root.draft);s.fpsNum=root.rates[i].n;s.fpsDen=root.rates[i].d;root.draft=s}
            }
            Caption { text: qsTr("Colour space") }
            Ui.ComboField {
                objectName:"newSequenceColor"; Layout.fillWidth:true; caption:""
                tip:qsTr("Colour space this timeline is viewed, graded and exported in")
                model:["Rec.601","Rec.709","Rec.2020"]; currentIndex:[601,709,2020].indexOf(root.draft.colorspace)
                onActivated:function(i){root.set("colorspace",[601,709,2020][i])}
            }
            Caption { text: qsTr("Audio") }
            RowLayout {
                Ui.ComboField { Layout.fillWidth:true; caption:qsTr("Sample rate"); tip:qsTr("Sample rate this timeline’s mix is rendered at"); model:["44.1 kHz","48 kHz","96 kHz"]; currentIndex:[44100,48000,96000].indexOf(root.draft.sampleRate); onActivated:function(i){root.set("sampleRate",[44100,48000,96000][i])} }
                Ui.ComboField { Layout.fillWidth:true; caption:qsTr("Channels"); tip:qsTr("Channel layout of this timeline’s mix: mono, stereo or 5.1"); model:[qsTr("Mono"),qsTr("Stereo"),qsTr("5.1 Surround")]; currentIndex:[1,2,6].indexOf(root.draft.channels); onActivated:function(i){root.set("channels",[1,2,6][i])} }
            }
            Caption { text: qsTr("Starting timecode") }
            Ui.TimecodeField {
                id: startField
                objectName: "newSequenceStart"
                Layout.fillWidth: true
                start: true
                refusalInForm: true
                // The count is kept with the rate it was typed at, so the same
                // timecode stands when another rate is chosen or matched.
                frame: backend.startFrameAtRate(root.draft.startFrame || 0, root.draft.startRate || 0, fps)
                fps: root.draft.fpsNum > 0 && root.draft.fpsDen > 0 ? root.draft.fpsNum / root.draft.fpsDen : 25
                tip: qsTr("Timecode the timeline starts counting from, such as 01:00:00:00")
                onEdited: function (f) { var s=Object.assign({},root.draft); s.startFrame=Math.max(0, f); s.startRate=Math.round(fps); root.draft=s }
            }
        }
        Caption { Layout.fillWidth:true; text:root.draft.custom ? qsTr("Custom formats use square pixels and progressive frames. The frame rate becomes fixed once this timeline contains clips.") : qsTr("Match first clip sets this timeline’s dimensions and frame rate when its first clip is added. Other timelines keep their own settings.") }
        Caption { objectName:"newSequenceValidation"; Layout.fillWidth:true; visible:text!==""; color:Ui.Theme.warning; text:root.error || root.problem || startField.refusedText }
    }
    footer: C.Control {
        padding: Ui.Theme.s2
        contentItem: RowLayout {
            Item { Layout.fillWidth:true }
            Ui.ToolButton { objectName:"newSequenceCancel"; text:qsTr("Cancel"); enabled:!root.busy; onClicked:root.close() }
            Ui.ToolButton { objectName:"newSequenceCreate"; text:root.busy ? qsTr("Creating…") : qsTr("Create Timeline"); enabled:!root.busy && !root.problem; onClicked:root.submit() }
        }
    }
}
