import QtQuick
import OmaEdit.Ui

// "N items, M selected", page buttons and the per-page choice.
Rectangle {
    id: root

    property int itemCount: 0
    property int timelineCount: 0   // counted, but they sit above the pages
    property int selectedCount: 0
    property int page: 0            // zero-based
    property int perPage: 24        // 0 = all
    readonly property int pageCount: perPage > 0 ? Math.max(1, Math.ceil(itemCount / perPage)) : 1

    signal pageRequested(int page)
    signal perPageRequested(int perPage)

    implicitHeight: Theme.hRow + Theme.s3
    color: Theme.panelBg

    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }

    Text {
        objectName: "pagerSummary"
        anchors.left: parent.left
        anchors.leftMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        text: root.selectedCount > 0
              ? qsTr("%1 items, %2 selected").arg(root.itemCount + root.timelineCount).arg(root.selectedCount)
              : qsTr("%1 items").arg(root.itemCount + root.timelineCount)
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsLabel
        color: Theme.textSecondary
    }

    // Page numbers with an ellipsis when there are many: 1 2 3 … 12.
    readonly property var pageModel: {
        var n = pageCount
        if (n <= 7) { var all = []; for (var i = 0; i < n; ++i) all.push(i); return all }
        var out = [0]
        var lo = Math.max(1, page - 1), hi = Math.min(n - 2, page + 1)
        if (lo > 1) out.push(-1)
        for (var j = lo; j <= hi; ++j) out.push(j)
        if (hi < n - 2) out.push(-1)
        out.push(n - 1)
        return out
    }

    Row {
        anchors.right: perPageCombo.left
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        visible: root.pageCount > 1

        IconButton {
            iconName: "chevron-left"; text: qsTr("Previous page")
            enabled: root.page > 0
            onClicked: root.pageRequested(root.page - 1)
        }
        Repeater {
            model: root.pageModel
            delegate: Rectangle {
                required property int modelData
                width: 22; height: 22
                radius: Theme.rControl
                color: modelData === root.page ? Theme.accent
                     : pageHover.hovered && modelData >= 0 ? Theme.hovered(Theme.controlBg) : "transparent"
                HoverHandler { id: pageHover }
                TapHandler { enabled: modelData >= 0; onTapped: root.pageRequested(modelData) }
                Text {
                    anchors.centerIn: parent
                    text: modelData < 0 ? "…" : modelData + 1
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fsLabel
                    color: modelData === root.page ? Theme.accentText : Theme.textSecondary
                }
            }
        }
        IconButton {
            iconName: "chevron-right"; text: qsTr("Next page")
            enabled: root.page < root.pageCount - 1
            onClicked: root.pageRequested(root.page + 1)
        }
    }

    ComboField {
        id: perPageCombo
        objectName: "perPage"
        tip: qsTr("How many clips the browser shows on one page")
        anchors.right: parent.right
        anchors.rightMargin: Theme.s3
        anchors.verticalCenter: parent.verticalCenter
        width: 100
        model: [qsTr("24 / page"), qsTr("48 / page"), qsTr("96 / page"), qsTr("All")]
        currentIndex: root.perPage === 24 ? 0 : root.perPage === 48 ? 1 : root.perPage === 96 ? 2 : 3
        onActivated: function (index) { root.perPageRequested([24, 48, 96, 0][index]) }
    }
}
