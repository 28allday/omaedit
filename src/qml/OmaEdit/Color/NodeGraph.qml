pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import OmaEdit.Ui

// A clip's corrections. They show as a stack of layers, the last one applied
// at the top: each with its own tools, its eye, its name, dragged up or down
// to change the order. (`stacked: false` draws the same corrections as a graph
// with image connections; the page does not offer it.)
Rectangle {
    id: root

    // The dock bar that hosts this panel's header actions; null when standalone.
    property Item dockBar: null
    property string pathLabel: ""
    signal leaveRequested()
    property string outputNode: ""
    property string connecting: ""
    property string graphMessage: ""
    signal maskRequested()
    signal sharingRequested()
    signal settingsRequested()
    signal outputRequested(string node)
    signal graphEdited(var nodes, string output)
    signal compoundRequested()
    signal editCompoundRequested()
    signal presetRequested(string action)
    property var nodes: []                // grade.nodes
    property int current: -1
    property string thumb: ""             // the clip's picture for the cards
    property bool bypassAll: false
    property bool editable: false
    property bool hasCopiedNode: false
    property bool hasCopiedGrade: false
    signal copyRequested(string kind)
    signal pasteRequested(string kind)
    signal nodeSelected(int index)
    signal nodesEdited(var nodes)
    // A layer changing places while it is dragged, and the end of that drag:
    // however many places it crosses, the drag is one undo step.
    signal nodesDragged(var nodes)
    signal dragFinished()
    signal addRequested(string kind)      // serial | parallel | layer
    // The layer's own corrections back to how a new layer starts.
    signal resetRequested(int index)
    signal bypassAllToggled(bool on)

    color: Theme.panelBg
    readonly property bool textActive: labelPrompt.visible || nodeMenu.visible || addMenu.visible
    property real zoom: 1.0
    property bool stacked: true
    property bool lean: false            // light colour work: no AI mask, no shared grades
    readonly property int rowHeight: Theme.hControl + Theme.s3
    // What a correction is called: its own name, or its place in the stack.
    function title(i) {
        const label = nodes[i] !== undefined && nodes[i].label !== undefined ? nodes[i].label : ""
        return label !== "" && !/^Node \d+$/.test(label) ? label : qsTr("Layer %1").arg(i + 1)
    }
    function kindNote(kind) {
        return kind === "parallel" ? qsTr("Parallel mix") : kind === "layer" ? qsTr("Layer mix")
             : kind === "splitter" ? qsTr("One channel") : kind === "combiner" ? qsTr("Channels joined")
             : kind === "compound" ? qsTr("Group") : ""
    }
    function focusLayer(index) {
        const i = Math.max(0, Math.min(nodes.length - 1, index))
        stack.positionViewAtIndex(nodes.length - 1 - i, ListView.Contain)
        const row = stack.itemAtIndex(nodes.length - 1 - i)
        if (row) row.forceActiveFocus(Qt.TabFocusReason)
    }

    function copy() { return nodes.map(function (n) { var c = {}; for (var k in n) c[k] = n[k]; return c }) }
    function toggleEnabled(i) { if(!editable)return;var n = copy(); n[i].enabled = !(n[i].enabled !== false); nodesEdited(n) }
    function remove(i) {
        if(!editable)return
        var n=copy();if(n.length<=1)return
        var removed=n[i], replacement=removed.inputs || [i>0?n[i-1].id:"input"]
        n.splice(i,1)
        for(var j=0;j<n.length;++j) {
            if(n[j].inputs) { var links=[];for(var k=0;k<n[j].inputs.length;++k)links=links.concat(n[j].inputs[k]===removed.id?replacement:[n[j].inputs[k]]);n[j].inputs=links.filter(function(id,index){return links.indexOf(id)===index}) }
            if(n[j].keyInput===removed.id)n[j].keyInput=""
        }
        root.graphEdited(n,root.outputNode===removed.id?n[Math.min(i,n.length-1)].id:root.outputNode)
        nodeSelected(Math.min(i,n.length-1))
    }
    function connectTo(i) {
        if(!editable || connecting==="") { nodeSelected(i);root.settingsRequested();return }
        if(nodes[i].id===connecting)return
        var byId={};for(var j=0;j<nodes.length;++j)byId[nodes[j].id]=j
        var seen={}
        function depends(id) { if(id===root.nodes[i].id)return true;if(seen[id]||byId[id]===undefined)return false;seen[id]=true;var index=byId[id],n=root.nodes[index],links=n.inputs || [index>0?root.nodes[index-1].id:"input"];for(var k=0;k<links.length;++k)if(depends(links[k]))return true;return !!n.keyInput && depends(n.keyInput) }
        if(depends(connecting)){graphMessage=qsTr("That connection would create a cycle.");connecting="";return}
        graphMessage=""
        var n=copy();var links=n[i].inputs || [i>0?n[i-1].id:"input"]
        n[i].inputs=links.indexOf(connecting)>=0?links.filter(function(id){return id!==root.connecting}):[connecting]
        root.nodesEdited(n);connecting=""
    }
    function focusNode(index) {
        const cell = nodeRepeater.itemAt(Math.max(0, Math.min(nodes.length - 1, index)))
        if (cell) {
            cell.focusItem.forceActiveFocus(Qt.TabFocusReason)
            const x = cell.x * root.zoom
            if (x < canvas.contentX) canvas.contentX = x
            else if (x + cell.width * root.zoom > canvas.contentX + canvas.width)
                canvas.contentX = x + cell.width * root.zoom - canvas.width
        }
    }
    function frameSelected() { root.zoom=1;canvas.contentX=Math.max(0,Math.min(canvas.contentWidth-canvas.width,root.current*144-canvas.width/2+52)) }
    function frameAll() { root.zoom=Math.max(.25,Math.min(1,(canvas.width-16)/Math.max(104,root.nodes.length*144)));canvas.contentX=0 }
    onNodesChanged: wires.requestPaint()
    onZoomChanged: wires.requestPaint()
    onHeightChanged: wires.requestPaint()
    function move(i, delta, dragging) { if(!editable)return;var n = copy(); var j = i + delta; if (j < 0 || j >= n.length) return; var t = n[i]; n[i] = n[j]; n[j] = t; if (dragging === true) nodesDragged(n); else nodesEdited(n); nodeSelected(j) }
    function relabel(i, label) { if(!editable)return;var n = copy(); n[i].label = label; nodesEdited(n) }

    focus: true
    Keys.onEscapePressed: { connecting="";graphMessage="" }
    Item {
        id: header
        width: parent.width
        height: nodeActions.inBar ? 0 : Theme.hTab
        visible: !nodeActions.inBar
        Text { visible: root.dockBar === null; anchors.left: parent.left; anchors.leftMargin: Theme.s3; anchors.verticalCenter: parent.verticalCenter; text: root.pathLabel || qsTr("Nodes"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsHeading; font.weight: Theme.wHeading; color: Theme.textPrimary }
        DockBarActions {
            id: nodeActions
            dockBar: root.dockBar
            fallback: header
            IconButton { objectName: "leaveCompound"; visible: root.pathLabel!==""; iconName: "arrow-left"; text: qsTr("Parent graph"); onClicked: root.leaveRequested() }
            IconButton { visible: !root.lean; objectName: "faceObjectMask"; iconName: "wand-sparkles"; text: qsTr("AI mask: grade the people in the picture"); enabled:root.editable; onClicked:root.maskRequested() }
            IconButton { visible: !root.lean; objectName: "sharedGrades"; iconName: "link"; text: qsTr("Shared grades and remote versions"); enabled:root.editable; onClicked:root.sharingRequested() }
            IconButton { visible: !root.stacked; objectName: "nodeSettings"; iconName: "settings"; text: qsTr("Connections and key"); enabled:root.editable && root.current>=0; onClicked: root.settingsRequested() }
            IconButton { visible: !root.stacked; objectName: "nodeFrameSelected"; iconName: "circle-dot"; text: qsTr("Frame selected node"); onClicked: root.frameSelected() }
            IconButton { visible: !root.stacked; objectName: "nodeFrameAll"; iconName: "maximize"; text: qsTr("Frame all nodes"); onClicked: root.frameAll() }
            IconButton { objectName: "nodeAdd"; enabled:root.editable; iconName: "plus"; Accessible.name: root.stacked ? qsTr("Add layer") : qsTr("Add serial node"); onClicked: root.addRequested("serial") }
            IconButton { objectName: "copyGradeNode"; iconName: "copy"; text: root.stacked ? qsTr("Copy layer") : qsTr("Copy node"); commandId: "color.copy-node"; shortcut: "Ctrl+C"; enabled: root.nodes.length > 0; onClicked: root.copyRequested("node") }
            IconButton { objectName: "pasteGradeNode"; iconName: "clipboard"; text: root.stacked ? qsTr("Paste layer above the selected one") : qsTr("Paste node after selected"); commandId: "color.paste-node"; shortcut: "Ctrl+V"; enabled: root.editable && root.hasCopiedNode; onClicked: root.pasteRequested("node") }
            IconButton { visible: !root.stacked; iconName: "layout-grid"; Accessible.name: qsTr("Add node…"); onClicked: addMenu.popup() }
            IconButton { objectName: "nodeDelete"; iconName: "trash-2"; enabled: root.editable && root.current >= 0 && root.nodes.length > 1; Accessible.name: root.stacked ? qsTr("Delete layer") : qsTr("Delete node"); onClicked: root.remove(root.current) }
            IconButton { id: optionsButton; objectName: "nodeMenuButton"; iconName: "more-horizontal"; text: root.stacked ? qsTr("Layer and grade options") : qsTr("Node and grade options"); onClicked: nodeMenu.popup(optionsButton, optionsButton.width - nodeMenu.width, optionsButton.height) }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }

    ListView {
        id: stack
        objectName: "layerStack"
        visible: root.stacked
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s1
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: ScrollBar {}
        model: root.stacked ? root.nodes.length : 0
        // A right-click below the layers offers a new one.
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.ReleaseWithinBounds
            enabled: root.editable
            onTapped: function (ev) { nodeMenu.popup(stack, ev.position.x, ev.position.y) }
        }
        delegate: Rectangle {
            id: layer
            required property int index
            // The top row is the last correction applied.
            readonly property int at: root.nodes.length - 1 - index
            readonly property var node: root.nodes[at] !== undefined ? root.nodes[at] : ({})
            readonly property bool on: node.enabled !== false
            readonly property bool selected: root.current === at
            readonly property string note: root.kindNote(node.kind !== undefined ? node.kind : "serial")
            objectName: "layer:" + at
            width: stack.width
            height: root.rowHeight
            radius: Theme.rControl
            color: selected ? Theme.controlBg : "transparent"
            border.color: activeFocus || selected ? Theme.accent : "transparent"
            border.width: activeFocus ? Theme.focusRing : selected ? Theme.hairline : 0
            activeFocusOnTab: true
            readonly property bool keyboardConsumesKeys: true
            Accessible.role: Accessible.ListItem
            Accessible.selectable: true
            Accessible.selected: selected
            Accessible.name: root.title(at) + (note !== "" ? ", " + note : "")
            Accessible.description: (on ? qsTr("Shown. ") : qsTr("Hidden. "))
                                    + qsTr("Arrows select layers; Alt and an arrow moves this one; Space hides or shows it; F2 renames; Delete removes; Menu opens actions.")
            Accessible.onPressAction: root.nodeSelected(at)
            onActiveFocusChanged: if (activeFocus) root.nodeSelected(at)
            Keys.onShortcutOverride: function (event) {
                if (event.key === Qt.Key_F2 || (event.modifiers & Qt.AltModifier
                    && (event.key === Qt.Key_Up || event.key === Qt.Key_Down))) event.accepted = true
            }
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    const delta = event.key === Qt.Key_Up ? 1 : -1 // up the stack is later in the order
                    const destination = layer.at + delta
                    if (event.modifiers & Qt.AltModifier) { root.move(layer.at, delta); Qt.callLater(function () { root.focusLayer(destination) }) }
                    else root.focusLayer(destination)
                } else if (event.key === Qt.Key_Space) root.toggleEnabled(layer.at)
                else if (event.key === Qt.Key_F2) { labelPrompt.target = layer.at; labelPrompt.open() }
                else if (event.key === Qt.Key_Delete) root.remove(layer.at)
                else if (event.key === Qt.Key_Menu) { root.nodeSelected(layer.at); nodeMenu.popup(layer, layer.width / 2, layer.height / 2) }
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) root.nodeSelected(layer.at)
                else { event.accepted = false; return }
                event.accepted = true
            }
            IconButton {
                id: eye
                objectName: "layerEye:" + layer.at
                x: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                iconName: layer.on && !root.bypassAll ? "eye" : "eye-off"
                iconColor: layer.on && !root.bypassAll ? Theme.textSecondary : Theme.textMuted
                text: layer.on ? qsTr("Hide this layer") : qsTr("Show this layer")
                enabled: root.editable
                onClicked: root.toggleEnabled(layer.at)
            }
            Rectangle {
                id: picture
                anchors.left: eye.right
                anchors.leftMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                height: parent.height - 2 * Theme.s1
                width: Math.round(height * 16 / 9)
                color: Theme.meterWell
                border.color: Theme.border
                border.width: Theme.hairline
                opacity: layer.on && !root.bypassAll ? 1 : 0.45
                clip: true
                Image { anchors.fill: parent; anchors.margins: Theme.hairline; source: root.thumb; fillMode: Image.PreserveAspectCrop; asynchronous: true; visible: status === Image.Ready }
            }
            Column {
                anchors.left: picture.right
                anchors.leftMargin: Theme.s2
                anchors.right: badges.left
                anchors.rightMargin: Theme.s1
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    width: parent.width
                    text: root.title(layer.at)
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsBase
                    color: layer.on && !root.bypassAll ? Theme.textPrimary : Theme.textMuted
                }
                Text {
                    width: parent.width
                    visible: layer.note !== ""
                    text: layer.note
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: Theme.textMuted
                }
            }
            Row {
                id: badges
                anchors.right: parent.right
                anchors.rightMargin: Theme.s2
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s1
                Icon { name: "circle-dot"; size: 12; color: Theme.textSecondary
                       visible: (layer.node.window !== undefined && layer.node.window.enabled === true) || (layer.node.qualifier !== undefined && layer.node.qualifier.enabled === true) }
                Icon { name: "diamond"; size: 12; color: Theme.accent
                       visible: layer.node.keyframes !== undefined && Object.keys(layer.node.keyframes).length > 0 }
            }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: root.nodeSelected(layer.at)
                onDoubleTapped: { labelPrompt.target = layer.at; labelPrompt.open() }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: function (ev) { root.nodeSelected(layer.at); nodeMenu.popup(layer, ev.position.x, ev.position.y) }
            }
            DragHandler { // dragged past the layer above or below, it changes places with it
                target: null
                enabled: root.editable
                yAxis.enabled: true
                xAxis.enabled: false
                property real taken: 0
                property bool moved: false
                function finish() { if (moved) { moved = false; root.dragFinished() } }
                onActiveChanged: if (active) { taken = 0; moved = false; root.nodeSelected(layer.at) } else finish()
                Component.onDestruction: finish()
                onTranslationChanged: {
                    if (!active) return
                    const step = root.rowHeight + stack.spacing, d = translation.y - taken
                    if (d > step * 0.6 && root.current > 0) { taken += step; moved = true; root.move(root.current, -1, true) }
                    else if (d < -step * 0.6 && root.current < root.nodes.length - 1) { taken -= step; moved = true; root.move(root.current, 1, true) }
                }
            }
        }
    }
    Flickable {
        id: canvas
        visible: !root.stacked
        objectName: "nodeCanvas"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s2
        contentWidth: Math.max(width, row.width * root.zoom + Theme.s4)
        contentHeight: Math.max(height, row.height * root.zoom)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.horizontal: ScrollBar {}
        C.ScrollBar.vertical: ScrollBar {}
        // A right-click on the empty graph offers a new node too.
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.ReleaseWithinBounds
            enabled: root.editable
            onTapped: function (ev) { addMenu.popup(canvas, ev.position.x, ev.position.y) }
        }
        Item {
            width: row.width * root.zoom
            height: canvas.contentHeight
            Canvas {
                id: wires
                anchors.fill: parent
                onPaint: {
                    var ctx=getContext("2d");ctx.reset();ctx.strokeStyle=String(Theme.audio);ctx.lineWidth=1.5
                    var byId={};for(var i=0;i<root.nodes.length;++i)byId[root.nodes[i].id]=i
                    var y=(height-row.height*root.zoom)/2+26*root.zoom
                    for(var j=0;j<root.nodes.length;++j) {
                        var links=root.nodes[j].inputs || [j>0?root.nodes[j-1].id:"input"]
                        for(var k=0;k<links.length;++k){var from=byId[links[k]],x1=from===undefined?0:(from*144+96)*root.zoom,x2=(j*144+8)*root.zoom
                            ctx.beginPath();ctx.moveTo(x1,y);var rise=from===j-1?y:Math.max(4,y-25-(j%3)*9);ctx.bezierCurveTo(x1+18,rise,x2-18,rise,x2,y);ctx.stroke()}
                    }
                }
            }
            Row {
                id: row
                scale: root.zoom
                transformOrigin: Item.TopLeft
                y: (parent.height - height * root.zoom) / 2
                spacing: 0
                Repeater {
                    id: nodeRepeater
                    model: root.stacked ? 0 : root.nodes.length
                    delegate: Row {
                        id: cell
                        property alias focusItem: nodeCard
                        required property int index
                        readonly property var node: root.nodes[index] !== undefined ? root.nodes[index] : ({})
                        readonly property bool on: node.enabled !== false
                        readonly property bool selected: root.current === index
                        spacing: 0
                        NodeItem {
                            id: nodeCard
                            objectName: "node:" + cell.index
                            number: cell.index + 1
                            label: cell.node.label !== undefined ? cell.node.label : ""
                            kind: cell.node.kind !== undefined ? cell.node.kind : "serial"
                            enabled2: cell.on && !root.bypassAll
                            selected: cell.selected
                            thumb: root.thumb
                            hasMask: (cell.node.window !== undefined && cell.node.window.enabled === true) || (cell.node.qualifier !== undefined && cell.node.qualifier.enabled === true)
                            hasKeyframes: cell.node.keyframes !== undefined && Object.keys(cell.node.keyframes).length > 0
                            onInputClicked: root.connectTo(cell.index)
                            onOutputClicked: { root.connecting = cell.node.id; root.nodeSelected(cell.index) }
                            onClicked: root.nodeSelected(cell.index)
                            onDoubleClicked: { labelPrompt.target = cell.index; labelPrompt.open() }
                            onToggled: root.toggleEnabled(cell.index)
                            onMoved: function (delta) {
                                const destination = cell.index + delta
                                root.move(cell.index, delta)
                                Qt.callLater(function() { root.focusNode(destination) })
                            }
                            onNavigationRequested: function(delta) { root.focusNode(cell.index + delta) }
                            onRemoveRequested: root.remove(cell.index)
                            onContextRequested: function (x, y) { root.nodeSelected(cell.index); nodeMenu.popup(nodeCard, x, y) }
                        }
                        Item { // the wire
                            width: cell.index < root.nodes.length - 1 ? 40 : 0
                            height: 1
                            anchors.verticalCenter: parent.verticalCenter
                            visible: cell.index < root.nodes.length - 1

                        }
                    }
                }
            }
        }
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: function (e) { root.zoom = Math.max(0.5, Math.min(2, root.zoom * (e.angleDelta.y > 0 ? 1.1 : 1 / 1.1))) }
        }
    }
    Text { anchors.left: parent.left; anchors.top: header.bottom; anchors.margins: Theme.s2; visible: root.connecting!=="" || root.graphMessage!==""; text: root.graphMessage || qsTr("Click another node's input port to connect. Esc cancels."); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: root.graphMessage?Theme.warning:Theme.accent }
    // Mini-map: the visible window over the whole chain.
    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s2
        width: 80
        height: 18
        radius: Theme.rControl
        color: Theme.controlBg
        border.color: Theme.border
        border.width: Theme.hairline
        visible: !root.stacked && canvas.contentWidth > canvas.width + 1
        Rectangle {
            x: 1 + (parent.width - 2) * canvas.contentX / Math.max(1, canvas.contentWidth)
            y: 1
            width: Math.max(6, (parent.width - 2) * canvas.width / Math.max(1, canvas.contentWidth))
            height: parent.height - 2
            radius: 2
            color: Theme.accent
            opacity: 0.6
        }
    }
    EmptyState {
        anchors.centerIn: parent
        visible: root.nodes.length === 0
        iconName: "git-branch"
        title: root.stacked ? qsTr("No layers") : qsTr("No node")
        description: root.stacked ? qsTr("Select a clip; its grade starts with one layer.") : qsTr("Select a clip; its grade starts with one serial node.")
    }

    ContextMenu {
        id: addMenu
        MenuAction { text: qsTr("Serial Node"); onTriggered: root.addRequested("serial") }
        MenuAction { text: qsTr("Parallel Node"); onTriggered: root.addRequested("parallel") }
        MenuAction { text: qsTr("Layer Node"); onTriggered: root.addRequested("layer") }
        MenuAction { text: qsTr("RGB Splitter / Combiner"); onTriggered: root.addRequested("splitter") }
        MenuAction { text: qsTr("Nested Node"); onTriggered: root.addRequested("compound") }
    }
    ContextMenu {
        id: nodeMenu
        // A new node after the one the menu was opened on.
        MenuAction { objectName: "addSerialNodeAction"; text: root.stacked ? qsTr("Add Layer") : qsTr("Add Serial Node"); iconName: "plus"; enabled: root.editable; onTriggered: root.addRequested("serial") }
        MenuAction { objectName: "addParallelNodeAction"; showInMenu: !root.stacked; text: qsTr("Add Parallel Node"); enabled: root.editable; onTriggered: root.addRequested("parallel") }
        MenuAction { objectName: "addLayerNodeAction"; showInMenu: !root.stacked; text: qsTr("Add Layer Node"); enabled: root.editable; onTriggered: root.addRequested("layer") }
        C.MenuSeparator {}
        MenuAction { objectName: "copyGradeNodeAction"; text: root.stacked ? qsTr("Copy Layer") : qsTr("Copy Node"); iconName: "copy"; commandId: "color.copy-node"; shortcut: "Ctrl+C"; enabled: root.nodes.length > 0; onTriggered: root.copyRequested("node") }
        MenuAction { objectName: "pasteGradeNodeAction"; text: root.stacked ? qsTr("Paste Layer") : qsTr("Paste Node"); iconName: "clipboard"; commandId: "color.paste-node"; shortcut: "Ctrl+V"; enabled: root.editable && root.hasCopiedNode; onTriggered: root.pasteRequested("node") }
        MenuAction { objectName: "copyCompleteGradeAction"; text: qsTr("Copy Grade"); iconName: "copy"; commandId: "color.copy-grade"; shortcut: "Ctrl+Alt+C"; enabled: root.nodes.length > 0; onTriggered: root.copyRequested("grade") }
        MenuAction { objectName: "pasteCompleteGradeAction"; text: qsTr("Paste Grade"); iconName: "clipboard"; commandId: "color.paste-grade"; shortcut: "Ctrl+Alt+P"; enabled: root.editable && root.hasCopiedGrade; onTriggered: root.pasteRequested("grade") }
        C.MenuSeparator {}
        MenuAction { showInMenu: !root.stacked; text: qsTr("Connections and Key…"); enabled:root.editable; onTriggered: root.settingsRequested() }
        MenuAction { showInMenu: !root.stacked; text: qsTr("Use as Output"); enabled:root.editable && root.current>=0 && root.current<root.nodes.length;onTriggered:root.outputRequested(root.nodes[root.current].id) }
        MenuAction { showInMenu: !root.stacked; text: qsTr("Disconnect Input"); enabled:root.editable && root.current>=0 && root.current<root.nodes.length;onTriggered:{var n=root.copy();n[root.current].inputs=[];root.nodesEdited(n)} }
        MenuAction { showInMenu: !root.stacked; text: qsTr("Nest All Nodes"); enabled:root.editable;onTriggered:root.compoundRequested() }
        MenuAction { showInMenu: !root.stacked; text: qsTr("Edit Nested Node…");enabled:root.editable && root.current>=0 && root.current<root.nodes.length && root.nodes[root.current].kind==="compound";onTriggered:root.editCompoundRequested() }
        MenuAction { text: root.stacked ? qsTr("Save Layer Preset…") : qsTr("Save Node Preset…");enabled:root.current>=0;onTriggered:root.presetRequested("save") }
        MenuAction { text: root.stacked ? qsTr("Layer Presets…") : qsTr("Node Presets…");onTriggered:root.presetRequested("browse") }
        C.MenuSeparator {}
        MenuAction { text: root.stacked ? qsTr("Rename…") : qsTr("Label…"); iconName: "pencil"; enabled: root.current >= 0; onTriggered: { labelPrompt.target = root.current; labelPrompt.open() } }
        MenuAction { text: root.current >= 0 && root.nodes[root.current] !== undefined && root.nodes[root.current].enabled === false ? (root.stacked ? qsTr("Show Layer") : qsTr("Enable Node")) : (root.stacked ? qsTr("Hide Layer") : qsTr("Bypass Node")); iconName: "eye-off"; enabled: root.current >= 0; onTriggered: root.toggleEnabled(root.current) }
        MenuAction { text: root.stacked ? qsTr("Move Up") : qsTr("Move Right"); iconName: root.stacked ? "chevron-up" : "chevron-right"; enabled: root.editable && root.current >= 0 && root.current < root.nodes.length - 1; onTriggered: root.move(root.current, 1) }
        MenuAction { text: root.stacked ? qsTr("Move Down") : qsTr("Move Left"); iconName: root.stacked ? "chevron-down" : "chevron-left"; enabled: root.editable && root.current > 0; onTriggered: root.move(root.current, -1) }
        MenuAction { objectName: "resetGradeNodeAction"; text: root.stacked ? qsTr("Reset Layer") : qsTr("Reset Node"); iconName: "rotate-ccw"; enabled: root.editable && root.current >= 0 && root.current < root.nodes.length; onTriggered: root.resetRequested(root.current) }
        C.MenuSeparator {}
        MenuAction { text: root.bypassAll ? qsTr("Enable All Grades") : qsTr("Bypass All Grades"); onTriggered: root.bypassAllToggled(!root.bypassAll) }
        MenuAction { text: root.stacked ? qsTr("Delete Layer") : qsTr("Delete Node"); iconName: "trash-2"; enabled: root.editable && root.current >= 0 && root.nodes.length > 1; onTriggered: root.remove(root.current) }
    }
    FormDialog {
        width: Math.min(420, parent ? parent.width - Theme.s5 * 2 : 420)
        id: labelPrompt
        property int target: -1
        title: root.stacked ? qsTr("Layer Name") : qsTr("Node Label")
        modal: true
        standardButtons: C.Dialog.Ok | C.Dialog.Cancel
        onOpened: { labelField.text = target >= 0 && root.nodes[target] !== undefined ? root.nodes[target].label : ""; labelField.forceActiveFocus(); labelField.selectAll() }
        onAccepted: if (target >= 0) root.relabel(target, labelField.text)
        LineEdit { id: labelField; Layout.fillWidth: true; hoverEnabled: true; tip: root.stacked ? qsTr("Name shown on this layer") : qsTr("Name shown on this node in the graph"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase }
    }
}
