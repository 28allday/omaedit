import QtQuick

Item {
    id: root

    property Item host: parent
    property var store: null
    property string workspaceKey: ""
    property var panels: []
    property var splits: []
    property string maximized: ""
    property bool ready: false
    property bool suspended: false
    property bool applying: false
    property int revision: 0
    // Raised when a workspace's default panel sizes change, so splits saved
    // under the old defaults give way to the new ones (panels keep their
    // shown, detached and fullscreen state).
    property int sizesVersion: 1
    readonly property bool detachedWindowActive: panels.some(function(p) {
        return p.window.visible && p.window.active;
    })
    readonly property bool fullscreenActive: panels.some(function(p) {
        return p.fullscreen;
    })
    readonly property var presetNames: {
        const r = store ? store.revision : 0;
        return store ? store.presetNames(workspaceKey) : [];
    }
    property Item emptyArea

    emptyArea: Item {
        parent: root.host
        anchors.fill: parent
        z: 20
        visible: root.ready && root.panels.length > 0 && root.maximized === "" && root.panels.every(function(p) {
            return !p.docked;
        })

        Rectangle {
            anchors.fill: parent
            color: Theme.windowBg
        }

        Column {
            anchors.centerIn: parent
            spacing: Theme.s3

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Panels are hidden or detached")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsControl
            }

            ToolButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Restore All Panels")
                showLabel: true
                iconName: "layout-grid"
                onClicked: root.restorePanels()
            }

        }

    }

    property Item maximumArea

    signal transportRequested(string action, int value)
    signal saveRequested()
    signal applicationFullscreenRequested()
    signal layoutRequested()

    function addPanel(panel) {
        panels = panels.concat([panel]);
        initializeTimer.restart();
    }

    function addSplit(split) {
        splits = splits.concat([split]);
        initializeTimer.restart();
    }

    function panel(id) {
        for (let p of panels) if (p.panelId === id) {
            return p;
        }
        return null;
    }

    function initialize() {
        if (ready)
            return ;

        if (splits.length === 0 || splits.some(function(s) {
            return s.width <= 0 || s.height <= 0;
        })) {
            initializeTimer.restart();
            return ;
        }
        panels = panels.slice().reverse();
        splits = splits.slice().reverse();
        for (let s of splits) s.rememberDefaults()
        ready = true;
        if (store) {
            const state = store.read(workspaceKey);
            if (state.version === 1) {
                if ((state.sizes || 1) !== sizesVersion)
                    delete state.splits;
                apply(state);
            }

        }
    }

    function capture() {
        let state = {
            "version": 1,
            "sizes": sizesVersion,
            "splits": {
            },
            "panels": {
            },
            "maximized": maximized
        };
        for (let s of splits) state.splits[s.splitId] = s.saveState()
        for (let p of panels) state.panels[p.panelId] = p.capture()
        return state;
    }

    function apply(state) {
        applying = true;
        maximized = "";
        for (let p of panels) p.restore(state.panels ? state.panels[p.panelId] || {
        } : {
        })
        for (let s of splits) {
            const value = state.splits ? state.splits[s.splitId] : undefined;
            if (value === undefined || !s.restoreState(value))
                s.resetSizes();

        }
        if (state.maximized && panel(state.maximized) && !panel(state.maximized).hidden && !panel(state.maximized).detached)
            maximized = state.maximized;

        applying = false;
        revision++;
    }

    function saveNow() {
        if (ready && !applying && store)
            store.write(workspaceKey, capture());

    }

    function changed() {
        revision++;
        if (ready && !applying)
            saveTimer.restart();

    }

    function reset() {
        apply({
        });
        saveNow();
    }

    function restorePreset(name) {
        if (store) {
            apply(store.preset(workspaceKey, name));
            saveNow();
        }
    }

    function savePreset(name, replace) {
        return store && store.savePreset(workspaceKey, name, capture(), replace);
    }

    function restorePanels() {
        maximized = "";
        for (let p of panels) {
            p.setShown(true);
            p.detached = false;
            p.exitFullscreen();
        }
        changed();
    }

    function toggleViewerFullscreen() {
        const p = panels.find(function(p) {
            return p.viewer;
        });
        if (p)
            p.toggleFullscreen();

    }

    function exitFullscreen() {
        for (let p of panels) if (p.fullscreen) {
            p.exitFullscreen();
        }
    }

    objectName: "layout:" + workspaceKey
    onStoreChanged: {
        if (ready && store)
            apply(store.read(workspaceKey));

    }
    onMaximizedChanged: changed()
    Timer {
        id: initializeTimer

        interval: 20
        onTriggered: root.initialize()
    }

    Timer {
        id: saveTimer

        interval: 150
        onTriggered: root.saveNow()
    }

    maximumArea: Item {
        parent: root.host
        anchors.fill: parent
        z: 100
        visible: root.maximized !== ""
    }

}
