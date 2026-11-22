import QtQml

// Follow a persisted cut through reordering. Legacy clips retain index selection.
QtObject {
    id: root
    property var entries: []
    property string scope: ""
    property int index: -1
    readonly property string clipId: identity
    property string identity: ""
    property bool restoring: false

    function remember() {
        var entry = index >= 0 && index < entries.length ? entries[index] : null
        identity = entry && entry.clipId ? String(entry.clipId) : ""
    }
    function reconcile() {
        if (identity !== "") {
            var found = -1
            for (var i = 0; i < entries.length; ++i) {
                if (String(entries[i].clipId || "") !== identity) continue
                if (found >= 0) { found = -1; break } // ambiguous IDs must not target an arbitrary clip
                found = i
            }
            restoring = true
            index = found
            restoring = false
            if (found < 0) identity = ""
        } else {
            if (index >= entries.length) index = -1
            remember()
        }
    }
    onIndexChanged: if (!restoring) remember()
    onEntriesChanged: reconcile()
    onScopeChanged: { index = -1; identity = "" }
}
