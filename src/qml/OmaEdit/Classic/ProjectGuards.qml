import QtQml
import OmaEdit.Ui as Ui

// Opening, closing and starting a project from the window: each asks first
// when the open project has unsaved changes, exactly like Ctrl+N. The
// window holds one as `win.project`.
QtObject {
    id: root
    // The window: selection, entries and the other commands.
    required property var win
    required property var openPrompt
    required property var closePrompt
    required property var creationDialog
    required property var newPrompt

    // Opening a project over unsaved changes prompts, exactly like Ctrl+N.
    function guardedOpen(path) {
        if (backend.projectBusy || backend.exporting) return
        if (backend.dirty) {
            win.pendingOpenPath = path
            openPrompt.open()
            return
        }
        proceedOpen(path)
    }
    function proceedOpen(path) {
        if (path.indexOf("recovery:") === 0)
            backend.restoreProjectCopy(path.slice(9))
        else if (path === "")
            backend.openProjectDialog()
        else
            backend.openRecent(path)
    }
    function close() {
        if (backend.projectBusy || backend.exporting) return
        if (backend.dirty) closePrompt.open()
        else backend.closeProject()
    }
    function startNew() {
        if (backend.projectBusy || backend.exporting) return
        creationDialog.openFor()
    }
    function requestNew() {
        if (backend.dirty) newPrompt.open()
        else commitNew()
    }
    function commitNew() {
        creationDialog.pending=backend.createProject(creationDialog.context,creationDialog.draft)
        creationDialog.awaiting=false
    }
}
