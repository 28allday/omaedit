import QtQuick
import QtQuick.Controls as C
import OmaEdit.Ui

// The Edit page. Every boundary here is a real SplitView handle: the brief's
// percentages are starting sizes, not a fixed grid, because the right split
// for a job depends on the job.
Item {
    id: root

    // Narrow-width policy: the Inspector folds first, then the media panel —
    // the viewer is never the thing that gives way.
    readonly property bool inspectorVisible: width >= 1180
    readonly property bool mediaVisible: width >= 980

    C.SplitView {
        anchors.fill: parent
        orientation: Qt.Vertical

        handle: SplitterHandle {
            vertical: true
            pressed: C.SplitHandle.pressed
            hovered: C.SplitHandle.hovered
        }

        // ── Upper working area: media | viewer | inspector ──────────────────
        C.SplitView {
            C.SplitView.preferredHeight: root.height * 0.54
            C.SplitView.minimumHeight: 260
            orientation: Qt.Horizontal

            handle: SplitterHandle {
                pressed: C.SplitHandle.pressed
                hovered: C.SplitHandle.hovered
            }

            MediaPanel {
                visible: root.mediaVisible
                clips: MockProject.clips
                bins: MockProject.bins
                currentBin: 1
                currentIndex: 0
                C.SplitView.preferredWidth: root.width * 0.28
                C.SplitView.minimumWidth: 260
                C.SplitView.maximumWidth: 520
            }

            ProgramMonitor {
                C.SplitView.fillWidth: true
                C.SplitView.minimumWidth: 480
                projectName: MockProject.name
                timecode: MockProject.timecode
                duration: MockProject.duration
            }

            InspectorPanel {
                visible: root.inspectorVisible
                clip: MockProject.selectedClip
                C.SplitView.preferredWidth: root.width * 0.22
                C.SplitView.minimumWidth: 280
                C.SplitView.maximumWidth: 440
            }
        }

        // ── Timeline ────────────────────────────────────────────────────────
        TimelinePanel {
            C.SplitView.fillHeight: true
            C.SplitView.minimumHeight: 220
        }
    }
}
