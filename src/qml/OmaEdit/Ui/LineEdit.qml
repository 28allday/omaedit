import QtQuick
import QtQuick.Controls as C
C.TextField {
    id: input
    property string hint: ""
    // Tooltip: what this control changes, shown after the usual delay.
    property string tip: ""
    hoverEnabled: true
    placeholderText: ""
    Accessible.name: hint
    Accessible.description: tip
    Text { anchors.fill: parent; anchors.margins: input.padding; text: input.hint; visible: input.text.length === 0; color: Theme.textMuted; font: input.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    implicitHeight: Theme.hControl + Theme.s2
    padding: Theme.s2
    topInset: 0; bottomInset: 0
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    font.family: Theme.fontFamily; font.pixelSize: Theme.fsBase
    selectByMouse: true
    background: Rectangle { color: Theme.controlBg; radius: Theme.rControl; border.width: Theme.hairline; border.color: input.activeFocus ? Theme.accent : Theme.border }

    Tooltip { text: input.tip; visible: input.hovered && !input.activeFocus && input.tip !== "" }
}
