import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui as Ui
import "../Ui/MediaTime.js" as MediaTime

Ui.ModalDialog {
    id: root
    objectName: "speedRampDialog"
    title: qsTr("Speed segments")
    width: Math.min(860,parent ? parent.width-Ui.Theme.s5*2 : 860)
    // As tall as what is in it needs (670 at the least) where the window
    // lets it; in a small window the graph gives up height first.
    height: Math.min(Math.max(670,implicitHeight),parent ? parent.height-Ui.Theme.s4*2 : 670)
    standardButtons: C.Dialog.NoButton
    closePolicy: pending ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    property var target: ({})
    property var draft: ({})
    property var initial: ({})
    property var preview: ({})
    property int preparing: 0
    property int pending: 0
    property int selected: 0
    property int cursor: 0
    property string error: ""
    property string problem: ""
    property bool filling: false
    property bool rateDirty: false
    property bool durationDirty: false
    property bool normal: false
    readonly property bool busy: preparing!==0 || pending!==0
    readonly property var entry: target.entry || ({})
    readonly property var rows: preview.segments || []
    readonly property var segment: selected>=0 && selected<rows.length ? rows[selected] : ({})
    readonly property real rate: target.fps > 0 ? target.fps : 25
    function lengthText(frames) { return MediaTime.durationText(frames,rate) }
    function percent(value) { return String(Number((Number(value || 0)*100).toPrecision(7))) }
    function fillFields() {
        filling=true
        fromRate.text=percent(segment.fromRate); toRate.text=percent(segment.toRate)
        duration.text=MediaTime.clockText(segment.frames || 1,rate)
        easing.currentIndex=segment.easing==="smooth" ? 1 : 0
        rateDirty=false;durationDirty=false
        filling=false
    }
    function take(result) {
        if (!result.valid) { error=result.error || qsTr("Check the segment settings."); return false }
        draft=result.definition; preview=result; error=""
        selected=Math.max(0,Math.min(selected,rows.length-1))
        cursor=Math.max(0,Math.min(cursor,preview.frames-1)); fillFields(); graph.requestPaint()
        return true
    }
    function change(action,options) {
        if (busy || problem) return false
        if(action!=="set" && !commitFields())return false
        var changed=take(backend.editSpeedRampDraft(draft,action,selected,options || ({})))
        if(changed)normal=action==="normal"
        return changed
    }
    function commitFields() {
        if(!rateDirty && !durationDirty)return !error
        if(filling || busy || !preview.valid)return false
        var options=({})
        if(rateDirty) {
            var decimal=/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/
            if(!decimal.test(fromRate.text.trim()) || !decimal.test(toRate.text.trim())) {
                error=qsTr("Enter a speed from −2000 to 2000 percent.");return false
            }
            options.speed=Number(fromRate.text)/100;options.endSpeed=Number(toRate.text)/100
            options.easing=easing.currentIndex===1 ? "smooth" : "linear"
            options.keepSource=keepSource.checked
        }
        if(durationDirty) {
            var frames=MediaTime.parseDuration(duration.text,rate)
            if(frames<1) {
                error=qsTr("Enter a duration in seconds, such as 2.5 or 1:30.");return false
            }
            options.frames=frames
        }
        return change("set",options)
    }
    function openFor(track,index) {
        target=backend.retimeTarget(track,index); pending=0; draft=({}); initial=({}); preview=({});normal=false
        error=""; problem=backend.retimeProblem(target,true); selected=0
        cursor=Math.max(0,backend.position-(entry.start || 0)+(entry.mixIn || 0))
        open(); preparing=backend.prepareSpeedRamp(target)
    }
    function openForSelection() { var e=win.selection.selectedEntry(); if(e && win.retiming.canRetimeSelection())openFor(e.track,e.index) }
    function refreshProblem() { if(visible && !pending)problem=backend.retimeProblem(target,true) }
    function submit() {
        if(!commitFields())return
        refreshProblem()
        if(busy || problem || error || !preview.valid)return
        pending=backend.applySpeedRamp(target,draft,normal)
    }
    onSelectedChanged: { fillFields(); graph.requestPaint() }
    onCursorChanged: graph.requestPaint()
    Connections {
        target: backend
        function onSpeedRampPrepared(request,state) {
            if(request!==root.preparing || !root.visible)return
            root.preparing=0
            if(root.take(state))root.initial=state.definition
        }
        function onRetimeFinished(request,changed,message) {
            if(request!==root.pending)return
            root.pending=0
            if(message)root.error=message;else root.close()
        }
        function onTimelineEntriesChanged(){Qt.callLater(root.refreshProblem)}
        function onTracksChanged(){Qt.callLater(root.refreshProblem)}
        function onSequencesChanged(){Qt.callLater(root.refreshProblem)}
        function onProjectBusyChanged(){Qt.callLater(root.refreshProblem)}
        function onProjectPlaybackReset(){root.preparing=0;root.close();root.target=({})}
    }
    component Caption: C.Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Ui.Theme.textSecondary
        font.family: Ui.Theme.fontFamily; font.pixelSize: Ui.Theme.fsLabel
    }
    contentItem: ColumnLayout {
        spacing: Ui.Theme.s2
        Caption { text:(root.entry.name || qsTr("Clip"))+" · "+(root.target.sequenceName || qsTr("Sequence"));color:Ui.Theme.textPrimary }
        RowLayout {
            Layout.fillWidth:true; enabled:!root.busy && !root.problem && !!root.preview.valid
            Ui.ComboField {
                id: preset; objectName:"speedRampPreset"; Layout.fillWidth:true
                model:[qsTr("Instant replay"),qsTr("Rewind and play"),qsTr("Reverse clip"),qsTr("Hold at cursor"),qsTr("Normal speed")]
                tip:qsTr("Replay repeats the clip; rewind plays back to the start and forwards again; hold pauses on the cursor picture")
            }
            Ui.ToolButton {
                objectName:"speedRampAddPreset";text:qsTr("Use preset")
                tip:qsTr("Apply the selected preset to this draft")
                onClicked:root.change(["replay","rewind","reverse","hold","normal"][preset.currentIndex],
                    preset.currentIndex===3 ? {frame:root.cursor,frames:Math.max(1,Math.round(root.target.fps || 25))} : ({}))
            }
            Ui.ToolButton { text:qsTr("Reset changes");tip:qsTr("Restore the timing from when this editor opened");onClicked:{root.normal=false;root.selected=0;root.take(backend.previewSpeedRamp(root.initial))} }
        }
        Rectangle {
            Layout.fillWidth:true;Layout.preferredHeight:125;Layout.minimumHeight:48;Layout.fillHeight:true;Layout.maximumHeight:125;color:Ui.Theme.panelBg;radius:Ui.Theme.rControl
            border.color:Ui.Theme.borderStrong
            Canvas {
                id:graph;objectName:"speedRampGraph";anchors.fill:parent;anchors.margins:8
                onWidthChanged:requestPaint();onHeightChanged:requestPaint()
                Connections { target:Ui.Theme;function onAccentChanged(){graph.requestPaint()} }
                onPaint:{
                    var ctx=getContext("2d");ctx.clearRect(0,0,width,height)
                    if(!root.preview.valid)return
                    var total=root.preview.frames,limit=1
                    for(var j=0;j<root.rows.length;j++)limit=Math.max(limit,Math.abs(root.rows[j].fromRate),Math.abs(root.rows[j].toRate))
                    var mid=height/2,scale=(height/2-12)/limit
                    ctx.strokeStyle=Ui.Theme.borderStrong;ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(0,mid);ctx.lineTo(width,mid);ctx.stroke()
                    for(var i=0;i<root.rows.length;i++){
                        var s=root.rows[i],left=s.start/total*width,right=(s.start+s.frames)/total*width
                        if(i===root.selected){ctx.fillStyle=Qt.rgba(Ui.Theme.accent.r,Ui.Theme.accent.g,Ui.Theme.accent.b,.12);ctx.fillRect(left,0,right-left,height)}
                        ctx.strokeStyle=Ui.Theme.borderStrong;ctx.beginPath();ctx.moveTo(left,0);ctx.lineTo(left,height);ctx.stroke()
                        ctx.strokeStyle=Ui.Theme.accent;ctx.lineWidth=2;ctx.beginPath()
                        for(var k=0;k<=24;k++){
                            var u=s.curveStart+(s.curveEnd-s.curveStart)*k/24
                            if(s.easing==="smooth")u=u*u*(3-2*u)
                            var v=s.speed+(s.endSpeed-s.speed)*u,x=left+(right-left)*k/24,y=mid-v*scale
                            if(k===0)ctx.moveTo(x,y);else ctx.lineTo(x,y)
                        }
                        ctx.stroke()
                    }
                    ctx.strokeStyle=Ui.Theme.textPrimary;ctx.lineWidth=1;ctx.beginPath();var px=root.cursor/total*width;ctx.moveTo(px,0);ctx.lineTo(px,height);ctx.stroke()
                }
                MouseArea {
                    anchors.fill:parent;enabled:!root.busy && !!root.preview.valid
                    onPressed:function(mouse){if(!root.commitFields())return;root.cursor=Math.max(0,Math.min(root.preview.frames-1,Math.floor(mouse.x/width*root.preview.frames)));for(var i=0;i<root.rows.length;i++)if(root.cursor>=root.rows[i].start)root.selected=i}
                    onPositionChanged:function(mouse){if(pressed)root.cursor=Math.max(0,Math.min(root.preview.frames-1,Math.floor(mouse.x/width*root.preview.frames)))}
                }
            }
        }
        RowLayout {
            Layout.fillWidth:true;enabled:!root.busy && !!root.preview.valid
            Caption { Layout.fillWidth:false;text:qsTr("Cursor") }
            C.Slider {
                objectName:"speedRampCursor";Layout.fillWidth:true;from:0;to:Math.max(1,(root.preview.frames || 1)-1);stepSize:1;value:root.cursor
                onMoved:root.cursor=Math.round(value)
                Accessible.name:qsTr("Cursor within the speed draft")
                C.ToolTip.text:qsTr("Choose the frame for Blade speed or Hold at cursor");C.ToolTip.visible:hovered;C.ToolTip.delay:450
            }
            Caption { Layout.fillWidth:false;text:qsTr("Frame %1").arg(root.cursor) }
            Ui.ToolButton { objectName:"speedRampBlade";text:qsTr("Blade speed");enabled:root.cursor>0 && !root.problem;tip:qsTr("Split the speed segment at the cursor without cutting the clip");onClicked:root.change("split",{frame:root.cursor}) }
        }
        RowLayout {
            Layout.fillWidth:true;Layout.fillHeight:true;spacing:Ui.Theme.s3
            C.ScrollView {
                C.ScrollBar.vertical: Ui.ScrollBar { atRest: true }
                Layout.preferredWidth:200;Layout.fillHeight:true;clip:true
                C.ScrollBar.horizontal.policy:C.ScrollBar.AlwaysOff
                ListView {
                    id:segmentList;objectName:"speedRampSegments";model:root.rows;spacing:2;clip:true
                    currentIndex:root.selected
                    delegate:Ui.ToolButton {
                        required property int index
                        required property var modelData
                        width:ListView.view.width;checkable:true;checked:root.selected===index
                        text:qsTr("%1 · %2 → %3%").arg(index+1).arg(root.percent(modelData.fromRate)).arg(root.percent(modelData.toRate))
                        tip:qsTr("%1, starting %2 into the clip").arg(root.lengthText(modelData.frames)).arg(root.lengthText(modelData.start))
                        onClicked:{if(root.commitFields()){root.selected=index;root.cursor=modelData.start}}
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth:true;Layout.alignment:Qt.AlignTop;spacing:Ui.Theme.s2
                enabled:!root.busy && !root.problem && !!root.preview.valid
                Caption {text:qsTr("Segment %1").arg(root.selected+1);color:Ui.Theme.textPrimary}
                GridLayout {
                    columns:2;Layout.fillWidth:true;columnSpacing:Ui.Theme.s3
                    Caption { Layout.fillWidth:false;text:qsTr("From (%)") }
                    Ui.LineEdit { id:fromRate;objectName:"speedRampFrom";Accessible.name:qsTr("Starting speed, percent");Layout.fillWidth:true;tip:qsTr("Starting speed: 100 is normal, 0 holds, negative values reverse");onTextEdited:root.rateDirty=true;onEditingFinished:root.commitFields() }
                    Caption { Layout.fillWidth:false;text:qsTr("To (%)") }
                    Ui.LineEdit { id:toRate;objectName:"speedRampTo";Accessible.name:qsTr("Ending speed, percent");Layout.fillWidth:true;tip:qsTr("Ending speed; use the same value for constant speed");onTextEdited:root.rateDirty=true;onEditingFinished:root.commitFields() }
                    Caption { Layout.fillWidth:false;text:qsTr("Change") }
                    Ui.ComboField { id:easing;objectName:"speedRampEasing";Layout.fillWidth:true;model:[qsTr("Linear"),qsTr("Smooth")];tip:qsTr("How speed changes between the two values");onActivated:{root.rateDirty=true;root.commitFields()} }
                    Caption { Layout.fillWidth:false;text:qsTr("Duration") }
                    Ui.LineEdit { id:duration;objectName:"speedRampFrames";Accessible.name:qsTr("Segment duration");Layout.fillWidth:true;tip:qsTr("How long the segment lasts, in seconds (2.5) or minutes and seconds (1:30)");onTextEdited:root.durationDirty=true;onEditingFinished:root.commitFields() }
                }
                C.CheckBox {
                    id:keepSource;checked:true;text:qsTr("Keep source range when changing speed")
                    C.ToolTip.text:qsTr("Adjust segment duration to keep the same material, rounded down to whole frames");C.ToolTip.visible:hovered;C.ToolTip.delay:450
                }
                Ui.ToolButton { text:qsTr("Join previous");enabled:root.selected>0;tip:qsTr("Replace this segment and the previous one with one constant-speed section");onClicked:{if(root.change("join",({})))root.selected=Math.max(0,root.selected-1)} }
            }
        }
        Caption {
            objectName:"speedRampDuration";color:Ui.Theme.textPrimary;visible:!!root.preview.valid
            readonly property int change:(root.preview.frames || 0)-((root.entry.out || 0)-(root.entry.in || 0)+1)
            text:qsTr("Result: %1 · %2").arg(root.lengthText(root.preview.frames || 0))
                .arg(change===0 ? qsTr("same length") : change>0 ? qsTr("%1 longer").arg(root.lengthText(change)) : qsTr("%1 shorter").arg(root.lengthText(-change)))
        }
        Caption {text:qsTr("Later clips follow the ripple rules. Sound follows the ramp's speed, so its pitch rises and falls with it; holds and speeds below 10% or above 1000% are silent.")}
        Caption {visible:root.normal;text:qsTr("Normal speed removes ramps, repeats and holds, playing the source range forwards once.")}
        Caption {objectName:"speedRampError";color:Ui.Theme.warning;visible:text!=="";text:root.problem || root.error}
        Caption {text:root.preparing ? qsTr("Reading source timing…") : root.pending ? qsTr("Updating clip…") : qsTr("Apply saves all timing changes in one undo step.")}
    }
    footer:C.Control {
        padding:Ui.Theme.s2
        contentItem:RowLayout {
            Item{Layout.fillWidth:true}
            Ui.ToolButton {objectName:"speedRampCancel";text:qsTr("Cancel");enabled:!root.pending;onClicked:root.close()}
            Ui.ToolButton {objectName:"speedRampApply";text:qsTr("Apply");enabled:!root.busy && !root.problem && (!root.error || root.rateDirty || root.durationDirty) && !!root.preview.valid;onClicked:root.submit()}
        }
    }
}
