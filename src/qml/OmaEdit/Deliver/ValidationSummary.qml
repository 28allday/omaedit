pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui
import OmaEdit.Media as Media

// The pre-flight readout under the settings: one line per check, green
// ticks for what passed, amber for warnings the editor may accept, red for
// what blocks the job; Details unfolds the long list; the size estimate
// and free space close the panel.
Rectangle {
    id: root

    // [{level: ok|warning|error, text}]
    property var rows: []
    property string sizeText: ""
    property string freeText: ""
    // The timecode the file will say it starts at ("" for a kind that holds none).
    property string startText: ""
    property bool hasMedia: false
    property real maximumHeight: 400

    signal detailsToggled()
    // The shape line's choices: how a picture of another shape fills the
    // delivery's frame ("blur", "fill", "fit"), or a reframed copy of the
    // timeline at the delivery's size.
    signal framingChosen(string framing)
    signal reshapeRequested(int width, int height)

    readonly property int errorCount: rows.filter(function (r) { return r.level === "error" }).length
    readonly property int warningCount: rows.filter(function (r) { return r.level === "warning" }).length
    property bool showAll: false
    readonly property int shown: showAll ? rows.length : Math.min(rows.length, 6)
    readonly property var ordered: {
        var e = rows.filter(function (r) { return r.level === "error" })
        var w = rows.filter(function (r) { return r.level === "warning" })
        // The shape line stays in view once answered, so its choice can change.
        var shape = rows.filter(function (r) { return r.level !== "error" && r.level !== "warning" && r.shape === true })
        var o = rows.filter(function (r) { return r.level !== "error" && r.level !== "warning" && r.shape !== true })
        return e.concat(w).concat(shape).concat(o).slice(0, shown)
    }

    color: Theme.panelBg
    implicitHeight: Math.min(maximumHeight, title.height + list.height + footer.height + Theme.s3 * 2)

    Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.hairline; color: Theme.border }

    Media.SectionTitle {
        id: title
        anchors.top: parent.top
        anchors.topMargin: Theme.s1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        text: qsTr("Validation Summary")

        ToolButton {
            objectName: "validationDetails"
            visible: root.rows.length > 6
            text: root.showAll ? qsTr("Less") : qsTr("Details")
            showLabel: true
            onClicked: root.showAll = !root.showAll
        }
    }

    C.ScrollView {
        id: viewport
        objectName: "validationViewport"
        anchors.top: title.bottom
        anchors.bottom: footer.top
        anchors.bottomMargin: Theme.s2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        contentWidth: availableWidth
        contentHeight: list.height
        clip: true
        C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
        C.ScrollBar.vertical: ScrollBar {}
        Column {
            id: list
            objectName: "validationList"
            width: viewport.availableWidth
            spacing: Theme.s1

        Repeater {
            model: root.ordered
            delegate: Row {
                id: line
                required property var modelData
                required property int index
                objectName: "validation:" + modelData.level + ":" + index
                width: list.width
                spacing: Theme.s2
                Icon {
                    name: line.modelData.level === "error" ? "circle-alert" : line.modelData.level === "warning" ? "circle-alert" : "circle-check"
                    size: Theme.szIcon
                    color: line.modelData.level === "error" ? Theme.danger : line.modelData.level === "warning" ? Theme.warning : Theme.audio
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    width: line.width - Theme.szIcon - Theme.s2
                    spacing: Theme.s1
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: line.modelData.text
                        wrapMode: Text.Wrap
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fsBase
                        color: line.modelData.level === "error" ? Theme.danger : line.modelData.level === "warning" ? Theme.warning : Theme.textPrimary
                    }
                    // Another shape than the edit's: the ways to fill the
                    // frame, the reframed copy first. Only that line has them.
                    Loader {
                        active: line.modelData.shape === true
                        visible: active
                        width: parent.width
                        sourceComponent: Component {
                            Flow {
                                width: parent ? parent.width : 0
                                spacing: Theme.s1
                                Chip {
                                    objectName: "shape:reframe"
                                    label: qsTr("Make a %1 copy and reframe it").arg(line.modelData.deliveryShape || "")
                                    tip: qsTr("Copy this timeline at %1×%2 and run Reframe to Fit on its clips, so each shot fills the frame and follows its subject. Export the copy.")
                                         .arg(line.modelData.deliveryWidth || 0).arg(line.modelData.deliveryHeight || 0)
                                    onClicked: root.reshapeRequested(line.modelData.deliveryWidth, line.modelData.deliveryHeight)
                                }
                                Chip {
                                    objectName: "shape:blur"
                                    label: qsTr("Blurred backdrop")
                                    tip: qsTr("Show the whole picture over a blurred, dimmed copy of itself that fills the frame")
                                    selected: line.modelData.framing === "blur"
                                    onClicked: root.framingChosen("blur")
                                }
                                Chip {
                                    objectName: "shape:fill"
                                    label: qsTr("Fill and crop")
                                    tip: qsTr("Fill the frame from the middle of the picture; the edges that do not fit are cropped")
                                    selected: line.modelData.framing === "fill"
                                    onClicked: root.framingChosen("fill")
                                }
                                Chip {
                                    objectName: "shape:fit"
                                    label: qsTr("Black bars")
                                    tip: qsTr("Show the whole picture with black bars around it")
                                    selected: line.modelData.framing === "fit"
                                    onClicked: root.framingChosen("fit")
                                }
                            }
                        }
                    }
                }
            }
        }
        Text {
            visible: root.rows.length === 0
            text: root.hasMedia ? qsTr("Checking…") : qsTr("Open a project to check its render")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fsLabel
            color: Theme.textMuted
        }
    }

    }
    Column {
        id: footer
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.s3
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.s3
        anchors.rightMargin: Theme.s3
        spacing: Theme.s1

        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.border }
        Item {
            width: parent.width
            height: Theme.hRow
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Start Timecode in the File")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textSecondary
            }
            Text {
                objectName: "deliverStartTimecode"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.startText !== "" ? root.startText : "—"
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textPrimary
            }
        }
        Item {
            width: parent.width
            height: Theme.hRow
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Estimated File Size")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textSecondary
            }
            Text {
                objectName: "estimateSize"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.sizeText !== "" ? root.sizeText : "—"
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textPrimary
            }
        }
        Item {
            width: parent.width
            height: Theme.hRow
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Available Disk Space")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textSecondary
            }
            Text {
                objectName: "diskSpace"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.freeText !== "" ? root.freeText : "—"
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsBase
                color: Theme.textPrimary
            }
        }
    }
}
