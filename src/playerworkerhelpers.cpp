// Helpers the PlayerWorker sources share: reading and writing what a cut carries.
#include "playerworker_p.h"

// Export stages run in their own process group (runExportProcess): signal
// all of it, or the process alone when its group is not set up.
void signalExportGroup(QProcess *process, int sig) {
    const qint64 pid = process ? process->processId() : 0;
    if (pid > 0 && ::kill(-pid_t(pid), sig) != 0)
        ::kill(pid_t(pid), sig);
}

bool completeSamplingFile(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && info.size() > 0;
}
bool publishSamplingFile(const QString &stage, const QString &path) {
    // Another job with the same cache identity may have finished first. Never
    // overwrite its completed file or touch its staging directory.
    return QFile::rename(stage, path) || completeSamplingFile(path);
}
QVariantMap sampledEntry(QVariantMap entry, const QString &path, const QVariantMap &profile) {
    const int frames = entry.value(ClipKey::frames).toInt();
    entry[ClipKey::path] = path;
    entry[ClipKey::speed] = 1.; entry[ClipKey::in] = 0; entry[ClipKey::out] = frames - 1; entry[ClipKey::length] = frames;
    entry.remove(ClipKey::frameBlend); entry.remove(ClipKey::freeze); entry.remove(ClipKey::sourceOrigin);
    // Interpretation has already been baked into these pictures and samples.
    // Keep the input colour transform, but do not slow the new file a second time.
    auto meta = entry.value(ClipKey::sourceMeta).toMap();
    meta.remove("forceFps"); meta.remove("sourceFps"); meta.remove("sourceDuration");
    const int num = profile.value("fpsNum").toInt(), den = profile.value("fpsDen").toInt();
    if (num > 0 && den > 0) {
        meta["fpsNum"] = num; meta["fpsDen"] = den; meta["fps"] = double(num) / den;
    }
    meta["duration"] = frames;
    entry[ClipKey::sourceMeta] = meta;
    return entry;
}
int entryEnd(const QVariantList &entries, int index) {
    const QVariantMap e = entries.value(index).toMap();
    return e.value(ClipKey::start).toInt()
           + e.value(ClipKey::frames).toInt();
}

QString historyLabel(const QList<QVariantList> &stack) {
    return stack.isEmpty() || stack.last().isEmpty() ? QString() : stack.last().last().toMap().value("omavid:undoLabel", QStringLiteral("Edit")).toString();
}
