pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

ModalDialog {
    id: root
    objectName: "nodeRouting"
    title: qsTr("Node Connections and Key")
    width: Math.min(620, parent ? parent.width-32 : 620)
    property string targetKey: ""
    onTargetKeyChanged: if(visible)close()
    property var nodes: []
    property int current: 0
    property var draft: ({})
    property string error: ""
    property bool editable: false
    signal nodeApplied(var node)
    signal locateMatteRequested()
    readonly property var kinds: ["serial","parallel","layer","splitter","combiner","compound"]
    function chooseMatte(path) { draft=Object.assign({},draft,{externalMatte:path}) }
    function put(key,v) { var m=Object.assign({},draft);m[key]=v;draft=m;error="" }
    function toggleInput(id,on) { var links=(draft.inputs || []).filter(function(k) { return k!==id });if(on)links.push(id);put("inputs",links) }
    function valid() {
        var all=nodes.map(function(n,i) { return i===root.current ? root.draft : n }), byId={}, active={}, done={}
        for(var i=0;i<all.length;++i)byId[all[i].id]=i
        function visit(index) {
            if(active[index])return false;if(done[index])return true;active[index]=true
            var n=all[index], links=n.inputs!==undefined?n.inputs.slice():[index>0?all[index-1].id:"input"]
            if(n.keyInput)links.push(n.keyInput)
            for(var j=0;j<links.length;++j)if(links[j]!=="input"&&(byId[links[j]]===undefined||!visit(byId[links[j]])))return false
            active[index]=false;done[index]=true;return true
        }
        for(var k=0;k<all.length;++k)if(!visit(k))return false
        return true
    }
    onOpened: {
        draft=JSON.parse(JSON.stringify(nodes[current] || {}))
        if(draft.inputs===undefined)draft.inputs=[current>0?nodes[current-1].id:"input"]
        error=""
    }
    standardButtons: C.Dialog.NoButton
    contentItem: ColumnLayout {
        spacing: Theme.s2
        Text { Layout.fillWidth: true; text: root.draft.label || qsTr("Correction"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; color: Theme.textPrimary }
        ComboField { objectName: "nodeKind"; Layout.fillWidth: true; model: [qsTr("Serial"),qsTr("Parallel mixer"),qsTr("Layer mixer"),qsTr("Channel splitter"),qsTr("Channel combiner"),qsTr("Nested")]; currentIndex: root.kinds.indexOf(root.draft.kind || "serial"); tip: qsTr("What this node is: a serial correction, a mixer, or a channel split"); onActivated: function(i) { root.put("kind",root.kinds[i]) } }
        Text { Layout.fillWidth: true; text: qsTr("Image inputs (layer order follows the order you select)"); wrapMode: Text.WordWrap; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textMuted }
        C.ScrollView {
            C.ScrollBar.vertical: ScrollBar { atRest: true }
            Layout.fillWidth: true; Layout.preferredHeight: 140
            contentWidth: availableWidth
            Column {
                width: parent.width
                Repeater {
                    model: [{id:"input",label:qsTr("Clip input")}].concat(root.nodes.filter(function(n,i) {return i!==root.current}))
                    delegate: Toggle {
                        required property var modelData
                        width: parent.width; label: modelData.label
                        checked: (root.draft.inputs || []).indexOf(modelData.id)>=0
                        tip: qsTr("Feed this source into the node; selection order sets layer order")
                        onToggled: root.toggleInput(modelData.id,checked)
                    }
                }
            }
        }
        ComboField { Layout.fillWidth: true; visible: root.draft.kind==="splitter"; model: [qsTr("Red channel"),qsTr("Green channel"),qsTr("Blue channel")]; currentIndex: ["red","green","blue"].indexOf(root.draft.channel || "red"); tip: qsTr("Which colour channel a splitter node sends on"); onActivated: function(i) { root.put("channel",["red","green","blue"][i]) } }
        SliderField { objectName: "nodeOpacity"; Layout.fillWidth: true; label: qsTr("Mix"); from: 0; to: 100; decimals: 0; suffix: "%"; value: (root.draft.opacity!==undefined?root.draft.opacity:1)*100; tip: qsTr("How much of this node's result is blended over its input"); onEdited: function(v) { root.put("opacity",v/100) } }
        ComboField {
            objectName: "nodeKeyInput"; Layout.fillWidth: true
            readonly property var options: [{id:"",label:qsTr("No key input")}].concat(root.nodes.filter(function(n,i) {return i!==root.current}))
            model: options.map(function(n) {return n.label})
            currentIndex: options.map(function(n) {return n.id}).indexOf(root.draft.keyInput || "")
            tip: qsTr("Node whose output is used as this node's key")
            onActivated: function(i) {root.put("keyInput",options[i].id)}
        }
        RowLayout {
            Layout.fillWidth: true
            Toggle { label: qsTr("Invert key"); checked: root.draft.keyInvert===true; tip: qsTr("Use the key inverted, so the correction hits the unmasked areas"); onToggled: root.put("keyInvert",checked) }
            SliderField { Layout.fillWidth: true; label: qsTr("Key gain"); from:0;to:2;stepSize:.01;decimals:2;value:root.draft.keyGain!==undefined?root.draft.keyGain:1;tip: qsTr("Multiplies the key strength before it limits the correction");onEdited:function(v){root.put("keyGain",v)} }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: root.draft.externalMatte || qsTr("No external matte"); elide: Text.ElideMiddle; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
            ToolButton { objectName: "nodeLocateMatte"; text: qsTr("Matte file…"); onClicked: root.locateMatteRequested() }
            IconButton { iconName:"x";text:qsTr("Remove matte");enabled:!!root.draft.externalMatte;onClicked:root.put("externalMatte","") }
        }
        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: root.error || qsTr("A key multiplies this node's windows and colour pick. External mattes use image luminance. Parallel mixers average their inputs; layer mixers composite bottom to top."); font.family: Theme.fontFamily;font.pixelSize:Theme.fsLabel;color:root.error?Theme.warning:Theme.textMuted }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ToolButton { text: qsTr("Cancel"); onClicked:root.reject() }
            ToolButton { objectName:"nodeRoutingApply";text:qsTr("Apply");enabled:root.editable;onClicked:{if(!root.valid()){root.error=qsTr("That connection creates a cycle. Choose another input.");return}root.nodeApplied(root.draft);root.accept()} }
        }
    }
}
