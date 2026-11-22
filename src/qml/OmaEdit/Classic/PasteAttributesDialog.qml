import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import omaedit
import OmaEdit.Ui as Ui

// Alt+V — Paste Attributes: the chosen attribute groups of the copied clip
// (or a preset) onto every selected clip. A group the source does not set
// is offered too — pasting it CLEARS that attribute on the targets. Remove
// Attributes is the same thing with no source: the ticked groups are cleared.
Ui.FormDialog {
    id: pasteDialog
    width: Math.min(520, parent ? parent.width - Ui.Theme.s5 * 2 : 520)
    title: removing ? qsTr("Remove Attributes") : qsTr("Paste Attributes")
    standardButtons: Dialog.Ok | Dialog.Cancel
    property var source: null
    property bool removing: false
    property string sourceName: ""
    // [{key, name, available}] for the current source.
    property var rows: []
    // The last choice, kept across opens.
    property var remembered: ({})

    function openForRemoval(entries) {
        removing = true
        source = { name: "none" }
        sourceName = ""
        var set = []
        entries.forEach(function (entry) {
            win.attributes.groupsOf(entry).forEach(function (key) {
                if (set.indexOf(key) < 0)
                    set.push(key)
            })
        })
        rows = win.attributes.groupDefs.map(function (def) {
            return { key: def.key, name: def.name, available: set.indexOf(def.key) >= 0,
                     checked: set.indexOf(def.key) >= 0 }
        })
        open()
    }
    function openFor(src, name) {
        removing = false
        source = src
        sourceName = name || ""
        var defs = win.attributes.groupDefs
        var avail = win.attributes.groupsOf(src)
        var list = []
        for (var i = 0; i < defs.length; i++) {
            var key = defs[i].key
            var available = avail.indexOf(key) >= 0
            list.push({ key: key, name: defs[i].name, available: available,
                        checked: remembered[key] !== undefined ? remembered[key] && available
                                                                : available })
        }
        rows = list
        open()
    }
    function chosenGroups() {
        var out = []
        var memo = {}
        for (var i = 0; i < groupRepeater.count; i++) {
            var box = groupRepeater.itemAt(i)
            if (box === null)
                continue
            memo[box.groupKey] = box.checked
            if (box.checked)
                out.push(box.groupKey)
        }
        remembered = memo
        return out
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: pasteDialog.removing ? qsTr("From %n clip(s)", "", win.selectionCount)
                  : (pasteDialog.sourceName !== "" ? qsTr("From “%1”").arg(pasteDialog.sourceName) : qsTr("From the clipboard"))
                  + " " + qsTr("onto %n clip(s)", "", win.selectionCount)
            color: win.textColor
        }
        Label {
            text: pasteDialog.removing ? qsTr("Ticked groups go back to their defaults; unticked groups are left alone.")
                  : qsTr("Unticked groups are left alone; a ticked group the source does not set clears it.")
            color: win.textDimColor
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            Layout.maximumWidth: 360
        }
        Label {
            visible:pasteDialog.source!==null && (pasteDialog.source.retime!==undefined || pasteDialog.source.speedPattern!==undefined)
            text:qsTr("Speed segments fit each destination's source range. Segment durations scale; rates may adjust slightly to land on whole frames.")
            color:win.textDimColor;font.pixelSize:12;wrapMode:Text.WordWrap;Layout.maximumWidth:360
        }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 0
            Repeater {
                id: groupRepeater
                model: pasteDialog.rows
                Ui.Toggle {
                    required property var modelData
                    readonly property string groupKey: modelData.key
                    objectName: "pasteGroup:" + modelData.key
                    tip: pasteDialog.removing ? qsTr("Clear this group of attributes on the selected clips") : qsTr("Paste this group of attributes onto the selected clips")
                    label: modelData.name + (modelData.available ? "" : "  (none)")
                    checked: modelData.checked
                }
            }
        }
    }

    onAccepted: {
        var groups = chosenGroups()
        if (pasteDialog.source === null || groups.length === 0)
            return
        backend.viewTimeline()
        backend.pasteAttributes(win.selection.selectedAddresses(), pasteDialog.source, groups)
    }
}
