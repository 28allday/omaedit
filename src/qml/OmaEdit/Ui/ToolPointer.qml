import QtQuick
import omaedit

// The pointer of the active timeline tool, drawn from the tool's toolbar
// icon so the pointer and the button match. Fill the area the tool works
// over; it takes no clicks. Tools with a stock pointer (select, hand, zoom,
// range) or none listed here leave the pointer alone. The Trim tool does
// several things by where a press lands, so its pointer follows the zone
// under it: `zoneAt(x, y)` in this item's coordinates answers "in", "out",
// "rollIn", "rollOut", "slip", "slide" or "". The Select tool keeps its stock
// pointer but on a cut it can drag, where it shows the roll too. `heldZone`
// is a zone to show wherever the pointer is, while a drag that began in it
// lasts.
ToolCursor {
    id: root
    property string tool: "select"
    property var zoneAt: null
    readonly property var icons: ({ blade: "scissors", trim: "move-horizontal", slip: "arrow-left-right",
                                    slide: "move", trackselect: "track-select" })
    readonly property var zoneIcons: ({ "in": "arrow-left-to-line", out: "arrow-right-to-line",
                                        rollIn: "unfold-horizontal", rollOut: "unfold-horizontal",
                                        slip: "arrow-left-right", slide: "move" })
    property string heldZone: ""
    readonly property string zone: heldZone !== "" ? heldZone
                                   : (tool === "trim" || tool === "select") && zoneAt !== null && hover.hovered
                                   ? zoneAt(hover.point.position.x, hover.point.position.y) : ""
    readonly property string iconName: zoneIcons[zone] !== undefined ? zoneIcons[zone]
                                     : icons[tool] !== undefined ? icons[tool] : ""
    // (Under the Select tool it is there with no pointer of its own, so that
    // it knows where the mouse is and can show the roll over a cut.)
    visible: iconName !== "" || tool === "select"
    // The blade's pointer: whole blades crossed at the centre, pointing down
    // at the clip, and the cut line running through the crossing. The hot
    // spot is the crossing, on the line, so the cut lands where it points.
    readonly property var pointerShapes: ({ scissors: '<circle cx="6" cy="6" r="3"/><circle cx="18" cy="6" r="3"/><path d="M8.12 8.12 20 20"/><path d="M15.88 8.12 4 20"/><path d="M12 1v22"/>' })
    shape: pointerShapes[iconName] !== undefined ? pointerShapes[iconName]
         : iconName !== "" && Icons.shapes[iconName] !== undefined ? Icons.shapes[iconName] : ""
    size: 24
    // Where the pointer is over this item, for marks that follow it.
    readonly property bool hovered: hover.hovered
    readonly property point hoverPosition: hover.point.position
    HoverHandler { id: hover }
}
