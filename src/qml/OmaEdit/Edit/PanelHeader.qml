pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The tab strip that caps every panel. One ruled line with one lit segment,
// optional trailing controls tucked to the right of the tabs.
Rectangle {
    id: root

    property var tabs: []
    property int currentIndex: 0
    property bool uppercase: true
    property string tabPrefix: ""
    readonly property bool compactTabs: {
        var naturalWidth = 0
        for (var i = 0; i < strip.children.length; ++i)
            naturalWidth += strip.children[i].naturalWidth || 0
        return naturalWidth > tabViewport.width
    }

    signal activated(int index)

    default property alias trailing: trailingRow.data

    implicitHeight: Theme.hTab
    color: Theme.panelBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    Flickable {
        id: tabViewport
        anchors.left: parent.left
        anchors.leftMargin: Theme.s1
        anchors.right: trailingRow.left
        anchors.rightMargin: trailingRow.width > 0 ? Theme.s1 : 0
        height: parent.height
        contentWidth: strip.width
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollIndicator.horizontal: C.ScrollIndicator {}

        Row {
            id: strip
            height: tabViewport.height
            Repeater {
                model: root.tabs
                delegate: TabButton {
                    required property int index
                    required property string modelData
                    objectName: root.tabPrefix ? root.tabPrefix + index : ""
                    text: modelData
                    tip: qsTr("Show the %1 tab in this panel").arg(modelData)
                    height: strip.height
                    horizontalPadding: root.compactTabs ? Theme.s1 : Theme.s3
                    uppercase: root.uppercase
                    fontSize: Theme.fsLabel
                    checked: index === root.currentIndex
                    onClicked: {
                        // Owners must not BIND currentIndex (this write would sever it);
                        // they mirror their own tab state into it — see InspectorPanel.
                        root.currentIndex = index
                        root.activated(index)
                        checked = Qt.binding(function () { return index === root.currentIndex })
                    }
                }
            }
        }
    }

    Row {
        id: trailingRow
        anchors.right: parent.right
        anchors.rightMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1
    }
}
