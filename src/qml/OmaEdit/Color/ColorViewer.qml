import QtQuick
import OmaEdit.Ui

// The colour-managed viewer: the programme picture with a transport, the
// grade tools in its header (grab still, bypass all, wipe / split / still
// comparison, highlight view), a window overlay for the current node's
// shape mask, and the reference-still wipe drawn over the picture.
Rectangle {
    id: root

    property Item dockBar: null
    property string sequenceName: ""
    property bool hasMedia: false
    property bool playing: false
    property int position: 0
    property int duration: 0
    property real imageAspect: 16 / 9
    property real fps: 25
    property Component videoSurface: null
    property Component beforeSurface: null
    property Component referenceSurface: null
    property var timecoder: null
    property bool bypassAll: false
    property string comparison: "none"     // none | wipe | split | still
    property real wipe: 0.5
    property string referenceSource: ""     // image://thumbs/still:<id>
    property string colorSpace: "Rec.709"
    property var window: ({})               // the current node's window (enabled, shape, x, y, w, h, rotation, softness, invert)
    property bool showWindow: false
    property string viewChannel: "rgb"
    signal viewChannelEdited(string channel)
    signal managementRequested()
    property bool highlight: false
    property bool picking: false            // the qualifier's eyedropper is armed
    property bool lean: false             // light colour work: no stills, no versions
    property var gradeVersions: []
    property int gradeVersion: 0
    property bool hasGradeClip: false
    property bool versionsEditable: false
    property string gradeClipKey: ""
    property string gradeClipName: ""
    readonly property bool textActive: versionsBar.textActive
    function newVersion() { versionsBar.openName("add") }
    function stepVersion(delta) { versionsBar.stepVersion(delta) }
    signal versionActionRequested(string action, int version, string name)

    signal playToggled()
    signal shuttled(int direction)
    signal startRequested()
    signal endRequested()
    signal stepRequested(int frames)
    signal grabRequested()
    signal bypassAllToggled(bool on)
    signal comparisonEdited(string mode)
    signal wipeEdited(real fraction)
    signal windowEdited(var window)
    signal windowCommitted()
    signal colorPicked(real fx, real fy, int modifiers)
    signal highlightToggled(bool on)
    signal previousClipRequested()
    signal nextClipRequested()

    color: Theme.windowBg

    function tc(f) { if (timecoder) return timecoder(f); var rate = fps > 0 ? fps : 25; var s = Math.floor(f / rate), ff = Math.round(f - s * rate); var pad = function (n) { return (n < 10 ? "0" : "") + n }; return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + ":" + pad(ff) }

    Item {
        id: header
        width: parent.width
        height: viewerActions.inBar ? 0 : Theme.hTab
        visible: !viewerActions.inBar
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            text: root.sequenceName !== "" ? root.sequenceName : qsTr("Timeline")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsHeading
            font.weight: Theme.wHeading
            color: Theme.textPrimary
        }
        DockBarActions {
            id: viewerActions
            dockBar: root.dockBar
            fallback: header
            IconButton { visible: !root.lean; objectName: "viewerGrab"; iconName: "camera"; enabled: root.hasMedia; Accessible.name: qsTr("Grab still"); onClicked: root.grabRequested() }
            IconButton { objectName: "viewerBypass"; iconName: "eye-off"; checked: root.bypassAll; Accessible.name: qsTr("Bypass all grades"); onClicked: root.bypassAllToggled(!root.bypassAll) }
            IconButton { objectName: "viewerWipe"; iconName: "compare"; checked: root.comparison === "wipe"; Accessible.name: qsTr("Before / after wipe"); onClicked: root.comparisonEdited(root.comparison === "wipe" ? "none" : "wipe") }
            IconButton { iconName: "panel-left"; checked: root.comparison === "split"; Accessible.name: qsTr("Split screen"); onClicked: root.comparisonEdited(root.comparison === "split" ? "none" : "split") }
            IconButton { iconName: "image"; checked: root.comparison === "still"; enabled: root.referenceSource !== ""; Accessible.name: qsTr("Compare with the reference still"); onClicked: root.comparisonEdited(root.comparison === "still" ? "none" : "still") }
            ToolButton { objectName: "openColorManagement"; anchors.verticalCenter: parent.verticalCenter; iconName: "settings"; showLabel: true; text: qsTr("Colour: %1").arg(root.colorSpace); tip: qsTr("What this clip was shot as, how it is graded and how your screen shows it"); onClicked: root.managementRequested() }
            IconButton { iconName: "sparkles"; checked: root.highlight; Accessible.name: qsTr("Highlight the matte"); onClicked: root.highlightToggled(!root.highlight) }
            Rectangle { width: Theme.hairline; height: Theme.szIconHit - 6; anchors.verticalCenter: parent.verticalCenter; color: Theme.border }
            ComboField { objectName: "viewerChannel"; width: Math.round(78 * Theme.densityScale); anchors.verticalCenter: parent.verticalCenter; model: ["RGB", "R", "G", "B", "Y"]; currentIndex: ["rgb", "red", "green", "blue", "luma"].indexOf(root.viewChannel); tip: qsTr("Isolate one colour channel in the viewer; stored pixels are unchanged"); onActivated: function(i) { root.viewChannelEdited(["rgb", "red", "green", "blue", "luma"][i]) } }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.border }
    }

    GradeVersions {
        id: versionsBar
        objectName: "gradeVersions"
        visible: !root.lean
        height: visible ? implicitHeight : 0
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        versions: root.gradeVersions
        activeVersion: root.gradeVersion
        hasClip: root.hasGradeClip
        editable: root.versionsEditable
        clipKey: root.gradeClipKey
        clipName: root.gradeClipName
        onActionRequested: function (action, version, name) { root.versionActionRequested(action, version, name) }
    }
    Item {
        id: picture
        objectName: "colorPicture"
        anchors.top: versionsBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.margins: Theme.s2
        readonly property real fitW: Math.min(width, height * root.imageAspect)
        readonly property real fitH: fitW / root.imageAspect
        Rectangle {
            id: frame
            anchors.centerIn: parent
            width: root.videoSurface !== null ? picture.width : picture.fitW
            height: root.videoSurface !== null ? picture.height : picture.fitH
            color: Theme.meterWell
            clip: true
            readonly property rect imageRect: videoLoader.item && videoLoader.item.contentRect !== undefined ? videoLoader.item.contentRect : Qt.rect(0,0,width,height)
            Loader { id: videoLoader; anchors.fill: parent; active: root.hasMedia && root.videoSurface !== null; sourceComponent: root.videoSurface }
            // Split: the before picture on the left half.
            Item {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.comparison === "split" ? parent.width / 2 : root.comparison === "wipe" ? parent.width * root.wipe : 0
                visible: (root.comparison === "wipe" || root.comparison === "split") && root.hasMedia
                clip: true
                Loader { width: frame.width; height: frame.height; active: parent.visible && root.beforeSurface !== null; sourceComponent: root.beforeSurface }
                Rectangle { anchors.right: parent.right; width: 2; height: parent.height; color: Theme.accent }
                Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.margins: Theme.s2; width: beforeLabel.width + Theme.s2; height: Theme.hControl - 6; radius: Theme.rControl; color: Theme.scrimStrong; Text { id: beforeLabel; anchors.centerIn: parent; text: qsTr("BEFORE"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textPrimary } }
            }
            // Reference still wipe.
            Item {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.comparison === "still" ? parent.width * root.wipe : 0
                visible: root.comparison === "still" && root.referenceSource !== ""
                clip: true
                Loader { width: frame.width;height:frame.height;active:parent.visible && root.referenceSurface!==null;sourceComponent:root.referenceSurface }
                Image { visible:root.referenceSurface===null; width: frame.width; height: frame.height; source: root.referenceSource; fillMode: Image.Stretch; asynchronous: true }
                Rectangle { anchors.right: parent.right; width: 2; height: parent.height; color: Theme.warning }
                Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.margins: Theme.s2; width: refLabel.width + Theme.s2; height: Theme.hControl - 6; radius: Theme.rControl; color: Theme.scrimStrong; Text { id: refLabel; anchors.centerIn: parent; text: qsTr("REFERENCE"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textPrimary } }
            }
            DragHandler { // drag the divider
                enabled: root.comparison === "wipe" || root.comparison === "still"
                target: null
                onCentroidChanged: if (active) root.wipeEdited(Math.max(0.02, Math.min(0.98, centroid.position.x / frame.width)))
            }
            WindowOverlay {
                x: frame.imageRect.x; y: frame.imageRect.y; width: frame.imageRect.width; height: frame.imageRect.height
                visible: root.showWindow && root.window !== undefined && root.window.enabled === true && !root.picking
                window: root.window
                onEdited: function (w) { root.windowEdited(w) }
                onCommitted: root.windowCommitted()
            }
            // The eyedropper: a click samples the pixel under it.
            TapHandler {
                id: pickTap
                objectName: "pickTap"
                enabled: root.picking
                // Any modifiers: the press's own (none, Ctrl, Shift) picks the
                // mode. An OR'd acceptedModifiers would demand Ctrl+Shift.
                onTapped: function (ev) {
                    const r = frame.imageRect, x = (ev.position.x - r.x) / r.width, y = (ev.position.y - r.y) / r.height
                    if (x >= 0 && x <= 1 && y >= 0 && y <= 1) root.colorPicked(x, y, pickTap.point.modifiers)
                }
            }
            Rectangle { // picking banner
                anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: Theme.s2
                visible: root.picking
                width: pickText.width + Theme.s3; height: Theme.hControl - 4; radius: Theme.rControl
                color: Theme.scrimStrong
                Text { id: pickText; anchors.centerIn: parent; text: qsTr("CLICK TO SAMPLE · CTRL ADDS · SHIFT NARROWS"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.accent }
            }
            Rectangle { // bypass banner
                anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: Theme.s2
                visible: root.bypassAll
                width: bypassText.width + Theme.s3; height: Theme.hControl - 4; radius: Theme.rControl
                color: Theme.scrimStrong
                Text { id: bypassText; anchors.centerIn: parent; text: qsTr("GRADES BYPASSED"); font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; color: Theme.warning }
            }
            EmptyState { anchors.centerIn: parent; visible: !root.hasMedia; iconName: "palette"; title: qsTr("No media"); description: qsTr("Cut something on the Edit page to grade it here.") }
        }
    }

    Item {
        id: transport
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        height: Theme.hControl + Theme.s3
        Text { objectName: "colorTimecode"; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: root.tc(root.position); font.family: Theme.monoFamily; font.pixelSize: Theme.fsTimecode; color: Theme.accent }
        Row {
            anchors.centerIn: parent
            spacing: 0
            IconButton { iconName: "skip-back"; enabled: root.hasMedia; Accessible.name: qsTr("Previous clip"); onClicked: root.previousClipRequested() }
            IconButton { iconName: "rewind"; enabled: root.hasMedia; Accessible.name: qsTr("Shuttle back"); onClicked: root.shuttled(-1) }
            IconButton { iconName: "chevron-left"; enabled: root.hasMedia; Accessible.name: qsTr("Step back"); onClicked: root.stepRequested(-1) }
            IconButton { objectName: "colorPlay"; iconName: root.playing ? "pause" : "play"; enabled: root.hasMedia; iconColor: Theme.textPrimary; Accessible.name: root.playing ? qsTr("Pause") : qsTr("Play"); onClicked: root.playToggled() }
            IconButton { iconName: "chevron-right"; enabled: root.hasMedia; Accessible.name: qsTr("Step forward"); onClicked: root.stepRequested(1) }
            IconButton { iconName: "fast-forward"; enabled: root.hasMedia; Accessible.name: qsTr("Shuttle forward"); onClicked: root.shuttled(1) }
            IconButton { iconName: "skip-forward"; enabled: root.hasMedia; Accessible.name: qsTr("Next clip"); onClicked: root.nextClipRequested() }
        }
        Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: root.tc(Math.max(0, root.duration - 1)); font.family: Theme.monoFamily; font.pixelSize: Theme.fsTimecode; color: Theme.textSecondary }
    }
}
