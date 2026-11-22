import QtCore
import QtQuick
import OmaEdit.Ui

// One group of the storage sidebar: a cyan caption, an optional trailing
// action, and a body that collapses. Its rows are the default children.
Column {
    id: root

    property string title: ""
    property bool expanded: true
    // Names the section in the settings, so a folded section stays folded
    // across sessions; empty = not remembered.
    property string settingsKey: ""
    Settings {
        id: sectionSettings
        category: root.settingsKey !== "" ? "storageSidebar/" + root.settingsKey : "storageSidebar/unsaved"
        property bool expanded: true
        Component.onCompleted: if (root.settingsKey !== "") root.expanded = expanded
    }
    onExpandedChanged: if (settingsKey !== "") sectionSettings.expanded = expanded
    property string actionIcon: ""
    property string actionText: ""
    property bool actionEnabled: true
    default property alias rows: body.data

    signal actionTriggered()

    width: parent ? parent.width : 200
    spacing: 0

    SectionTitle {
        width: parent.width - Theme.s3 * 2
        x: Theme.s3
        text: root.title
        collapsible: true
        expanded: root.expanded
        onToggled: root.expanded = !root.expanded

        // Built only when the section has an action, so an unused section
        // leaves no unlabelled button behind for the pointer or a reader.
        Loader {
            active: root.actionIcon !== ""
            sourceComponent: IconButton {
                iconName: root.actionIcon
                text: root.actionText
                enabled: root.actionEnabled
                implicitWidth: Theme.szIcon + Theme.s1
                implicitHeight: Theme.szIcon + Theme.s1
                onClicked: root.actionTriggered()
            }
        }
    }

    Column {
        id: body
        width: parent.width
        visible: root.expanded
        spacing: 0
    }

    Item { width: 1; height: Theme.s1 }
}
