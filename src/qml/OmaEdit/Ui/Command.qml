import QtQuick

Item {
    id: root

    required property string commandId
    required property string text
    property string category: "Edit"
    property string scope: "Global"
    property var defaultShortcuts: []
    property bool available: true
    property bool shortcutEnabled: true
    property var registry: Commands.registry
    property bool attached: false
    property bool complete: false

    signal triggered()

    function execute() {
        triggered();
    }

    function attach() {
        if (complete && registry)
            attached = registry.attach(root);

    }

    width: 0
    height: 0
    objectName: "command:" + commandId
    Component.onCompleted: {
        complete = true;
        attach();
    }
    onRegistryChanged: attach()
    onAvailableChanged: {
        if (attached && registry) {
            registry.refresh();
        }
    }
    onTextChanged: {
        if (attached && registry) {
            registry.refresh();
        }
    }

    Shortcut {
        id: binding

        sequences: {
            const rev = root.registry ? root.registry.bindingRevision : 0;
            return root.attached ? root.registry.dispatchSequences(root.commandId) : [];
        }
        context: Qt.ApplicationShortcut
        enabled: root.attached && root.available && root.shortcutEnabled
        onActivated: root.registry.trigger(root.commandId)
    }

}
