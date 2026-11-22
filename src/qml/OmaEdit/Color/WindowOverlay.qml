import QtQuick
import OmaEdit.Ui

// The shape mask drawn over the picture: the shape outline with a
// softness ring, a centre handle to move it, corner handles to size it.
// Coordinates are fractions of the frame, like the grade's window map.
Item {
    id: root

    property var window: ({})
    signal edited(var window)
    signal committed()

    readonly property var points: window.points && window.points.length>=3 ? window.points : [[0,-1],[1,1],[-1,1]]
    readonly property real wx: window.x !== undefined ? window.x : 0.5
    readonly property real wy: window.y !== undefined ? window.y : 0.5
    readonly property real ww: window.w !== undefined ? window.w : 0.4
    readonly property real wh: window.h !== undefined ? window.h : 0.4
    readonly property real soft: window.softness !== undefined ? window.softness : 0.1
    readonly property string shape: window.shape !== undefined ? window.shape : "circle"
    readonly property real rot: window.rotation !== undefined ? window.rotation : 0

    function put(x, y, w, h) {
        var m = {}
        for (var k in window) m[k] = window[k]
        m.x = Math.max(0, Math.min(1, x)); m.y = Math.max(0, Math.min(1, y))
        m.w = Math.max(0.02, Math.min(2, w)); m.h = Math.max(0.02, Math.min(2, h))
        root.edited(m)
    }

    Item {
        id: box
        x: (root.wx - root.ww / 2) * root.width
        y: (root.wy - root.wh / 2) * root.height
        width: root.ww * root.width
        height: root.wh * root.height
        rotation: root.rot
        Canvas {
            id: shape
            anchors.fill: parent
            anchors.margins: -Theme.s4
            Connections { target: Theme; function onAccentChanged() { shape.requestPaint() } } // the desktop accent can change while this is on screen
            readonly property string k: root.shape + root.soft + width + height + JSON.stringify(root.points)
            onKChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var m = Theme.s4
                var w = width - 2 * m, h = height - 2 * m
                var draw = function (inset, color, dash) {
                    ctx.beginPath()
                    ctx.strokeStyle = String(color)
                    ctx.lineWidth = 1.5
                    ctx.setLineDash(dash ? [4, 4] : [])
                    var x = m - inset, y = m - inset, ww = w + 2 * inset, hh = h + 2 * inset
                    if (root.shape === "circle") ctx.ellipse(x, y, ww, hh)
                    else if (root.shape === "diamond") { ctx.moveTo(x + ww / 2, y); ctx.lineTo(x + ww, y + hh / 2); ctx.lineTo(x + ww / 2, y + hh); ctx.lineTo(x, y + hh / 2); ctx.closePath() }
                    else if (root.shape === "polygon") { for(var i=0;i<root.points.length;++i){var px=x+(root.points[i][0]+1)*ww/2,py=y+(root.points[i][1]+1)*hh/2;if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py)}ctx.closePath() }
                    else if (root.shape === "gradient") { ctx.moveTo(x+ww/2, y); ctx.lineTo(x+ww/2, y + hh) }
                    else ctx.rect(x, y, ww, hh)
                    ctx.stroke()
                }
                draw(0, Theme.textPrimary, false)
                draw(-root.soft * Math.min(w, h) * 0.5, Theme.accent, true)
            }
        }
        Rectangle { // centre handle
            anchors.centerIn: parent
            width: 12; height: 12; radius: 6
            color: Theme.textPrimary
            border.color: Theme.windowBg
            border.width: 2
        }
        DragHandler { // move
            target: null
            property real sx: 0
            property real sy: 0
            onActiveChanged: if (active) { sx = root.wx; sy = root.wy } else root.committed()
            onTranslationChanged: if (active) root.put(sx + translation.x / root.width, sy + translation.y / root.height, root.ww, root.wh)
        }
        Repeater {
            model:root.shape==="polygon"?root.points.length:0
            delegate:Rectangle {
                id:vertexHandle
                required property int index
                objectName:"windowVertex:"+index
                x:(root.points[index][0]+1)*box.width/2-width/2;y:(root.points[index][1]+1)*box.height/2-height/2
                width:12;height:12;radius:6;color:Theme.accent;border.color:Theme.windowBg;border.width:2
                DragHandler {
                    target:null
                    property var initial:[]
                    onActiveChanged:if(active)initial=root.points[vertexHandle.index].slice();else root.committed()
                    onTranslationChanged:if(active){
                        var a=-root.rot*Math.PI/180,dx=Math.cos(a)*translation.x-Math.sin(a)*translation.y,dy=Math.sin(a)*translation.x+Math.cos(a)*translation.y
                        var points=JSON.parse(JSON.stringify(root.points));points[vertexHandle.index]=[Math.max(-1,Math.min(1,initial[0]+2*dx/box.width)),Math.max(-1,Math.min(1,initial[1]+2*dy/box.height))]
                        var w=Object.assign({},root.window,{points:points});root.edited(w)
                    }
                }
            }
        }
        Repeater { // size handles
            model: [{ fx: 1, fy: 0.5 }, { fx: 0.5, fy: 1 }, { fx: 1, fy: 1 }]
            delegate: Rectangle {
                required property var modelData
                objectName: "windowSize:" + modelData.fx + ":" + modelData.fy
                x: parent.width * modelData.fx - width / 2
                y: parent.height * modelData.fy - height / 2
                width: 10; height: 10; radius: Theme.rControl
                color: Theme.accent
                border.color: Theme.windowBg
                border.width: 1
                DragHandler {
                    target: null
                    property real sw: 0
                    property real sh: 0
                    onActiveChanged: if (active) { sw = root.ww; sh = root.wh } else root.committed()
                    onTranslationChanged: {
                        if (!active) return
                        var a = -root.rot * Math.PI / 180
                        var dx = Math.cos(a) * translation.x - Math.sin(a) * translation.y
                        var dy = Math.sin(a) * translation.x + Math.cos(a) * translation.y
                        var w = modelData.fx === 1 ? sw + 2 * dx / root.width : sw
                        var h = modelData.fy === 1 ? sh + 2 * dy / root.height : sh
                        root.put(root.wx, root.wy, w, h)
                    }
                }
            }
        }
    }
}
