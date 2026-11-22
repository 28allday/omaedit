#include "playerworker_p.h"
#include <utility>

namespace {

const QEvent::Type gradeEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
const QEvent::Type flushGradeEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
struct GradeEvent : QEvent {
    GradeEvent(int track, int index, const QVariantMap &grade, quint64 gesture)
        : QEvent(gradeEventType), track(track), index(index), grade(grade), gesture(gesture) {}
    int track, index;
    QVariantMap grade;
    quint64 gesture;
};
const QEvent::Type editPropertiesEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
const QEvent::Type flushEditPropertiesEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
struct EditPropertiesEvent : QEvent {
    EditPropertiesEvent(int track, int index, const QVariantMap &patch, quint64 gesture)
        : QEvent(editPropertiesEventType), track(track), index(index), patch(patch), gesture(gesture) {}
    int track, index;
    QVariantMap patch;
    quint64 gesture;
};
}

// ── Color page ────────────────────────────────────────────────────────────

void PlayerWorker::queueGrade(int track, int index, const QVariantMap &grade, quint64 gesture) {
    QCoreApplication::postEvent(this, new GradeEvent(track, index, grade, gesture));
}

void PlayerWorker::queueEditProperties(int track, int index, const QVariantMap &patch, quint64 gesture) {
    QCoreApplication::postEvent(this, new EditPropertiesEvent(track, index, patch, gesture));
}

void PlayerWorker::flushQueuedEditProperties() {
    if (m_queuedEditTrack < 0) return;
    const int track = m_queuedEditTrack, index = m_queuedEditIndex;
    const auto patch = m_queuedEditProperties;
    const auto gesture = m_queuedEditGesture;
    m_queuedEditTrack = m_queuedEditIndex = -1;
    m_queuedEditProperties.clear();
    applyEditProperties(track, index, patch, gesture);
}

bool PlayerWorker::staleEdit() {
    if (!std::exchange(m_dispatchFresh, false) || !m_replayAwaited)
        return false;
    emit editRefused(tr("The timeline changed before this edit arrived, so it was not applied."));
    return true;
}

void PlayerWorker::acknowledgeReplay(int serial) {
    m_replayAnswered = true;
    m_replayDrawn = serial;
    if (serial == m_replaySerial)
        m_replayAwaited = false;
}

bool PlayerWorker::event(QEvent *event) {
    // A drag's queued grade or property change names its clip by place too.
    if (m_replayAwaited && (event->type() == editPropertiesEventType || event->type() == gradeEventType)) {
        emit editRefused(tr("The timeline changed before this edit arrived, so it was not applied."));
        return true;
    }
    if (event->type() == editPropertiesEventType) {
        flushQueuedGrade();
        const auto *edit = static_cast<EditPropertiesEvent *>(event);
        if (m_queuedEditTrack != edit->track || m_queuedEditIndex != edit->index
            || m_queuedEditGesture != edit->gesture)
            flushQueuedEditProperties();
        m_queuedEditTrack = edit->track; m_queuedEditIndex = edit->index;
        m_queuedEditGesture = edit->gesture;
        for (auto it = edit->patch.begin(); it != edit->patch.end(); ++it)
            m_queuedEditProperties[it.key()] = it.value();
        if (!m_editFlushPosted) {
            m_editFlushPosted = true;
            QCoreApplication::postEvent(this, new QEvent(flushEditPropertiesEventType));
        }
        return true;
    }
    // Saves, undo and timeline switches must observe the last pointer position.
    flushQueuedEditProperties();
    if (event->type() == flushEditPropertiesEventType) {
        m_editFlushPosted = false;
        return true;
    }
    if (event->type() == gradeEventType) {
        const auto *edit = static_cast<GradeEvent *>(event);
        if (m_queuedGradeTrack != edit->track || m_queuedGradeIndex != edit->index
            || m_queuedGradeGesture != edit->gesture)
            flushQueuedGrade();
        m_queuedGradeTrack = edit->track;
        m_queuedGradeIndex = edit->index;
        m_queuedGrade = edit->grade;
        m_queuedGradeGesture = edit->gesture;
        if (!m_gradeFlushPosted) {
            m_gradeFlushPosted = true;
            QCoreApplication::postEvent(this, new QEvent(flushGradeEventType));
        }
        return true;
    }
    // Queued saves, undo, clip edits and sequence switches must see all
    // preceding grades. Never let a delayed grade cross one of those commands
    // and land on a different clip/sequence that happens to reuse its index.
    flushQueuedGrade();
    if (event->type() == flushGradeEventType) {
        m_gradeFlushPosted = false;
        return true;
    }
    // A deferred delete ends this object inside QObject::event: nothing of it
    // may be touched after that.
    if (event->type() == QEvent::DeferredDelete)
        return QObject::event(event);
    m_dispatchFresh = event->type() == QEvent::MetaCall;
    const bool handled = QObject::event(event);
    m_dispatchFresh = false;
    // An undo, redo or timeline switch ran and announced a new clip list:
    // edits by place wait until the window says it has that list. (Called
    // directly, as a test does, the next call already knows the new list.)
    if (std::exchange(m_replaying, false) && m_replayAnswered && m_replayDrawn != m_replaySerial)
        m_replayAwaited = true;
    return handled;
}

void PlayerWorker::flushQueuedGrade() {
    if (m_queuedGradeTrack < 0)
        return;
    const int track = m_queuedGradeTrack, index = m_queuedGradeIndex;
    const QVariantMap grade = m_queuedGrade;
    const auto gesture = m_queuedGradeGesture;
    m_queuedGradeTrack = m_queuedGradeIndex = -1;
    m_queuedGrade.clear();
    applyGrade(track, index, grade, gesture);
}

void PlayerWorker::setGrade(int track, int index, const QVariantMap &grade) {
    if (staleEdit())
        return;
    applyGrade(track, index, grade, 0);
}

void PlayerWorker::applyGrade(int track, int index, const QVariantMap &grade, quint64 gesture) {
    if (exportActive() || m_timeline.trackLocked(track))
        return;
    const QString error = GradeEngine::validate(grade);
    if (!error.isEmpty()) { emit gradeVersionFinished(error); return; }
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setGradeAt(track, index, grade))
        return;
    recordGestureEdit(before, gesture, tr("Set grade"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::applyColourSource(int track, int index, const QString &scope, const QVariantMap &fields) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    QString file;
    for (const QVariant &v : before) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index) file = e.value(ClipKey::path).toString();
    }
    bool changed = false;
    for (const QVariant &v : before) {
        const QVariantMap e = v.toMap();
        const QString path = e.value(ClipKey::path).toString();
        // Pictures made here (titles, colour) are not footage: they keep their own.
        if (path.isEmpty() || path.startsWith(QLatin1String("color:")) || !e.value(ClipKey::title).toString().isEmpty()
            || e.value(ClipKey::gap).toBool() || e.value(ClipKey::audioOnly).toBool()) continue;
        if (scope == QLatin1String("file") ? path != file : scope != QLatin1String("timeline")) continue;
        if (m_timeline.trackLocked(e.value(ClipKey::track).toInt())) continue;
        QVariantMap grade = e.value(ClipKey::grade).toMap();
        QVariantMap management = ColourPipeline::effective(grade);
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) management[it.key()] = it.value();
        grade["management"] = management;
        changed |= m_timeline.setGradeAt(e.value(ClipKey::track).toInt(), e.value(ClipKey::index).toInt(), grade);
    }
    if (!changed)
        return;
    recordEdit(before, true, tr("Set colour source"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setGradeVersions(int track, int index, const QVariantList &versions, int active) {
    if (staleEdit())
        return;
    if (exportActive() || m_timeline.trackLocked(track))
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setGradeVersionsAt(track, index, versions, active))
        return;
    recordEdit(before, true, tr("Set grade versions"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::editGradeVersion(int track, int index, const QString &action,
                                    int version, const QString &name) {
    if (exportActive()) {
        emit gradeVersionFinished(QStringLiteral("Cannot change grade versions while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    if (!m_timeline.editGradeVersionAt(track, index, action, version, name, &error)) {
        if (!error.isEmpty()) emit gradeVersionFinished(error);
        return;
    }
    recordEdit(before, true, tr("Edit grade version"));
    announceTimeline();
    refreshFrame();
    emit gradeVersionFinished(action == QStringLiteral("add") ? QStringLiteral("Grade version created")
        : action == QStringLiteral("rename") ? QStringLiteral("Grade version renamed")
        : action == QStringLiteral("remove") ? QStringLiteral("Grade version deleted")
        : QStringLiteral("Grade version switched"));
}

void PlayerWorker::pasteColor(int track, int index, const QString &kind,
                              const QVariantMap &payload, int afterNode, const QString &parentPath) {
    if (exportActive()) {
        emit colorPasteFinished(track, index, -1, QStringLiteral("Cannot paste a correction while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    const int selected = m_timeline.pasteColorAt(track, index, kind, payload, afterNode, &error, parentPath);
    if (selected < 0) {
        emit colorPasteFinished(track, index, -1, error);
        return;
    }
    recordEdit(before, true, tr("Paste colour"));
    announceTimeline();
    refreshFrame();
    emit colorPasteFinished(track, index, selected, kind == QStringLiteral("node")
        ? QStringLiteral("Correction node pasted") : QStringLiteral("Complete grade pasted"));
}

void PlayerWorker::setNodeLut(int track, int index, int node, const QString &nodeId, const QString &path, double intensity) {
    if (exportActive()) { emit nodeLutFinished(false, QStringLiteral("Cannot change LUTs while exporting.")); return; }
    const QVariantList before = m_timeline.entries();
    QString error;
    if (!m_timeline.setNodeLutAt(track, index, node, nodeId, path, intensity, &error)) {
        if (error.isEmpty()) refreshFrame();
        emit nodeLutFinished(false, error.isEmpty() ? QStringLiteral("LUT is unchanged") : error); return;
    }
    recordEdit(before, true, tr("Set layer lut"));
    announceTimeline();
    refreshFrame();
    emit nodeLutFinished(true, path.isEmpty() ? QStringLiteral("LUT removed") : QStringLiteral("LUT applied"));
}

void PlayerWorker::previewNodeLut(int request, int track, int index, int node, const QString &nodeId,
                                   int frame, const QString &path, double intensity) {
    if (exportActive()) { emit lutPreviewReady(request, {}, {}, QStringLiteral("LUT previews are unavailable while exporting.")); return; }
    QImage before, after;
    QString error;
    m_timeline.previewNodeLutAt(track, index, node, nodeId, frame, path, intensity, &before, &after, &error);
    emit lutPreviewReady(request, before, after, error);
}

void PlayerWorker::colorMatte(int request, int track, int index, const QString &nodeId, int frame) {
    QString error;
    const QImage image = exportActive() ? QImage() : m_timeline.colorMatteAt(track, index, nodeId, frame, &error);
    emit colorMatteReady(request, image, error);
}

void PlayerWorker::effectPreview(int request, int track, int index, int frame, bool matte, const QSize &size) {
    QString error;
    const QImage image = exportActive() ? QImage() : m_timeline.effectPreviewAt(track, index, frame, matte, size, &error);
    emit effectPreviewReady(request, image, error);
}

void PlayerWorker::autoBalance(int track, int index, int clipFrame) {
    if (exportActive()) {
        emit autoBalanceFinished(track, index, -1, QStringLiteral("Cannot auto balance while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    const int node = m_timeline.autoBalanceAt(track, index, clipFrame, &error);
    if (node < 0) {
        emit autoBalanceFinished(track, index, -1, error);
        return;
    }
    recordEdit(before, true, tr("Auto balance"));
    announceTimeline();
    refreshFrame();
    emit autoBalanceFinished(track, index, node, QStringLiteral("Auto Balance added — adjust its Gain wheel or undo to restore."));
}

void PlayerWorker::matchColor(int track, int index, int clipFrame, int refTrack, int refIndex, int refFrame) {
    if (exportActive()) {
        emit matchColorFinished(track, index, -1, tr("Cannot match colour while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    const int node = m_timeline.matchColorAt(track, index, clipFrame, refTrack, refIndex, refFrame, &error);
    if (node < 0) {
        emit matchColorFinished(track, index, -1, error);
        return;
    }
    recordEdit(before, true, tr("Match colour"));
    announceTimeline();
    refreshFrame();
    emit matchColorFinished(track, index, node, tr("Match Colour added — adjust its Gain wheel or undo to restore."));
}
