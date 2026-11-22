pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import OmaEdit.Ui

Item {
    id: root
    objectName: "mediaRelinkControls"
    required property var engine
    property var requestedIds: []
    property string currentPath: ""
    property string pendingBrowseSource: ""
    property string searchId: ""
    property string applyId: ""
    property var candidates: []
    property var unresolved: []
    property var selected: ({})
    property var skipped: ({})
    readonly property bool textActive: dialog.visible
    readonly property var entries: {
        var rows = requestedIds.length ? engine.library.filter(function(e) { return root.requestedIds.indexOf(e.libraryId) >= 0 && !e.multicam }) : engine.missingMedia
        var seen = {}, unique = []
        rows.forEach(function(e) { if (e.path && !seen[e.path]) { seen[e.path] = true; unique.push(e) } })
        return unique
    }
    readonly property var currentSource: entries.find(function(e) { return e.path === root.currentPath }) || entries[0] || ({})
    readonly property var eligible: entries.filter(function(e) { return !root.skipped[e.path] })
    readonly property var searchJob: engine.mediaTasks.jobs.find(function(j) { return j.id === root.searchId }) || ({})
    readonly property var applyJob: engine.mediaTasks.jobs.find(function(j) { return j.id === root.applyId }) || ({})
    readonly property bool searching: searchJob.state === "queued" || searchJob.state === "running"
    readonly property string applyLabel: ({queued: qsTr("Queued"), running: qsTr("Validating"), applying: qsTr("Applying"), completed: qsTr("Completed"), failed: qsTr("Failed"), partial: qsTr("Validation failed"), cancelled: qsTr("Cancelled")})[applyJob.state] || ""
    readonly property bool applying: ["queued","running","applying"].indexOf(applyJob.state) >= 0
    readonly property bool busy: searching || applying
    readonly property var rules: ({matchFilename: matchName.checked, matchRelative: matchRelative.checked,
        matchSize: matchSize.checked, matchDuration: matchFrames.checked, matchTimecode: matchTc.checked,
        matchChecksum: matchHash.checked, caseSensitive: matchCase.checked,
        originalRoot: originalBase.text.trim(), replacementRoot: folder.text.trim()})
    onRulesChanged: if (!busy && candidates.length) { candidates = []; unresolved = []; selected = ({}); searchId = "" }
    function folderOf(path) { return path.slice(0,path.lastIndexOf("/")) || "/" }
    function commonFolder(rows) {
        if (!rows.length) return ""
        var parts = folderOf(rows[0].path).split("/")
        rows.forEach(function(e) { var other = folderOf(e.path).split("/"); while (parts.length && parts.some(function(p,i) { return p !== other[i] })) parts.pop() })
        return parts.join("/") || "/"
    }
    function open(indexes) {
        requestedIds = (indexes || []).filter(function(i) { return i >= 0 && i < root.engine.library.length }).map(function(i) { return root.engine.library[i].libraryId })
        currentPath = entries.length ? entries[0].path : ""
        candidates = []; unresolved = []; selected = ({}); skipped = ({})
        searchId = ""; applyId = ""; pendingBrowseSource = ""; replacement.text = ""; folder.text = ""
        originalBase.text = commonFolder(entries)
        dialog.open(); engine.refreshMediaStatus()
    }
    function selectSource(path) { currentPath = path; replacement.text = "" }
    function choose(original,destination,on) {
        var next = Object.assign({},selected)
        if (on && !skipped[original]) next[original] = destination
        else delete next[original]
        selected = next
    }
    function skipSource() {
        var next = Object.assign({},skipped), path = currentSource.path
        if (next[path]) delete next[path]; else next[path] = true
        skipped = next
        if (next[path]) choose(path,"",false)
    }
    function selectUnique() {
        var choices = {}, result = {}
        candidates.forEach(function(r) {
            if (root.skipped[r.source] || !root.entries.some(function(e) { return e.path === r.source })) return
            if (!choices[r.source]) choices[r.source] = []
            if (choices[r.source].indexOf(r.destination) < 0) choices[r.source].push(r.destination)
        })
        Object.keys(choices).forEach(function(path) { if (choices[path].length === 1) result[path] = choices[path][0] })
        selected = result
    }
    function startSearch() {
        if (busy || !eligible.length || !folder.text.trim()) return
        candidates = []; unresolved = []; selected = ({})
        searchId = engine.startMediaTask("relink",[folder.text.trim()],Object.assign({missing: eligible},rules))
    }
    function nearby(path) {
        var base = folderOf(path)
        if (matchRelative.checked && originalBase.text.trim()) {
            var original = currentSource.path || "", oldBase = originalBase.text.trim().replace(/\/$/,"")
            if (original.indexOf(oldBase + "/") === 0) {
                var depth = original.slice(oldBase.length + 1).split("/").length
                base = path.split("/").slice(0,-depth).join("/") || "/"
            }
        }
        folder.text = base
        startSearch()
    }
    function applySelected() {
        if (busy || !Object.keys(selected).length) return
        applyId = engine.relinkMedia(selected,rules)
    }
    function cancelTask() {
        if (searching) engine.mediaTasks.cancel(searchId)
        if (applyJob.state === "queued" || applyJob.state === "running") engine.mediaTasks.cancel(applyId)
    }
    Connections {
        target: root.engine
        function onLibraryChanged() {
            var paths = root.entries.map(function(e) { return e.path }), next = {}
            Object.keys(root.selected).forEach(function(path) { if (paths.indexOf(path) >= 0) next[path] = root.selected[path] })
            root.selected = next
            if (paths.indexOf(root.currentPath) < 0) { root.currentPath = paths[0] || ""; replacement.text = "" }
        }
        function onProjectPlaybackReset() { root.cancelTask(); root.searchId = ""; root.applyId = ""; root.selected = ({}); dialog.close() }
    }
    Connections {
        target: root.engine.mediaTasks
        function onPathsChosen(purpose,paths) {
            if (!dialog.visible || !paths.length) return
            if (purpose === "relink-file") {
                if (!root.entries.some(function(e) { return e.path === root.pendingBrowseSource })) return
                root.currentPath = root.pendingBrowseSource; replacement.text = paths[0]
                if (findNearby.checked) root.nearby(paths[0])
            }
            if (purpose === "relink-search") folder.text = paths[0]
            if (purpose === "relink-original-base") originalBase.text = paths[0]
        }
        function onFinished(id,result) {
            if (id !== root.searchId) return
            root.candidates = (result.report || []).filter(function(r) { return r.state === "match" })
            root.unresolved = (result.report || []).filter(function(r) { return r.state === "unmatched" })
            root.selected = ({})
        }
    }
    component Caption: C.Label { Layout.fillWidth: true; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.fsLabel; wrapMode: Text.Wrap }
    ModalDialog {
        id: dialog
        objectName: "mediaRelinkDialog"
        width: Math.min(840,parent ? parent.width - Theme.s5 * 2 : 840)
        height: Math.min(780,parent ? parent.height - Theme.s5 * 2 : 780)
        title: qsTr("Relink Media")
        standardButtons: C.Dialog.Close
        onClosed: if (root.searching) root.engine.mediaTasks.cancel(root.searchId)
        contentItem: C.ScrollView {
            clip: true
            contentWidth: availableWidth
            C.ScrollBar.horizontal.policy: C.ScrollBar.AlwaysOff
            C.ScrollBar.vertical: ScrollBar { atRest: true }
            ColumnLayout {
                width: parent.width
                spacing: Theme.s2
                Caption { text: qsTr("%1 sources · %2 included. Review replacements before applying; edits, logging and timing are preserved.").arg(root.entries.length).arg(root.eligible.length) }
                ListView {
                    objectName: "relinkSources"
                    Layout.fillWidth: true; Layout.preferredHeight: 100
                    model: root.entries; clip: true
                    C.ScrollBar.vertical: ScrollBar { atRest: true }
                    delegate: C.ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        height: 48
                        enabled: !root.busy
                        highlighted: root.currentSource.path === modelData.path
                        onClicked: root.selectSource(modelData.path)
                        Accessible.name: (modelData.name || "") + " " + modelData.path + (root.skipped[modelData.path] ? qsTr(" skipped") : "")
                        contentItem: Column {
                            Text { width: parent.width; elide: Text.ElideRight; text: (modelData.name || modelData.path.split("/").pop()) + (root.skipped[modelData.path] ? qsTr(" · Skipped") : modelData.offline ? qsTr(" · Missing") : ""); color: Theme.textPrimary; font.pixelSize: Theme.fsControl }
                            Text { width: parent.width; elide: Text.ElideMiddle; text: modelData.path; color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
                        }
                    }
                    Caption { anchors.centerIn: parent; width: parent.width; visible: root.entries.length === 0; text: qsTr("No missing sources."); horizontalAlignment: Text.AlignHCenter }
                }
                RowLayout {
                    ToolButton { objectName: "relinkSkip"; text: root.skipped[root.currentSource.path] ? qsTr("Include Source") : qsTr("Skip Source"); enabled: !!root.currentSource.path && !root.busy; onClicked: root.skipSource() }
                    LineEdit { id: replacement; objectName: "mediaReplacementPath"; tip: qsTr("File on disk that stands in for the selected missing source"); Layout.fillWidth: true; hint: qsTr("Replacement for selected source"); enabled: !root.busy }
                    ToolButton { text: qsTr("Browse…"); enabled: !!root.currentSource.path && !root.busy; onClicked: { root.pendingBrowseSource = root.currentSource.path; root.engine.mediaTasks.browse("relink-file","file") } }
                    ToolButton { objectName: "relinkOne"; text: qsTr("Relink One"); enabled: !!root.currentSource.path && !root.skipped[root.currentSource.path] && replacement.text.trim() !== "" && !root.busy; onClicked: { var mapping = {}; mapping[root.currentSource.path] = replacement.text.trim(); root.applyId = root.engine.relinkMedia(mapping) } }
                }
                RowLayout {
                    Toggle { id: findNearby; tip: qsTr("After locating one file, look beside it for the others"); label: qsTr("Search nearby after locating a file"); checked: true; enabled: !root.busy }
                    ToolButton { objectName: "relinkNearby"; text: qsTr("Find Nearby"); enabled: replacement.text.trim() !== "" && !root.busy; onClicked: root.nearby(replacement.text.trim()) }
                }
                Rectangle { Layout.fillWidth: true; height: Theme.hairline; color: Theme.border }
                Caption { text: qsTr("Every selected rule must match. File size and frame count use recorded values when available; timecode and SHA256 require recorded source information.") }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.s2
                    enabled: !root.busy
                    Toggle { id: matchName; objectName: "relinkMatchName"; tip: qsTr("Require the replacement to carry the same filename"); checked: true; label: qsTr("Filename") }
                    Toggle { id: matchRelative; objectName: "relinkMatchRelative"; tip: qsTr("Require the same path below the two base folders"); label: qsTr("Relative path") }
                    Toggle { id: matchSize; tip: qsTr("Require the file size to match the recorded size"); checked: true; label: qsTr("File size") }
                    Toggle { id: matchFrames; tip: qsTr("Require the same frame count as the missing source"); checked: true; label: qsTr("Frame count") }
                    Toggle { id: matchTc; objectName: "relinkMatchTimecode"; tip: qsTr("Require the same start timecode, where one was recorded"); label: qsTr("Timecode") }
                    Toggle { id: matchHash; objectName: "relinkMatchHash"; tip: qsTr("Require the same SHA256 checksum, where one was recorded"); label: qsTr("SHA256") }
                    Toggle { id: matchCase; tip: qsTr("Compare filenames and paths with letter case exactly"); label: qsTr("Case sensitive") }
                }
                RowLayout {
                    visible: matchRelative.checked
                    Caption { Layout.preferredWidth: 100; Layout.fillWidth: false; text: qsTr("Original base") }
                    LineEdit { id: originalBase; objectName: "relinkOriginalBase"; tip: qsTr("Folder the missing media used to sit under"); Layout.fillWidth: true; hint: qsTr("Original folder before media moved"); enabled: !root.busy }
                    ToolButton { text: qsTr("Browse…"); enabled: !root.busy; onClicked: root.engine.mediaTasks.browse("relink-original-base") }
                }
                RowLayout {
                    Caption { Layout.preferredWidth: 100; Layout.fillWidth: false; text: qsTr("Search folder") }
                    LineEdit { id: folder; objectName: "relinkSearchFolder"; tip: qsTr("Folder, and everything below it, searched for replacements"); Layout.fillWidth: true; hint: qsTr("Replacement base folder, including children"); enabled: !root.busy }
                    ToolButton { text: qsTr("Browse…"); enabled: !root.busy; onClicked: root.engine.mediaTasks.browse("relink-search") }
                    ToolButton { objectName: "relinkSearch"; text: qsTr("Search"); enabled: folder.text.trim() !== "" && root.eligible.length > 0 && !root.busy; onClicked: root.startSearch() }
                    ToolButton { objectName: "relinkCancel"; text: qsTr("Cancel Task"); enabled: root.searching || root.applyJob.state === "queued" || root.applyJob.state === "running"; onClicked: root.cancelTask() }
                }
                Caption { objectName: "relinkSearchSummary"; text: root.searchId ? (root.searchJob.state === "cancelled" ? qsTr("Cancelled · ") : "") + (root.searchJob.message || (root.searchJob.state === "cancelled" ? qsTr("Search stopped before it started.") : qsTr("Searching…"))) : qsTr("Search finds candidates without changing project references.") }
                ListView {
                    objectName: "relinkCandidates"
                    Layout.fillWidth: true; Layout.preferredHeight: 132
                    clip: true; model: root.candidates; spacing: Theme.s2
                    C.ScrollBar.vertical: ScrollBar { atRest: true }
                    delegate: RowLayout {
                        id: candidateRow
                        required property var modelData
                        width: ListView.view.width
                        Toggle { tip: qsTr("Use this candidate as the replacement for its missing source"); Accessible.name: qsTr("Use replacement %1 for %2").arg(candidateRow.modelData.destination).arg(candidateRow.modelData.source); checked: root.selected[parent.modelData.source] === parent.modelData.destination; enabled: !root.busy && !root.skipped[parent.modelData.source] && root.entries.some(function(e) { return e.path === candidateRow.modelData.source }); onClicked: root.choose(parent.modelData.source,parent.modelData.destination,checked) }
                        Caption { text: parent.modelData.source + "\n→ " + parent.modelData.destination + "\n" + parent.modelData.message; Layout.minimumWidth: 0; wrapMode: Text.WrapAnywhere }
                    }
                    Caption { anchors.centerIn: parent; width: parent.width; visible: root.candidates.length === 0; text: root.searching ? qsTr("Searching…") : qsTr("No matching candidates."); horizontalAlignment: Text.AlignHCenter }
                }
                RowLayout {
                    ToolButton { objectName: "relinkSelectUnique"; text: qsTr("Select All Unique Matches"); enabled: root.candidates.length > 0 && !root.busy; onClicked: root.selectUnique() }
                    ToolButton { objectName: "relinkApplySelected"; text: qsTr("Apply %1 Selected").arg(Object.keys(root.selected).length); enabled: Object.keys(root.selected).length > 0 && !root.busy; onClicked: root.applySelected() }
                    ToolButton { text: qsTr("Clear Selection"); enabled: Object.keys(root.selected).length > 0 && !root.busy; onClicked: root.selected = ({}) }
                }
                Caption { text: qsTr("Ambiguous matches require an explicit choice. Applied replacements must have the same frame count and rate as their source.") }
                Caption { objectName: "relinkApplySummary"; visible: root.applyId !== ""; text: root.applyLabel + " · " + (root.applyJob.message || ""); color: root.applyJob.state === "failed" || root.applyJob.state === "partial" ? Theme.warning : Theme.textSecondary }
                Caption { visible: root.unresolved.length > 0; text: qsTr("%1 unresolved sources").arg(root.unresolved.length) }
                ListView {
                    objectName: "relinkUnresolved"
                    visible: root.unresolved.length > 0
                    Layout.fillWidth: true; Layout.preferredHeight: visible ? Math.min(140,root.unresolved.length * 56) : 0
                    model: root.unresolved; clip: true; spacing: Theme.s2
                    C.ScrollBar.vertical: ScrollBar { atRest: true }
                    delegate: Caption { required property var modelData; width: ListView.view.width; text: modelData.source + "\n" + modelData.message; wrapMode: Text.WrapAnywhere }
                }
            }
        }
    }
}
