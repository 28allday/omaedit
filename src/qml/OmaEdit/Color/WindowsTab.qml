pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// Shape masks: a shape (circle, rectangle, polygon, diamond, gradient),
// its position / size / rotation / softness, invert, on / off, and the
// overlay toggle for the viewer. Edits the node's `window` map (fractions
// of the frame). Additional windows can add, subtract or intersect.
Item {
    id: root

    property var node: ({})
    property var displayedWindow: null
    property int current: 0
    signal selected(int index)
    signal windowsEdited(var windows)
    property bool showOverlay: true
    signal windowEdited(var window, int index)
    signal overlayToggled(bool on)
    signal committed()

    readonly property var extra: node.windows || []
    readonly property var w: displayedWindow !== null ? displayedWindow : current > 0 && extra[current-1] ? extra[current-1] : node.window || ({})
    property int vertex: 0
    onCurrentChanged: vertex = 0
    onVerticesChanged: vertex = Math.max(0, Math.min(vertex, vertices.length - 1))
    readonly property var vertices: w.points && w.points.length>=3 ? w.points : [[0,-1],[1,1],[-1,1]]
    readonly property bool on: w.enabled === true
    readonly property var shapes: [
        { key: "circle", icon: "circle", label: qsTr("Circle") },
        { key: "rect", icon: "square", label: qsTr("Rectangle") },
        { key: "polygon", icon: "git-branch", label: qsTr("Polygon") },
        { key: "diamond", icon: "diamond", label: qsTr("Diamond") },
        { key: "gradient", icon: "panel-left", label: qsTr("Gradient") }
    ]
    function put(key, value) {
        var m = {}
        for (var k in w) m[k] = w[k]
        m[key] = value
        root.windowEdited(m, root.current)
    }

    // Three columns share the panel width; the sliders stretch with them.
    readonly property real usable: width - 2 * Theme.s2 - 4 * Theme.s3 - 2 * Theme.hairline
    readonly property real shapeWidth: Math.max(200, Math.min(320, usable * 0.24))
    readonly property real sideWidth: Math.max(170, Math.min(420, usable * 0.30, usable - shapeWidth - 300))
    readonly property real slidersWidth: Math.max(300, Math.min(640, usable - shapeWidth - sideWidth))
    // The narrowest the three columns work at (the Masks page adds the AI
    // mask and the tracker to this to fit a 1280-wide window).
    readonly property real minimumWidth: 200 + 170 + 300 + 2 * Theme.s2 + 4 * Theme.s3 + 2 * Theme.hairline
    readonly property real rowSpacing: Math.max(Theme.s1, Math.min(Theme.s3, (height - 2 * Theme.s2 - 6 * Theme.hControl) / 5))

    Row {
        anchors.fill: parent
        anchors.margins: Theme.s2
        spacing: Theme.s3

        Column {
            width: root.shapeWidth
            spacing: Theme.s2
            Toggle { objectName: "windowOn"; label: qsTr("Shape Mask"); tip: qsTr("Limit this layer's correction to the shape you draw"); checked: root.on; onToggled: { root.put("enabled", checked); root.committed() } }
            Row {
                spacing: Theme.s1
                ComboField { objectName: "windowSelect"; width: 110; tip: qsTr("Which of this layer's windows the controls below edit"); model: [qsTr("Window 1")].concat(root.extra.map(function(w,i) { return qsTr("Window %1").arg(i+2) })); currentIndex: root.current; onActivated: function(i) { root.selected(i) } }
                IconButton { objectName: "windowAdd"; iconName: "plus"; text: qsTr("Add window"); onClicked: { var list=root.extra.slice(); list.push({enabled:true,shape:"circle",x:.5,y:.5,w:.3,h:.3,softness:.1,combine:"add"}); root.windowsEdited(list); root.selected(list.length); root.committed() } }
                IconButton { iconName: "minus"; text: qsTr("Remove window"); enabled: root.current>0; onClicked: { var list=root.extra.slice();list.splice(root.current-1,1);root.windowsEdited(list);root.selected(0);root.committed() } }
            }
            ComboField { width: 190; tip: qsTr("How this window combines with the ones before it"); visible: root.current > 0; model: [qsTr("Add"),qsTr("Intersect"),qsTr("Subtract")]; currentIndex: ["add","intersect","subtract"].indexOf(root.w.combine || "intersect"); onActivated: function(i) { root.put("combine",["add","intersect","subtract"][i]);root.committed() } }
            Text { text: qsTr("Shape"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.textSecondary }
            Row {
                spacing: Theme.s1
                Repeater {
                    model: root.shapes.length
                    delegate: IconButton {
                        id: shapeButton
                        required property int index
                        readonly property var s: root.shapes[index]
                        objectName: "shape:" + s.key
                        iconName: s.icon
                        checked: root.w.shape === s.key
                        enabled: root.on
                        Accessible.name: s.label
                        onClicked: { root.put("shape", s.key); root.committed() }
                    }
                }
            }
            Toggle { label: qsTr("Invert"); tip: qsTr("Grade everything outside the window instead of inside it"); enabled: root.on; checked: root.w.invert === true; onToggled: { root.put("invert", checked); root.committed() } }
            Toggle { objectName: "windowOverlay"; label: qsTr("Show overlay in viewer"); tip: qsTr("Draw the window outline over the viewer picture"); checked: root.showOverlay; onToggled: root.overlayToggled(checked) }
        }

        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.border }

        Column {
            width: root.slidersWidth
            spacing: root.rowSpacing
            SliderField { liveEdit: true; objectName: "windowX"; width: parent.width; label: qsTr("Position X"); tip: qsTr("Horizontal centre of the window across the frame"); from: 0; to: 1; stepSize: 0.005; decimals: 3; enabled: root.on; value: root.w.x !== undefined ? root.w.x : 0.5; onEdited: function (v) { root.put("x", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Position Y"); tip: qsTr("Vertical centre of the window down the frame"); from: 0; to: 1; stepSize: 0.005; decimals: 3; enabled: root.on; value: root.w.y !== undefined ? root.w.y : 0.5; onEdited: function (v) { root.put("y", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Width"); tip: qsTr("Width of the window as a fraction of the frame"); from: 0.02; to: 2; stepSize: 0.005; decimals: 3; enabled: root.on; value: root.w.w !== undefined ? root.w.w : 0.4; onEdited: function (v) { root.put("w", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Height"); tip: qsTr("Height of the window as a fraction of the frame"); from: 0.02; to: 2; stepSize: 0.005; decimals: 3; enabled: root.on; value: root.w.h !== undefined ? root.w.h : 0.4; onEdited: function (v) { root.put("h", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Rotation"); tip: qsTr("Turns the window around its centre, in degrees"); from: -180; to: 180; stepSize: 1; decimals: 0; suffix: "°"; origin: 0; enabled: root.on; value: root.w.rotation !== undefined ? root.w.rotation : 0; onEdited: function (v) { root.put("rotation", v) }; onEditingFinished: root.committed() }
            SliderField { liveEdit: true; width: parent.width; label: qsTr("Softness"); tip: qsTr("Feathers the window edge so the grade fades out"); from: 0; to: 1; stepSize: 0.01; decimals: 2; enabled: root.on; value: root.w.softness !== undefined ? root.w.softness : 0.1; onEdited: function (v) { root.put("softness", v) }; onEditingFinished: root.committed() }
        }

        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.border }

        Column {
            width: root.sideWidth
            spacing: Theme.s2
            Column {
                visible:root.w.shape==="polygon"
                enabled:root.on
                width:parent.width;spacing:Theme.s1
                Row {
                    spacing:Theme.s1
                    ComboField { objectName:"polygonVertex";width:110;tip:qsTr("Which polygon corner the X and Y sliders edit");model:root.vertices.map(function(p,i){return qsTr("Vertex %1").arg(i+1)});currentIndex:Math.min(root.vertex,root.vertices.length-1);onActivated:function(i){root.vertex=i} }
                    IconButton { objectName:"polygonAdd";iconName:"plus";text:qsTr("Add vertex");enabled:root.vertices.length<64;onClicked:{var p=root.vertices.slice(),i=Math.min(root.vertex,p.length-1),b=p[(i+1)%p.length];p.splice(i+1,0,[(p[i][0]+b[0])/2,(p[i][1]+b[1])/2]);root.put("points",p);root.vertex=i+1;root.committed()} }
                    IconButton { objectName:"polygonRemove";iconName:"minus";text:qsTr("Remove vertex");enabled:root.vertices.length>3;onClicked:{var p=root.vertices.slice();p.splice(root.vertex,1);root.vertex=Math.max(0,root.vertex-1);root.put("points",p);root.committed()} }
                }
                Repeater {
                    model:2
                    delegate:SliderField { liveEdit: true;
                        required property int index
                        width:parent.width;tip:qsTr("Position of the selected polygon corner in the frame");label:index===0?qsTr("Vertex X"):qsTr("Vertex Y");from:-1;to:1;decimals:3;stepSize:.01
                        value:root.vertices[Math.min(root.vertex,root.vertices.length-1)][index]
                        onEdited:function(v){var points=JSON.parse(JSON.stringify(root.vertices));points[Math.min(root.vertex,points.length-1)][index]=v;root.put("points",points)}
                        onEditingFinished:root.committed()
                    }
                }
            }
            ToolButton { objectName: "windowReset"; showLabel: true; iconName: "rotate-ccw"; text: qsTr("Reset"); tip: qsTr("Reset this window's shape adjustments and clear its tracking path"); onClicked: { var m = {}; for (var k in root.w) m[k] = root.w[k]; m.x = 0.5; m.y = 0.5; m.w = 0.4; m.h = 0.4; m.rotation = 0; m.softness = 0.1; m.invert = false; delete m.track; delete m.points; root.vertex = 0; root.windowEdited(m, root.current); root.committed() } }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.w.track !== undefined && root.w.track.length > 1
                      ? qsTr("Tracked: %1 positions ride the clip. Drag the overlay to offset the whole path.").arg(root.w.track.length)
                      : qsTr("A window combines with the colour pick on the same layer (the grade lands only where both agree).")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsLabel
                color: Theme.textMuted
            }
        }
    }
}
