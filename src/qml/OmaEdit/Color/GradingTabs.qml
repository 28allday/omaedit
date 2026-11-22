pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import OmaEdit.Ui

// The grading controls under the clip strip: a tab bar — Primaries (with the
// qualifier beside or below them), Curves, Masks (shapes, the AI mask and the
// tracker), HDR, Hue Mesh — and the tab. Every tab edits ONE part of the current node and hands the whole
// node back.
Rectangle {
    id: root

    property var node: ({})
    property var displayedWindow: null
    property bool linearCurves: false
    property int currentWindow: 0
    property var luts: []
    property bool lutEditable: false
    property bool bypass: false
    property bool canAutoBalance: false
    property string comparison: "none"
    property bool picking: false
    property string pickMode: "set"
    signal pickModeEdited(string mode)
    property bool highlight: false
    property bool showOverlay: true
    property var trackJob: ({})
    property int tab: 0
    property bool qualifierOpen: false
    property bool lean: false            // light colour work: primaries, curves and masks only
    // Light work on a wide panel: the curves and the masks share one page,
    // so neither leaves half the panel empty.
    readonly property bool merged: lean && controlsViewport.width >= 1700
    readonly property int page: tab === 2 ? 1 : tab     // curves and masks live on one page
    readonly property var keyedKeys: node.keyframes !== undefined ? Object.keys(node.keyframes).filter(function (k) { return node.keyframes[k].length > 0 }) : []

    signal nodeEdited(var node)
    signal windowEdited(var window, int index)
    signal committed()
    signal keyToggled(string key)
    signal lutFolderRequested()
    signal lutBrowseRequested()
    signal autoBalanceRequested()
    signal bypassToggled(bool on)
    signal comparisonEdited(string mode)
    signal pickToggled(bool on)
    signal highlightToggled(bool on)
    signal overlayToggled(bool on)
    signal trackRequested(int direction, bool singleFrame)
    signal trackStopped(bool keep)
    signal trackCleared()
    signal trackMotionCopied(var points)
    signal frameRequested(int clipFrame)

    color: Theme.panelBg
    // The height at which every tab shows whole (the qualifier shut under the
    // primaries): the same for all of them, so changing tab moves nothing.
    readonly property real fitHeight: bar.height + Theme.hairline
        + Math.max(primaries.wantedHeight + (primariesPage.side ? 0 : Theme.hTab), curvesTab.minimumHeight,
                   6 * Theme.hControl + 5 * Theme.s1 + 2 * Theme.s2, hdrTab.minimumHeight, 220)
    // The least that shows every tab whole, with the wheels at their smallest.
    readonly property real leastHeight: fitHeight - primaries.wantedHeight + primaries.minimumContentHeight
    readonly property var tabs: [qsTr("Primaries"), qsTr("Curves"), qsTr("Masks"), qsTr("HDR"), qsTr("Hue Mesh")]
    function part(key, value) {
        var n = {}
        for (var k in node) n[k] = node[k]
        n[key] = value
        root.nodeEdited(n)
    }

    Row {
        id: bar
        width: parent.width
        height: Theme.hTab
        Repeater {
            model: root.lean ? (root.merged ? 2 : 3) : root.tabs.length
            delegate: TabButton {
                required property int index
                objectName: "gradeTab:" + index
                height: bar.height
                text: root.merged && index === 1 ? qsTr("Curves and Masks") : root.tabs[index]
                tip: qsTr("Open this tool for the selected layer")
                checked: root.tab === index || (root.merged && index === 1 && root.tab === 2)
                onClicked: root.tab = index
            }
        }
    }
    Row {
        objectName: "gradingActions"
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: bar.verticalCenter
        ToolButton {
            objectName: "browseLuts"
            text: qsTr("LUTs"); iconName: "grid-2x2"; showLabel: true
            enabled: root.lutEditable
            onClicked: root.lutBrowseRequested()
        }
        ToolButton {
            objectName: "autoBalance"
            iconName: "wand-sparkles"
            showLabel: true
            text: qsTr("Auto Balance")
            enabled: root.canAutoBalance
            Accessible.description: qsTr("Estimate neutral colours from this clip's current frame and add an editable Gain correction layer")
            onClicked: root.autoBalanceRequested()
            Tooltip {
                text: qsTr("Balance this frame's colours in a new correction layer")
                visible: parent.hovered && !parent.down
            }
        }
    }
    Rectangle { anchors.top: bar.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }

    Flickable {
        id: controlsViewport
        objectName: "gradingViewport"
        anchors.top: bar.bottom; anchors.topMargin: Theme.hairline
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        // Each tool lays itself out to the panel; the scroll bars only take
        // over below its smallest workable size. Not drag-to-scroll: a drag
        // on empty space used to slide the whole tool out of view.
        readonly property var minimumSizes: [[primariesPage.side ? 0 : Math.max(primaries.minimumContentWidth, 880), primaries.minimumContentHeight + (primariesPage.side ? 0 : root.qualifierOpen ? primariesPage.belowHeight : Theme.hTab)],
                                             [root.merged ? 1700 : 720, curvesTab.minimumHeight], [root.merged ? 1700 : masksPage.minimumWidth, 6 * Theme.hControl + 5 * Theme.s1 + 2 * Theme.s2], [900, hdrTab.minimumHeight], [560, 220]]
        contentWidth: Math.max(width, minimumSizes[root.tab][0])
        contentHeight: Math.max(height, minimumSizes[root.tab][1])
        interactive: false
        clip: true
        // The wheel scrolls whichever way the tool overflows: down first,
        // sideways when only the width runs out or with Shift held.
        readonly property bool overflowX: contentWidth > width + 0.5
        readonly property bool overflowY: contentHeight > height + 0.5
        WheelHandler {
            enabled: controlsViewport.overflowX || controlsViewport.overflowY
            onWheel: function (event) {
                var dx = event.angleDelta.x, dy = event.angleDelta.y
                if (dx === 0 && ((event.modifiers & Qt.ShiftModifier) || !controlsViewport.overflowY)) { dx = dy; dy = 0 }
                if (controlsViewport.overflowX)
                    controlsViewport.contentX = Math.max(0, Math.min(controlsViewport.contentWidth - controlsViewport.width,
                                                                     controlsViewport.contentX - dx / 2))
                if (controlsViewport.overflowY)
                    controlsViewport.contentY = Math.max(0, Math.min(controlsViewport.contentHeight - controlsViewport.height,
                                                                     controlsViewport.contentY - dy / 2))
            }
        }
        // Shown whenever the tool is bigger than the panel, so cut-off
        // controls never look like the whole tool.
        C.ScrollBar.vertical: ScrollBar { objectName: "gradingScrollY"; policy: controlsViewport.overflowY ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded }
        C.ScrollBar.horizontal: ScrollBar { objectName: "gradingScrollX"; policy: controlsViewport.overflowX ? C.ScrollBar.AlwaysOn : C.ScrollBar.AsNeeded }
        StackLayout {
        width: controlsViewport.contentWidth; height: controlsViewport.contentHeight
        currentIndex: root.page
        // The primaries and the qualifier on one page: side by side when there
        // is the width for both at their working sizes, otherwise one over the other.
        Item {
            id: primariesPage
            objectName: "primariesPage"
            // The wheels come first: the qualifier widens only with the
            // width they leave once their rings are full size.
            readonly property int sideWidth: Math.max(640, Math.min(900, controlsViewport.width - primaries.wantedWidth))
            readonly property int belowHeight: 214
            readonly property bool side: controlsViewport.width >= Math.max(1910, primaries.compactWidth + 640)
            PrimariesTab {
                id: primaries
                objectName: "primariesTab"
                compact: controlsViewport.width - (primariesPage.side ? primariesPage.sideWidth : 0) < 1380
                width: primariesPage.side ? parent.width - qualifierSection.width : parent.width
                height: primariesPage.side ? parent.height : parent.height - qualifierSection.height
                node: root.node
                luts: root.luts
                lutEditable: root.lutEditable
                onLutBrowseRequested: root.lutBrowseRequested()
                bypass: root.bypass
                comparison: root.comparison
                keyedKeys: root.keyedKeys
                keyable: !root.lean
                onPrimariesEdited: function (p) { root.part("primaries", p) }
                onLutEdited: function (l) { root.part("lut", l) }
                onLutFolderRequested: root.lutFolderRequested()
                onBypassToggled: function (on) { root.bypassToggled(on) }
                onComparisonEdited: function (m) { root.comparisonEdited(m) }
                onKeyToggled: function (k) { root.keyToggled(k) }
                onCommitted: root.committed()
            }
            Rectangle {
                id: qualifierSection
                objectName: "qualifierSection"
                color: Theme.panelBg
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: primariesPage.side ? primariesPage.sideWidth : parent.width
                height: primariesPage.side ? parent.height : root.qualifierOpen ? primariesPage.belowHeight : Theme.hTab
                // The rule against the primaries.
                Rectangle {
                    color: Theme.border
                    width: primariesPage.side ? Theme.hairline : parent.width
                    height: primariesPage.side ? parent.height : Theme.hairline
                }
                Row {
                    id: qualifierHeader
                    visible: !primariesPage.side
                    x: Theme.s2; y: Theme.hairline
                    height: Theme.hTab - Theme.hairline
                    spacing: Theme.s1
                    IconButton {
                        objectName: "qualifierDisclosure"
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: root.qualifierOpen ? "chevron-down" : "chevron-right"
                        text: root.qualifierOpen ? qsTr("Hide the colour pick") : qsTr("Show the colour pick")
                        onClicked: root.qualifierOpen = !root.qualifierOpen
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Colour Pick")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; font.weight: Theme.wHeading
                    }
                }
                QualifierTab {
                    id: qualifierTab
                    objectName: "qualifierTab"
                    visible: primariesPage.side || root.qualifierOpen
                    anchors.fill: parent
                    anchors.topMargin: primariesPage.side ? 0 : Theme.hTab
                    anchors.leftMargin: primariesPage.side ? Theme.hairline : 0
                    node: root.node
                    picking: root.picking
                    pickMode: root.pickMode
                    onPickModeEdited: function (mode) { root.pickModeEdited(mode) }
                    highlight: root.highlight
                    onQualifierEdited: function (q) { root.part("qualifier", q) }
                    onPickToggled: function (on) { root.pickToggled(on) }
                    onHighlightToggled: function (on) { root.highlightToggled(on) }
                    onCommitted: root.committed()
                }
            }
        }
        Item {
        id: shapingPage
        objectName: "shapingPage"
        CurvesTab {
            id: curvesTab
            objectName: "curvesTab"
            visible: root.merged || root.tab === 1
            // Beside the masks: as wide as its editor wants, no wider.
            width: root.merged ? Math.min(parent.width * 0.4, wantedWidth) : parent.width
            height: parent.height
            linearCurves: root.linearCurves
            node: root.node
            onCurvesEdited: function (c) { root.part("curves", c) }
            onCommitted: root.committed()
        }
        // A shape, the people in the picture, and the tracker that keeps a shape
        // on one of them, side by side. On a narrow panel the tracker's table goes.
        Rectangle { visible: root.merged; x: curvesTab.width; width: Theme.hairline; height: parent.height; color: Theme.border }
        Item {
            id: masksPage
            objectName: "masksPage"
            visible: root.merged || root.tab === 2
            x: root.merged ? curvesTab.width + Theme.hairline : 0
            width: parent.width - x
            height: parent.height
            readonly property int minimumWidth: Math.ceil(windowsTab.minimumWidth + (root.lean ? 0 : aiMask.width + 268 + 2 * Theme.hairline))
            readonly property bool trackTable: controlsViewport.width >= 1660
            WindowsTab {
                id: windowsTab
                objectName: "windowsTab"
                width: root.lean ? parent.width : parent.width - aiMask.width - trackerTab.width - 2 * Theme.hairline
                height: parent.height
                displayedWindow: root.displayedWindow
                node: root.node
                showOverlay: root.showOverlay
                current: root.currentWindow
                onSelected: function (index) { root.currentWindow = index }
                onWindowEdited: function (w,index) { root.windowEdited(w,index) }
                onWindowsEdited: function (windows) { root.part("windows", windows) }
                onOverlayToggled: function (on) { root.overlayToggled(on) }
                onCommitted: root.committed()
            }
            Rectangle { visible: !root.lean; anchors.right: aiMask.left; width: Theme.hairline; height: parent.height; color: Theme.border }
            AiMaskPanel {
                id: aiMask
                objectName: "aiMaskPanel"
                visible: !root.lean
                anchors.right: aiMaskRule.left
                width: 270
                height: parent.height
                node: root.node
                highlight: root.highlight
                onAiMaskEdited: function (m) { root.part("aiMask", m) }
                onHighlightToggled: function (on) { root.highlightToggled(on) }
                onCommitted: root.committed()
            }
            Rectangle { id: aiMaskRule; visible: !root.lean; anchors.right: trackerTab.left; width: Theme.hairline; height: parent.height; color: Theme.border }
            TrackerTab {
                id: trackerTab
                objectName: "trackerTab"
                visible: !root.lean
                anchors.right: parent.right
                showTable: masksPage.trackTable
                width: showTable ? Math.max(520, Math.min(860, parent.width * 0.3)) : 268
                height: parent.height
                node: root.node
                job: root.trackJob
                currentWindow: root.currentWindow
                canTrack: root.lutEditable && (w.enabled === true)
                onTrackRequested: function(direction,singleFrame) { root.trackRequested(direction,singleFrame) }
                onStopRequested: function(keep) { root.trackStopped(keep) }
                onClearRequested: root.trackCleared()
                onMotionCopyRequested: function(points) { root.trackMotionCopied(points) }
                onFrameRequested: function (f) { root.frameRequested(f) }
            }
        }
        }
        Item {}     // the masks' old place: they share the curves' page
        HdrTab {
            id: hdrTab
            objectName: "hdrTab"
            node: root.node
            onHdrEdited: function (h) { root.part("hdr", h) }
            onKeyToggled: function (k) { root.keyToggled(k) }
            onCommitted: root.committed()
        }
        WarperTab {
            objectName: "warperTab"
            node: root.node
            onWarperEdited: function (m) { root.part("warper", m) }
            onCommitted: root.committed()
        }
    }
    }
    // The AI mask switched on from the node graph's wand, and shown.
    function openAiMask() {
        root.tab = 2
        var m = {}
        var was = root.node.aiMask !== undefined ? root.node.aiMask : {}
        for (var k in was) m[k] = was[k]
        m.enabled = true
        root.part("aiMask", m)
        root.committed()
    }
    // A picked colour arrives from the page; the qualifier folds it in.
    function pickedColor(hex, mode) { qualifierTab.picked(hex, mode) }
}
