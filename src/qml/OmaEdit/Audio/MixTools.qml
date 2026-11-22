pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "audioMixTools"
    title: qsTr("Audio tools — %1").arg(stripName)
    modal: true
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(850, parent ? parent.width - 32 : 850)
    height: Math.min(620, parent ? parent.height - 32 : 620)
    standardButtons: C.Dialog.Close
    property string stripName: ""
    property var mix: ({})
    property var buses: []
    property var selectedClip: null
    property int duration: 0
    property int position: 0
    property real fps: 25
    property bool canEdit: false
    property bool isMaster: false
    property bool liveAutomation: false
    property string parameter: "volume"
    property string interpolation: "linear"
    property var draft: null
    readonly property var keys: draft !== null ? draft : mix.automationKeys && mix.automationKeys[parameter] ? mix.automationKeys[parameter] : []
    readonly property real low: parameter === "pan" ? -1 : -100
    readonly property real high: parameter === "pan" ? 1 : 24
    signal mixEdited(string key, var value)
    signal busesEdited(var buses)
    signal duplicateRequested()
    signal timingRequested()
    signal speedRequested(real speed)
    signal fadesRequested(int fadeIn, int fadeOut)
    signal fadeCurveRequested(string curve)
    function saveKeys(next) {
        var unique={};next.forEach(function(k){unique[String(k.frame)]=k});next=Object.keys(unique).map(function(k){return unique[k]})
        next.sort(function (a,b) { return a.frame-b.frame })
        var all=Object.assign({},mix.automationKeys || {})
        all[parameter]=next
        mixEdited("automationKeys",all)
    }
    function valueAt(frame) {
        if (mix.automation === "off" || keys.length === 0) return mix[parameter] || 0
        var sorted=keys.slice().sort(function(a,b){return a.frame-b.frame})
        if (frame <= sorted[0].frame) return sorted[0].value
        for (var i=1;i<sorted.length;i++) {
            var a=sorted[i-1], b=sorted[i]
            if (frame <= b.frame) {
                if (frame === b.frame) return b.value
                if (a.interpolation === "hold") return a.value
                var t=(frame-a.frame)/(b.frame-a.frame)
                if (a.interpolation === "smooth") t=t*t*(3-2*t)
                return a.value+(b.value-a.value)*t
            }
        }
        return sorted[sorted.length-1].value
    }
    function keyAtPlayhead() {
        var mode=interpolation, frame=-1
        keys.forEach(function(k){if(k.frame<=position && k.frame>frame){frame=k.frame;mode=k.interpolation || "linear"}})
        point(position,valueAt(position),mode)
    }
    function point(frame,value,mode) {
        frame=Math.max(0,Math.min(duration-1,Math.round(frame)))
        var next=keys.filter(function(k){return k.frame!==frame})
        next.push({frame:frame,value:Math.max(low,Math.min(high,value)),interpolation:mode || interpolation})
        saveKeys(next)
    }
    contentItem: Column {
        spacing: Theme.s2
        C.TabBar {
            id: tabs
            objectName: "audioMixTabs"
            width: parent.width
            C.TabButton { text: qsTr("Automation"); property string tip; tip: qsTr("Draw volume or pan curves that play back with the strip"); C.ToolTip.text: tip; C.ToolTip.visible: hovered; C.ToolTip.delay: 450 }
            C.TabButton { text: qsTr("Routing"); property string tip; tip: qsTr("Send this strip to buses and manage the bus list"); C.ToolTip.text: tip; C.ToolTip.visible: hovered; C.ToolTip.delay: 450 }
            C.TabButton { text: qsTr("Clip"); property string tip; tip: qsTr("Change timing, duplicate, respeed, reverse and fade the selected clip"); C.ToolTip.text: tip; C.ToolTip.visible: hovered; C.ToolTip.delay: 450 }
        }
        Flickable {
            width: parent.width
            height: Math.max(0, parent.height - y)
            visible: tabs.currentIndex===0
            contentHeight: automationColumn.height
            clip: true
            C.ScrollBar.vertical: ScrollBar { atRest: true }
        Column {
            id: automationColumn
            width: parent.width
            enabled: root.canEdit
            spacing: Theme.s2
            Flow {
                width: parent.width
                spacing: Theme.s2
                ComboField { width: 180; caption: qsTr("Parameter"); tip: qsTr("Which curve you are editing, the fader or the pan"); model: [qsTr("Volume"),qsTr("Pan")]; currentIndex: root.parameter==="pan"?1:0; onActivated: function(i){root.parameter=i===1?"pan":"volume"} }
                ComboField { width: 180; caption: qsTr("Mode"); tip: qsTr("How the strip follows or records its curve while playing"); property var modes: root.liveAutomation ? ["read","touch","latch","write","off"] : ["read","off"]; model: modes.map(function(m){return m.charAt(0).toUpperCase()+m.slice(1)}); currentIndex: Math.max(0,modes.indexOf(root.mix.automation || "read")); onActivated: function(i){root.mixEdited("automation",modes[i])} }
                ComboField { width: 180; caption: qsTr("Curve"); tip: qsTr("Shape given to new points: straight, eased or held"); model: [qsTr("Linear"),qsTr("Smooth"),qsTr("Hold")]; onActivated: function(i){root.interpolation=["linear","smooth","hold"][i]} }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("Double-click the graph to add a point. Drag points to edit. Touch returns to the existing curve on release; Latch holds until playback stops; Write replaces the curve while playing."); font.pixelSize: Theme.fsBase; color: Theme.textSecondary }
            Rectangle {
                width: parent.width; height: 155
                color: Theme.controlBg; border.color: Theme.border
                Canvas {
                    id: graph
                    anchors.fill: parent
                    property var points: root.keys
                    onPointsChanged: requestPaint()
                    Connections { target: Theme; function onAccentChanged() { graph.requestPaint() } } // the desktop accent can change while this is on screen
                    onWidthChanged: requestPaint()
                    onPaint: {
                        var c=getContext("2d");c.reset();c.strokeStyle=Theme.accent;c.fillStyle=Theme.textPrimary;c.lineWidth=2
                        c.beginPath()
                        for(var i=0;i<points.length;++i){var x=points[i].frame/Math.max(1,root.duration-1)*width,y=(root.high-points[i].value)/(root.high-root.low)*height;if(i===0)c.moveTo(x,y);else{var prev=points[i-1],x0=prev.frame/Math.max(1,root.duration-1)*width,y0=(root.high-prev.value)/(root.high-root.low)*height;if(prev.interpolation==="hold"){c.lineTo(x,y0);c.lineTo(x,y)}else if(prev.interpolation==="smooth"){for(var step=1;step<=32;step++){var t=step/32,k=t*t*(3-2*t);c.lineTo(x0+(x-x0)*t,y0+(y-y0)*k)}}else c.lineTo(x,y)}}c.stroke()
                        for(var j=0;j<points.length;++j){var px=points[j].frame/Math.max(1,root.duration-1)*width,py=(root.high-points[j].value)/(root.high-root.low)*height;c.fillRect(px-4,py-4,8,8)}
                    }
                    MouseArea {
                        anchors.fill: parent
                        property int moving: -1
                        onPressed: function(mouse){moving=-1;for(var i=0;i<root.keys.length;++i){var k=root.keys[i];if(Math.abs(mouse.x-k.frame/Math.max(1,root.duration-1)*width)<12 && Math.abs(mouse.y-(root.high-k.value)/(root.high-root.low)*height)<12){moving=i;root.draft=root.keys.map(function(p){return Object.assign({},p)});break}}}
                        onPositionChanged: function(mouse){if(!pressed||moving<0)return;var next=root.draft.slice();next[moving]=Object.assign({},next[moving],{frame:Math.max(0,Math.min(root.duration-1,Math.round(mouse.x/width*(root.duration-1)))),value:Math.max(root.low,Math.min(root.high,root.high-mouse.y/height*(root.high-root.low)))});root.draft=next}
                        onReleased: {if(moving>=0){var next=root.draft;root.draft=null;root.saveKeys(next)}moving=-1}
                        onCanceled: {root.draft=null;moving=-1}
                        onDoubleClicked: function(mouse){root.point(mouse.x/width*(root.duration-1),root.high-mouse.y/height*(root.high-root.low))}
                    }
                }
            }
            Row {
                spacing: Theme.s2
                ToolButton { objectName: "audioKeyAtPlayhead"; text: qsTr("Key at playhead"); showLabel: true; onClicked: root.keyAtPlayhead() }
                ToolButton { text: qsTr("Clear parameter"); showLabel: true; onClicked: root.saveKeys([]) }
            }
            ListView {
                width: parent.width; height: 130; clip: true
                model: root.keys.length
                delegate: Row {
                    required property int index
                    readonly property var point: root.keys[index] || {frame:0,value:0}
                    spacing: Theme.s2
                    Text { width: 100; text: qsTr("Frame %1").arg(parent.point.frame); color: Theme.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                    C.TextField { width: 100; C.ToolTip.text: qsTr("Value at this point, in dB for volume or -1 to 1 for pan"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; text: Number(parent.point.value).toFixed(2); onEditingFinished: {var next=root.keys.map(function(k){return Object.assign({},k)});var value=Number(text);if(isFinite(value)&&next[parent.index]){next[parent.index].value=Math.max(root.low,Math.min(root.high,value));root.saveKeys(next)}} }
                    ComboField { width: 160; tip: qsTr("Shape of the curve leaving this point"); model: [qsTr("Linear"),qsTr("Smooth"),qsTr("Hold")]; currentIndex: Math.max(0,["linear","smooth","hold"].indexOf(parent.point.interpolation)); onActivated: function(i){var next=root.keys.map(function(k){return Object.assign({},k)});next[parent.index].interpolation=["linear","smooth","hold"][i];root.saveKeys(next)} }
                    ToolButton { text: qsTr("Remove"); showLabel: true; onClicked: {var next=root.keys.slice();next.splice(parent.index,1);root.saveKeys(next)} }
                }
            }
        }
        }
        Flickable {
            width: parent.width; height: Math.max(0, parent.height - y); visible: tabs.currentIndex===1; clip: true; contentHeight: routingColumn.height
            C.ScrollBar.vertical: ScrollBar { atRest: true }
            Column {
            id: routingColumn
            width: parent.width; spacing: Theme.s2
            ComboField { width: parent.width; caption: qsTr("Output"); tip: qsTr("Bus this strip feeds, or the master directly"); enabled: root.canEdit && !root.isMaster; model: [qsTr("Master")].concat(root.buses.map(function(b){return b.name})); currentIndex: Math.max(0,["master"].concat(root.buses.map(function(b){return b.id})).indexOf(root.mix.output || "master")); onActivated: function(i){root.mixEdited("output",i===0?"master":root.buses[i-1].id)} }
            Repeater {
                model: root.isMaster ? [] : root.mix.sends || []
                delegate: Row {
                    required property int index
                    required property var modelData
                    spacing: Theme.s2
                    function update(key,value){var next=(root.mix.sends || []).map(function(s){return Object.assign({},s)});next[index][key]=value;root.mixEdited("sends",next)}
                    ComboField { width: 160; tip: qsTr("Bus this send feeds alongside the strip output"); model: root.buses.map(function(b){return b.name});currentIndex: Math.max(0,root.buses.map(function(b){return b.id}).indexOf(parent.modelData.bus));onActivated: function(i){parent.update("bus",root.buses[i].id)} }
                    C.TextField { width: 85;C.ToolTip.text: qsTr("Level this send is fed at, -100 to +12 dB"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; text: String(parent.modelData.level===undefined?-12:parent.modelData.level);onEditingFinished: {var v=Number(text);if(isFinite(v))parent.update("level",Math.max(-100,Math.min(12,v)))} }
                    C.CheckBox { text: qsTr("Pre");C.ToolTip.text: qsTr("Take the send ahead of the fader so fader moves do not change it"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; checked: parent.modelData.pre===true;onClicked: parent.update("pre",checked) }
                    C.CheckBox { text: qsTr("Bypass");C.ToolTip.text: qsTr("Stop this send feeding its bus without removing it"); C.ToolTip.visible: hovered; C.ToolTip.delay: 450; checked: parent.modelData.bypass===true;onClicked: parent.update("bypass",checked) }
                    ToolButton { text: qsTr("Remove");showLabel: true;onClicked: {var next=root.mix.sends.slice();next.splice(parent.index,1);root.mixEdited("sends",next)} }
                }
            }
            ToolButton { text: qsTr("Add send");showLabel: true;enabled: !root.isMaster && root.canEdit && root.buses.length>0 && (root.mix.sends || []).length<32;onClicked: {var next=(root.mix.sends || []).slice();next.push({bus:root.buses[0].id,level:-12,pre:false});root.mixEdited("sends",next)} }
            Row {
                spacing: Theme.s2
                C.TextField { id: busName; width: 230; placeholderText: qsTr("Bus name"); C.ToolTip.text: qsTr("Name for the new bus created by the button beside it"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true }
                ToolButton { text: qsTr("Add bus"); showLabel: true; enabled: busName.text.trim()!=="" && root.buses.length<32; onClicked: {var next=root.buses.slice();next.push({id:"bus-"+Date.now(),name:busName.text.trim(),mix:{}});root.busesEdited(next);busName.clear()} }
            }
            Repeater {
                model: root.buses.length
                delegate: Row {
                    required property int index
                    width: parent.width; spacing: Theme.s2
                    C.TextField { width: 230; C.ToolTip.text: qsTr("Rename this bus; strips and sends follow the new name"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; text: root.buses[parent.index].name; onEditingFinished: {var next=root.buses.map(function(b){return Object.assign({},b)});next[parent.index].name=text.trim();root.busesEdited(next)} }
                    ToolButton { text: qsTr("Remove bus"); showLabel: true; onClicked: {var next=root.buses.slice();next.splice(parent.index,1);root.busesEdited(next)} }
                }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("Route tracks and sends into buses. Used buses must be disconnected before removal; feedback loops are refused."); color: Theme.textSecondary }
        }
        }
        Column {
            width: parent.width; visible: tabs.currentIndex===2; enabled: root.selectedClip!==null && root.canEdit; spacing: Theme.s3
            Text { text: root.selectedClip ? root.selectedClip.name || root.selectedClip.path : qsTr("Select an audio clip"); width: parent.width; elide: Text.ElideMiddle; color: Theme.textPrimary }
            ToolButton { text: qsTr("Duplicate after clip"); showLabel: true; onClicked: root.duplicateRequested() }
            ToolButton { objectName: "audioClipTiming"; text: qsTr("Sound timing…"); showLabel: true; tip: qsTr("Set position, source in and duration in samples or milliseconds"); onClicked: root.timingRequested() }
            Row {
                spacing: Theme.s2
                C.TextField { id: rate; width: 120; C.ToolTip.text: qsTr("Playback speed as a percentage, 5 to 2000, with pitch kept"); C.ToolTip.visible: hovered && !activeFocus; C.ToolTip.delay: 450; hoverEnabled: true; text: root.selectedClip ? String(Math.abs(root.selectedClip.speed || 1)*100) : "100"; validator: DoubleValidator { bottom: 5; top: 2000 } }
                ToolButton { text: qsTr("Set speed %"); showLabel: true; onClicked: root.speedRequested(Number(rate.text)/100 * (root.selectedClip && root.selectedClip.speed<0?-1:1)) }
                ToolButton { text: qsTr("Reverse"); showLabel: true; onClicked: root.speedRequested(-(root.selectedClip.speed || 1)) }
            }
            ComboField { width: parent.width; caption: qsTr("Fade curve"); tip: qsTr("Shape of the ramps at the clip's fade in and fade out"); model: [qsTr("Logarithmic"),qsTr("Linear"),qsTr("Equal power"),qsTr("Smooth")]; currentIndex: Math.max(0,["log","linear","equalPower","smooth"].indexOf(root.selectedClip ? root.selectedClip.audioCurve || "log" : "log")); onActivated: function(i){root.fadeCurveRequested(["log","linear","equalPower","smooth"][i])} }
            Row {
                spacing: Theme.s2
                Text { text: qsTr("Fade in / out"); color: Theme.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                ValueField { id: fadeIn; objectName: "audioFadeIn"; width: 80; label: qsTr("Fade in"); tip: qsTr("Length of the fade at the start of the clip"); fps: root.fps; from: 0; to: root.selectedClip ? Math.max(0,root.selectedClip.frames-1) : 0; property real modelValue: root.selectedClip ? root.selectedClip.fadeIn || 0 : 0; value: modelValue; onModelValueChanged: value = modelValue; onEdited: function (v) { root.fadesRequested(v,fadeOut.value) } }
                ValueField { id: fadeOut; objectName: "audioFadeOut"; width: 80; label: qsTr("Fade out"); tip: qsTr("Length of the fade at the end of the clip"); fps: root.fps; from: 0; to: root.selectedClip ? Math.max(0,root.selectedClip.frames-1) : 0; property real modelValue: root.selectedClip ? root.selectedClip.fadeOut || 0 : 0; value: modelValue; onModelValueChanged: value = modelValue; onEdited: function (v) { root.fadesRequested(fadeIn.value,v) } }
            }
        }
    }
}
