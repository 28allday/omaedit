#include "playerworker_p.h"
#include <QScopedValueRollback>

// Pre-edit storyline geometry for marker rippling, off an entries() list.
static int entryStart(const QVariantList &entries, int index) {
    return entries.value(index).toMap().value(ClipKey::start).toInt();
}

// {track, index} pairs sorted so removals never invalidate a later one:
// higher tracks first, then higher indices.
static QList<QPair<int, int>> sortedAddresses(const QVariantList &addrs) {
    QList<QPair<int, int>> out;
    for (const QVariant &v : addrs) {
        const QVariantMap m = v.toMap();
        const QPair<int, int> a(m.value(QStringLiteral("track")).toInt(),
                                m.value(QStringLiteral("index")).toInt());
        if (a.first >= 0 && a.second >= 0 && !out.contains(a))
            out.append(a);
    }
    std::sort(out.begin(), out.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    return out;
}

// A clip's sound alone. It goes where the sound of a whole edit would have
// gone: the first audio track with room for it (an overwrite or an append,
// which cover nothing), or the first audio track, parted at the point and
// rippled as an insert on an audio track is. The pictures are not touched.
void PlayerWorker::placeSound(int mode, const QString &path, int position, int in, int out) {
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    // The first clip of a timeline gives it its clock, sound alone too.
    if (!m_timeline.profile())
        m_timeline.entryForPaste({{"path", path}}, 0.);
    // Where an append puts the picture; with no picture yet (a timeline of
    // sounds), after the last sound, so appended recordings follow each other.
    if (mode == 0)
        position = primaryBefore > 0 ? primaryBefore : m_timeline.frameCount();
    // A whole source's length is not known here: it needs the rest of the track.
    const int span = in >= 0 && out >= in ? out - in + 1 : std::numeric_limits<int>::max() / 4;
    const int track = m_timeline.soundTrackFor(position, span, mode != 1);
    const bool ok = m_timeline.withSourceOrigin(m_pendingTag, [&] {
        return mode == 1 ? m_timeline.insertOn(track, path, position, in, out, true)
                         : m_timeline.overwriteOn(track, path, position, in, out, true);
    });
    if (!ok) {
        m_pendingTag.clear();
        reportRefusedPlacement(track);
        return;
    }
    applyPendingTag(track, position);
    recordEdit(before, true, mode == 0 ? tr("Append Sound") : mode == 1 ? tr("Insert Sound") : tr("Overwrite Sound"));
    if (mode == 1) // V1 rode the ripple: its markers follow it
        rippleMarkers(position, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::appendClip(const QString &path, int in, int out, int takes) {
    // A recording has no picture for V1 to hold: it is its sound, whatever
    // the edit was asked to take.
    if (takes == EditTakes::Sound || m_timeline.soundOnlySource(path)) {
        placeSound(0, path, 0, in, out);
        return;
    }
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const bool wasEmpty = m_timeline.isEmpty();
    const QVariantList before = m_timeline.entries();
    const bool pictureOnly = takes == EditTakes::Picture;
    if (!m_timeline.withSourceOrigin(m_pendingTag,[&]{return m_timeline.append(path,in,out,1.0,false,pictureOnly);})) {
        m_pendingTag.clear();
        return;
    }
    applyPendingTagAt(0, m_timeline.lastClipIndex()); // by clip index: V1 may hold blanks
    recordEdit(before, true, tr("Append Clip"));
    announceTimeline();
    if (wasEmpty) {
        m_position = 0;
        refreshFrame();
    }
}

void PlayerWorker::insertClip(const QString &path, int position, int in, int out, int takes) {
    if (takes == EditTakes::Sound || m_timeline.soundOnlySource(path)) { // (a recording: see appendClip)
        placeSound(1, path, position, in, out);
        return;
    }
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const bool wasEmpty = m_timeline.isEmpty();
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    const bool pictureOnly = takes == EditTakes::Picture;
    if (!m_timeline.withSourceOrigin(m_pendingTag,[&]{return m_timeline.insertAt(path,position,in,out,1.0,pictureOnly);})) {
        m_pendingTag.clear();
        reportRefusedPlacement(0);
        return;
    }
    applyPendingTag(0, wasEmpty ? 0 : position);
    recordEdit(before, true, tr("Insert Clip"));
    rippleMarkers(position, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    if (wasEmpty)
        m_position = 0;
    refreshFrame();
}

void PlayerWorker::overwriteClip(const QString &path, int position, int in, int out, int takes) {
    if (takes == EditTakes::Sound || m_timeline.soundOnlySource(path)) { // (a recording: see appendClip)
        placeSound(2, path, position, in, out);
        return;
    }
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const bool wasEmpty = m_timeline.isEmpty();
    const QVariantList before = m_timeline.entries();
    const bool pictureOnly = takes == EditTakes::Picture;
    if (!m_timeline.withSourceOrigin(m_pendingTag,[&]{return m_timeline.overwriteAt(path,position,in,out,1.0,pictureOnly);})) {
        m_pendingTag.clear();
        reportRefusedPlacement(0);
        return;
    }
    applyPendingTag(0, wasEmpty ? 0 : position);
    {
        // A picture alone covers pictures: the sound under it stays.
        QScopedValueRollback<bool> keep(m_pictureOnlyEdit, pictureOnly);
        recordEdit(before, true, tr("Overwrite Clip"));
    }
    setSpeed(0.0);
    announceTimeline();
    if (wasEmpty)
        m_position = 0;
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::removeClip(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.remove(index))
        return;
    recordEdit(before, true, tr("Delete Clip"));
    if (m_timeline.primaryFrameCount() == primaryBefore) // lifted: its sound stayed
        emit editOperationFinished(tr("The picture's sound stays, so its place is kept as a gap."));
    rippleMarkers(entryStart(before, index),
                  m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::liftClip(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.lift(index))
        return;
    recordEdit(before, true, tr("Lift clip")); // length unchanged — markers and tracks hold still
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::removeRange(int start, int end) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.removeRange(start, end))
        return;
    recordEdit(before, true, tr("Ripple Delete Range"));
    rippleMarkers(start, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::insertGap(int position, int frames) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.insertGapAt(position, frames)) {
        // Refused on the frames a transition plays (and at its last frame's
        // end, where the gap would part it from the clip it leads into).
        for (const QVariant &v : before) {
            const QVariantMap e = v.toMap();
            const int mixed = e.value(ClipKey::mixOut).toInt();
            const int end = e.value(ClipKey::start).toInt() + e.value(ClipKey::frames).toInt();
            if (e.value(ClipKey::track).toInt() == 0 && mixed > 0 && position > end - mixed && position <= end) {
                emit editRefused(tr("A transition plays there; no gap was inserted."));
                break;
            }
        }
        return;
    }
    recordEdit(before, true, tr("Insert gap"));
    rippleMarkers(position, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::closeGap(int track, int position) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    QString error;
    int from = position;
    const int frames = m_timeline.closeGapOn(track, position, &error, &from);
    if (frames < 1) {
        emit editRefused(!error.isEmpty() ? error : tr("The gap could not be closed; nothing was changed."));
        return;
    }
    recordEdit(before, true, tr("Delete gap"));
    rippleMarkers(from, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::deleteGaps() {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    QList<QPair<int, int>> spans;
    const int closed = m_timeline.closeCommonGaps(&spans);
    if (closed < 0) {
        QVariantList snap = before;
        snap.append(markerSentinel());
        restoreSnapshot(snap);
        emit editRefused(tr("The gaps could not be deleted; nothing was changed."));
        return;
    }
    if (closed == 0) {
        emit editRefused(tr("There are no gaps that are empty on every track."));
        return;
    }
    recordEdit(before, true, tr("Delete gaps"));
    for (const auto &span : std::as_const(spans)) // last first, as closed
        rippleMarkers(span.first, -span.second);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
    emit editOperationFinished(tr("Deleted %n gap(s)", nullptr, closed));
}

void PlayerWorker::rollEditOn(int track, int index, int delta) {
    if (staleEdit())
        return;
    if (exportActive() || delta == 0) return;
    const auto before = m_timeline.entries();
    QString error;
    // (A transition on the cut rolls with it, as far as the two clips' spare frames go.)
    if (!m_timeline.rollOn(track, index, delta, &error, &delta)) {
        emit editRefused(error.isEmpty() ? tr("Select a valid edit point to roll; nothing was changed.") : error);
        return;
    }
    // rollOn already moves linked sound edges, retaining independent trims.
    recordEdit(before, false, tr("Roll edit"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
    emit editOperationFinished(delta < 0 ? tr("Rolled edit %n frame(s) left", nullptr, qAbs(delta))
                                        : tr("Rolled edit %n frame(s) right", nullptr, qAbs(delta)));
}

namespace {
// A trim that changed nothing: whether a transition on an edge it asked to
// move is why, to be said. So for a trim that does not ripple, for a slip
// (both edges by the same frames), and for an edge of a transition whose
// clips share frames. A ripple trim under a transition across its cut is
// made: where it changes nothing it has reached the end of the clip's spare
// frames, and stops there as a plain trim does, without a word.
bool heldByTransition(const QVariantList &clips, const QVariantMap &entry, int in, int out, int newStart, bool ripple) {
    const int movedIn = in - entry.value(ClipKey::in).toInt(), movedOut = out - entry.value(ClipKey::out).toInt();
    const bool heldIn = entry.value(ClipKey::mixIn).toInt() > 0 && (movedIn != 0 || newStart != entry.value(ClipKey::start).toInt());
    const bool heldOut = entry.value(ClipKey::mixOut).toInt() > 0 && movedOut != 0;
    if (!heldIn && !heldOut)
        return false;
    if (!ripple || (movedIn != 0 && movedIn == movedOut))
        return true;
    if (heldOut && MixShape::parts(entry).overlap > 0)
        return true;
    if (heldIn)
        for (const QVariant &v : clips) {
            const QVariantMap other = v.toMap();
            if (other.value(ClipKey::track) == entry.value(ClipKey::track)
                && other.value(ClipKey::index).toInt() == entry.value(ClipKey::index).toInt() - 1
                && !other.value(ClipKey::gap).toBool() && MixShape::parts(other).overlap > 0)
                return true;
        }
    return false;
}
}

void PlayerWorker::trimClipOn(int track, int index, int in, int out) {
    if (staleEdit())
        return;
    if (track == 0) {
        trimClip(index, in, out, true);
        return;
    }
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.trimOn(track, index, in, out, 0, true)) {
        if (m_timeline.trackLocked(track))
            emit editRefused(tr("%1 is locked; nothing was trimmed.").arg(m_timeline.trackName(track)));
        return;
    }
    recordEdit(before, true, tr("Trim clip"));
    // V1 may have ridden the lane's ripple — markers follow the storyline.
    rippleMarkers(m_timeline.primaryFrameCount(),
                  m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::trimToFrame(int track, int index, int frame, bool end, bool ripple) {
    if (staleEdit())
        return;
    if (exportActive()) return;
    const auto before = m_timeline.entries();
    const auto entry = m_timeline.entryMapOn(track, index);
    if (entry.isEmpty() || entry.value(ClipKey::gap).toBool() || frame < 0) return;
    const auto link = entry.value(ClipKey::link).toString();
    for (const auto &v : before) {
        const auto other = v.toMap();
        if ((other.value(ClipKey::track).toInt() == track || (!link.isEmpty() && other.value(ClipKey::link) == link))
            && m_timeline.trackLocked(other.value(ClipKey::track).toInt())) {
            emit editRefused(tr("Unlock the clip and its linked tracks before trimming."));
            return;
        }
    }
    const int start = entry.value(ClipKey::start).toInt();
    // A clip in a transition is drawn from its cut to its cut, where its
    // entry starts after the transition into it and runs to the end of the
    // one out of it: the playhead is measured against the clip drawn.
    int lead = 0, tail = 0;
    {
        const QList<MixShape::Block> blocks = MixShape::blocks(before);
        for (int i = 0; i < before.size(); ++i) {
            const QVariantMap e = before.at(i).toMap();
            if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index && !e.value(ClipKey::gap).toBool()) {
                lead = blocks.at(i).lead;
                tail = blocks.at(i).tail;
            }
        }
    }
    const int mixIn = entry.value(ClipKey::mixIn).toInt();
    const int in = end ? entry.value(ClipKey::in).toInt() : entry.value(ClipKey::in).toInt() + frame - (start - lead);
    const int out = end ? entry.value(ClipKey::in).toInt() + mixIn + frame - start + tail : entry.value(ClipKey::out).toInt();
    if (in < 0 || out < in || out >= entry.value(ClipKey::length).toInt()) {
        emit editRefused(tr("The playhead is outside this clip's available material; nothing was trimmed."));
        return;
    }
    if (in == entry.value(ClipKey::in).toInt() && out == entry.value(ClipKey::out).toInt()) return;
    const int primaryBefore = m_timeline.primaryFrameCount();
    const bool ok = track == 0
        ? (ripple ? m_timeline.trim(index, in, out) : m_timeline.trimNormal(index, in, out))
        : m_timeline.trimOn(track, index, in, out, end ? start : frame, ripple);
    if (!ok) {
        emit editRefused(heldByTransition(before, entry, in, out, entry.value(ClipKey::start).toInt(), ripple)
                             ? tr("A transition holds that edge of the clip; nothing was trimmed.")
                             : tr("The trim does not fit here; nothing was trimmed."));
        return;
    }
    recordEdit(before, true, end ? tr("Trim End to Playhead") : tr("Trim Start to Playhead"));
    if (ripple)
        rippleMarkers(start + entry.value(ClipKey::frames).toInt(), m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::rollEdit(int index, int delta) {
    if (staleEdit())
        return;
    rollEditOn(0, index, delta);
}

void PlayerWorker::makeCompound(int start, int end) {
    if (exportActive()) {
        emit compoundEditFinished(false, -1, QStringLiteral("Cannot make a nested clip while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    if (!m_timeline.makeCompound(start, end, &error)) {
        emit compoundEditFinished(false, -1, error);
        return;
    }
    // Grouping wraps the video in place. It is not an overwrite: leave
    // linked audio on its track instead of treating it as orphaned sound.
    recordEdit(before, false, tr("Make nested clip"));
    setSpeed(0.0);
    announceTimeline();
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap entry = v.toMap();
        if (entry.value(ClipKey::track).toInt() == 0
            && entry.value(ClipKey::start).toInt() == start
            && entry.value(ClipKey::compound).toBool()) {
            emit compoundEditFinished(false, entry.value(ClipKey::index).toInt(), QString());
            break;
        }
    }
    refreshFrame();
}

void PlayerWorker::breakApartClip(int index) {
    if (exportActive()) {
        emit compoundEditFinished(true, index, QStringLiteral("Cannot ungroup a nested clip while exporting."));
        return;
    }
    const QVariantList before = m_timeline.entries();
    QString error;
    if (!m_timeline.breakApart(index, &error)) {
        emit compoundEditFinished(true, index, error);
        return;
    }
    recordEdit(before, false, tr("Ungroup clip")); // unwrap picture without rewriting separate audio
    setSpeed(0.0);
    announceTimeline();
    emit compoundEditFinished(true, index, QString());
    refreshFrame();
}

void PlayerWorker::setClipDisabled(int index, bool disabled) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipDisabled(index, disabled))
        return;
    recordEdit(before, true, tr("Set clip disabled"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::connectClip(const QString &path, int position, int in, int out,
                               bool audioOnly) {
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const QVariantList before = m_timeline.entries();
    // A recording connects as the sound it is, on an audio track.
    audioOnly = audioOnly || m_timeline.soundOnlySource(path);
    if (!m_timeline.withSourceOrigin(m_pendingTag,[&]{return m_timeline.connectAt(path,position,in,out,audioOnly);})) {
        m_pendingTag.clear();
        return;
    }
    {
        int t = -1, c = -1;
        if (m_timeline.connectedAddress(m_timeline.connectedCount() - 1, &t, &c))
            applyPendingTagAt(t, c);
        m_pendingTag.clear();
    }
    recordEdit(before, true, tr("Connect clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // the frame under the playhead may now be composited
}

void PlayerWorker::updateTitle(int index, const QString &text,
                               const QVariantMap &style) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    // A caption's words changed by hand are marked so (Update Captions asks
    // before it replaces them).
    QVariantMap drawn = style;
    int track = -1, clip = -1;
    if (m_timeline.connectedAddress(index, &track, &clip)) {
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        if (e.value(ClipKey::caption).toBool() && e.value(ClipKey::title).toString() != text)
            drawn.insert(QStringLiteral("typed"), true);
    }
    if (!m_timeline.updateTitleAt(index, text, drawn))
        return;
    recordEdit(before, true, tr("Update title"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::trimConnectedClip(int index, int in, int out, int newStart,
                                     bool ripple) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    bool ok = false;
    if (ripple) {
        int track = -1, clip = -1;
        ok = m_timeline.connectedAddress(index, &track, &clip)
             && m_timeline.trimOn(track, clip, in, out, newStart, true);
    } else {
        ok = m_timeline.trimConnected(index, in, out, newStart);
    }
    if (!ok) {
        int track = -1, clip = -1;
        const QVariantMap entry = m_timeline.connectedAddress(index, &track, &clip) ? m_timeline.entryMapOn(track, clip) : QVariantMap();
        if (heldByTransition(before, entry, in, out, newStart, ripple))
            emit editRefused(tr("A transition holds that edge of the clip; nothing was trimmed."));
        return;
    }
    recordEdit(before, true, tr("Trim connected clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::connectTitle(const QString &text, int position, int length,
                                const QVariantMap &style) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.connectTitleAt(text, position, length, style))
        return;
    recordEdit(before, true, tr("Connect title"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // the frame under the playhead may now carry the title
}

void PlayerWorker::removeConnectedClip(int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.removeConnected(index))
        return;
    recordEdit(before, true, tr("Remove connected clip"));
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::moveConnectedClip(int index, int newStart) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.moveConnected(index, newStart))
        return;
    recordEdit(before, true, tr("Move connected clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setConnectedRect(int index, double x, double y, double w,
                                    double h) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setConnectedRect(index, x, y, w, h))
        return;
    recordEdit(before, true, tr("Transform connected clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // the composite under the playhead changed
}

void PlayerWorker::splitAt(int position) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.splitAt(position)) {
        reportRefusedBlade(0);
        return;
    }
    recordEdit(before, true, tr("Blade Clip"));
    announceTimeline();
}

void PlayerWorker::splitMany(int track, const QVariantList &positions, const QString &label) {
    if (exportActive() || positions.isEmpty())
        return;
    const QVariantList before = m_timeline.entries();
    int cuts = 0;
    for (const QVariant &v : positions)
        if (track == 0 ? m_timeline.splitAt(v.toInt()) : m_timeline.splitOn(track, v.toInt())) ++cuts;
    if (!cuts) {
        reportRefusedBlade(track);
        return;
    }
    recordEdit(before, true, label);
    announceTimeline();
}

void PlayerWorker::splitAllAt(int position) {
    if (exportActive()) return;
    const auto before = m_timeline.entries();
    const int cuts = m_timeline.splitAllAt(position);
    if (cuts <= 0) {
        emit editRefused(cuts < 0 ? tr("The clips could not be cut; the edit was restored.")
            : tr("No unlocked clips to cut here. Linked clips need both tracks unlocked and material on both sides."));
        return;
    }
    // The engine split each linked member explicitly, preserving independent
    // sound trims. Settling audio here would conform them back to the picture.
    recordEdit(before, false, tr("Blade All at Playhead"));
    announceTimeline();
    emit editOperationFinished(tr("Cut %n clip(s) at the playhead.", nullptr, cuts));
}

void PlayerWorker::splitOn(int track, int position) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.splitOn(track, position)) {
        reportRefusedBlade(track);
        return;
    }
    recordEdit(before, true, tr("Blade Clip"));
    announceTimeline();
}

void PlayerWorker::trimClip(int index, int in, int out, bool ripple) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!(ripple ? m_timeline.trim(index, in, out)
                 : m_timeline.trimNormal(index, in, out))) {
        const QVariantMap entry = m_timeline.entryMapOn(0, index);
        if (m_timeline.trackLocked(0))
            emit editRefused(tr("%1 is locked; nothing was trimmed.").arg(m_timeline.trackName(0)));
        else if (heldByTransition(before, entry, in, out, entry.value(ClipKey::start).toInt(), ripple))
            emit editRefused(tr("A transition holds that edge of the clip; nothing was trimmed."));
        return;
    }
    recordEdit(before, true, tr("Trim clip"));
    // A ripple trim moves the clip's END (starts are derived), so the
    // ripple begins at the old end; a normal trim moves nothing else.
    if (ripple)
        rippleMarkers(entryEnd(before, index),
                      m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::moveClip(int from, int to) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.move(from, to))
        return;
    recordEdit(before, true, tr("Move clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame(); // same duration, but the frame under the playhead changed
}

void PlayerWorker::addTrack(bool audio) {
    if (exportActive() || !m_timeline.addTrack(audio))
        return;
    discardPausedAudio();
    emit tracksChanged(m_timeline.tracks());
}

void PlayerWorker::removeTrack(int track) {
    if (exportActive())
        return;
    // An edit, with the headers in its snapshot: every track above this one
    // takes a lower number, and the earlier undo steps name their clips'
    // tracks by number. Undo must put the track back before it reaches them.
    const QVariantList before = snapshotNow();
    if (!m_timeline.removeTrack(track)) {
        // Only an empty track goes: deleting clips is its own, undoable edit.
        if (track >= 1 && track < m_timeline.trackCount() && m_timeline.trackClipCount(track) > 0)
            emit editRefused(tr("%1 still holds clips; delete them first, then the track.")
                                 .arg(m_timeline.trackName(track)));
        return;
    }
    recordEdit(before, false, tr("Delete Track"));
    setSpeed(0.0);
    announceTimeline(); // lane numbers above it shifted down
    refreshFrame();
}

void PlayerWorker::swapTracks(int a, int b) {
    if (exportActive())
        return;
    QVariantList before = m_timeline.entries();
    QVariantMap sentinel = markerSentinel();
    sentinel.insert(QStringLiteral("omavid:tracks"), m_timeline.trackFlagsVariant());
    sentinel.insert(QStringLiteral("omavid:master"), m_timeline.masterMix());
    sentinel.insert(QStringLiteral("omavid:adr"), m_timeline.adrCues());
    sentinel.insert(QStringLiteral("omavid:captions"), m_timeline.captionSettings());
    before.append(sentinel);
    if (!m_timeline.swapTracks(a, b))
        return;
    recordEdit(before, true, tr("Swap tracks"));
    setSpeed(0.0);
    announceTimeline(); // every entry on the pair changed track number
    refreshFrame();     // the composite order did too
}

void PlayerWorker::placeOnTrack(int track, int mode, const QString &path,
                                int position, int in, int out, bool audioHint, int takes) {
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    const bool audioOnly =
        track < m_timeline.trackCount()
            ? m_timeline.trackKind(track) == Timeline::Track::Audio
            : audioHint;
    // An audio track takes the sound whatever was asked for. On a video
    // track the choice holds: the sound alone goes to an audio track, as
    // does a recording, which has no picture for a video track to hold.
    if (!audioOnly && (takes == EditTakes::Sound || m_timeline.soundOnlySource(path))) {
        placeSound(mode, path, position, in, out);
        return;
    }
    const bool pictureOnly = !audioOnly && takes == EditTakes::Picture;
    const int endBefore = m_timeline.frameCount();
    const auto place=[&] {
        return m_timeline.withSourceOrigin(m_pendingTag,[&] {
            if(mode==0)return m_timeline.overwriteOn(track,path,endBefore,in,out,audioOnly,1.0,pictureOnly);
            if(mode==1)return m_timeline.insertOn(track,path,position,in,out,audioOnly,1.0,pictureOnly);
            return m_timeline.overwriteOn(track,path,position,in,out,audioOnly,1.0,pictureOnly);
        });
    };
    // A Replace says which clip the new one takes the place of: the
    // transitions on that clip's cuts stay, as far as the new clip has spare
    // frames for them.
    const int replaces = mode == 2 ? m_pendingTag.value(QStringLiteral("_replaces"), -1).toInt() : -1;
    QList<Timeline::KeptMix> kept;
    // (The new clip's sound is laid while the cuts are plain; conformed to
    // the pictures as they then stood, the sound takes the crossfades of
    // the transitions that went back.)
    QVariantList plain;
    const auto settled = [&] {
        m_timeline.settleLinkedAudio(before, pictureOnly);
        plain = m_timeline.entries();
    };
    if (!(replaces >= 0 ? m_timeline.withMixesKept(track, replaces, place, &kept, settled) : place())) {
        m_pendingTag.clear();
        reportRefusedPlacement(track);
        return;
    }
    if (!plain.isEmpty())
        m_timeline.settleLinkedAudio(plain, pictureOnly);
    applyPendingTag(track, mode == 0 ? endBefore : position);
    {
        QScopedValueRollback<bool> keep(m_pictureOnlyEdit, pictureOnly && mode != 1);
        recordEdit(before, true, tr("Place on track"));
    }
    reportKeptTransitions(kept);
    if (mode == 1) // V1 may have ridden the ripple — markers follow it
        rippleMarkers(position, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

// What a Replace did to the transitions it could not keep whole: the status
// line says which was shortened, or removed, and why.
void PlayerWorker::reportKeptTransitions(const QList<Timeline::KeptMix> &kept) {
    QStringList said;
    for (const Timeline::KeptMix &mix : kept) {
        if (mix.now >= mix.was)
            continue;
        if (mix.now < 1)
            said << (mix.into ? tr("The transition into the new clip was removed: the clip has no spare frames for it.")
                              : tr("The transition out of the new clip was removed: the clip has no spare frames for it."));
        else
            said << (mix.into ? tr("The transition into the new clip was shortened to %n frame(s): the clip has no more spare frames.", nullptr, mix.now)
                              : tr("The transition out of the new clip was shortened to %n frame(s): the clip has no more spare frames.", nullptr, mix.now));
    }
    if (!said.isEmpty())
        emit editOperationFinished(said.join(QLatin1Char(' ')));
}

void PlayerWorker::fitToFill(int track, const QString &path, int position, int frames,
                             int in, int out, bool audioHint) {
    if (exportActive()) {
        m_pendingTag.clear();
        return;
    }
    const QVariantList before = m_timeline.entries();
    bool audioOnly =
        track < m_timeline.trackCount()
            ? m_timeline.trackKind(track) == Timeline::Track::Audio
            : audioHint;
    // A recording fitted over frames of a video track fills them on the
    // first audio track with room: it has no picture to lay there.
    if (!audioOnly && m_timeline.soundOnlySource(path)) {
        track = m_timeline.soundTrackFor(position, frames, true);
        audioOnly = true;
    }
    const auto fit=[&] {
        return m_timeline.withSourceOrigin(m_pendingTag,[&]{return m_timeline.fitToFill(track,path,position,frames,in,out,audioOnly,m_pendingTag.value("sourceSpan").toDouble());});
    };
    // Fitted over a clip, it takes that clip's place: the transitions on its cuts stay (see placeOnTrack).
    const int replaces = m_pendingTag.value(QStringLiteral("_replaces"), -1).toInt();
    QList<Timeline::KeptMix> kept;
    QVariantList plain;
    const auto settled = [&] {
        m_timeline.settleLinkedAudio(before, false);
        plain = m_timeline.entries();
    };
    if (!(replaces >= 0 ? m_timeline.withMixesKept(track, replaces, fit, &kept, settled) : fit())) {
        m_pendingTag.clear();
        reportRefusedPlacement(track);
        return;
    }
    if (!plain.isEmpty())
        m_timeline.settleLinkedAudio(plain, false);
    if(m_pendingTag.contains("sourceOrigin")) {
        const auto entry=m_timeline.entryMapOn(track,m_timeline.clipIndexAt(track,position));
        const double speed=entry.value(ClipKey::speed,1.).toDouble();
        const double factor=Timeline::interpretationFactor(entry.value(ClipKey::sourceMeta).toMap());
        const double shift=(m_pendingTag.value("sourceOrigin").toDouble()-entry.value(ClipKey::sourceOrigin).toDouble())*m_timeline.fps()/factor;
        auto markers=m_pendingTag.value("markers").toList();
        for(auto &v:markers){auto marker=v.toMap();marker["frame"]=qMax(0,qRound((marker.value("frame").toInt()+shift)/speed));v=marker;}
        m_pendingTag["markers"]=markers;
    }
    applyPendingTag(track, position);
    recordEdit(before, true, tr("Fit to fill"));
    reportKeptTransitions(kept);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::pasteClip(int track, int position, const QVariantMap &entry,
                             bool insert) {
    if (exportActive()) return;
    if (track<0) {reportRefusedPlacement(track);return;}
    QString error; QVariantList added;
    if (!pasteFrameClipboard({entry},position,track,insert,false,&error,&added))
        emit editRefused(error);
}
void PlayerWorker::nudgeClip(int track, int index, int delta) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    int start = 0;
    for (const QVariant &v : before) {
        const QVariantMap map = v.toMap();
        if (map.value(ClipKey::track).toInt() == track
            && map.value(ClipKey::index).toInt() == index)
            start = map.value(ClipKey::start).toInt();
    }
    if (!m_timeline.nudgeAt(track, index, delta)) {
        reportRefusedNudge(track, delta);
        return;
    }
    recordEdit(before, true, tr("Nudge clip"));
    if (track == 0)
        rippleMarkers(start, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}
// The Backend announced the nudge before the worker tried it; a refusal
// replaces that line with the reason. On V1 a nudge ripples (the clip and
// everything after it move), so left needs empty space before the clip;
// on the other tracks the clip moves alone, into free space.
void PlayerWorker::reportRefusedNudge(int track, int delta) {
    if (m_timeline.trackLocked(track))
        emit editRefused(tr("%1 is locked; nothing was nudged.").arg(m_timeline.trackName(track)));
    else if (track == 0 && delta < 0)
        emit editRefused(tr("No gap before the clip on %1 to nudge into; nothing moved.").arg(m_timeline.trackName(0)));
    else if (track == 0)
        emit editRefused(tr("A dissolve holds the clip's edge; nothing was nudged."));
    else
        emit editRefused(tr("No free space on %1 there; nothing was nudged.").arg(m_timeline.trackName(track)));
}

// A blade that cannot cut says why instead of leaving the old status up.
void PlayerWorker::reportRefusedBlade(int track) {
    if (track >= 0 && track < m_timeline.trackCount() && m_timeline.trackLocked(track))
        emit editRefused(tr("%1 is locked; nothing was cut.").arg(m_timeline.trackName(track)));
    else
        emit editRefused(tr("No clip to cut there (a gap, a dissolve or an edit point); nothing was cut."));
}

// Insert, overwrite, paste and fit to fill are announced by the Backend
// before the worker tries them; a refusal says what actually happened.
void PlayerWorker::reportRefusedPlacement(int track) {
    if (track >= 0 && track < m_timeline.trackCount() && m_timeline.trackLocked(track))
        emit editRefused(tr("%1 is locked; nothing was placed.").arg(m_timeline.trackName(track)));
    else
        emit editRefused(tr("The clip does not fit there; nothing was placed."));
}

void PlayerWorker::removeClips(const QVariantList &addrs, bool ripple) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    // The markers ripple before recordEdit: their pre-edit list rides in
    // the snapshot so undo (or a rollback) puts them back.
    QVariantList before = m_timeline.entries();
    before.append(markerSentinel());
    // The whole set in one go: taken a clip at a time, each ripple moved
    // clips the next address was meant for (see Timeline::removeSet).
    bool keptGap = false;
    QList<QPair<int, int>> closed;
    if (!m_timeline.removeSet(sortedAddresses(addrs), ripple, &closed, &keptGap))
        return;
    for (const auto &c : std::as_const(closed))
        rippleMarkers(c.first, c.second);
    recordEdit(before, true, tr("Remove clips"));
    if (keptGap)
        emit editOperationFinished(tr("The picture's sound stays, so its place is kept as a gap."));
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::nudgeClips(const QVariantList &addrs, int delta) {
    if (staleEdit())
        return;
    if (exportActive() || delta == 0)
        return;
    QVariantList before = m_timeline.entries();
    before.append(markerSentinel()); // markers ripple before recordEdit (see removeClips)
    QList<QPair<int, int>> list = sortedAddresses(addrs);
    int firstPrimary = -1;
    for (const auto &a : list)
        if (a.first == 0)
            firstPrimary = firstPrimary < 0 ? a.second : qMin(firstPrimary, a.second);
    bool any = false;
    if (firstPrimary >= 0) {
        const int primaryBefore = m_timeline.primaryFrameCount();
        const int start = [&] {
            for (const QVariant &v : before) {
                const QVariantMap m = v.toMap();
                if (m.value(ClipKey::track).toInt() == 0
                    && m.value(ClipKey::index).toInt() == firstPrimary)
                    return m.value(ClipKey::start).toInt();
            }
            return 0;
        }();
        if (m_timeline.nudgeAt(0, firstPrimary, delta)) {
            any = true;
            rippleMarkers(start, m_timeline.primaryFrameCount() - primaryBefore);
        }
    } else {
        // Lane clips: walk in the direction of travel so a clip never
        // collides with a still-unmoved neighbour of the same group.
        std::sort(list.begin(), list.end(), [&](const QPair<int, int> &a, const QPair<int, int> &b) {
            return a.first != b.first ? a.first > b.first
                   : (delta > 0 ? a.second > b.second : a.second < b.second);
        });
        // The set moves together or not at all: one of its clips with no
        // free frame to move into holds the others where they are.
        for (const auto &a : list) {
            if (m_timeline.nudgeAt(a.first, a.second, delta)) {
                any = true;
                continue;
            }
            if (any) {
                QVariantList clips = before;
                clips.removeLast(); // (the markers' entry)
                m_timeline.rebuild(clips, clips);
                announceTimeline();
            }
            any = false;
            break;
        }
    }
    if (!any) {
        if (!list.isEmpty())
            reportRefusedNudge(firstPrimary >= 0 ? 0 : list.first().first, delta);
        return;
    }
    recordEdit(before, true, tr("Nudge clips"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

// The link a clip is left with when it is parted from its partner: none. But
// the picture of a multicam clip with a sound source of its own would then
// read as one that never had its sound, and be given the source's again at
// its next edit (or when the parted sound is deleted): it keeps a link of
// its own, to no sound, as a picture whose sound was removed does.
static QString partedLink(const Timeline &timeline, const QVariantMap &entry) {
    return !entry.value(ClipKey::audioOnly).toBool() && !Timeline::fixedSoundOf(entry).isEmpty() ? timeline.newLinkId() : QString();
}

void PlayerWorker::moveClips(const QVariantList &moves) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    struct Move { int track; int start; int newTrack; int newStart; };
    QList<Move> list;
    for (const QVariant &v : moves) {
        const QVariantMap m = v.toMap();
        const int track = m.value(QStringLiteral("track")).toInt();
        const int index = m.value(QStringLiteral("index")).toInt();
        if (track < 0)
            continue;
        for (const QVariant &e : before) {
            const QVariantMap map = e.toMap();
            if (map.value(ClipKey::track).toInt() == track
                && map.value(ClipKey::index).toInt() == index) {
                list.append({track, map.value(ClipKey::start).toInt(),
                             m.value(QStringLiteral("newTrack")).toInt(),
                             qMax(0, m.value(QStringLiteral("newStart")).toInt())});
                break;
            }
        }
    }
    if (list.isEmpty())
        return;
    const int countBefore = m_timeline.trackCount();
    // Every clip leaves before any lands (the engine lifts them all first),
    // so two rows can swap and a later start can cross an earlier tail.
    QList<Timeline::ClipMove> engineMoves;
    for (const Move &mv : list) {
        const int index = m_timeline.clipIndexAt(mv.track, mv.start);
        if (index >= 0)
            engineMoves.append({mv.track, index, mv.newTrack, mv.newStart});
    }
    if (engineMoves.isEmpty())
        return;
    // A clip moved without its linked partner (picked alone) stops being
    // linked: the partner stays where it is, now and at later edits.
    QSet<QString> loose;
    for (const QVariant &v : moves) {
        const QVariantMap m = v.toMap();
        if (!m.value(QStringLiteral("alone")).toBool())
            continue;
        const QString link = m_timeline.entryMapOn(m.value(QStringLiteral("track")).toInt(),
                                                   m.value(QStringLiteral("index")).toInt())
                                 .value(QStringLiteral("link")).toString();
        if (!link.isEmpty())
            loose.insert(link);
    }
    if (!loose.isEmpty())
        for (const QVariant &v : before) {
            const QVariantMap e = v.toMap();
            if (loose.contains(e.value(ClipKey::link).toString()))
                m_timeline.setLinkAt(e.value(ClipKey::track).toInt(),
                                     e.value(ClipKey::index).toInt(), partedLink(m_timeline, e));
        }
    if (!m_timeline.moveClips(engineMoves)) {
        m_timeline.rebuild(before, before); // a lost placement must not lose a clip
        announceTimeline();
        return;
    }
    recordEdit(before, true, tr("Move clips"));
    setSpeed(0.0);
    if (m_timeline.trackCount() != countBefore)
        emit tracksChanged(m_timeline.tracks());
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setClipsLinked(const QVariantList &addrs, bool link) {
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    QList<QVariantMap> pictures, sounds;
    for (const auto &a : sortedAddresses(addrs)) {
        const QVariantMap e = m_timeline.entryMapOn(a.first, a.second);
        if (e.isEmpty() || e.value(ClipKey::gap).toBool())
            continue;
        (e.value(ClipKey::audioOnly).toBool() ? sounds : pictures).append(e);
    }
    const auto linkOf = [](const QVariantMap &e) { return e.value(ClipKey::link).toString(); };
    const auto at = [](const QVariantMap &e) {
        return qMakePair(e.value(ClipKey::track).toInt(), e.value(ClipKey::index).toInt());
    };
    bool any = false;
    if (link) {
        // One picture with no sound of its own on a track yet, and sound
        // cut from that picture's own file: anything else would have the
        // picture's sound written over the other material at its next edit.
        const auto refuse = [this](const QString &why) { emit editRefused(why); };
        if (pictures.size() != 1 || sounds.isEmpty())
            return refuse(tr("Link needs one picture and its sound selected."));
        const QVariantMap &picture = pictures.first();
        if (!picture.value(ClipKey::title).toString().isEmpty() || !picture.value(ClipKey::videoOnly).toBool())
            return refuse(tr("This picture still carries its own sound, so there is nothing to link."));
        const QString path = picture.value(ClipKey::path).toString();
        for (const QVariantMap &s : std::as_const(sounds))
            if (s.value(ClipKey::path).toString() != path)
                return refuse(tr("Only sound from the picture's own file can be linked to it."));
        if (const QString own = linkOf(picture); !own.isEmpty())
            for (const QVariant &v : before) {
                const QVariantMap e = v.toMap();
                if (linkOf(e) == own && e.value(ClipKey::audioOnly).toBool()
                    && !sounds.contains(e))
                    return refuse(tr("This picture is already linked to other sound. Unlink it first."));
            }
        const QString id = m_timeline.newLinkId();
        any = m_timeline.setLinkAt(at(picture).first, at(picture).second, id);
        for (const QVariantMap &s : std::as_const(sounds))
            any = m_timeline.setLinkAt(at(s).first, at(s).second, id) && any;
    } else {
        QSet<QString> pairs;
        for (const QVariantMap &e : pictures + sounds)
            if (!linkOf(e).isEmpty())
                pairs.insert(linkOf(e));
        for (const QVariant &v : before) {
            const QVariantMap e = v.toMap();
            if (pairs.contains(linkOf(e)))
                any = m_timeline.setLinkAt(at(e).first, at(e).second, partedLink(m_timeline, e)) || any;
        }
    }
    if (!any) {
        m_timeline.rebuild(before, before);
        announceTimeline();
        return;
    }
    // A new pair's sound is put back under its picture (the linked sound
    // follows a picture it has not followed before).
    recordEdit(before, true, link ? tr("Link clips") : tr("Unlink clips"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setClipsDisabled(const QVariantList &addrs, bool disabled) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    bool any = false;
    for (const auto &a : sortedAddresses(addrs))
        any = m_timeline.setDisabledAt(a.first, a.second, disabled) || any;
    if (!any)
        return;
    recordEdit(before, true, tr("Set clips disabled"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setLabels(const QVariantList &addrs, const QString &label) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    bool any = false;
    for (const auto &a : sortedAddresses(addrs))
        any = m_timeline.setLabelAt(a.first, a.second, label) || any;
    if (!any)
        return;
    recordEdit(before, true, tr("Set labels"));
    announceTimeline();
}

void PlayerWorker::setCategories(const QVariantList &addrs, const QString &category) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    bool any = false;
    for (const auto &a : sortedAddresses(addrs))
        any = m_timeline.setCategoryAt(a.first, a.second, category) || any;
    if (!any)
        return;
    recordEdit(before, true, tr("Set category"));
    announceTimeline();
}

void PlayerWorker::pasteAttributes(const QVariantList &addrs, const QVariantMap &source,
                                   const QStringList &groups) {
    if (staleEdit())
        return;
    if (exportActive() || groups.isEmpty())
        return;
    const QVariantList before = m_timeline.entries();
    bool any = false;
    // Descending per track: a retime changes a clip's length, so the
    // addresses after it on V1 keep their meaning only walking backwards.
    const auto addresses=sortedAddresses(addrs);QSet<QString> pictureLinks;
    for(const auto &a:addresses){const auto e=m_timeline.entryMapOn(a.first,a.second);if(!e.value(ClipKey::audioOnly).toBool() && !e.value(ClipKey::link).toString().isEmpty())pictureLinks.insert(e.value(ClipKey::link).toString());}
    QList<QPair<int,int>> ripples;
    for (const auto &a : addresses) {
        const auto e=m_timeline.entryMapOn(a.first,a.second);auto selected=groups;
        // A selected linked pair is one timing target. Other audio groups
        // still apply to the sound; the picture edit carries its speed map.
        if(e.value(ClipKey::audioOnly).toBool() && pictureLinks.contains(e.value(ClipKey::link).toString()))selected.removeAll("speed");
        if(selected.isEmpty())continue;
        const int length=m_timeline.primaryFrameCount();QString error;
        any=m_timeline.pasteAttributes(a.first,a.second,source,selected,&error)||any;
        if(!error.isEmpty()) {
            if(m_timeline.entries()!=before)restoreSnapshot(before,false);
            emit editRefused(error);return;
        }
        const int delta=m_timeline.primaryFrameCount()-length;
        if(delta)ripples<<qMakePair(e.value(ClipKey::start).toInt()+e.value(ClipKey::frames).toInt(),delta);
    }
    if (!any)
        return;
    recordEdit(before, true, tr("Paste attributes"));
    for(const auto &r:ripples)rippleMarkers(r.first+qMin(0,r.second),r.second);
    setSpeed(0.0);
    announceTimeline();
    m_position = qBound(0, m_position, qMax(0, m_timeline.frameCount() - 1));
    refreshFrame();
}

void PlayerWorker::pasteClips(const QVariantList &entries, int position) {
    if (exportActive() || entries.isEmpty() || position < 0) return;
    QString error; QVariantList added;
    if (!pasteFrameClipboard(entries,position,-1,false,false,&error,&added))
        emit editRefused(error);
}

// A transition whose two clips were copied together is pasted with them: its
// note rides the copy of the clip it left (Timeline::clipsApart), and here it
// goes back across the two where they landed, with the sides and kind it
// had, as far as the pasted clips have spare frames for it. `landed` are the
// clips the paste placed. A copy pasted without its partner beside it is the
// clip alone.
void PlayerWorker::carryCopiedTransitions(const QVariantList &copies, const QVariantList &landed) {
    const auto origin = [](const QVariantList &clips) {
        int first = std::numeric_limits<int>::max();
        for (const QVariant &v : clips)
            if (!v.toMap().isEmpty())
                first = qMin(first, v.toMap().value(ClipKey::start).toInt());
        return first;
    };
    const int copied = origin(copies), pasted = origin(landed);
    for (const QVariant &v : copies) {
        const QVariantMap copy = v.toMap(), carried = copy.value(ClipKey::mixCarry).toMap();
        if (carried.isEmpty())
            continue;
        for (const QVariant &l : landed) {
            const QVariantMap clip = l.toMap();
            if (clip.isEmpty() || clip.value(ClipKey::path) != copy.value(ClipKey::path)
                || clip.value(ClipKey::audioOnly).toBool() != copy.value(ClipKey::audioOnly).toBool()
                || clip.value(ClipKey::start).toInt() - pasted != copy.value(ClipKey::start).toInt() - copied)
                continue;
            // Where it stands now, and the clip after it: the partner's copy, touching.
            const int track = clip.value(ClipKey::track).toInt();
            const int index = m_timeline.clipIndexAt(track, clip.value(ClipKey::start).toInt());
            const QVariantMap a = m_timeline.entryMapOn(track, index), b = m_timeline.entryMapOn(track, index + 1);
            if (a.isEmpty() || b.value(ClipKey::path).toString() != carried.value(QStringLiteral("into")).toString()
                || b.value(ClipKey::start).toInt() != a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt())
                continue;
            m_timeline.putMixAcrossOn(track, index, carried.value(QStringLiteral("after")).toInt(), carried.value(QStringLiteral("before")).toInt(),
                                      carried.value(QStringLiteral("style")).toString(), carried.value(QStringLiteral("align")).toString());
            break;
        }
    }
}

bool PlayerWorker::pasteFrameClipboard(const QVariantList &entries, int position,
                                      int singleTrack, bool insert, bool duplicate,
                                      QString *error, QVariantList *added, const QVariantMap &exactPosition) {
    added->clear();
    const auto before=snapshotNow();
    const auto beforeClips=m_timeline.entries();
    const int primaryBefore=m_timeline.primaryFrameCount();
    const auto flags=m_timeline.trackFlagsVariant();
    const auto sources=m_timeline.mediaSources();
    auto refuse=[&](const QString &reason) {
        m_timeline.setMediaSources(sources,m_timeline.proxyMode());
        putBack(beforeClips,flags);
        *error=reason.isEmpty()?tr("The copied clips could not be placed. The previous edit was preserved."):reason;
        return false;
    };
    if (entries.isEmpty() || position<0 || (insert && entries.size()!=1))
        return refuse({});
    QString prepareError;
    if(m_timeline.entryForPaste(entries.first().toMap(),0.,&prepareError).isEmpty())
        return refuse(prepareError);
    // An empty timeline acquires its media profile during preparation. Convert
    // the captured time only after that clock is known, including first paste.
    if(!exactPosition.isEmpty()) {
        AudioTimeline::Time at;const auto *profile=m_timeline.profile();
        const auto invalid=QCoreApplication::translate("Timeline","The copied clips or destination timing are invalid.");
        if(!profile || !AudioTimeline::readTime(exactPosition,&at) || at.ticks<0)return refuse(invalid);
        const auto n=__int128(at.ticks)*profile->frame_rate_num(),d=__int128(at.rate)*profile->frame_rate_den();
        if(d<=0 || n%d || n/d>100000000)return refuse(invalid);
        position=int(n/d);
    }
    double earliest = std::numeric_limits<double>::infinity();
    for (const QVariant &v : entries) {
        const auto entry=v.toMap();const double rate=TimelineRate::sourceRate(entry);
        const double start=entry.value(ClipKey::start).toDouble()/(rate>0?rate:qMax(1.,m_timeline.fps()));
        earliest=qMin(earliest,start);
    }
    // Place V1 before upper tracks and retain the clipboard's relative spacing.
    // (A copied pair's note of its transition is for carryCopiedTransitions below, not for the clips.)
    QVariantList ordered;
    for (const QVariant &v : entries) {
        QVariantMap copy = v.toMap();
        copy.remove(ClipKey::mixCarry);
        ordered.append(copy);
    }
    std::sort(ordered.begin(), ordered.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap ma = a.toMap(), mb = b.toMap();
        const int ta = ma.value(ClipKey::track).toInt(), tb = mb.value(ClipKey::track).toInt();
        return ta != tb ? ta < tb : ma.value(ClipKey::start).toInt() < mb.value(ClipKey::start).toInt();
    });
    QHash<QString, QString> links;
    QHash<QString, int> linkKinds;
    for(const auto &v:ordered) {
        const auto entry=v.toMap();const auto link=entry.value(ClipKey::link).toString();
        if(!link.isEmpty())linkKinds[link]|=entry.value(ClipKey::audioOnly).toBool()?1:2;
    }
    QVariantList placed;
    for (const QVariant &v : ordered) {
        QVariantMap entry=m_timeline.entryForPaste(v.toMap(),earliest,&prepareError);
        if(entry.isEmpty()) return refuse(prepareError);
        const QString link = entry.value(ClipKey::link).toString();
        if (!link.isEmpty() && linkKinds.value(link)==3) {
            if (!links.contains(link)) links.insert(link, m_timeline.newLinkId());
            entry[ClipKey::link]=links.value(link);
        } else entry.remove(ClipKey::link); // a mate outside this clipboard is not part of the copy
        const int track=singleTrack>=0?singleTrack:
            qBound(0,entry.value(ClipKey::track).toInt(),m_timeline.trackCount());
        const qint64 at=qint64(position)+(singleTrack>=0?0:entry.value(ClipKey::start).toInt());
        if(at>100000000 || !m_timeline.pasteClip(track,int(at),entry,insert)) {
            if(m_timeline.trackLocked(track))
                return refuse(tr("%1 is locked; nothing was placed.").arg(m_timeline.trackName(track)));
            return refuse({});
        }
        placed.append(m_timeline.entryMapOn(track,m_timeline.clipIndexAt(track,int(at))));
    }
    carryCopiedTransitions(entries,placed);
    for(auto &v:placed) { // (as they stand with their transitions, for the addresses found below)
        const auto was=v.toMap();if(was.isEmpty())continue;
        const int track=was.value(ClipKey::track).toInt();
        v=m_timeline.entryMapOn(track,m_timeline.clipIndexAt(track,was.value(ClipKey::start).toInt()));
    }
    // Settlement can detach embedded sound and change indices. Resolve the
    // committed addresses afterwards, and include the newly linked sound.
    recordEdit(before,true,duplicate?tr("Duplicate clips"):insert?tr("Insert clips"):
        singleTrack>=0?tr("Paste clip"):tr("Paste clips"));
    if(insert)
        rippleMarkers(position,m_timeline.primaryFrameCount()-primaryBefore
            -(singleTrack==0?qMax(0,position-primaryBefore):0));
    QSet<QString> selectedLinks;
    for(const auto &v:placed) {
        const auto expected=v.toMap();if(expected.isEmpty())continue;
        const int track=expected.value(ClipKey::track).toInt();
        const auto actual=m_timeline.entryMapOn(track,m_timeline.clipIndexAt(track,expected.value(ClipKey::start).toInt()));
        bool matches=!actual.isEmpty();
        for(const char *key:{"path","title","in","out","frames"})
            matches=matches && actual.value(QLatin1String(key))==expected.value(QLatin1String(key));
        if(!matches)continue; // a later member may have overwritten this copy
        if(!added->contains(actual))added->append(actual);
        const auto link=actual.value(ClipKey::link).toString();
        if(!link.isEmpty())selectedLinks.insert(link);
    }
    for(const auto &v:m_timeline.entries())
        if(selectedLinks.contains(v.toMap().value(ClipKey::link).toString()) && !added->contains(v))added->append(v);
    setSpeed(0.);announceTimeline();refreshFrame();
    // A multicam clip pasted is one placed (a duplicate is the clip there already).
    for (const QVariant &v : entries)
        if (!duplicate && !v.toMap().value(ClipKey::mcam).toString().isEmpty()) { emit multicamPlaced(); break; }
    return true;
}

void PlayerWorker::setLabel(int track, int index, const QString &label) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setLabelAt(track, index, label))
        return;
    recordEdit(before, true, tr("Set label"));
    announceTimeline();
}
void PlayerWorker::slideClip(int track, int index, int delta) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.slideOn(track, index, delta)) {
        // A slide moves the clip's two cuts: it is refused where a transition sits on either.
        for (int at = index - 1; at <= index; ++at)
            if (at >= 0 && m_timeline.entryMapOn(track, at).value(ClipKey::mixOut).toInt() > 0) {
                emit editRefused(tr("A transition holds an edge of the clip; nothing was slid."));
                break;
            }
        return;
    }
    recordEdit(before, true, tr("Slide clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}
void PlayerWorker::joinClips(int track, int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const QVariantList tracksBefore = m_timeline.trackFlagsVariant();
    // An angle cut (two parts of one multicam clip, cut to different angles
    // at the same moment) is joined too: the second part takes the first
    // part's angle, and the two are one segment again.
    const bool angleCut = m_timeline.angleCutOn(track, index);
    if (angleCut && !m_timeline.switchAngleOn(track, index + 1, m_timeline.entryMapOn(track, index).value(ClipKey::mcamAngle).toInt())) {
        if (putBack(before, tracksBefore))
            emit editRefused(m_timeline.trackLocked(track) || m_timeline.angleSwitchAudioLocked(track, index + 1)
                                 ? tr("A locked track holds the second clip or its sound; nothing was joined.")
                                 : tr("The second clip could not be put on the first clip's angle; nothing was joined."));
        return;
    }
    const QVariantList standing = angleCut ? m_timeline.entries() : before;
    // On one angle now. Where the pictures follow on they become one clip
    // below; where they do not (the angle has no footage across the cut, or
    // the sound stayed on its own angles) they are one segment as they stand.
    const auto asTheyStand = [&] {
        if (m_timeline.entries() != standing) m_timeline.restore(standing);
        recordEdit(before, true, tr("Join clips"));
        announceTimeline();
        refreshFrame();
    };
    // A blade cuts a clip's linked mates with it. Whichever half is joined,
    // picture or sound, a mate pair still side by side on another track
    // heals too, or the join would leave the other half split and the
    // halves' links out of step for later nudges and ripples.
    const QString linkA = m_timeline.entryMapOn(track, index).value(ClipKey::link).toString();
    const QString linkB = m_timeline.entryMapOn(track, index + 1).value(ClipKey::link).toString();
    const auto mateOn = [&](const QString &link, int onTrack) {
        for (const QVariant &v : standing) {
            const QVariantMap m = v.toMap();
            if (!link.isEmpty() && m.value(ClipKey::link).toString() == link
                && m.value(ClipKey::track).toInt() == onTrack)
                return m;
        }
        return QVariantMap();
    };
    // A multicam clip with a sound source of its own: the joined picture has
    // the one sound, conformed to the whole of it when the edit settles (a
    // part the source began after, or ended before, had less of it or none,
    // so the two sounds need not follow on). The second part's goes.
    const bool ownSound = !Timeline::fixedSoundOf(m_timeline.entryMapOn(track, index)).isEmpty()
                          && !m_timeline.entryMapOn(track, index).value(ClipKey::audioOnly).toBool();
    QList<QPair<int, int>> mates; // (track, index of the first half)
    for (const QVariant &v : standing) {
        const QVariantMap m = v.toMap();
        const int t = m.value(ClipKey::track).toInt();
        if (ownSound || linkA.isEmpty() || linkA == linkB || t == track
            || m.value(ClipKey::link).toString() != linkA)
            continue;
        const QVariantMap b = mateOn(linkB, t);
        const int i = m.value(ClipKey::index).toInt();
        if (!b.isEmpty() && b.value(ClipKey::index).toInt() == i + 1)
            mates << qMakePair(t, i);
    }
    if (!m_timeline.joinOn(track, index)) {
        if (angleCut) { asTheyStand(); return; }
        emit editRefused(tr("These clips are not one continuous piece of the same source; nothing was joined."));
        return;
    }
    if (ownSound && !linkB.isEmpty() && linkB != linkA) {
        const QVariantList joined = m_timeline.entries();
        bool firstHasSound = false;
        for (const QVariant &v : joined)
            firstHasSound = firstHasSound || (!linkA.isEmpty() && v.toMap().value(ClipKey::audioOnly).toBool()
                                              && v.toMap().value(ClipKey::link).toString() == linkA);
        for (const QVariant &v : joined) {
            const QVariantMap m = v.toMap();
            if (!m.value(ClipKey::audioOnly).toBool() || m.value(ClipKey::link).toString() != linkB)
                continue;
            if (!firstHasSound) {
                // (The first part had no sound: the second's is the joined picture's.)
                if (linkA.isEmpty()) m_timeline.setLinkAt(track, index, linkB);
                else m_timeline.setLinkAt(m.value(ClipKey::track).toInt(), m.value(ClipKey::index).toInt(), linkA);
            } else {
                m_timeline.removeOn(m.value(ClipKey::track).toInt(), m.value(ClipKey::index).toInt(), false);
            }
            break;
        }
    }
    for (const auto &mate : std::as_const(mates)) {
        if (!m_timeline.joinOn(mate.first, mate.second)) {
            if (angleCut) { asTheyStand(); return; }
            if (!putBack(before, tracksBefore))
                return;
            emit editRefused(tr("The linked picture or sound of these clips is no longer one piece; nothing was joined."));
            return;
        }
    }
    recordEdit(before, true, tr("Join clips"));
    announceTimeline();
    refreshFrame();
}
void PlayerWorker::setTrackName(int track, const QString &name) {
    if (!m_timeline.setTrackName(track, name))
        return;
    emit tracksChanged(m_timeline.tracks());
}
void PlayerWorker::moveClipTo(int track, int index, int newTrack, int newStart) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.moveTo(track, index, newTrack, newStart))
        return;
    recordEdit(before, true, tr("Move clip"));
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setTrackFlag(int track, const QString &flag, bool value) {
    bool ok = false;
    if (flag == QStringLiteral("locked"))
        ok = m_timeline.setTrackLocked(track, value);
    else if (flag == QStringLiteral("disabled"))
        ok = m_timeline.setTrackDisabled(track, value);
    else if (flag == QStringLiteral("muted"))
        ok = m_timeline.setTrackMuted(track, value);
    else if (flag == QStringLiteral("solo"))
        ok = m_timeline.setTrackSolo(track, value);
    else if (flag == QStringLiteral("autoSelect"))
        ok = m_timeline.setTrackAutoSelect(track, value);
    if (!ok)
        return;
    discardPausedAudio();
    soundMayHaveChanged(); // a mute or a solo is heard at once
    emit tracksChanged(m_timeline.tracks());
    refreshFrame(); // disable/mute show at once
}

void PlayerWorker::setClipMarkers(int track, int index, const QVariantList &markers) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setClipMarkersAt(track, index, markers))
        return;
    recordEdit(before, true, tr("Set clip markers"));
    announceTimeline();
}

void PlayerWorker::editClipMarker(int track, int index, const QVariantMap &marker, bool remove) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    QVariantList markers = m_timeline.entryMapOn(track, index).value(ClipKey::markers).toList();
    const int frame = marker.value(QStringLiteral("frame")).toInt();
    bool found = false;
    for (int i = markers.size() - 1; i >= 0; --i) {
        if (markers.at(i).toMap().value(QStringLiteral("frame")).toInt() != frame)
            continue;
        found = true;
        if (remove) markers.removeAt(i);
        else markers[i] = marker;
    }
    if (!found && remove)
        return;
    if (!found)
        markers.append(marker);
    std::stable_sort(markers.begin(), markers.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("frame")).toInt() < b.toMap().value(QStringLiteral("frame")).toInt();
    });
    setClipMarkers(track, index, markers);
}

void PlayerWorker::applyAudioEdit(int request, const QVariantMap &target, const QVariantMap &values, bool split) {
    const auto fail=[&](const QString &error) { emit audioEditFinished(request,false,error); };
    const auto id=target.value("clipId").toString();
    if(exportActive() || !AudioTimeline::sameEditContext(target.value("context").toMap(),audioEditContext(),id.isEmpty())) {
        fail(tr("The project or timeline changed. Select the sound cut again.")); return;
    }
    const auto before=m_timeline.entries();
    const auto entry=id.isEmpty() ? m_timeline.entryMapOn(target.value("track",-1).toInt(),target.value("index",-1).toInt())
                                 : AudioTimeline::entryWithId(before,id);
    const int track=entry.value(ClipKey::track,-1).toInt(),index=entry.value(ClipKey::index,-1).toInt();
    if(entry.isEmpty() || !entry.value(ClipKey::audioOnly).toBool() || m_timeline.trackLocked(track)
        || AudioTimeline::editState(entry)!=target.value("state").toString()) {
        fail(tr("The sound cut changed, is locked or is no longer available. Select it again.")); return;
    }
    QString error;
    const bool ok=split ? m_timeline.splitAudioAt(track,index,values,&error) : m_timeline.setAudioTimingAt(track,index,values,&error);
    if(!ok) { fail(error.isEmpty()?tr("The precise sound edit could not be applied; nothing was changed."):error); return; }
    if(m_timeline.entries()==before) { emit audioEditFinished(request,false,{}); return; }
    // This operation changes sound alone; it must not conform any linked picture.
    recordEdit(before,false,split?tr("Split sound"):tr("Set sound timing"));
    setSpeed(0.); announceTimeline(); refreshFrame(); emit audioEditFinished(request,true,{});
}
void PlayerWorker::applyAudioGesture(int request, const QVariantMap &target, const QString &action,
                                     const QVariantMap &changes, int sampleRate) {
    const auto fail=[&](const QString &error) { emit audioGestureFinished(request,false,error,{}); };
    const auto id=target.value("clipId").toString();
    const bool arrangement=changes.contains("destinationTrack") || changes.value("ripple").toBool();
    if (exportActive() || !AudioTimeline::sameEditContext(target.value("context").toMap(),audioEditContext(),id.isEmpty() || arrangement)) {
        fail(tr("The project or track arrangement changed. Start the sound gesture again.")); return;
    }
    if (sampleRate!=m_timeline.projectSettings().value("sampleRate",48000).toInt()) {
        fail(tr("The project sample rate changed. Start the sound gesture again.")); return;
    }
    const auto before=m_timeline.entries();
    const auto entry=id.isEmpty()?m_timeline.entryMapOn(target.value("track",-1).toInt(),target.value("index",-1).toInt())
        :AudioTimeline::entryWithId(before,id);
    const int track=entry.value(ClipKey::track,-1).toInt(),index=entry.value(ClipKey::index,-1).toInt();
    if (entry.isEmpty() || !entry.value(ClipKey::audioOnly).toBool() || m_timeline.trackLocked(track)
        || AudioTimeline::editState(entry)!=target.value("state").toString()) {
        fail(tr("The sound cut changed, is locked or is no longer available. Select it again.")); return;
    }
    QString error; QVariantMap selected;
    if (!m_timeline.gestureAudioAt(track,index,action,changes,sampleRate,&selected,&error)) {
        fail(error.isEmpty()?tr("The precise sound edit could not be applied; nothing was changed."):error); return;
    }
    if (m_timeline.entries()==before) {emit audioGestureFinished(request,false,{},{});return;}
    const auto label=action=="move"?tr("Move sound"):action=="split"?tr("Split sound"):
        action=="slip"?tr("Slip sound"):changes.value("ripple").toBool()?tr("Ripple trim sound"):tr("Trim sound");
    recordEdit(before,false,label);
    setSpeed(0.); announceTimeline(); refreshFrame();
    emit audioGestureFinished(request,true,{},selected);
}

QVariantMap PlayerWorker::timelinePlacementTarget() const {
    return {{"context",audioEditContext()},{"trackCount",m_timeline.trackCount()},
            {"sampleRate",m_timeline.projectSettings().value("sampleRate",48000)}};
}

void PlayerWorker::replayTimelinePlacements(const QVariantMap &expected,
        const QList<std::function<void(const QVariantMap &)>> &placements) {
    const auto initial=timelinePlacementTarget();
    const auto context=expected.value("context").toMap();
    bool current=!exportActive()
        && AudioTimeline::sameEditContext(context,initial.value("context").toMap(),true)
        && expected.value("trackCount",-1).toInt()==initial.value("trackCount").toInt()
        && expected.value("sampleRate",-1).toInt()==initial.value("sampleRate").toInt();
    bool reported=false;
    for(const auto &placement:placements) {
        const auto target=timelinePlacementTarget();
        const auto live=target.value("context").toMap();
        current=current && !exportActive()
            && target.value("sampleRate").toInt()==initial.value("sampleRate").toInt();
        // The batch's own edits can establish a profile, add tracks and advance
        // history, but must never move a deferred clip to another timeline.
        for(const char *key:{"session","generation","sequenceId","activation"})
            current=current && context.value(QLatin1String(key))==live.value(QLatin1String(key));
        if(!current && !reported) {
            emit editRefused(tr("The project or track arrangement changed. Paste the clips again."));
            reported=true;
        }
        placement(current?target:QVariantMap{});
    }
}

void PlayerWorker::placeSourceSnapshot(const QVariantMap &target,const QVariantMap &snapshot,
                                     const QVariantMap &action) {
    // A deferred placement owns its tag, including an empty tag. Neither a
    // refusal nor an operation that consumes only part of it may leak metadata.
    m_pendingTag.clear();const auto clearTag=qScopeGuard([this]{m_pendingTag.clear();});
    const auto live=timelinePlacementTarget();
    const auto changed=tr("The project, timeline or source settings changed. Add the clip again.");
    if(exportActive() || !AudioTimeline::sameEditContext(target.value("context").toMap(),live.value("context").toMap(),true)
        || target.value("trackCount",-1).toInt()!=live.value("trackCount").toInt()
        || target.value("sampleRate",-1).toInt()!=live.value("sampleRate").toInt()) {
        emit editRefused(changed);return;
    }
    const auto source=snapshot.value("source").toMap();const auto path=source.value("path").toString();
    const auto captured=snapshot.value("playbackSources").toMap(),sources=m_timeline.mediaSources();
    if(path.isEmpty() || !captured.contains(path)) {reportRefusedPlacement(action.value("track",0).toInt());return;}
    for(auto it=captured.cbegin();it!=captured.cend();++it)
        if(it.value().toMap()!=sources.value(it.key()).toMap()) {emit editRefused(changed);return;}
    const auto op=action.value("op").toString();
    const int position=action.value("position",0).toInt(),track=action.value("track",0).toInt();
    const int mode=action.value("mode",2).toInt(),frames=action.value("frames",0).toInt();
    if(!QStringList{"append","insert","overwrite","connect","place","fit"}.contains(op)
        || position<0 || position>100000000 || ((op=="place" || op=="fit") && (track<0 || track>qMax(1,m_timeline.trackCount())))
        || (op=="place" && (mode<0 || mode>2)) || (op=="fit" && (frames<1 || frames>100000000))) {
        reportRefusedPlacement(track);return;
    }
    double rate=m_timeline.profile()?m_timeline.profile()->fps():0.;
    if(!m_timeline.profile()) {
        Timeline probe;probe.setProjectSettings(m_timeline.projectSettings());
        probe.setMediaSources(sources,m_timeline.proxyMode());
        probe.entryForPaste({{"path",path}},0.);
        if(!probe.profile()) {reportRefusedPlacement(track);return;}
        rate=probe.profile()->fps();
    }
    const auto resolved=SourceTiming::placement(snapshot,rate);
    if(resolved.isEmpty()) {reportRefusedPlacement(track);return;}
    const int duration=resolved.value("duration").toInt();
    int in=resolved.value("rangeIn",-1).toInt(),out=resolved.value("rangeOut",-1).toInt();
    if(in>=0 || out>=0) {in=qMax(0,in);if(out<0)out=duration-1;}
    if(action.contains("sourceFrames")) {
        const auto count=action.value("sourceFrames").toLongLong();in=qMax(0,in);
        if(count<1 || count>100000000 || qint64(in)+count>duration) {reportRefusedPlacement(track);return;}
        out=in+int(count)-1;
    }
    if(op=="fit") {in=qMax(0,in);if(out<0)out=duration-1;}
    if(duration<1 || (in>=0 && (out<in || out>=duration))) {reportRefusedPlacement(track);return;}
    const auto before=m_timeline.entries();const auto flags=m_timeline.trackFlagsVariant();
    const int revision=m_editRevision;
    // Explicit upper-track placement needs a playlist even when it is the
    // first content. The scratch probe validated its clock without altering
    // the live empty timeline; restore below also removes this setup on failure.
    if(!m_timeline.profile() && (op=="place" || op=="fit"))
        m_timeline.entryForPaste({{"path",path}},0.);
    m_pendingTag=resolved.value("placementTag").toMap();
    const bool audio=action.value("audio").toBool();
    const int takes=qBound(0,action.value("takes").toInt(),2);
    if(op=="append")appendClip(path,in,out,takes);
    else if(op=="insert")insertClip(path,position,in,out,takes);
    else if(op=="overwrite")overwriteClip(path,position,in,out,takes);
    else if(op=="connect")connectClip(path,position,in,out,audio);
    else if(op=="place")placeOnTrack(track,mode,path,position,in,out,audio,takes);
    else fitToFill(track,path,position,frames,in,out,audio);
    if(m_editRevision==revision) {
        m_timeline.setMediaSources(sources,m_timeline.proxyMode());
        if(!putBack(before,flags))return;
        if(op=="append" || op=="connect")reportRefusedPlacement(track);
    }
}

void PlayerWorker::pasteClipboardSnapshot(int request,const QVariantMap &target,
                                        const QVariantMap &input,const QVariantMap &placement) {
    const auto fail=[&](const QString &error){emit audioClipboardFinished(request,false,error,{});};
    const auto live=timelinePlacementTarget();
    if(exportActive() || !AudioTimeline::sameEditContext(target.value("context").toMap(),live.value("context").toMap(),true)
        || target.value("trackCount",-1).toInt()!=live.value("trackCount").toInt()) {
        fail(tr("The project or track arrangement changed. Paste the clips again."));return;
    }
    if(target.value("sampleRate",-1).toInt()!=live.value("sampleRate").toInt()) {
        fail(tr("The project sample rate changed. Paste the clips again."));return;
    }
    auto clipboard=input;auto copies=clipboard.value("entries").toList();
    const auto position=placement.value("position").toMap();AudioTimeline::Time at;
    const int sourceNum=clipboard.value("fpsNum").toInt(),sourceDen=clipboard.value("fpsDen").toInt();
    const int destinationTrack=placement.value("destinationTrack",-1).toInt();
    const auto invalid=QCoreApplication::translate("Timeline","The copied clips or destination timing are invalid.");
    if(copies.isEmpty() || sourceNum<1 || sourceDen<1 || destinationTrack<-1
        || !AudioTimeline::readTime(position,&at) || at.ticks<0) {fail(invalid);return;}
    for(auto &value:copies) {
        if(value.metaType().id()!=QMetaType::QVariantMap) {fail(invalid);return;}
        auto entry=value.toMap();
        // The captured clipboard clock owns frame coordinates even when an
        // entry lacks, or carries an outdated, display-rate annotation.
        if(!entry.contains(ClipKey::audioTiming))entry[ClipKey::timelineFps]=double(sourceNum)/sourceDen;
        if(destinationTrack>=0 && copies.size()==1) {
            entry[ClipKey::track]=destinationTrack;entry[ClipKey::lane]=destinationTrack;
        }
        value=entry;
    }
    if(!(copies.size()==1 && destinationTrack>=0)) {
        // Resolve automatic destinations once against the initial layout.
        // Existing lane addresses keep their meaning; missing source lanes
        // each receive one consecutive new lane, even when several copies
        // share that source lane or preceding queued edits created headers.
        const int firstMissing=qMax(1,m_timeline.trackCount());
        QMap<int,int> lanes;
        for(const auto &value:copies) {
            const auto entry=value.toMap();const int track=entry.value(ClipKey::track,-1).toInt();
            if(track<0 || entry.value(ClipKey::lane,track).toInt()!=track) {fail(invalid);return;}
            if(track>=firstMissing)lanes.insert(track,0);
        }
        int next=firstMissing;for(auto it=lanes.begin();it!=lanes.end();++it)it.value()=next++;
        for(auto &value:copies) {
            auto entry=value.toMap();const int track=entry.value(ClipKey::track).toInt();
            if(lanes.contains(track))entry[ClipKey::track]=entry[ClipKey::lane]=lanes.value(track);
            value=entry;
        }
    }
    const auto *profile=m_timeline.profile();int num=0,den=0;
    if(profile) {num=profile->frame_rate_num();den=profile->frame_rate_den();}
    else {
        // Match-first-clip and custom formats acquire their real clock only
        // when media is prepared. Probe a scratch graph so refusal leaves the
        // empty destination's profile, tracks and settings untouched.
        Timeline probe;probe.setProjectSettings(m_timeline.projectSettings());
        probe.setMediaSources(m_timeline.mediaSources(),m_timeline.proxyMode());
        probe.entryForPaste(copies.first().toMap(),0.);
        if(!probe.profile()) {fail(invalid);return;}
        num=probe.profile()->frame_rate_num();den=probe.profile()->frame_rate_den();
    }
    if(num<1 || den<1) {fail(invalid);return;}
    const bool insert=placement.value("insert").toBool();
    const auto entries=m_timeline.entries();
    bool precise=AudioTimeline::containsPreciseCuts(copies)
        || (insert && AudioTimeline::containsPreciseCuts(entries))
        || (__int128(at.ticks)*num)%(__int128(at.rate)*den)!=0;
    for(const auto &value:copies)if(value.toMap().value(ClipKey::audioOnly).toBool()) {
        const int track=value.toMap().value(ClipKey::track).toInt();
        for(const auto &entry:entries)
            if(entry.toMap().value(ClipKey::track).toInt()==track && entry.toMap().contains(ClipKey::audioTiming))precise=true;
    }
    const bool legacyInsert=copies.size()==1 && copies.first().toMap().value(ClipKey::title).toString().isEmpty();
    auto prepared=placement;prepared["mixed"]=true;
    prepared["framePlacement"]=!precise && (!insert || legacyInsert);
    prepared["singleTrack"]=copies.size()==1 && (destinationTrack>=0 || insert)
        ?copies.first().toMap().value(ClipKey::track).toInt():-1;
    clipboard["entries"]=copies;
    pasteAudioClipboard(request,target,clipboard,prepared);
}

void PlayerWorker::pasteAudioClipboard(int request,const QVariantMap &target,
                                     const QVariantMap &clipboard,const QVariantMap &placement) {
    const auto fail=[&](const QString &error){emit audioClipboardFinished(request,false,error,{});};
    if(exportActive() || !AudioTimeline::sameEditContext(target.value("context").toMap(),audioEditContext(),true)
        || target.value("trackCount",-1).toInt()!=m_timeline.trackCount()) {
        fail(tr("The project or track arrangement changed. Paste the clips again."));return;
    }
    if(target.value("sampleRate",-1).toInt()!=m_timeline.projectSettings().value("sampleRate",48000).toInt()) {
        fail(tr("The project sample rate changed. Paste the clips again."));return;
    }
    if(placement.value("framePlacement").toBool()) {
        const auto position=placement.value("position").toMap();
        if(position.isEmpty()) {
            fail(QCoreApplication::translate("Timeline","The copied clips or destination timing are invalid."));return;
        }
        QString error;QVariantList added;
        const bool applied=pasteFrameClipboard(clipboard.value("entries").toList(),0,
            placement.value("singleTrack",-1).toInt(),placement.value("insert").toBool(),
            placement.value("duplicate").toBool(),&error,&added,position);
        emit audioClipboardFinished(request,applied,error,added);
        return;
    }
    // Capture flags and routing before preparation, including a possibly new
    // destination track. Undo/redo restores the committed identities, not copies.
    const auto before=snapshotNow();QString error;QVariantList added;
    const int primaryBefore=m_timeline.primaryFrameCount();
    const bool insert=placement.value("insert").toBool();
    const bool mixed=placement.value("mixed").toBool();
    QVariantList copies; // without a copied pair's note of its transition (carryCopiedTransitions has that)
    for(const auto &v:clipboard.value("entries").toList()) {auto copy=v.toMap();copy.remove(ClipKey::mixCarry);copies.append(copy);}
    const bool applied=mixed ? m_timeline.pasteClipboardClips(copies,
        clipboard.value("fpsNum").toInt(),clipboard.value("fpsDen").toInt(),placement.value("position").toMap(),&added,&error,insert)
        : m_timeline.pasteAudioClips(copies,clipboard.value("fpsNum").toInt(),
            clipboard.value("fpsDen").toInt(),placement.value("position").toMap(),insert,&added,&error);
    if(!applied) {
        fail(error.isEmpty()?tr("The precise sound edit could not be applied; nothing was changed."):error);return;
    }
    if(mixed) {
        carryCopiedTransitions(clipboard.value("entries").toList(),added);
        for(auto &v:added) { // as they stand with their transitions
            const auto was=v.toMap();if(was.isEmpty() || was.contains(ClipKey::audioTiming))continue;
            const int track=was.value(ClipKey::track).toInt();
            const auto is=m_timeline.entryMapOn(track,m_timeline.clipIndexAt(track,was.value(ClipKey::start).toInt()));
            if(!is.isEmpty())v=is;
        }
    }
    recordEdit(before,false,placement.value("duplicate").toBool()?tr("Duplicate clips"):
        mixed?(insert?tr("Insert clips"):tr("Paste clips")):insert?tr("Insert sound"):tr("Paste sound"));
    if(mixed && insert) {
        AudioTimeline::Time at;AudioTimeline::readTime(placement.value("position"),&at);
        const auto *profile=m_timeline.profile();
        const auto n=__int128(at.ticks)*profile->frame_rate_num(),d=__int128(at.rate)*profile->frame_rate_den();
        const int pivot=int((n+d-1)/d);
        rippleMarkers(pivot,qMax(0,m_timeline.primaryFrameCount()-primaryBefore-qMax(0,pivot-primaryBefore)));
    }
    setSpeed(0.);announceTimeline();refreshFrame();
    emit audioClipboardFinished(request,true,{},added);
}

void PlayerWorker::setAudioTiming(int track, int index, const QVariantMap &timing) {
    if (staleEdit())
        return;
    if (exportActive()) return;
    const auto before=m_timeline.entries(); QString error;
    if (!m_timeline.setAudioTimingAt(track,index,timing,&error)) {
        emit editRefused(error.isEmpty()?tr("The precise sound timing could not be applied; nothing was changed."):error); return;
    }
    recordEdit(before,true,tr("Set sound timing")); setSpeed(0.); announceTimeline(); refreshFrame();
}
void PlayerWorker::splitAudio(int track, int index, const QVariantMap &position) {
    if (staleEdit())
        return;
    if (exportActive()) return;
    const auto before=m_timeline.entries(); QString error;
    if (!m_timeline.splitAudioAt(track,index,position,&error)) {
        emit editRefused(error.isEmpty()?tr("Choose a position inside the sound cut; nothing was changed."):error); return;
    }
    recordEdit(before,true,tr("Split sound")); setSpeed(0.); announceTimeline(); refreshFrame();
}

void PlayerWorker::setBypass(int track, int index, const QString &key, bool on) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setBypassAt(track, index, key, on))
        return;
    recordEdit(before, true, tr("Set bypass"));
    announceTimeline();
    refreshFrame();
}
