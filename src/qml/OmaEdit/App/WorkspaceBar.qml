pragma ComponentBehavior: Bound
import QtQuick
import OmaEdit.Ui

// The five workspaces, centred in the title bar. Order is the editing order a
// job actually moves through — ingest, cut, grade, mix, ship — so the bar
// doubles as a reminder of where a project is.
Row {
    id: root

    readonly property var workspaces: ["Media", "Edit", "Color", "Audio", "Deliver"]
    readonly property var titles: ({ "Media": qsTr("Media"), "Edit": qsTr("Edit"),
        "Color": qsTr("Colour"), "Audio": qsTr("Audio"), "Deliver": qsTr("Export") })
    property string current: "Edit"

    signal activated(string name)

    spacing: 0

    Repeater {
        model: root.workspaces

        delegate: TabButton {
            required property string modelData

            text: root.titles[modelData]
            tip: qsTr("Switch to the %1 workspace").arg(root.titles[modelData])
            height: root.height
            checked: root.current === modelData
            fontSize: Theme.fsControl
            // Controlled: the owner decides and binds `current` back, so a
            // programmatic switch (shortcut, restore) and a click agree.
            onClicked: root.activated(modelData)
        }
    }
}
