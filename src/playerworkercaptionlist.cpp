#include "playerworker_p.h"

// The caption list's edits (Edit page): the timeline's caption settings,
// captions' words, their times and their speakers' names. Each one undo step.

void PlayerWorker::setCaptionSettings(const QVariantMap &settings, const QString &label) {
    if (staleEdit() || exportActive() || settings == m_timeline.captionSettings())
        return;
    const QVariantList before = snapshotNow();
    m_timeline.setCaptionSettings(settings);
    recordEdit(before, false, label);
    announceTimeline();
}

void PlayerWorker::editTranscript(const QVariantMap &edit, const QString &label) {
    if (staleEdit() || exportActive() || edit.isEmpty())
        return;
    // Nothing on the timeline changes: the step carries the line's words
    // before and after, and restoreHistory hands them back.
    QVariantList before = snapshotNow();
    QVariantMap sentinel = before.last().toMap();
    sentinel.insert(QStringLiteral("omavid:transcript"), edit);
    before.last() = sentinel;
    recordEdit(before, false, label);
    emit transcriptEditReplayed(edit, true);
    announceTimeline();
}

void PlayerWorker::setCaptionTexts(const QVariantList &changes, const QString &label) {
    if (staleEdit() || exportActive() || changes.isEmpty())
        return;
    const QVariantList before = snapshotNow();
    int changed = 0;
    for (const QVariant &v : changes) {
        const QVariantMap change = v.toMap();
        const int index = change.value(QStringLiteral("index")).toInt();
        const QString text = change.value(QStringLiteral("text")).toString();
        int track = -1, clip = -1;
        if (text.trimmed().isEmpty() || !m_timeline.connectedAddress(index, &track, &clip) || m_timeline.trackLocked(track))
            continue;
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        if (!e.value(ClipKey::caption).toBool() || e.value(ClipKey::title).toString() == text)
            continue;
        // Words typed by hand: Update Captions asks before it replaces them.
        QVariantMap style = Timeline::titleStyleOf(e);
        style.insert(QStringLiteral("typed"), true);
        if (m_timeline.updateTitleAt(index, text, style))
            ++changed;
    }
    emit captionTextsChanged(changed);
    if (changed == 0)
        return;
    recordEdit(before, false, label);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::retimeCaptions(const QVariantList &changes, const QString &label) {
    if (staleEdit() || exportActive() || changes.isEmpty())
        return;
    // Each caption by its place on its track (its clip number there, which a
    // move keeps, as no caption passes another), where it is and where it goes.
    struct Move { int track, clip, in, start, frames, toStart, toFrames, fromStart, fromFrames; };
    QList<Move> moves;
    QHash<int, QList<QPair<int, int>>> spans; // every clip of the tracks moved, by track: start, end
    int kept = 0;
    for (const QVariant &v : changes) {
        const QVariantMap change = v.toMap();
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(change.value(QStringLiteral("index")).toInt(), &track, &clip)
            || m_timeline.trackLocked(track)) { ++kept; continue; }
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        const int toStart = change.value(QStringLiteral("start")).toInt(), toFrames = change.value(QStringLiteral("frames")).toInt();
        if (!e.value(ClipKey::caption).toBool() || toStart < 0 || toFrames < 1) { ++kept; continue; }
        const int start = e.value(ClipKey::start).toInt(), frames = e.value(ClipKey::frames).toInt();
        if (start == toStart && frames == toFrames) continue;
        moves.append({track, clip, qMax(0, e.value(ClipKey::in).toInt()), start, frames, toStart, toFrames, start, frames});
        spans.insert(track, {});
    }
    if (moves.isEmpty()) {
        emit captionsRetimed(0, kept);
        return;
    }
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap e = v.toMap();
        const int track = e.value(ClipKey::track).toInt();
        if (spans.contains(track) && e.value(ClipKey::lane).toInt() >= 1) {
            const int start = e.value(ClipKey::start).toInt();
            spans[track].append({start, start + e.value(ClipKey::frames).toInt()});
        }
    }
    const QVariantList before = snapshotNow();
    bool trimmed = false;
    // A caption goes from where it is to [start, end) when nothing else on
    // its track lies between: it can neither cover nor pass another.
    const auto place = [&](Move &m, int start, int frames) {
        const int from = qMin(m.start, start), to = qMax(m.start + m.frames, start + frames);
        QList<QPair<int, int>> &onTrack = spans[m.track];
        int self = -1;
        for (int i = 0; i < onTrack.size(); ++i) {
            if (onTrack[i].first == m.start && onTrack[i].second == m.start + m.frames && self < 0) { self = i; continue; }
            if (onTrack[i].first < to && from < onTrack[i].second) return false;
        }
        // A title shows the same picture whatever its first frame: one moved
        // earlier than its first frame allows starts from the producer's own.
        const int in = qMax(0, m.in + (start - m.start));
        if (!m_timeline.trimOn(m.track, m.clip, in, in + frames - 1, start, false)) return false;
        trimmed = true;
        if (self >= 0) onTrack[self] = {start, start + frames};
        m.in = in; m.start = start; m.frames = frames;
        return true;
    };
    // First each one to the part of its old place it keeps (never in another's
    // way), then the rest, those going later from the last.
    for (Move &m : moves) {
        const int start = qMax(m.start, m.toStart), end = qMin(m.start + m.frames, m.toStart + m.toFrames);
        if (end > start && (start != m.start || end != m.start + m.frames)) place(m, start, end - start);
    }
    std::sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
        const bool laterA = a.toStart > a.start, laterB = b.toStart > b.start;
        if (laterA != laterB) return !laterA;
        return laterA ? a.start > b.start : a.start < b.start;
    });
    int moved = 0;
    QList<Move> pending;
    for (const Move &m : std::as_const(moves))
        if (m.start != m.toStart || m.frames != m.toFrames) pending.append(m);
        else ++moved;
    for (bool progress = true; progress && !pending.isEmpty();) {
        progress = false;
        for (int i = 0; i < pending.size();) {
            if (place(pending[i], pending[i].toStart, pending[i].toFrames)) { pending.removeAt(i); ++moved; progress = true; }
            else ++i;
        }
    }
    // One that cannot go (another caption, or a clip, is in its way) goes
    // back to where it was.
    for (Move &m : pending) place(m, m.fromStart, m.fromFrames);
    kept += int(pending.size());
    emit captionsRetimed(moved, kept);
    if (moved == 0) {
        if (trimmed) restoreSnapshot(before);
        return;
    }
    recordEdit(before, true, label);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::renameCaptionSpeaker(const QString &from, const QString &to, const QVariantMap &settings) {
    if (staleEdit() || exportActive() || from.isEmpty())
        return;
    const QVariantList before = snapshotNow();
    int changed = 0;
    for (int i = 0; i < m_timeline.connectedCount(); ++i) {
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(i, &track, &clip) || m_timeline.trackLocked(track)) continue;
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        if (!e.value(ClipKey::caption).toBool() || e.value(ClipKey::speaker).toString() != from) continue;
        QVariantMap style = Timeline::titleStyleOf(e);
        style.insert(QStringLiteral("speaker"), to);
        if (m_timeline.updateTitleAt(i, e.value(ClipKey::title).toString(), style)) ++changed;
    }
    const bool listed = settings != m_timeline.captionSettings();
    if (changed == 0 && !listed)
        return;
    m_timeline.setCaptionSettings(settings);
    recordEdit(before, false, to.isEmpty() ? tr("Remove speaker") : tr("Rename speaker"));
    announceTimeline();
}
