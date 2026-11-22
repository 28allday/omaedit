import OmaEdit.Ui as Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit

// Flat, theme-accented button — the stock Material pill reads as a
// default widget; an editing surface wants quieter chrome. highlighted
// = filled with the accent (the one suggested next action).
Button {
    id: flatBtn
    property string kbd: ""
    // Tooltip: what the button does beyond its caption.
    property string tip: ""
    ToolTip.text: tip
    ToolTip.visible: hovered && !down && tip !== ""
    ToolTip.delay: 450
    Accessible.description: tip
    // quiet: a selected state that doesn't shout — controlHi fill with
    // an accent hairline, for panel tabs/filters/toggles. The full
    // accent fill stays for real calls to action (Export).
    property bool quiet: false
    readonly property bool loud: highlighted && !quiet
    focusPolicy: Qt.NoFocus
    implicitHeight: 34
    background: Rectangle {
        radius: win.rPill(flatBtn.height)
        border.color: flatBtn.loud ? "transparent"
                    : flatBtn.highlighted ? win.accent
                                          : win.hairlineColor
        border.width: 1
        color: flatBtn.loud
               ? (flatBtn.down ? Qt.darker(win.accent, 1.25)
                               : flatBtn.hovered ? Qt.lighter(win.accent, 1.1)
                                                 : win.accent)
               : flatBtn.highlighted
                 ? win.controlHiColor
                 : (flatBtn.down ? "#45454d"
                                 : flatBtn.hovered ? win.controlHiColor
                                                   : win.controlColor)
        opacity: flatBtn.enabled ? 1 : 0.4
    }
    contentItem: Item {
        implicitWidth: flatRow.implicitWidth + 30 // pills breathe wider
        implicitHeight: 34
        Row {
            id: flatRow
            anchors.centerIn: parent
            spacing: 8
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: flatBtn.text
                color: flatBtn.loud ? win.accentForeground
                     : flatBtn.highlighted ? win.textBrightColor
                                           : win.textColor
                opacity: flatBtn.enabled ? 1 : 0.5
                font.pixelSize: 12
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                visible: flatBtn.kbd !== ""
                text: flatBtn.kbd
                color: flatBtn.loud ? win.accentForeground : win.textDimColor
                opacity: flatBtn.enabled ? 1 : 0.5
                font.family: Ui.Theme.monoFamily
                font.pixelSize: 11
            }
        }
    }
}
