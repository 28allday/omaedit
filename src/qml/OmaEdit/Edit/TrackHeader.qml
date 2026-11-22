import QtQuick
import OmaEdit.Ui

// Fixed-width header beside each track. Video and audio expose different
// controls because they do different jobs: video gets visibility, audio gets
// mute/solo. The destination badge, lock and auto-select are shared.
//
// Owner-driven: every control reports through a signal; the owner flips the
// engine flag and the binding brings the new state back.
Rectangle {
    id: root

    property string name: ""
    property string tip: ""          // replaces the generic badge help when set
    property string kind: "video"
    property bool destination: false
    property bool locked: false
    property bool autoSelect: false
    property bool armed: false
    property bool recordable: true
    signal armToggled(bool armed)
    property bool visibleTrack: true   // video: picture enabled
    property bool muted: false         // audio
    property bool soloed: false        // audio

    signal destinationRequested()
    signal lockToggled(bool on)
    signal autoSelectToggled(bool on)
    signal visibilityToggled(bool on)
    signal muteToggled(bool on)
    signal soloToggled(bool on)
    signal contextRequested()
    // Double-click the badge to rename; "" hands the name back to the
    // V1/A1 convention.
    signal renameRequested(string name)
    property bool editing: false
    function startRename() {
        nameEditor.text = name
        editing = true
        nameEditor.forceActiveFocus()
        nameEditor.selectAll()
    }

    implicitWidth: Theme.wTrackHeader
    color: destination ? Theme.controlBg : Theme.panelBg
    opacity: locked ? 0.7 : 1.0

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Track %1").arg(name)

    Rectangle {
        anchors.right: parent.right
        width: Theme.hairline
        height: parent.height
        color: Theme.border
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.contextRequested()
    }

    // Destination badge — cyan for video, green for audio, so the two stacks
    // stay distinguishable at a glance down the left edge. Clicking it makes
    // the track the destination for E / W / D.
    Rectangle {
        id: badge
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(26, Math.min(root.width - (root.kind === "audio" ? (root.recordable ? 5 : 4) : 3) * 18 - Theme.s2 * 2 - Theme.s1, 64, badgeText.implicitWidth + Theme.s2))
        height: 20
        radius: Theme.rControl
        color: root.destination
               ? (root.kind === "audio" ? Theme.audio : Theme.accent)
               : badgeHover.hovered ? Theme.controlBg : "transparent"
        border.width: root.destination ? 0 : Theme.hairline
        border.color: Theme.border

        HoverHandler { id: badgeHover }
        TapHandler {
            onTapped: root.destinationRequested()
            onDoubleTapped: root.startRename()
        }
        Tooltip {
            text: root.tip !== "" ? root.tip : qsTr("Destination track for E / W / D · double-click to rename")
            visible: badgeHover.hovered && !root.editing
        }

        Text {
            id: badgeText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - Theme.s1)
            text: root.name
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            font.weight: Theme.wHeading
            color: root.destination ? Theme.accentText : Theme.textSecondary
        }
    }

    // The rename editor overlays the whole header while it is open.
    Rectangle {
        visible: root.editing
        anchors.fill: parent
        anchors.rightMargin: Theme.hairline
        color: Theme.controlBg
        z: 10
        TextInput {
            id: nameEditor
            objectName: "trackNameEditor"
            anchors.fill: parent
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            verticalAlignment: TextInput.AlignVCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsControl
            color: Theme.textPrimary
            selectionColor: Theme.accent
            selectedTextColor: Theme.accentText
            clip: true
            maximumLength: 24
            onAccepted: {
                root.editing = false
                root.renameRequested(text.trim())
            }
            Keys.onEscapePressed: root.editing = false
            onActiveFocusChanged: if (!activeFocus) root.editing = false
        }
    }

    Row {
        id: controls
        readonly property real controlWidth: Math.min(22, (root.width - x - Theme.s2) / (root.kind === "audio" ? (root.recordable ? 5 : 4) : 3))
        anchors.left: badge.right
        anchors.leftMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0

        IconButton {
            iconName: root.locked ? "lock" : "unlock"
            text: qsTr("Lock %1").arg(root.name)
            implicitWidth: controls.controlWidth; implicitHeight: 22
            checkable: true; checked: root.locked
            iconColor: root.locked ? Theme.warning : Theme.textSecondary
            onClicked: root.lockToggled(!root.locked)
        }

        IconButton {
            iconName: "link"
            text: qsTr("Sync lock %1 (rides ripple edits)").arg(root.name)
            implicitWidth: controls.controlWidth; implicitHeight: 22
            checkable: true; checked: root.autoSelect
            onClicked: root.autoSelectToggled(!root.autoSelect)
        }

        IconButton {
            visible: root.kind === "video"
            iconName: root.visibleTrack ? "eye" : "eye-off"
            text: root.visibleTrack ? qsTr("Hide %1").arg(root.name) : qsTr("Show %1").arg(root.name)
            implicitWidth: controls.controlWidth; implicitHeight: 22
            checkable: true; checked: !root.visibleTrack
            iconColor: root.visibleTrack ? Theme.textSecondary : Theme.danger
            onClicked: root.visibilityToggled(!root.visibleTrack)
        }

        IconButton {
            objectName:"editArmTrack"
            visible:root.kind==="audio" && root.recordable;iconName:"mic";text:qsTr("Arm %1 for recording").arg(root.name)
            enabled: !root.locked
            implicitWidth:controls.controlWidth;implicitHeight:22;checkable:true;checked:root.armed
            onClicked:root.armToggled(!root.armed)
        }
        IconButton {
            visible: root.kind === "audio"
            iconName: "volume-x"
            text: qsTr("Mute %1").arg(root.name)
            implicitWidth: controls.controlWidth; implicitHeight: 22
            checkable: true; checked: root.muted
            iconColor: root.muted ? Theme.danger : Theme.textSecondary
            onClicked: root.muteToggled(!root.muted)
        }

        IconButton {
            visible: root.kind === "audio"
            iconName: "headphones"
            text: qsTr("Solo %1").arg(root.name)
            implicitWidth: controls.controlWidth; implicitHeight: 22
            checkable: true; checked: root.soloed
            iconColor: root.soloed ? Theme.audio : Theme.textSecondary
            onClicked: root.soloToggled(!root.soloed)
        }
    }
}
