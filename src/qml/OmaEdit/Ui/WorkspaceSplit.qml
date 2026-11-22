import QtQuick
import QtQuick.Controls as C
import QtQuick.Controls

C.SplitView {
    id: root

    property var controller: null
    property string splitId: ""
    property var defaultState
    property var defaults: []
    property real defaultWidth: 1
    property real defaultHeight: 1

    function rememberDefaults() {
        defaultState = saveState();
        defaultWidth = Math.max(1, width);
        defaultHeight = Math.max(1, height);
        let values = [];
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i);
            values.push({
                "width": item.SplitView.preferredWidth,
                "height": item.SplitView.preferredHeight
            });
        }
        defaults = values;
    }

    function resetSizes() {
        for (let i = 0; i < Math.min(count, defaults.length); ++i) {
            const item = itemAt(i), value = defaults[i];
            item.SplitView.preferredWidth = value.width < 0 ? value.width : Qt.binding(function() {
                return Math.min(item.openWidest || Infinity, value.width * root.width / root.defaultWidth);
            });
            item.SplitView.preferredHeight = value.height < 0 ? value.height : Qt.binding(function() {
                return value.height * root.height / root.defaultHeight;
            });
        }
    }

    objectName: controller ? "split:" + controller.workspaceKey + ":" + splitId : ""
    onResizingChanged: {
        if (!resizing && controller)
            controller.saveNow();

    }
    Component.onCompleted: {
        if (controller)
            controller.addSplit(root);

    }

    handle: SplitterHandle {
        vertical: root.orientation === Qt.Vertical
        pressed: C.SplitHandle.pressed
        hovered: C.SplitHandle.hovered
    }

}
