import QtQuick
import OmaEdit.Ui

// One node card: thumbnail, number, label, an input port on the left and
// an output on the right, a bypass dot, a mask badge, a keyframe badge.
Item {
    id: root

    property int number: 1
    property string label: ""
    property string kind: "serial"
    property bool enabled2: true
    property bool selected: false
    property string thumb: ""
    property bool hasMask: false
    property bool hasKeyframes: false
    signal inputClicked()
    signal outputClicked()
    signal clicked()
    signal doubleClicked()
    signal toggled()
    signal moved(int delta)
    signal contextRequested(real x, real y) // where on the node its menu belongs
    signal navigationRequested(int delta)
    signal removeRequested()
    activeFocusOnTab: true
    readonly property bool keyboardConsumesKeys: true
    Accessible.role: Accessible.Button
    Accessible.selectable: true
    Accessible.selected: root.selected
    Accessible.onPressAction: root.clicked()
    Accessible.name: (label || qsTr("Node %1").arg(number)) + ", "
                     + (kind === "layer" ? qsTr("Layer") : kind === "parallel" ? qsTr("Parallel") : qsTr("Serial"))
    Accessible.description: (enabled2 ? qsTr("Enabled. ") : qsTr("Bypassed. "))
                           + qsTr("Arrows select nodes; Space toggles bypass; F2 renames; Delete removes; Menu opens actions. I and O select connection ports.")
    onActiveFocusChanged: if (activeFocus) root.clicked()
    Keys.onShortcutOverride: function(event) {
        if (event.key === Qt.Key_F2 || (event.modifiers & Qt.AltModifier
            && (event.key === Qt.Key_Left || event.key === Qt.Key_Right))) event.accepted = true
    }
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            const delta = event.key === Qt.Key_Left ? -1 : 1
            if (event.modifiers & Qt.AltModifier) root.moved(delta)
            else root.navigationRequested(delta)
        } else if (event.key === Qt.Key_Space) root.toggled()
        else if (event.key === Qt.Key_F2) root.doubleClicked()
        else if (event.key === Qt.Key_Delete) root.removeRequested()
        else if (event.key === Qt.Key_Menu) root.contextRequested(root.width / 2, root.height / 2)
        else if (event.key === Qt.Key_I) root.inputClicked()
        else if (event.key === Qt.Key_O) root.outputClicked()
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) root.clicked()
        else { event.accepted = false; return }
        event.accepted = true
    }

    width: 104
    height: 88

    Rectangle {
        id: card
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: 86
        height: 52
        radius: Theme.rControl
        color: Theme.meterWell
        border.color: root.activeFocus || root.selected ? Theme.accent : root.kind === "layer" ? Theme.aiAccent : root.kind === "parallel" ? Theme.warning : Theme.border
        border.width: root.activeFocus ? Theme.focusRing : root.selected ? Theme.selectionRing : Theme.hairline
        opacity: root.enabled2 ? 1 : 0.45
        clip: true
        Image { anchors.fill: parent; anchors.margins: Theme.selectionRing; source: root.thumb; fillMode: Image.PreserveAspectCrop; asynchronous: true; visible: status === Image.Ready }
        Rectangle { // mask badge
            anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 3
            width: 14; height: 14; radius: 7
            visible: root.hasMask
            color: Theme.scrimStrong
            Icon { anchors.centerIn: parent; name: "circle-dot"; size: 10; color: Theme.textPrimary }
        }
        Rectangle { // bypassed cross
            anchors.fill: parent
            visible: !root.enabled2
            color: "transparent"
            Rectangle { anchors.centerIn: parent; width: parent.width * 1.2; height: 2; rotation: -30; color: Theme.danger }
        }
    }
    // Ports.
    Rectangle { anchors.verticalCenter: card.verticalCenter; anchors.right: card.left; anchors.rightMargin: -3; width: 12; height: 12; radius: 6; color: root.enabled2 ? Theme.audio : Theme.textMuted; border.color: Theme.windowBg; border.width: 1; TapHandler { onTapped: root.inputClicked() } }
    Rectangle { anchors.verticalCenter: card.verticalCenter; anchors.left: card.right; anchors.leftMargin: -3; width: 12; height: 12; radius: 6; color: root.enabled2 ? Theme.audio : Theme.textMuted; border.color: Theme.windowBg; border.width: 1; TapHandler { onTapped: root.outputClicked() } }
    Text {
        anchors.top: card.bottom
        anchors.topMargin: 3
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.label !== "" ? root.label : qsTr("Node %1").arg(root.number)
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
        elide: Text.ElideRight
        width: parent.width - 4
        horizontalAlignment: Text.AlignHCenter
    }
    Row {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.s1
        Text { text: (root.number < 10 ? "0" : "") + root.number; font.family: Theme.monoFamily; font.pixelSize: Theme.fsLabel - 1; color: Theme.textMuted }
        Rectangle { // enable dot
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 8; radius: 4
            color: root.enabled2 ? Theme.audio : Theme.textMuted
            TapHandler { onTapped: root.toggled() }
        }
        Icon { anchors.verticalCenter: parent.verticalCenter; name: "diamond"; size: 10; color: Theme.accent; visible: root.hasKeyframes }
    }
    TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.clicked(); onDoubleTapped: root.doubleClicked() }
    // Taken outright: a hand that moves a little while it clicks still opens the menu.
    TapHandler { acceptedButtons: Qt.RightButton; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: function (ev) { root.contextRequested(ev.position.x, ev.position.y) } }
    DragHandler { // drag a card past its neighbour to reorder
        target: null
        property real acc: 0
        onActiveChanged: if (active) acc = 0
        onTranslationChanged: {
            if (!active) return
            var d = translation.x - acc
            if (d > 70) { acc = translation.x; root.moved(1) }
            else if (d < -70) { acc = translation.x; root.moved(-1) }
        }
    }
}
