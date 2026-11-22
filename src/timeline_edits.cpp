#include "timeline_p.h"

// A clip leaving the main track hands back the spare frames its transitions
// had borrowed from its neighbours (withMixesReleased); so do a lift, a range
// delete, a reorder, an insert and an overwrite below.
bool Timeline::remove(int index) {
    return withMixesReleased(0, {index - 1, index}, 0, [&] { return removePlain(index); });
}

bool Timeline::removePlain(int index, QPair<int, int> *closed) {
    const int entry = entryIndexFor(index);
    if (entry < 0)
        return false;
    const QVariantList before = entries();
    // A picture deleted without its linked sound (Alt-click, linked
    // selection off) cannot close up: the sound stays where it is, and
    // pulling the rest of V1 left would slide every later picture off its
    // own sound. Its place stays as a gap instead — a lift.
    {
        QString link;
        for (const QVariant &v : before) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::track).toInt() == 0
                && m.value(ClipKey::index).toInt() == index)
                link = m.value(ClipKey::link).toString();
        }
        for (const QVariant &v : before) {
            const QVariantMap m = v.toMap();
            if (!link.isEmpty() && m.value(ClipKey::audioOnly).toBool()
                && m.value(ClipKey::link).toString() == link)
                return lift(index);
        }
    }
    const int primaryBefore = primaryFrameCount();
    // Sync-lock pivot: the leaving clip's old end. Track clips over the
    // clip itself hold position; everything downstream slides left.
    const int pivot = storylineEndIn(before, index);
    const bool mixBefore = entry > 0 && primary()->is_mix(entry - 1);
    const bool mixAfter = entry + 1 < primary()->count()
                          && primary()->is_mix(entry + 1);
    if (!mixBefore && !mixAfter) {
        if (primary()->remove(entry) != 0)
            return false;
        tidyPrimary(); // the blank before it (if any) stays, merged
        if (closed)
            *closed = {pivot, primaryFrameCount() - primaryBefore};
        else
            rippleOthersFrom(0, pivot, primaryFrameCount() - primaryBefore);
        return true;
    }

    // A clip leaving takes its dissolves with it — rebuild from a snapshot
    // with the clip dropped and the mix leading into it cleared.
    const QVariantList all = entries();
    QVariantList result;
    int clip = 0;
    int prevLane0 = -1;
    int carryGap = -1; // the leaving clip's own blank stays, on the next clip
    for (const QVariant &v : all) {
        QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1) {
            result.append(map);
            continue;
        }
        if (carryGap >= 0) {
            map.insert(ClipKey::gapBefore,
                       map.value(ClipKey::gapBefore).toInt() + carryGap);
            carryGap = -1;
        }
        if (clip++ == index) {
            carryGap = map.value(ClipKey::gapBefore).toInt();
            if (prevLane0 >= 0) {
                QVariantMap prev = result.at(prevLane0).toMap();
                prev.insert(ClipKey::mixOut, 0);
                result[prevLane0] = prev;
            }
            continue;
        }
        prevLane0 = result.size();
        result.append(map);
    }
    if (!rebuild(result, before))
        return false;
    if (closed)
        *closed = {pivot, primaryFrameCount() - primaryBefore};
    else
        rippleOthersFrom(0, pivot, primaryFrameCount() - primaryBefore);
    return true;
}

// Lift: the clip leaves and empty space of exactly its span stays. One
// snapshot rebuild: the clip is dropped and its span (plus any blank that
// already preceded it) becomes the NEXT clip's gapBefore; the storyline's
// length is unchanged, so the tracks above never move. Lifting the last
// clip leaves nothing behind (a gap can't end the timeline).
bool Timeline::lift(int index) {
    return withMixesReleased(0, {index - 1, index}, 0, [&] { return liftPlain(index); });
}

bool Timeline::liftPlain(int index) {
    if (trackLocked(0))
        return false;
    if (entryIndexFor(index) < 0)
        return false;
    const QVariantList before = entries();
    QVariantList result;
    int clip = 0;
    int prevLane0 = -1;
    int carry = -1; // frames the next storyline clip inherits as gapBefore
    for (const QVariant &v : before) {
        QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1) {
            result.append(map);
            continue;
        }
        if (carry >= 0) {
            map.insert(ClipKey::gapBefore,
                       map.value(ClipKey::gapBefore).toInt() + carry);
            carry = -1;
        }
        if (clip++ == index) {
            // `frames` is the entry's own count: a dissolve INTO the clip
            // has already taken its overlap off it, a dissolve OUT of it is
            // still counted. Clearing the mixes gives each overlap back to
            // the neighbour, and the next clip takes its head back at the
            // front: the gap is the frames this clip showed alone, so the
            // storyline keeps its length and nothing after it moves.
            carry = map.value(ClipKey::frames).toInt()
                    - map.value(ClipKey::mixOut).toInt()
                    + map.value(ClipKey::gapBefore).toInt();
            if (prevLane0 >= 0) {
                QVariantMap prev = result.at(prevLane0).toMap();
                prev.insert(ClipKey::mixOut, 0);
                result[prevLane0] = prev;
            }
            continue;
        }
        prevLane0 = result.size();
        result.append(map);
    }
    return rebuild(result, before);
}

// Ripple-delete a timeline span (range delete): storyline clips fully
// inside go, straddling clips are trimmed (a clip containing the whole
// range splits in two), everything after slides left. Lane clips starting
// inside the range go with it; later ones shift. Rebuilt from a snapshot —
// one undo step. Refused when a dissolve's mix region overlaps the range.
bool Timeline::removeRange(int start, int end) {
    // A transition made of handles goes first when the range takes the
    // whole of the clip it leads into, or cuts through the frames it plays
    // on (which the status line then says).
    QList<int> released;
    int through = 0;
    for (const HandleMix &mix : handleMixesOn(0)) {
        const bool cuts = mix.from <= end && mix.to - 1 >= start;
        if (!cuts && !(mix.to >= start && mix.nextEnd - 1 <= end))
            continue;
        released.append(mix.index);
        // Taking a whole clip of the pair, the transition's going needs no saying.
        const bool wholeBefore = start <= mix.start && end >= mix.cut - 1;
        const bool wholeAfter = start <= mix.cut && end >= mix.nextEnd - 1;
        if (cuts && !wholeBefore && !wholeAfter)
            ++through;
    }
    return withMixesReleased(0, released, through, [&] { return removeRangePlain(start, end); });
}

bool Timeline::removeRangePlain(int start, int end) {
    if (trackLocked(0))
        return false;
    if (!primary() || start < 0 || end < start
        || start >= primaryFrameCount())
        return false;
    end = qMin(end, primaryFrameCount() - 1);
    const int span = end - start + 1;

    // Without dissolves the playlist's remove_region does the whole job — blanks
    // included, ripple included; the tracks above follow from the range's
    // end. (The snapshot path below keeps the dissolve bookkeeping, and
    // takes the range's share out of the empty space before each clip.)
    if (!hasMixes()) {
        keepCarvedClips(0, start, end + 1);
        if (primary()->remove_region(start, span) < 0) // returns the clip index
            return false;
        tidyPrimary();
        rippleOthersFrom(0, end + 1, -span);
        return true;
    }

    const QVariantList before = entries();
    QVariantList result;
    int carryGap = 0; // empty space a removed clip leaves to the next one
    for (const QVariant &v : before) {
        QVariantMap map = v.toMap();
        const int clipStart = map.value(ClipKey::start).toInt();
        const int frames = map.value(ClipKey::frames).toInt();
        const int clipEnd = clipStart + frames - 1;

        if (map.value(ClipKey::lane).toInt() >= 1) {
            // Sync-lock: track clips downstream of the range slide left
            // with it; ones starting inside it hold their position — a
            // storyline edit never deletes a track clip.
            if (clipStart > end && !moveEntryStart(map, clipStart - span, *m_profile))
                return false;
            result.append(map);
            continue;
        }

        // Empty space before the clip is part of V1 too: the frames of it
        // the range covers go, so V1 gets shorter by exactly the range and
        // the tracks that follow it stay in step.
        {
            const int gap = map.value(ClipKey::gapBefore).toInt();
            const int covered = qMax(0, qMin(end, clipStart - 1) - qMax(start, clipStart - gap) + 1);
            if (covered > 0 || carryGap > 0)
                map.insert(ClipKey::gapBefore, gap - covered + carryGap);
            carryGap = 0;
        }

        // A dissolve's overlap must not be carved through.
        if (map.value(ClipKey::mixOut).toInt() > 0) {
            const int seam = clipEnd - map.value(ClipKey::mixOut).toInt();
            if (seam <= end && clipEnd >= start)
                return false;
        }
        if (clipEnd < start || clipStart > end) {
            result.append(map); // untouched (starts are derived on rebuild)
            continue;
        }
        if (clipStart >= start && clipEnd <= end) {
            carryGap = map.value(ClipKey::gapBefore).toInt();
            continue; // fully inside — gone; what is left of its space stays
        }

        // A post-dissolve clip's entries `in` is pre-shrunk by mixIn (the
        // re-applied mix consumes that head), so its material at timeline
        // offset d actually sits at source frame in + mixIn + d. Head-keep
        // and split bounds must account for it; the tail-keep case must NOT
        // (start == clipStart there, the seam dissolve survives and eats
        // the entry head again on rebuild).
        const int in = map.value(ClipKey::in).toInt();
        const int mixIn = map.value(ClipKey::mixIn).toInt();
        if (clipStart < start && clipEnd > end) {
            // The range sits inside this one clip — split around it.
            QVariantMap head = map;
            head.insert(ClipKey::out,
                        in + mixIn + (start - clipStart) - 1);
            head.insert(ClipKey::fadeOut, 0);
            head.insert(ClipKey::mixOut, 0);
            result.append(head);
            QVariantMap tail = map;
            tail.insert(ClipKey::in,
                        in + mixIn + (end - clipStart) + 1);
            tail.insert(ClipKey::fadeIn, 0);
            tail.insert(ClipKey::mixIn, 0); // no seam behind it now
            tail.insert(ClipKey::gapBefore, 0); // it follows its own head
            rebaseEntryKeys(tail, end - clipStart + 1);
            result.append(tail);
        } else if (clipStart < start) {
            map.insert(ClipKey::out,
                       in + mixIn + (start - clipStart) - 1);
            map.insert(ClipKey::fadeOut, 0);
            map.insert(ClipKey::mixOut, 0);
            result.append(map); // keeps its head
        } else {
            map.insert(ClipKey::in, in + (end - clipStart) + 1);
            map.insert(ClipKey::fadeIn, 0);
            rebaseEntryKeys(map, end - clipStart + 1);
            result.append(map); // keeps its tail
        }
    }
    return rebuild(result, before);
}

// Volume and opacity keyframes, edit keys and animated effect keys are counted
// in frames from the clip's first frame. Where an edit moves a clip's in
// point by `moved` frames the material moves under them: the keys go the
// other way and stay on the frames they were set on, as both trims do it.
void rebaseEntryKeys(QVariantMap &entry, int moved) {
    if (moved == 0)
        return;
    for (const QString &key : {ClipKey::volumeKf, ClipKey::opacityKf})
        if (const QVariantList keys = entry.value(key).toList(); !keys.isEmpty())
            entry.insert(key, EditEngine::shiftedCurve(keys, moved));
    if (const QVariantMap keys = entry.value(ClipKey::editKeys).toMap(); !keys.isEmpty())
        entry.insert(ClipKey::editKeys, EditEngine::shiftedKeys(keys, moved, 0));
    if (const QVariantList effects = entry.value(ClipKey::videoEffects).toList(); !effects.isEmpty())
        entry.insert(ClipKey::videoEffects, EditEngine::shiftedEffects(effects, moved));
}

bool Timeline::roll(int index, int delta) {
    return rollOn(0, index, delta);
}

bool Timeline::slideOn(int track, int index, int delta) {
    if (!primary() || delta == 0 || index < 1 || trackLocked(track)
        || index + 1 >= trackClipCount(track))
        return false;
    const QVariantList before = entries();
    QVariantList clips = before;
    int clip = 0, aIndex = -1, mIndex = -1, bIndex = -1;
    for (int i = 0; i < clips.size(); ++i) {
        if (clips.at(i).toMap().value(ClipKey::lane).toInt() != track)
            continue;
        if (clip == index - 1)
            aIndex = i;
        else if (clip == index)
            mIndex = i;
        else if (clip == index + 1)
            bIndex = i;
        ++clip;
    }
    if (aIndex < 0 || mIndex < 0 || bIndex < 0)
        return false;
    QVariantMap a = clips.at(aIndex).toMap();
    QVariantMap m = clips.at(mIndex).toMap();
    QVariantMap b = clips.at(bIndex).toMap();
    if (a.value(ClipKey::mixOut).toInt() > 0
        || m.value(ClipKey::mixIn).toInt() > 0
        || m.value(ClipKey::mixOut).toInt() > 0
        || b.value(ClipKey::mixIn).toInt() > 0)
        return false; // dissolves own those seams
    const int aEnd = a.value(ClipKey::start).toInt()
                     + a.value(ClipKey::frames).toInt();
    const int mStart = m.value(ClipKey::start).toInt();
    const int mEnd = mStart + m.value(ClipKey::frames).toInt();
    if (mStart != aEnd || b.value(ClipKey::start).toInt() != mEnd)
        return false; // a slide needs both neighbours touching
    const int newAOut = a.value(ClipKey::out).toInt() + delta;
    const int newBIn = b.value(ClipKey::in).toInt() + delta;
    if (newAOut < a.value(ClipKey::in).toInt()
        || newAOut >= a.value(ClipKey::length).toInt()
        || newBIn < 0 || newBIn > b.value(ClipKey::out).toInt())
        return false;
    a.insert(ClipKey::out, newAOut);
    b.insert(ClipKey::in, newBIn);
    rebaseEntryKeys(b, delta);
    if (track >= 1) { // lane starts are absolute
        m.insert(ClipKey::start, mStart + delta);
        b.insert(ClipKey::start,
                 b.value(ClipKey::start).toInt() + delta);
    }
    clips[aIndex] = a;
    clips[mIndex] = m;
    clips[bIndex] = b;
    return rebuild(clips, before); // total length unchanged: no ripple
}

bool Timeline::joinOn(int track, int index) {
    if (!primary() || index < 0 || trackLocked(track)
        || index + 1 >= trackClipCount(track))
        return false;
    const QVariantList before = entries();
    QVariantList clips = before;
    int clip = 0, aIndex = -1, bIndex = -1;
    for (int i = 0; i < clips.size(); ++i) {
        if (clips.at(i).toMap().value(ClipKey::lane).toInt() != track)
            continue;
        if (clip == index)
            aIndex = i;
        else if (clip == index + 1)
            bIndex = i;
        ++clip;
    }
    if (aIndex < 0 || bIndex < 0)
        return false;
    QVariantMap a = clips.at(aIndex).toMap();
    const QVariantMap b = clips.at(bIndex).toMap();
    auto plain = [](const QVariantMap &e) {
        return e.value(ClipKey::title).toString().isEmpty()
               && !e.value(ClipKey::gap).toBool()
               && e.value(ClipKey::compoundJson).toString().isEmpty();
    };
    if (!plain(a) || !plain(b)
        || a.value(ClipKey::mixOut).toInt() > 0
        || b.value(ClipKey::mixIn).toInt() > 0
        || a.value(ClipKey::path) != b.value(ClipKey::path)
        || !sameSourceOrigin(a, b)
        || !qFuzzyCompare(a.value(ClipKey::speed, 1.0).toDouble(),
                          b.value(ClipKey::speed, 1.0).toDouble())
        || a.value(ClipKey::audioOnly) != b.value(ClipKey::audioOnly)
        || a.value(ClipKey::videoOnly) != b.value(ClipKey::videoOnly))
        return false;
    const int aEnd = a.value(ClipKey::start).toInt()
                     + a.value(ClipKey::frames).toInt();
    if (b.value(ClipKey::start).toInt() != aEnd
        || b.value(ClipKey::in).toInt()
               != a.value(ClipKey::out).toInt() + 1)
        return false; // not one continuous piece of the source
    a.insert(ClipKey::out, b.value(ClipKey::out));
    a.insert(ClipKey::fadeOut, b.value(ClipKey::fadeOut));
    a.insert(ClipKey::mixOut, b.value(ClipKey::mixOut));
    a.insert(ClipKey::mixStyle, b.value(ClipKey::mixStyle));
    a.insert(ClipKey::mixAlign, b.value(ClipKey::mixAlign));
    a.insert(ClipKey::mixExtA, b.value(ClipKey::mixExtA));
    a.insert(ClipKey::mixExtB, b.value(ClipKey::mixExtB));
    clips[aIndex] = a;
    clips.removeAt(bIndex);
    return rebuild(clips, before);
}

bool Timeline::rollOn(int track, int index, int delta, QString *error, int *applied) {
    auto refuse = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!primary() || delta == 0 || index < 0 || track < 0 || track >= trackCount()
        || index >= trackClipCount(track) - 1)
        return false;
    const auto before = entries();
    int aIndex = -1, bIndex = -1;
    for (int i = 0; i < before.size(); ++i) {
        const auto e = before[i].toMap();
        if (e.value(ClipKey::track).toInt() != track) continue;
        if (e.value(ClipKey::index).toInt() == index) aIndex = i;
        if (e.value(ClipKey::index).toInt() == index + 1) bIndex = i;
    }
    if (aIndex < 0 || bIndex < 0) return false;
    const auto a = before[aIndex].toMap(), b = before[bIndex].toMap();
    if (b.value(ClipKey::start).toInt() != a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt())
        return refuse(QCoreApplication::translate("Timeline", "A roll needs two touching clips; nothing was changed."));

    // Picture edits carry each linked sound edge by the same delta. Preserve
    // independently trimmed sound (J/L cuts, offsets, alternate camera audio),
    // instead of re-conforming it to the picture after the edit. A roll made
    // directly on a sound track remains an independent sound edit.
    QMap<int, bool> edges{{aIndex, true}, {bIndex, false}}; // row -> outgoing edge
    bool ownSound = false; // a side is a multicam clip with a sound source of its own
    for (int side : {aIndex, bIndex}) {
        const auto source = before[side].toMap();
        const auto link = source.value(ClipKey::link).toString();
        if (trackLocked(source.value(ClipKey::track).toInt()))
            return refuse(QCoreApplication::translate("Timeline", "Unlock the clips and their linked tracks before rolling this edit."));
        if (link.isEmpty()) continue;
        for (int row = 0; row < before.size(); ++row) {
            const auto mate = before[row].toMap();
            if (mate.value(ClipKey::link).toString() != link) continue;
            if (trackLocked(mate.value(ClipKey::track).toInt()))
                return refuse(QCoreApplication::translate("Timeline", "Unlock the clips and their linked tracks before rolling this edit."));
            if (source.value(ClipKey::audioOnly).toBool() || !mate.value(ClipKey::audioOnly).toBool()) continue;
            // (The sound of a multicam clip with a sound source of its own is
            // that source under the picture wherever the cut lies: it is
            // conformed when the edit settles, and has no handles to run out of.)
            if (!fixedSoundOf(source).isEmpty()) { ownSound = true; continue; }
            const bool outgoing = side == aIndex;
            if (edges.contains(row) && edges[row] != outgoing)
                return refuse(QCoreApplication::translate("Timeline", "This linked group has no separate edit point; nothing was changed."));
            edges[row] = outgoing;
        }
    }
    // A transition on this cut rolls with it: the cut moves, and the
    // transition stays across it with the sides, length and kind it has. Its
    // frames come from the two clips' spare frames, so the roll goes as far
    // as both still cover their halves (and each keeps a frame of its own
    // outside it) and stops there, rather than being refused.
    // (One whose clips share frames, an overlap, has no cut of its own to
    // move: it refuses the roll, as it always has.)
    const bool across = a.value(ClipKey::mixOut).toInt() > 0 && b.value(ClipKey::mixIn).toInt() > 0
                        && mixParts(a).overlap == 0;
    if (across) {
        qint64 least = std::numeric_limits<int>::min(), most = std::numeric_limits<int>::max();
        for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
            const auto e = before[edge.key()].toMap();
            const qint64 frames = e.value(ClipKey::frames).toInt(), own = e.value(ClipKey::mixOut).toInt();
            if (edge.value()) {
                least = qMax(least, own + 1 - frames);
                most = qMin(most, qint64(e.value(ClipKey::length).toInt()) - 1 - e.value(ClipKey::out).toInt());
            } else {
                least = qMax(least, -qint64(qMin(e.value(ClipKey::in).toInt(), e.value(ClipKey::start).toInt())));
                most = qMin(most, frames - own - 1);
            }
        }
        delta = int(qBound(qMin(least, 0LL), qint64(delta), qMax(most, 0LL)));
        if (delta == 0)
            return refuse(QCoreApplication::translate("Timeline", "The roll exceeds available picture or sound handles; nothing was changed."));
    }
    if (applied)
        *applied = delta;
    auto clips = before;
    for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
        auto e = clips[edge.key()].toMap();
        const int lane = e.value(ClipKey::track).toInt();
        const bool mixed = e.value(ClipKey::mixIn).toInt() > 0 || e.value(ClipKey::mixOut).toInt() > 0;
        if (e.value(ClipKey::gap).toBool() || m_tracks[size_t(lane)].mix.value("editKind") == "caption"
            || (mixed && !across))
            return refuse(QCoreApplication::translate("Timeline", "Roll plain picture or sound clips without gaps or transitions; nothing was changed."));
        const qint64 in = e.value(ClipKey::in).toInt() + (edge.value() ? 0LL : qint64(delta));
        const qint64 out = e.value(ClipKey::out).toInt() + (edge.value() ? qint64(delta) : 0LL);
        const qint64 start = e.value(ClipKey::start).toInt() + (edge.value() ? 0LL : qint64(delta));
        if (in < 0 || out < in || out >= e.value(ClipKey::length).toInt() || start < 0
            || start + out - in + 1 > std::numeric_limits<int>::max())
            return refuse(QCoreApplication::translate("Timeline", "The roll exceeds available picture or sound handles; nothing was changed."));
        // (An entry in a transition counts its frames from the transition's
        // end, or to it: it grows or shrinks by the roll, as a plain one does.)
        const int frames = mixed ? e.value(ClipKey::frames).toInt() + (edge.value() ? delta : -delta) : int(out - in + 1);
        if (frames < 1)
            return refuse(QCoreApplication::translate("Timeline", "The roll exceeds available picture or sound handles; nothing was changed."));
        e[ClipKey::in] = int(in); e[ClipKey::out] = int(out); e[ClipKey::start] = int(start);
        e[ClipKey::frames] = frames;
        if (!edge.value()) {
            rebaseEntryKeys(e, delta); // an incoming edge: its in point moved
            // (Under a transition the clip's running effects keep their
            // place too, as they do when the transition's own edge moves:
            // the frames it goes on showing are the pictures they were.)
            if (across)
                rebaseEntryClocks(e, delta);
        }
        clips[edge.key()] = e;
    }
    // An independently trimmed mate may border unrelated sound. Never let
    // restore overwrite that neighbour while extending the linked edge.
    for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
        const auto e = clips[edge.key()].toMap();
        if (e.value(ClipKey::track).toInt() == 0) continue;
        for (int row = 0; row < clips.size(); ++row) {
            if (row == edge.key()) continue;
            const auto other = clips[row].toMap();
            if (other.value(ClipKey::track) != e.value(ClipKey::track)) continue;
            const qint64 from = e.value(ClipKey::start).toInt(), to = from + e.value(ClipKey::frames).toInt();
            const qint64 otherFrom = other.value(ClipKey::start).toInt(), otherTo = otherFrom + other.value(ClipKey::frames).toInt();
            if (from < otherTo && otherFrom < to)
                return refuse(QCoreApplication::translate("Timeline", "The roll would overlap another clip on a linked track; nothing was changed."));
        }
    }
    if (!rebuild(clips, before))
        return false;
    if (ownSound)
        conformLinkedSound(before);
    return true;
}

bool Timeline::connectAt(const QString &path, int position, int in, int out,
                            bool audioOnly, int lane, double speed,
                            bool videoOnly, const QVariantMap &retime) {
    if(lane>0 && lane<trackCount() && m_tracks[size_t(lane)].mix.value("editKind")=="caption" && !m_restoring)return false;
    if (position < 0 || path.isEmpty())
        return false;
    if (!primary() && !lockProfileTo(path))
        return false;
    edit::Producer producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;
    if (lane > 0 && lane < trackCount() && !m_restoring && !trackLocked(lane)) {
        const int wanted = in >= 0 && out >= in ? out-in+1 : producer.get_playtime();
        if (wanted < 1)
            return false;
        const bool reopen = hasLaneDissolve();
        if (reopen)
            producer = edit::Producer(); // closed while its profile lives
        if (breakLaneDissolves(lane, position, position + wanted) < 0)
            return false;
        if (reopen && !(producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime)).is_valid())
            return false;
    }

    return placeConnected(producer, position, in, out,
                          lane > 0 ? lane - 1 : -1);
}

// A track clip's producer: optionally through timewarp (pitch kept), and
// audio-only ones drop their video stream. Invalid producer = bad path.
edit::Producer Timeline::makeLaneProducer(const QString &path, bool audioOnly,
                                            double speed, bool videoOnly, const QVariantMap &retime) {
    if ((FrameSampling::explicitPictures(m_restoreSampling) || m_sourceOrigin)
        && !path.startsWith("color:") && !MediaPlayer::isStillPath(path)
        && generatorKindOf(path).isEmpty() && !path.startsWith("tone:")
        && !(QFileInfo(path).isAbsolute() && !QFileInfo::exists(path) && m_allowOffline
             && !QFileInfo::exists(playbackPath(path))))
        return makeSampledProducer(path,audioOnly,speed,videoOnly,retime,m_restoreSampling);
    if (!retime.isEmpty()) {
        if (!qFuzzyCompare(speed, 1.0)) return edit::Producer();
        auto source = makeLaneProducer(path, audioOnly, 1.0, videoOnly);
        return wrapRetime(source, retime);
    }
    const QVariantMap settings = m_mediaSources.value(path).toMap();
    const bool userRetimed = speed != 0.0 && !qFuzzyCompare(speed, 1.0);
    const double effective = (speed == 0.0 ? 1.0 : speed) * interpretationFactor(settings);
    const bool retimed = !qFuzzyCompare(effective, 1.0);
    const QString playback = AudioEngine::projectResource(playbackPath(path));
    const bool offline = QFileInfo(playback).isAbsolute() && !QFileInfo::exists(playback) && m_allowOffline && m_restoring;
    const QString resource = offline ? QStringLiteral("color:0x181820") :
        retimed ? QStringLiteral("timewarp:%1:%2")
                      .arg(QString::number(effective), AnimatedWebP::resource(playback))
                : AnimatedWebP::resource(playback);
    edit::Producer producer = offline ? edit::Producer(*m_profile, resource.toUtf8().constData())
                                      : sourceProducer(resource, playback, retimed ? effective : 1.);
    if (!producer.is_valid())
        return producer;
    producer.set("omavid:path", path.toUtf8().constData());
    if(m_sourceOrigin)producer.set("omavid:sourceOrigin",*m_sourceOrigin);
    if (offline) {
        producer.set("omavid:offline", 1);
        const int sourceFrames = m_offlineLengths.value(path, settings.value(QStringLiteral("duration"), 100000000).toInt());
        const int length = qMax(1, qRound(sourceFrames / qMax(0.001, qAbs(speed))));
        producer.set("length", length);
        producer.set_in_and_out(0, length - 1);
    }
    applyInputTransform(*m_profile, producer, settings);
    if (!settings.isEmpty()) producer.set("omavid:sourceMeta", QJsonDocument::fromVariant(settings).toJson(QJsonDocument::Compact).constData());
    if (userRetimed)
        producer.set("omavid:speed", speed);
    if (retimed && !offline) {
        producer.set("warp_pitch", 1); // pitch-preserved, like append
        if (!audioOnly)
            holdRetimedTail(*m_profile, producer, effective);
    }
    if (audioOnly) {
        producer.set("video_index", -1); // avformat: drop the video stream
        producer.set("omavid:audioOnly", 1);
    } else if (videoOnly) {
        producer.set("audio_index", -1); // the detached-video half, on a lane
        producer.set("omavid:videoOnly", 1);
        if(playback.contains("omaedit.project:"))attachRecord(producer,"omaedit.fader",{{"muted",true}},"omavid:nestVideoOnly");
    }
    return producer;
}

// ── Edit operations at any address (A2) ─────────────────────────────────
bool Timeline::splitOn(int track, int position) {
    if (track == 0)
        return splitAt(position);
    if (track < 1 || track >= trackCount() || trackLocked(track))
        return false;
    if (!m_tracks[size_t(track)].audioEntries.isEmpty()) {
        const auto at=AudioTimeline::writeTime({qint64(position)*m_profile->frame_rate_den(),m_profile->frame_rate_num()});
        const int count=trackClipCount(track);
        for (int i=0;i<count;++i) if (splitAudioAt(track,i,at)) return true;
        return false;
    }
    edit::Playlist *pl = laneAt(track - 1);
    const int entry = pl->get_clip_index_at(position);
    if (entry < 0 || entry >= pl->count() || pl->is_blank(entry)
        || pl->is_mix(entry))
        return false;
    const int relative = position - pl->clip_start(entry);
    if (relative < 1 || relative >= pl->clip_length(entry))
        return false; // a blade on a boundary would make an empty clip
    const int index = clipIndexContaining(track, position);
    const QVariantMap map = index >= 0 ? entryMapOn(track, index) : QVariantMap();
    if (pl->split(entry, relative - 1) != 0)
        return false;
    if (index >= 0)
        decorateSplit(track, index, map, relative);
    // A blade on a linked picture or sound blades its mate on the other track
    // at the same frame, wherever that is; the right halves get a link of
    // their own. (Left alone, the sound would follow its shortened picture
    // and lose everything after the cut.)
    const QString link = map.value(ClipKey::link).toString();
    if (index >= 0 && !link.isEmpty() && !m_linkingSplit) {
        const bool sound = map.value(ClipKey::audioOnly).toBool();
        bool soundless = !sound && !fixedSoundOf(map).isEmpty();
        for (const QVariant &e : entries()) {
            const QVariantMap mate = e.toMap();
            const int mateTrack = mate.value(ClipKey::track).toInt();
            if (mateTrack == track || mate.value(ClipKey::link).toString() != link
                || mate.value(ClipKey::audioOnly).toBool() == sound)
                continue;
            soundless = false;
            const int mateStart = mate.value(ClipKey::start).toInt();
            if (position <= mateStart || position >= mateStart + mate.value(ClipKey::frames).toInt())
                continue;
            const int mateIndex = mateTrack == 0 ? clipIndexContaining(0, position)
                                                 : mate.value(ClipKey::index).toInt();
            if (mateIndex < 0)
                break;
            m_linkingSplit = true;
            const bool ok = splitOn(mateTrack, position);
            m_linkingSplit = false;
            if (ok) {
                const QString fresh = newLinkId();
                setLinkAt(track, index + 1, fresh);
                setLinkAt(mateTrack, mateIndex + 1, fresh);
            }
            break;
        }
        // (See splitAt: a sound the editor removed stays removed for both parts.)
        if (soundless)
            setLinkAt(track, index + 1, newLinkId());
    }
    return true;
}

int Timeline::splitAllAt(int position) {
    if (position < 1 || !primary())
        return 0;
    const QVariantList before = entries();
    QMap<int, QVariantMap> targets;
    for (int track = 0; track < trackCount(); ++track) {
        if (trackLocked(track) || trackMix(track).value("editKind") == "caption")
            continue;
        auto *playlist = m_tracks[size_t(track)].playlist.get();
        const int entry = playlist->get_clip_index_at(position);
        if (entry < 0 || entry >= playlist->count() || playlist->is_blank(entry)
            || playlist->is_mix(entry) || position <= playlist->clip_start(entry)
            || position >= playlist->clip_start(entry) + playlist->clip_length(entry))
            continue;
        const auto map = entryMapOn(track, clipIndexContaining(track, position));
        if (!map.isEmpty()) targets.insert(track, map);
    }
    // A locked or otherwise uncuttable mate protects the entire linked group.
    // Playback visibility, mute and sync lock do not remove editing targets.
    // (The sound of a multicam clip with a sound source of its own lies only
    // where that source has sound: not under the frame, it is no part of the
    // cut and holds nothing up. The picture's two parts keep its link, and
    // the conform below gives the sound to the part it lies under.)
    QSet<QString> ownSound, blockedLinks, uncutSound;
    for (const QVariant &v : before)
        if (!v.toMap().value(ClipKey::audioOnly).toBool() && !fixedSoundOf(v.toMap()).isEmpty())
            ownSound.insert(v.toMap().value(ClipKey::link).toString());
    for (const QVariant &v : before) {
        const auto entry = v.toMap();
        const auto link = entry.value(ClipKey::link).toString();
        const auto target = targets.value(entry.value(ClipKey::track).toInt());
        if (link.isEmpty() || (!target.isEmpty() && target.value("index") == entry.value(ClipKey::index)))
            continue;
        if (ownSound.contains(link) && entry.value(ClipKey::audioOnly).toBool() && !trackLocked(entry.value(ClipKey::track).toInt()))
            uncutSound.insert(link);
        else
            blockedLinks.insert(link);
    }
    QHash<QString, QString> rightLinks;
    QScopedValueRollback<bool> linking(m_linkingSplit, true);
    int cuts = 0;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        const auto link = it.value().value(ClipKey::link).toString();
        if (blockedLinks.contains(link)) continue;
        if (!splitOn(it.key(), position)) {
            restore(before);
            return -1;
        }
        if (!link.isEmpty() && !uncutSound.contains(link)) {
            if (!rightLinks.contains(link)) rightLinks.insert(link, newLinkId());
            setLinkAt(it.key(), it.value().value(ClipKey::index).toInt() + 1, rightLinks.value(link));
        }
        ++cuts;
    }
    if (cuts > 0 && !ownSound.isEmpty())
        conformLinkedSound(before);
    return cuts;
}

bool Timeline::removeOn(int track, int index, bool ripple) {
    if (track == 0)
        return ripple ? remove(index) : lift(index);
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty()) {
        if (trackLocked(track) || ripple || index<0 || index>=trackClipCount(track)) return false;
        auto cuts=m_tracks[size_t(track)].audioEntries; cuts.removeAt(index);
        if (!cuts.isEmpty()) return restoreAudioTrack(track,cuts);
        
        auto &target=m_tracks[size_t(track)]; target.audioEntries.clear(); target.playlist=std::make_unique<edit::Playlist>(*m_profile);
        refreshTrackFilters(); return true;
    }
    int entry = entryIndexOn(track, index);
    if (entry < 0)
        return false;
    // A clip leaving takes its dissolves with it: the playlist would keep
    // the mix entry beside the hole, and the clip after it would then read
    // as dissolved, its head cut short.
    {
        const edit::Playlist *lane = laneAt(track - 1);
        const int from = lane->clip_start(entry);
        const int broken = breakLaneDissolves(track, from, from + lane->clip_length(entry));
        if (broken < 0 || (broken > 0 && (entry = entryIndexOn(track, index)) < 0))
            return false;
    }
    edit::Playlist *pl = laneAt(track - 1);
    const int start = pl->clip_start(entry);
    const int frames = pl->clip_length(entry);
    if (!ripple) {
        delete pl->replace_with_blank(entry);
        tidyConnected();
        return true;
    }
    if (pl->remove(entry) != 0)
        return false;
    pl->consolidate_blanks(0);
    rippleOthersFrom(track, start + frames, -frames);
    tidyConnected();
    return true;
}

// Several clips leave as one edit. Taken one at a time, each closing its
// place and pulling the other tracks along before the next was looked up,
// a clip of another track slid over clips still waiting to go: the
// addresses then named other clips, and a picture and its sound, which
// leave the same frames, closed them twice. So every clip is lifted first
// (nothing else moves, and each address stays good: higher tracks first,
// then higher indices), and the frames they left are closed afterwards,
// once: a track closes its own clips' places, and the tracks that ride a
// ripple follow the frames closed on any track.
bool Timeline::removeSet(const QList<QPair<int, int>> &clips, bool ripple, QList<QPair<int, int>> *primaryClosed, bool *keptGap) {
    const QVariantList pristine = entries();
    // The transitions of the clips that go come off first (see removeOn):
    // one that shares frames of its pair moves what follows it as it goes,
    // and the places below are read once nothing moves any more.
    for (const auto &a : clips) {
        if (a.first < 1 || a.first >= trackCount())
            continue;
        const int entry = entryIndexOn(a.first, a.second);
        if (entry < 0)
            continue;
        const edit::Playlist *lane = laneAt(a.first - 1);
        const int from = lane->clip_start(entry);
        if (breakLaneDissolves(a.first, from, from + lane->clip_length(entry)) < 0) {
            restore(pristine);
            return false;
        }
    }
    const QVariantList base = entries();
    QHash<QPair<int, int>, QVariantMap> chosen;
    // A clip whose linked partner stays (a picture without its sound, a
    // sound without its picture) leaves its place empty: closing it would
    // slide every later clip of its track off its own partner (see removePlain).
    QSet<QString> soundStays, pictureStays;
    QHash<QString, int> pictureBefore;
    for (const QVariant &v : base) {
        const QVariantMap m = v.toMap();
        const QPair<int, int> at(m.value(ClipKey::track).toInt(), m.value(ClipKey::index).toInt());
        const QString link = m.value(ClipKey::link).toString();
        if (m.value(ClipKey::lane).toInt() < 1 && !link.isEmpty())
            pictureBefore.insert(link, m.value(ClipKey::start).toInt());
        if (clips.contains(at))
            chosen.insert(at, m);
        else if (!link.isEmpty())
            (m.value(ClipKey::audioOnly).toBool() ? soundStays : pictureStays).insert(link);
    }
    struct Span { int from, to; };
    QHash<int, QList<Span>> left; // track → the places its clips left, to be closed
    QList<Span> all;
    bool any = false;
    for (const auto &a : clips) {
        if (!chosen.contains(a))
            continue;
        const QVariantMap m = chosen.value(a);
        const QString link = m.value(ClipKey::link).toString();
        const bool sound = m.value(ClipKey::audioOnly).toBool();
        const bool held = !link.isEmpty() && (sound ? pictureStays : soundStays).contains(link);
        if (a.first == 0) {
            if (!ripple || held) {
                const bool lifted = lift(a.second);
                any = lifted || any;
                if (lifted && ripple && keptGap)
                    *keptGap = true;
                continue;
            }
            QPair<int, int> closed{0, 0};
            if (!withMixesReleased(0, {a.second - 1, a.second}, 0, [&] { return removePlain(a.second, &closed); }))
                continue;
            any = true;
            if (primaryClosed)
                primaryClosed->append({m.value(ClipKey::start).toInt(), closed.second});
            if (closed.second < 0) {
                left[0].append({closed.first + closed.second, closed.first});
                all.append(left[0].last());
            }
            continue;
        }
        if (!removeOn(a.first, a.second, false))
            continue;
        any = true;
        if (!ripple)
            continue;
        if (held) {
            if (keptGap && !sound)
                *keptGap = true;
            continue;
        }
        const int from = m.value(ClipKey::start).toInt();
        left[a.first].append({from, from + m.value(ClipKey::frames).toInt()});
        all.append(left[a.first].last());
    }
    if (all.isEmpty())
        return any;
    // The frames to close, each once: places that meet or share frames are one.
    std::sort(all.begin(), all.end(), [](const Span &x, const Span &y) { return x.from < y.from; });
    QList<Span> closing;
    for (const Span &s : std::as_const(all)) {
        if (!closing.isEmpty() && s.from <= closing.last().to)
            closing.last().to = qMax(closing.last().to, s.to);
        else
            closing.append(s);
    }
    // The frames closed before `at`: of every track's, and of one track's own.
    const auto closedBefore = [](const QList<Span> &spans, int at) {
        int frames = 0;
        for (const Span &s : spans)
            if (s.to <= at)
                frames += s.to - s.from;
        return frames;
    };
    // The main track has closed its own clips' places. Where none of its
    // clips went it rides as it does any other track's ripple: it closes
    // empty frames and never gives up a picture (shiftPrimaryFrom).
    if (trackCount() > 0 && !m_tracks[0].locked && m_tracks[0].autoSelect)
        for (int i = int(closing.size()) - 1; i >= 0; --i) {
            const Span &c = closing.at(i);
            bool own = false;
            for (const Span &s : left.value(0))
                own = own || (s.from < c.to && s.to > c.from);
            if (!own)
                shiftPrimaryFrom(c.to - closedBefore(left.value(0), c.to), c.from - c.to);
        }
    if (connectedCount() == 0)
        return true;
    QHash<QString, int> pictureAfter;
    QList<int> wanted;
    bool moves = false;
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        const int track = m.value(ClipKey::lane).toInt();
        const QString link = m.value(ClipKey::link).toString();
        if (track < 1) {
            if (!link.isEmpty())
                pictureAfter.insert(link, m.value(ClipKey::start).toInt());
            continue;
        }
        const int start = m.value(ClipKey::start).toInt();
        const Track &t = m_tracks[size_t(track)];
        int by = closedBefore(left.value(track), start);
        if (!t.locked && t.autoSelect)
            by = qMax(by, closedBefore(closing, start));
        // Sound linked to a picture of the main track moves only when its picture did (see rippleFrom).
        if (pictureBefore.contains(link) && pictureAfter.contains(link) && pictureAfter.value(link) == pictureBefore.value(link))
            by = 0;
        moves = moves || by > 0;
        wanted.append(qMax(0, start - by));
    }
    if (moves)
        replaceLaneStarts(wanted);
    return true;
}

bool Timeline::insertOn(int track, const QString &path, int position,
                           int in, int out, bool audioOnly, double speed,
                           bool videoOnly, const QVariantMap &retime) {
    if(track>=0 && track<trackCount() && m_tracks[size_t(track)].mix.value("editKind")=="caption" && !m_restoring)return false;
    if (track == 0)
        return audioOnly ? false
                         : insertAt(path, position, in, out, speed, videoOnly, retime);
    if (track < 1 || track > trackCount() || position < 0 || !primary())
        return false;
    if (track < trackCount()
        && (trackLocked(track) || !laneIsKind(track - 1, audioOnly)))
        return false;
    edit::Producer producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;
    if ((in >= 0 && out >= in ? out-in+1 : producer.get_playtime()) < 1)
        return false;
    {
        const bool reopen = hasLaneDissolve();
        if (reopen)
            producer = edit::Producer(); // closed while its profile lives
        if (breakLaneDissolves(track, position, position) < 0
            || breakRippledLaneDissolves(track, position) < 0)
            return false;
        if (reopen && !(producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime)).is_valid())
            return false;
    }
    std::unique_ptr<edit::Producer> cut = boundedCut(producer, in, out);
    if (!cut)
        return false;
    const int span = cut->get_playtime();
    edit::Playlist *pl = ensureLane(track - 1);
    m_tracks[size_t(track)].kind = audioOnly ? Track::Audio : Track::Video;
    bool ok;
    if (position >= pl->get_playtime()) {
        ok = pl->insert_at(position, *cut, 1) >= 0; // pads a blank, appends
    } else {
        int entry = pl->get_clip_index_at(position);
        const int entryStart = pl->clip_start(entry);
        if (position > entryStart) { // split whatever is under the point
            // A clip is cut as the blade cuts it: both halves stay dressed,
            // and linked sound or picture is cut with it so the half that
            // rides the ripple takes its own along.
            const bool bladed = !pl->is_blank(entry) && !pl->is_mix(entry) && splitOn(track, position);
            if (!bladed && pl->split(entry, position - entryStart - 1) != 0)
                return false;
            ++entry;
        }
        ok = pl->insert(*cut, entry) == 0;
    }
    if (ok)
        rippleOthersFrom(track, position, span);
    tidyConnected();
    return ok;
}

bool Timeline::overwriteOn(int track, const QString &path, int position,
                              int in, int out, bool audioOnly, double speed,
                              bool videoOnly, const QVariantMap &retime) {
    if(track>=0 && track<trackCount() && m_tracks[size_t(track)].mix.value("editKind")=="caption" && !m_restoring)return false;
    if (track == 0)
        return audioOnly ? false
                         : overwriteAt(path, position, in, out, speed, videoOnly, retime);
    if (track < 1 || track > trackCount() || position < 0 || !primary())
        return false;
    if (track < trackCount()
        && (trackLocked(track) || !laneIsKind(track - 1, audioOnly)))
        return false;
    edit::Producer producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;
    const int wanted = in >= 0 && out >= in ? out-in+1 : producer.get_playtime();
    if (wanted < 1)
        return false;
    {
        const bool reopen = hasLaneDissolve();
        if (reopen)
            producer = edit::Producer(); // closed while its profile lives
        if (breakLaneDissolves(track, position, position + wanted) < 0)
            return false;
        if (reopen && !(producer = makeLaneProducer(path, audioOnly, speed, videoOnly, retime)).is_valid())
            return false;
    }
    std::unique_ptr<edit::Producer> cut = boundedCut(producer, in, out);
    if (!cut)
        return false;
    const int span = cut->get_playtime();
    edit::Playlist *pl = ensureLane(track - 1);
    m_tracks[size_t(track)].kind = audioOnly ? Track::Audio : Track::Video;
    // Carve the span out (remove_region closes it), then drop the clip in
    // at the same position — downstream keeps its place.
    if (position < pl->get_playtime()) {
        keepCarvedClips(track, position, position + span);
        pl->remove_region(position, span);
    }
    bool ok;
    if (position >= pl->get_playtime()) {
        ok = pl->insert_at(position, *cut, 1) >= 0;
    } else {
        int entry = pl->get_clip_index_at(position);
        const int entryStart = pl->clip_start(entry);
        if (position > entryStart) {
            if (pl->split(entry, position - entryStart - 1) != 0)
                return false;
            ++entry;
        }
        ok = pl->insert(*cut, entry) == 0;
    }
    tidyConnected();
    return ok;
}

// A cut at `position` inside a clip of `track`, both halves dressed as the
// clip was (decorateSplit). The right half is a blade's by default, with no
// link; where the left half is about to be discarded the right one IS the
// clip, its head trimmed, and keeps the link too. An edge, empty space or a
// dissolve under the position is left alone.
bool Timeline::cutKeepingAttributes(int track, int position, bool rightKeepsLink) {
    edit::Playlist *pl = playlistOn(track);
    if (!pl || position <= 0 || position >= pl->get_playtime())
        return false;
    const int entry = pl->get_clip_index_at(position);
    if (entry < 0 || entry >= pl->count() || pl->is_blank(entry) || pl->is_mix(entry))
        return false;
    const int relative = position - pl->clip_start(entry);
    if (relative < 1)
        return false;
    const int index = clipIndexFromEntry(track, entry);
    const QVariantMap map = entryMapOn(track, index);
    if (map.isEmpty() || pl->split(entry, relative - 1) != 0)
        return false;
    decorateSplit(track, index, map, relative);
    const QString link = map.value(ClipKey::link).toString();
    if (rightKeepsLink && !link.isEmpty())
        setLinkAt(track, index + 1, link);
    return true;
}

// An edit is about to carve [from, to) out of `track` through the playlist,
// whose own split gives the right half none of the clip's attributes (grade,
// level, crop, effects, label, link, markers, fades …). Make the two
// boundaries here first, dressed, so every piece that survives is still its
// clip:
// - a clip the span only reaches into keeps its head, or its tail; the tail
//   keeps the link, so its linked sound goes on following it;
// - a clip that holds the whole span is bladed at `from` as the blade does
//   it (linked sound or picture is cut with it), and its tail is the right
//   half of that.
// A restore places clips from a snapshot that already says what each carries.
void Timeline::keepCarvedClips(int track, int from, int to) {
    if (m_restoring || track < 0 || track >= trackCount() || to <= from
        || trackLocked(track) || !m_tracks[size_t(track)].audioEntries.isEmpty())
        return;
    edit::Playlist *pl = playlistOn(track);
    if (!pl)
        return;
    // The playlist entry of the clip a position falls strictly inside.
    const auto under = [&](int position) {
        if (position <= 0 || position >= pl->get_playtime())
            return -1;
        const int entry = pl->get_clip_index_at(position);
        if (entry < 0 || entry >= pl->count() || pl->is_blank(entry) || pl->is_mix(entry)
            || pl->clip_start(entry) == position)
            return -1;
        return entry;
    };
    const int first = under(from);
    if (first >= 0 && first == under(to)) {
        if (!(track == 0 ? splitAt(from) : splitOn(track, from)))
            cutKeepingAttributes(track, from, false);
    } else if (first >= 0) {
        cutKeepingAttributes(track, from, false);
    }
    cutKeepingAttributes(track, to, true);
}

int Timeline::clipIndexAt(int track, int position) const {
    for (const QVariant &v : entries()) {
        const QVariantMap map = v.toMap();
        if (map.value(ClipKey::track).toInt() == track
            && (map.contains(ClipKey::audioTiming) ? m_profile && AudioTimeline::startsAt(map,{qint64(position)*m_profile->frame_rate_den(),m_profile->frame_rate_num()})
                                            : map.value(ClipKey::start).toInt() == position))
            return map.value(ClipKey::index).toInt();
    }
    return -1;
}

// The attributes an entries() map carries, re-applied to the clip at
// (track, index) through the same setters the Inspector uses — a pasted,
// bladed or restyled clip reads back exactly like its source. `link` is
// left to the caller (a bladed half must not share its mate's group).
void Timeline::applyEntryAttributes(int at, int index, const QVariantMap &entry) {
    applyEntryAttributes(at, index, ClipEntry::read(entry));
}

// Everything a snapshot entry carries, put on a clip that already exists.
void Timeline::applyEntryAttributes(int at, int index, const ClipEntry &e) {
    if (e.volume != 0.0)
        setVolumeAt(at, index, e.volume);
    if (!e.volumeKf.isEmpty())
        setVolumeKfAt(at, index, e.volumeKf);
    if (e.pan != 0.0)
        setPanAt(at, index, e.pan);
    if (!e.eq.isEmpty())
        setEqAt(at, index, e.eq);
    if (e.opacity < 1.0)
        setOpacityAt(at, index, e.opacity);
    if (!e.opacityKf.isEmpty())
        setOpacityKfAt(at, index, e.opacityKf);
    if (at >= 1 && (e.rectX != 0.0 || e.rectY != 0.0 || e.rectW != 1.0 || e.rectH != 1.0 || e.rotation != 0.0
                    || e.anchorX != 0.5 || e.anchorY != 0.5))
        setRectAt(at, index, e.rectX, e.rectY, e.rectW, e.rectH, e.rotation, e.anchorX, e.anchorY);
    if (e.cropL != 0.0 || e.cropR != 0.0 || e.cropT != 0.0 || e.cropB != 0.0)
        setCropAt(at, index, e.cropL, e.cropR, e.cropT, e.cropB, e.cropFeather, e.cropInvert);
    if (ClipEntry::orDefault(e.frameBlend) > 0)
        setFrameBlendAt(at, index, *e.frameBlend);
    if (e.adjust)
        setAdjustAt(at, index, true);
    if (e.colorB != 0.0 || e.colorC != 1.0 || e.colorS != 1.0)
        setColorAt(at, index, e.colorB, e.colorC, e.colorS);
    if (e.fadeIn > 0 || e.fadeOut > 0)
        setFadesOn(at, index, e.fadeIn, e.fadeOut);
    if (e.disabled)
        setDisabledAt(at, index, true);
    if (!e.label.isEmpty())
        setLabelAt(at, index, e.label);
    if (!ClipEntry::orDefault(e.category).isEmpty())
        setCategoryAt(at, index, *e.category);
    if (!ClipEntry::orDefault(e.link).isEmpty())
        setLinkAt(at, index, *e.link);
    if (!ClipEntry::orDefault(e.markers).isEmpty())
        setClipMarkersAt(at, index, *e.markers);
    if (!ClipEntry::orDefault(e.original).isEmpty())
        setOriginalAt(at, index, *e.original);
    applyLooks(at, index, e);
    if (!ClipEntry::orDefault(e.mcam).isEmpty())
        setMulticamAt(at, index, *e.mcam, ClipEntry::orDefault(e.mcamAngle));
    if (e.flipH || e.flipV)
        setFlipAt(at, index, e.flipH, e.flipV);
    if (e.freeze)
        setFreezeAt(at, index, *e.freeze, true);
    if (!ClipEntry::orDefault(e.audioMap).isEmpty())
        setAudioMapAt(at, index, *e.audioMap);
    if (e.normalize)
        setNormalizeAt(at, index, true);
    applyEditExtras(at, index, e);
    for (const QString &key : ClipEntry::orDefault(e.bypass))
        setBypassAt(at, index, key, true);
}

bool Timeline::fitToFill(int track, const QString &path, int position, int frames,
                            int in, int out, bool audioOnly, double sourceSpan) {
    if (frames < 1 || in < 0 || out < in || position < 0)
        return false;
    const int sourceFrames = out - in + 1;
    const double speed = (m_sourceOrigin && sourceSpan>0. ? sourceSpan : double(sourceFrames)) / double(frames);
    if (!std::isfinite(speed) || speed < 0.05 || speed > 20.0)
        return false;
    if(m_sourceOrigin) {
        if(!primary() && !lockProfileTo(path))return false;
        const int tin=int(std::floor(in/speed+1e-9));
        const double factor=interpretationFactor(m_mediaSources.value(path).toMap());
        const double origin=*m_sourceOrigin+qMax(0.,in-tin*speed)*factor/m_profile->fps();
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
        return overwriteOn(track,path,position,tin,tin+frames-1,audioOnly,speed);
    }
    if (qAbs(speed - 1.0) < 1e-9)
        return overwriteOn(track, path, position, in, out, audioOnly, 1.0);
    // Timewarp frames: the retimed producer counts source / speed.
    const int tin = qRound(in / speed);
    const int tout = tin + frames - 1;
    return overwriteOn(track, path, position, tin, tout, audioOnly, speed);
}

QStringList Timeline::attributeGroups() {
    return {QStringLiteral("color"),   QStringLiteral("crop"),     QStringLiteral("transform"),
            QStringLiteral("flip"),    QStringLiteral("opacity"),  QStringLiteral("level"),
            QStringLiteral("eq"),      QStringLiteral("channels"), QStringLiteral("blend"),
            QStringLiteral("lens"),    QStringLiteral("fades"),    QStringLiteral("speed"), QStringLiteral("effects")};
}

bool Timeline::pasteAttributes(int track, int index, const QVariantMap &src,
                                  const QStringList &groups, QString *error) {
    if(error)error->clear();
    auto refuse=[&](const QString &reason){if(error)*error=reason;return false;};
    if(trackLocked(track))return refuse(QCoreApplication::translate("SpeedRamp","The clip's track is locked."));
    if (clipIndexContaining(track, entryMapOn(track, index).value(ClipKey::start).toInt()) < 0)
        return false;
    const QVariantMap me = entryMapOn(track, index);
    if (me.isEmpty() || me.value(ClipKey::gap).toBool())
        return false;
    const bool lane = track >= 1;
    const bool audio = me.value(ClipKey::audioOnly).toBool();
    const bool title = !me.value(ClipKey::title).toString().isEmpty();
    auto has = [&](const char *g) { return groups.contains(QLatin1String(g)); };
    const bool speedGroup=has("speed") && !title && !me.contains(ClipKey::compoundJson);
    const bool pattern=src.contains(ClipKey::retime) || src.contains("speedPattern");
    const bool reset=speedGroup && !pattern && me.contains(ClipKey::retime);
    QVariantMap timing;
    // Validate timing before applying any other groups. A rejected speed
    // preset must not leave the target's picture or sound partly changed.
    if(speedGroup) {
        auto check=me;
        if(pattern || reset) {
            const auto prepared=speedRampState(track,index);
            if(!prepared.value("valid").toBool())return refuse(prepared.value("error").toString());
            QVariantMap draft;
            if(pattern) {
                const auto saved=SpeedRampEdit::preset(src);
                if(!saved.value("valid").toBool())return refuse(saved.value("error").toString());
                draft=SpeedRampEdit::fitPreset(saved.value("pattern").toMap(),prepared.value("definition").toMap());
            } else draft=SpeedRampEdit::edit(prepared.value("definition").toMap(),"normal",0);
            if(!draft.value("valid").toBool())return refuse(draft.value("error").toString());
            timing=draft.value("definition").toMap();
            const double sourceRate=m_profile->fps()/interpretationFactor(m_mediaSources.value(me.value(ClipKey::path).toString()).toMap());
            const auto placement=SpeedRampEdit::splice(me,prepared.value("parent").toMap(),timing,reset,sourceRate);
            if(!placement.value("valid").toBool())return refuse(placement.value("error").toString());
            if(reset){for(const auto &key:QStringList{"in","out","frames","length"})check[key]=placement.value(key);
                if(placement.contains(ClipKey::sourceOrigin)) {
                    check[ClipKey::sourceOrigin]=placement.value(ClipKey::sourceOrigin);
                    check[ClipKey::sourceTiming]=QVariantMap{{"rate",sourceRate},{"duration",prepared.value("parent").toMap().value("sourceLength").toDouble()/sourceRate}};
                }
                check[ClipKey::speed]=1.;check.remove(ClipKey::retime);}
        }
        if(!pattern) {
            const auto speed=RetimeEdit::preview(check,src.value(ClipKey::speed,1.).toDouble());
            if(!speed.value("valid").toBool())return refuse(speed.value("error").toString());
        }
    }
    const auto before=entries();
    bool any = false;
    if (has("effects") && !audio) {
        auto stack = src.value(ClipKey::videoEffects).toList();
        for (auto &v : stack) { auto state = v.toMap(); state["uid"] = QUuid::createUuid().toString(QUuid::WithoutBraces); v = state; }
        any = setVideoEffectsAt(track, index, stack) || any;
    }
    if (has("color") && !audio)
        any = setColorAt(track, index, src.value(ClipKey::colorB, 0.0).toDouble(),
                         src.value(ClipKey::colorC, 1.0).toDouble(),
                         src.value(ClipKey::colorS, 1.0).toDouble()) || any;
    if (has("crop") && !audio)
        any = setCropAt(track, index, src.value(ClipKey::cropL, 0.0).toDouble(),
                        src.value(ClipKey::cropR, 0.0).toDouble(),
                        src.value(ClipKey::cropT, 0.0).toDouble(),
                        src.value(ClipKey::cropB, 0.0).toDouble(),
                        src.value(ClipKey::cropFeather, 0.0).toDouble(),
                        src.value(ClipKey::cropInvert).toBool()) || any;
    if (has("transform") && !audio)
        any = setScalingAt(track, index, src.value(ClipKey::scaling).toString()) || any;
    if (has("transform") && !audio)
        any = setRectAt(track, index, src.value(ClipKey::rectX, 0.0).toDouble(),
                        src.value(ClipKey::rectY, 0.0).toDouble(),
                        src.value(ClipKey::rectW, 1.0).toDouble(),
                        src.value(ClipKey::rectH, 1.0).toDouble(),
                        src.value(ClipKey::rotation, 0.0).toDouble(),
                        src.value(ClipKey::anchorX, 0.5).toDouble(),
                        src.value(ClipKey::anchorY, 0.5).toDouble()) || any;
    if (has("flip") && !audio && !title)
        any = setFlipAt(track, index, src.value(ClipKey::flipH).toBool(),
                        src.value(ClipKey::flipV).toBool()) || any;
    if (has("opacity") && !audio) {
        any = setOpacityAt(track, index, src.value(ClipKey::opacity, 1.0).toDouble()) || any;
        const QVariantList kf = src.value(ClipKey::opacityKf).toList();
        if (!kf.isEmpty())
            any = setOpacityKfAt(track, index, kf) || any;
    }
    if (has("level") && !title) {
        any = setVolumeAt(track, index, src.value(ClipKey::volume, 0.0).toDouble()) || any;
        any = setPanAt(track, index, src.value(ClipKey::pan, 0.0).toDouble()) || any;
        const QVariantList kf = src.value(ClipKey::volumeKf).toList();
        if (!kf.isEmpty())
            any = setVolumeKfAt(track, index, kf) || any;
    }
    if (has("eq") && !title)
        any = setEqAt(track, index, src.value(ClipKey::eq).toString()) || any;
    if (has("channels") && !title) {
        any = setAudioMapAt(track, index, src.value(ClipKey::audioMap).toString()) || any;
        any = setNormalizeAt(track, index, src.value(ClipKey::normalize).toBool()) || any;
    }
    if (has("blend") && lane && !audio)
        any = setBlendAt(track, index, src.value(ClipKey::blend, 0).toInt()) || any;
    if (has("lens") && !audio && !title)
        any = setLensAt(track, index, src.value(ClipKey::lensK1, 0.0).toDouble(),
                        src.value(ClipKey::lensK2, 0.0).toDouble(),
                        src.value(ClipKey::lensCx, 0.5).toDouble(),
                        src.value(ClipKey::lensCy, 0.5).toDouble()) || any;
    if (has("fades"))
        any = setFadesOn(track, index, src.value(ClipKey::fadeIn, 0).toInt(),
                         src.value(ClipKey::fadeOut, 0).toInt()) || any;
    if(!audio){
        auto target=entryMapOn(track,index);auto keys=target.value(ClipKey::editKeys).toMap();const auto sourceKeys=src.value(ClipKey::editKeys).toMap();
        for(const auto &group:QStringList{"rect","crop","color"})if(has(group=="rect"?"transform":group.toUtf8().constData())){
            for(const auto &key:EditEngine::parameters(group)){keys.remove(key);if(sourceKeys.contains(key))keys[key]=sourceKeys[key];}
        }
        target[ClipKey::editKeys]=keys;
        if(src.contains(ClipKey::effectOrder))target[ClipKey::effectOrder]=src[ClipKey::effectOrder];
        applyEditExtras(track,index,ClipEntry::read(target));
    }
    // Last: a retime rebuilds the clip (and shifts what follows).
    if (speedGroup) {
        if(pattern || reset) {
            QString failure;const bool applied=setSpeedRampOn(track,index,timing,&failure,reset);
            if(!failure.isEmpty()){rebuild(before,entries());return refuse(failure);}
            any=applied||any;
        }
        const double speed = src.value(ClipKey::speed, 1.0).toDouble();
        if (!pattern && !qFuzzyCompare(speed, entryMapOn(track,index).value(ClipKey::speed, 1.0).toDouble())) {
            if(!setClipSpeedOn(track,index,speed)) {
                rebuild(before,entries());return refuse(QCoreApplication::translate("SpeedRamp","The speed attributes could not be applied. The previous clip was preserved."));
            }
            any=true;
        }
        const int blend = src.value(ClipKey::frameBlend, 0).toInt();
        // The retime may have rebuilt the clip and changed sampling.
        const int now = clipIndexContaining(track, me.value(ClipKey::start).toInt());
        const int target=now>=0?now:index;
        if (blend != entryMapOn(track,target).value(ClipKey::frameBlend,0).toInt()) {
            any = setFrameBlendAt(track,target,blend) || any;
        }
    }
    return any;
}

// Clip index of the clip on `track` whose span contains `position`.
int Timeline::clipIndexContaining(int track, int position) const {
    for (const QVariant &v : entries()) {
        const QVariantMap map = v.toMap();
        if (map.value(ClipKey::track).toInt() != track
            || map.value(ClipKey::gap).toBool())
            continue;
        if(map.contains(ClipKey::audioTiming)) {
            if(m_profile && AudioTimeline::contains(map,{qint64(position)*m_profile->frame_rate_den(),m_profile->frame_rate_num()}))
                return map.value(ClipKey::index).toInt();
            continue;
        }
        const int start = map.value(ClipKey::start).toInt();
        if (position >= start && position < start + map.value(ClipKey::frames).toInt())
            return map.value(ClipKey::index).toInt();
    }
    return -1;
}

// A playlist split copies only meta.* properties to the right half — its
// cut filters (volume, colour, crop, markers, keyframes, …) stay on the
// left. Re-apply the entry's attributes to the new half, partitioning what
// is positional: markers by source frame, keyframes at the cut, fades to
// their own ends.
void Timeline::decorateSplit(int track, int index, const QVariantMap &entry, int relative) {
    QVariantMap right = entry;
    right.remove(ClipKey::link);
    right.remove(ClipKey::fadeIn);
    const int splitFrame = entry.value(ClipKey::in).toInt()
                           + entry.value(ClipKey::mixIn).toInt() + relative;
    QVariantList leftMarkers, rightMarkers;
    for (const QVariant &v : entry.value(ClipKey::markers).toList())
        (v.toMap().value(QStringLiteral("frame")).toInt() < splitFrame ? leftMarkers
                                                                        : rightMarkers).append(v);
    right.insert(ClipKey::markers, rightMarkers);
    QVariantList vl, vr, ol, orr;
    splitKeys(entry.value(ClipKey::volumeKf).toList(), "db", relative, &vl, &vr);
    splitKeys(entry.value(ClipKey::opacityKf).toList(), "alpha", relative, &ol, &orr);
    right.insert(ClipKey::volumeKf, vr);
    right.insert(ClipKey::opacityKf, orr);
    right[ClipKey::editKeys]=EditEngine::shiftedKeys(entry.value(ClipKey::editKeys).toMap(),relative,entry.value(ClipKey::frames).toInt()-relative);
    // Effects that change as the clip plays carry on where they were: each
    // keeps the frames it had run on the left half.
    QVariantList effects=EditEngine::shiftedEffects(entry.value(ClipKey::videoEffects).toList(),relative);
    for(QVariant &v:effects){auto state=v.toMap();state["clock"]=state.value("clock").toInt()+relative;v=state;}
    if(!effects.isEmpty())right[ClipKey::videoEffects]=effects;
    applyEntryAttributes(track, index + 1, right);
    // A dissolve after the clip is now after its right half: the marker that
    // says how it is styled and aligned (kept on the clip before a dissolve)
    // moves there with it.
    if (entry.value(ClipKey::mixOut).toInt() > 0) {
        const std::unique_ptr<edit::ClipInfo> from = clipInfoAt(track, index), to = clipInfoAt(track, index + 1);
        if (from && to) {
            for (int i = from->cut->filter_count() - 1; i >= 0; --i) {
                std::unique_ptr<edit::Filter> f(from->cut->filter(i));
                if (f && f->is_valid() && (f->get("omavid:mixStyle") || f->get("omavid:mixAlign"))) {
                    from->cut->detach(*f); // the wrapper keeps it alive
                    to->cut->attach(*f);
                }
            }
        }
    }
    // The left half: markers past the cut go, a tail fade no longer ends
    // a clip, keys re-anchor to the shorter cut.
    if (!entry.value(ClipKey::markers).toList().isEmpty())
        setClipMarkersAt(track, index, leftMarkers);
    const int fadeIn = entry.value(ClipKey::fadeIn).toInt();
    if (fadeIn > 0 || entry.value(ClipKey::fadeOut).toInt() > 0)
        setFadesOn(track, index, fadeIn, 0);
    if (!entry.value(ClipKey::volumeKf).toList().isEmpty())
        setVolumeKfAt(track, index, vl);
    if (!entry.value(ClipKey::opacityKf).toList().isEmpty())
        setOpacityKfAt(track, index, ol);
    applyEditExtras(track,index,ClipEntry::read(entry));
}

QVariantMap Timeline::entryForPaste(const QVariantMap &entry,double originSeconds,QString *error) {
    const QString path=entry.value(ClipKey::path).toString();
    const auto profilePath=path.isEmpty() || !entry.value(ClipKey::title).toString().isEmpty()?QStringLiteral("color:black"):path;
    if(!primary() && !lockProfileTo(profilePath))return {};
    const auto metadata=m_mediaSources.value(path,entry.value(ClipKey::sourceMeta)).toMap();
    auto timed=entry;
    if(TimelineRate::sourceRate(timed)==0 && !timed.contains(ClipKey::timelineFps))timed[ClipKey::timelineFps]=fps();
    auto converted=TimelineRate::convert(timed,fps(),originSeconds,error,interpretationFactor(metadata));
    if(converted.isEmpty())return {};
    // Future copies describe the clock actually used by this destination.
    if(!metadata.isEmpty()) {
        converted[ClipKey::sourceMeta]=metadata;
        if(!m_mediaSources.contains(path))m_mediaSources[path]=metadata;
    }
    return converted;
}

bool Timeline::pasteClip(int track, int position, const QVariantMap &input,
                            bool insert) {
    const double rate=TimelineRate::sourceRate(input);
    const auto entry=entryForPaste(input,rate>0?input.value(ClipKey::start).toDouble()/rate:0.);
    if(entry.isEmpty())return false;
    QScopedValueRollback<int> sampling(m_restoreSampling,entry.value(ClipKey::frameBlend).toInt());
    std::optional<double> origin;if(!SourceTiming::readOrigin(entry,origin))return false;
    QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
    if (track < 0 || track > trackCount() || position < 0)
        return false;
    if (!entry.value(ClipKey::compoundJson).toString().isEmpty())
        return false; // v1 limit: a compound has no path to reopen
    const QString title = entry.value(ClipKey::title).toString();
    const int frames = entry.value(ClipKey::frames).toInt();
    bool ok = false;
    if (!title.isEmpty()) {
        if (track < 1 || frames < 1)
            return false; // titles live on the tracks above V1
        ok = connectTitleAt(title, position, frames, titleStyleOf(entry), track);
    } else {
        const QString path = entry.value(ClipKey::path).toString();
        if (path.isEmpty() || entry.value(ClipKey::gap).toBool())
            return false;
        const bool audioOnly = entry.value(ClipKey::audioOnly).toBool();
        const bool videoOnly = entry.value(ClipKey::videoOnly).toBool();
        const double speed = entry.value(ClipKey::speed, 1.0).toDouble();
        const int in = entry.value(ClipKey::in, -1).toInt();
        const int out = entry.value(ClipKey::out, -1).toInt();
        if(track==0 && position>primaryFrameCount()) {
            // Clipboard placement retains absolute spacing, including leading
            // and inter-clip gaps. Ordinary insert/overwrite may clamp to V1's
            // end; doing that here also loses the address used for attributes.
            if(audioOnly || trackLocked(0))return false;
            auto producer=makeClipProducer(path,-1,-1,speed,false,videoOnly,entry.value(ClipKey::retime).toMap());
            if(!producer.is_valid())return false;
            const auto cut=boundedCut(producer,in,out);
            if(!cut || cut->get_playtime()<1)return false;
            ok=primary()->insert_at(position,*cut,1)>=0;
            if(ok && insert)rippleOthersFrom(0,position,cut->get_playtime());
        } else ok = insert ? insertOn(track, path, position, in, out, audioOnly,
                               speed, videoOnly, entry.value(ClipKey::retime).toMap())
                    : overwriteOn(track, path, position, in, out, audioOnly,
                                  speed, videoOnly, entry.value(ClipKey::retime).toMap());
    }
    if (!ok)
        return false;
    // A title auto-stacks when its span is taken — find it wherever it
    // landed; media lands exactly where asked.
    int at = track, index = clipIndexAt(track, position);
    if (index < 0 && !title.isEmpty()) {
        for (int t = 1; t < trackCount() && index < 0; ++t) {
            index = clipIndexAt(t, position);
            at = t;
        }
    }
    if (index < 0)
        return true; // placed; nothing to decorate
    applyEntryAttributes(at, index, entry);
    return true;
}

bool Timeline::nudgeAt(int track, int index, int delta) {
    if (delta == 0 || track < 0 || track >= trackCount() || trackLocked(track))
        return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    const int start = info->start;
    const int span = info->frame_count;
    info.reset();
    if (start + delta < 0)
        return false;
    if (track >= 1) {
        // Off V1 a nudge moves the clip alone, into free space only: a
        // move would overwrite the neighbour it runs into. A transition
        // holds the clip to its neighbour on that side: there is none.
        if (const int entry = entryIndexOn(track, index); entry >= 0) {
            const edit::Playlist *lane = playlistOn(track);
            if (delta > 0 ? entry + 1 < lane->count() && lane->is_mix(entry + 1) : entry > 0 && lane->is_mix(entry - 1))
                return false;
        }
        if (delta < 0) {
            if (std::unique_ptr<edit::ClipInfo> prev = index > 0 ? clipInfoAt(track, index - 1) : nullptr;
                prev && start + delta < prev->start + prev->frame_count)
                return false;
        } else if (std::unique_ptr<edit::ClipInfo> next = clipInfoAt(track, index + 1);
                   next && start + delta + span > next->start) {
            return false;
        }
        return moveTo(track, index, track, start + delta);
    }
    const int entry = entryIndexFor(index);
    if (entry < 0)
        return false;
    // Dissolved edges hold their seam — never wedge space into a mix.
    if (entry > 0 && primary()->is_mix(entry - 1))
        return false;
    if (delta > 0) {
        primary()->insert_blank(entry, delta - 1);
    } else {
        // Only empty space before the clip can be eaten.
        if (entry == 0 || !primary()->is_blank(entry - 1))
            return false;
        const int blank = primary()->clip_length(entry - 1);
        if (blank < -delta)
            return false;
        if (blank == -delta)
            primary()->remove(entry - 1);
        else if (primary()->resize_clip(entry - 1, 0, blank + delta - 1) != 0)
            return false;
    }
    tidyPrimary();
    // V1 got longer or shorter at the clip's old start — the auto-selected
    // other tracks ride it from there.
    rippleOthersFrom(0, start, delta);
    return true;
}

bool Timeline::moveTo(int track, int index, int newTrack, int newStart) {
    if (track < 0 || newTrack < 0 || newTrack > trackCount() || newStart < 0)
        return false;
    int entry = entryIndexOn(track, index);
    if (entry < 0)
        return false;
    if (newTrack < trackCount() && trackLocked(newTrack))
        return false;
    edit::Playlist *source = playlistOn(track);
    std::unique_ptr<edit::ClipInfo> info(source->clip_info(entry));
    if (!info || !info->producer)
        return false;
    const bool audioOnly = info->producer->get_int("omavid:audioOnly") != 0;
    int span = info->frame_count;
    const int sourceStart = info->start;
    info.reset();
    if (newTrack < trackCount() && newTrack != track
        && !trackTakes(newTrack, audioOnly))
        return false;
    {
        // The clip's own dissolves go with it, wherever it is going (a track
        // that does not exist yet too), and any it lands across.
        int broken = breakLaneDissolves(track, sourceStart, sourceStart + span);
        if (broken > 0) { // the rebuild replaced every playlist
            source = playlistOn(track);
            entry = entryIndexOn(track, index);
            if (entry < 0)
                return false;
            // A transition made of handles gave the clip its own first
            // frames back, and it starts at the cut: it goes from there, as
            // far as it was asked to.
            newStart = qMax(0, newStart + source->clip_start(entry) - sourceStart);
            span = source->clip_length(entry);
        }
        if (broken >= 0 && newTrack < trackCount()) {
            const int landed = breakLaneDissolves(newTrack, newStart, newStart + span);
            if (landed > 0) {
                source = playlistOn(track);
                entry = entryIndexOn(track, index);
                if (entry < 0)
                    return false;
            }
            broken = landed < 0 ? -1 : broken + landed;
        }
        if (broken < 0)
            return false;
    }
    std::unique_ptr<edit::Producer> cut(source->replace_with_blank(entry));
    if (!cut || !cut->is_valid())
        return false;
    source->consolidate_blanks(0); // the hole it left merges with its neighbours
    edit::Playlist *dest = newTrack == 0 ? primary() : ensureLane(newTrack - 1);
    if (newTrack > 0)
        m_tracks[size_t(newTrack)].kind = audioOnly ? Track::Audio : Track::Video;
    keepCarvedClips(newTrack, newStart, newStart + span);
    const bool ok = laneOverwrite(dest, newStart, *cut);
    tidyPrimary();
    tidyConnected();
    return ok;
}

// A group of lane clips at once: every clip leaves its track FIRST, then
// each lands — so a clip never overwrites a group member that has not
// moved yet (two rows swapping, or a later start landing on an earlier
// one's tail). `newTrack == trackCount()` is one new track for the batch.
bool Timeline::moveClips(const QList<ClipMove> &asked) {
    struct Lifted { std::unique_ptr<edit::Producer> cut; bool audioOnly; int newTrack; int newStart; };
    std::vector<Lifted> lifted;
    QSet<int> sourceLanes;
    for (const ClipMove &mv : asked) {
        if (mv.track < 0 || mv.newTrack < 0 || mv.newTrack > trackCount() || mv.newStart < 0)
            return false;
        if (trackLocked(mv.track) || (mv.newTrack < trackCount() && trackLocked(mv.newTrack)))
            return false;
    }
    QList<ClipMove> moves = asked;
    for (ClipMove &mv : moves) {
        std::unique_ptr<edit::ClipInfo> info = clipInfoAt(mv.track, mv.index);
        int span = info ? info->frame_count : 0;
        const int start = info ? info->start : 0;
        info.reset(); // nothing may outlive the rebuild a removed dissolve causes
        // Its own dissolves go whatever the destination, a new track too.
        const int broken = span > 0 ? breakLaneDissolves(mv.track, start, start + span) : 0;
        if (broken < 0)
            return false;
        if (broken > 0) {
            // A transition made of handles gave the clip its own first
            // frames back: it goes from the cut, as far as it was asked to.
            info = clipInfoAt(mv.track, mv.index);
            if (info) {
                mv.newStart = qMax(0, mv.newStart + info->start - start);
                span = info->frame_count;
            }
            info.reset();
        }
        if (span > 0 && mv.newTrack < trackCount()
            && breakLaneDissolves(mv.newTrack, mv.newStart, mv.newStart + span) < 0)
            return false;
    }
    // Resolve every playlist entry BEFORE lifting any: a lift turns a clip
    // into a blank, which renumbers the clip indices after it (entry
    // indices hold — replace_with_blank swaps in place).
    struct Resolved { int track; int entry; bool audioOnly; int newTrack; int newStart; };
    std::vector<Resolved> resolved;
    for (const ClipMove &mv : moves) {
        const int entry = entryIndexOn(mv.track, mv.index);
        if (entry < 0)
            continue;
        std::unique_ptr<edit::ClipInfo> info(playlistOn(mv.track)->clip_info(entry));
        if (!info || !info->producer)
            continue;
        const bool audioOnly = info->producer->get_int("omavid:audioOnly") != 0;
        if (mv.newTrack < trackCount() && mv.newTrack != mv.track
            && !trackTakes(mv.newTrack, audioOnly))
            continue;
        resolved.push_back({mv.track, entry, audioOnly, mv.newTrack, mv.newStart});
    }
    for (const Resolved &r : resolved) {
        std::unique_ptr<edit::Producer> cut(playlistOn(r.track)->replace_with_blank(r.entry));
        if (!cut || !cut->is_valid())
            continue;
        sourceLanes.insert(r.track);
        lifted.push_back({std::move(cut), r.audioOnly, r.newTrack, r.newStart});
    }
    if (lifted.empty())
        return false;
    for (int track : sourceLanes)
        playlistOn(track)->consolidate_blanks(0);
    const int countBefore = trackCount();
    // "New track" makes one track per kind: picture and sound never share.
    int created[2] = {-1, -1}; // [video, audio]
    int next = countBefore;
    bool ok = true;
    for (Lifted &l : lifted) {
        int target = l.newTrack;
        const bool fresh = target >= countBefore;
        if (fresh) {
            int &slot = created[l.audioOnly ? 1 : 0];
            if (slot < 0)
                slot = next++;
            target = slot;
        }
        edit::Playlist *dest = target == 0 ? primary() : ensureLane(target - 1);
        if (fresh)
            m_tracks[size_t(target)].kind = l.audioOnly ? Track::Audio : Track::Video;
        else if (!trackTakes(target, l.audioOnly)) {
            ok = false;
            continue;
        }
        keepCarvedClips(target, l.newStart, l.newStart + l.cut->get_playtime());
        ok = laneOverwrite(dest, l.newStart, *l.cut) && ok;
    }
    tidyPrimary();
    tidyConnected();
    return ok;
}

bool Timeline::trimOn(int track, int index, int in, int out, int newStart,
                         bool ripple) {
    if (track == 0)
        return trim(index, in, out); // V1 trims always ripple
    // The edge a transition sits on ripples with the transition kept across it.
    if (ripple)
        if (const std::optional<bool> under = trimUnderMixOn(track, index, in, out))
            return *under;
    if (const std::optional<bool> held = trimLaneClipInTransition(track, index, in, out, newStart, ripple))
        return *held;
    int entry = entryIndexOn(track, index);
    if (entry < 0)
        return false;
    edit::Playlist *pl = laneAt(track - 1);
    const int oldStart = pl->clip_start(entry);
    const int oldFrames = pl->clip_length(entry);
    if (!ripple)
        return trimLaneEntry(track - 1, entry, in, out, newStart);
    // Ripple trim: the clip's start holds; downstream on this track and the
    // auto-selected others rides the delta. Make room BEFORE growing (a
    // colliding re-place would slide the clip instead of overlapping).
    const int delta = (out - in + 1) - oldFrames;
    const int oldEnd = oldStart + oldFrames;
    if (delta > 0) {
        rippleFrom(track, oldEnd, delta, true);
        entry = entryIndexOn(track, index);
        if (entry < 0 || !trimLaneEntry(track - 1, entry, in, out, oldStart))
            return false;
    } else {
        if (!trimLaneEntry(track - 1, entry, in, out, oldStart))
            return false;
        rippleFrom(track, oldEnd, delta, true);
    }
    return true;
}

bool Timeline::splitAt(int position) {
    if (trackLocked(0))
        return false;
    if (!primary())
        return false;

    const int index = primary()->get_clip_index_at(position);
    if (index < 0 || index >= primary()->count())
        return false;
    if (primary()->is_mix(index) || primary()->is_blank(index))
        return false; // no blade inside a dissolve, nothing to blade in a gap

    edit::ClipInfo *info = primary()->clip_info(index);
    if (!info)
        return false;
    const int relative = position - info->start;
    const int length = info->frame_count;
    delete info;

    // A blade right on a clip boundary would make a zero-length clip.
    if (relative < 1 || relative >= length)
        return false;

    const int clip = clipIndexContaining(0, position);
    const QVariantMap map = clip >= 0 ? entryMapOn(0, clip) : QVariantMap();
    // The split point is the last frame of the left part.
    if (primary()->split(index, relative - 1) != 0)
        return false;
    if (clip >= 0)
        decorateSplit(0, clip, map, relative);
    // A blade on V1 blades the linked audio at the same frame; the right
    // halves share a fresh link.
    const QString link = map.value(ClipKey::link).toString();
    if (clip >= 0 && !link.isEmpty() && !m_linkingSplit) {
        bool soundless = !fixedSoundOf(map).isEmpty();
        for (const QVariant &e : entries()) {
            const QVariantMap a = e.toMap();
            if (!a.value(ClipKey::audioOnly).toBool()
                || a.value(ClipKey::link).toString() != link)
                continue;
            soundless = false;
            const int aStart = a.value(ClipKey::start).toInt();
            if (position <= aStart || position >= aStart + a.value(ClipKey::frames).toInt())
                break;
            const int track = a.value(ClipKey::track).toInt();
            const int aIndex = a.value(ClipKey::index).toInt();
            m_linkingSplit = true;
            const bool ok = splitOn(track, position);
            m_linkingSplit = false;
            if (ok) {
                const QString fresh = newLinkId();
                setLinkAt(0, clip + 1, fresh);
                setLinkAt(track, aIndex + 1, fresh);
            }
            break;
        }
        // A multicam clip with a sound source of its own, linked to a sound
        // that is not there: the editor removed it. A picture of such a clip
        // with no link is given the source's sound when the edit settles,
        // so the part after the blade says what the picture said: it has a
        // link of its own, to no sound.
        if (soundless)
            setLinkAt(0, clip + 1, newLinkId());
    }
    return true;
}

// Make sure a clip boundary exists at `position`, blading if needed. A
// position at 0, at the end, or already on a boundary needs no cut.
bool Timeline::ensureBoundaryAt(int position) {
    if (position <= 0 || position >= primaryFrameCount())
        return true;
    const int index = primary()->get_clip_index_at(position);
    edit::ClipInfo *info = primary()->clip_info(index);
    if (!info)
        return false;
    const bool onBoundary = info->start == position;
    const int relative = position - info->start;
    delete info;
    if (onBoundary)
        return true;
    if (primary()->is_blank(index)) { // empty space splits freely
        return primary()->split(index, relative - 1) == 0;
    }
    return splitAt(position);
}

bool Timeline::insertAt(const QString &path, int position, int in, int out,
                           double speed, bool videoOnly, const QVariantMap &retime) {
    // A clip put in at the cut a handle-made transition sits across, or on
    // the frames it plays on, parts the pair: the transition goes first.
    QList<int> released;
    for (const HandleMix &mix : handleMixesOn(0))
        if (position >= mix.from && position <= mix.to)
            released.append(mix.index);
    return withMixesReleased(0, released, int(released.size()),
                             [&] { return insertAtPlain(path, position, in, out, speed, videoOnly, retime); });
}

bool Timeline::insertAtPlain(const QString &path, int position, int in, int out,
                                double speed, bool videoOnly, const QVariantMap &retime) {
    if (trackLocked(0))
        return false;
    if (!primary() || clipCount() == 0)
        return append(path, in, out, speed, false, videoOnly, retime);

    // The playhead can sit past the storyline's end when the connected lane
    // extends the timeline — storyline edits clamp to the storyline.
    position = qBound(0, position, primaryFrameCount());
    if (position == primaryFrameCount())
        return append(path, in, out, speed, false, videoOnly, retime);

    // Open the producer before any cut — a bad file must not leave a blade in.
    edit::Producer producer =
        makeClipProducer(path, -1, -1, speed, false, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;
    if (in >= 0 && out >= 0 && out < in)
        return false; // bounds the wrong way round are no clip

    if (!ensureBoundaryAt(position))
        return false;
    int entry = primary()->get_clip_index_at(position);
    // Never wedge a clip into a dissolve's mix region or between a mix and
    // its clip — clear the dissolve first.
    if (primary()->is_mix(entry)
        || (entry > 0 && primary()->is_mix(entry - 1)))
        return false;
    // A lane dissolve the ripple would tear goes first (it rebuilds, and can
    // replace the profile the producer was opened against).
    const bool reopen = hasLaneDissolve();
    if (reopen)
        producer = edit::Producer();
    if (breakRippledLaneDissolves(0, position) < 0)
        return false;
    if (reopen && !(producer = makeClipProducer(path, -1, -1, speed, false, videoOnly, retime)).is_valid())
        return false;
    entry = primary()->get_clip_index_at(position);
    const int primaryBefore = primaryFrameCount();
    if (primary()->insert(producer, entry, in, out) != 0)
        return false;
    // Sync-lock: track clips at/after the insertion point ride the ripple,
    // by the frames that landed: a clip asked for past its source's end is
    // shorter than the bounds say.
    rippleOthersFrom(0, position, primaryFrameCount() - primaryBefore);
    return true;
}

bool Timeline::overwriteAt(const QString &path, int position, int in, int out,
                              double speed, bool videoOnly, const QVariantMap &retime) {
    // A clip laid across, or right up against, the frames a handle-made
    // transition plays on: the transition goes first.
    QList<int> released;
    if (const QList<HandleMix> mixes = handleMixesOn(0); !mixes.isEmpty() && position >= 0 && position < primaryFrameCount()) {
        int span = in >= 0 && out >= in ? out - in + 1 : 0;
        {
            // As long as the clip will be: it stops at its source's last frame.
            edit::Producer producer = makeClipProducer(path, -1, -1, speed, false, videoOnly, retime);
            if (producer.is_valid())
                span = in >= 0 ? qMin(out, producer.get_length() - 1) - in + 1 : producer.get_playtime();
        }
        for (const HandleMix &mix : mixes)
            if (span > 0 && mix.from <= position + span && mix.to >= position)
                released.append(mix.index);
    }
    return withMixesReleased(0, released, int(released.size()),
                             [&] { return overwriteAtPlain(path, position, in, out, speed, videoOnly, retime); });
}

bool Timeline::overwriteAtPlain(const QString &path, int position, int in, int out,
                                   double speed, bool videoOnly, const QVariantMap &retime) {
    if (trackLocked(0))
        return false;
    if (!primary() || clipCount() == 0)
        return append(path, in, out, speed, false, videoOnly, retime);

    position = qBound(0, position, primaryFrameCount());
    if (position >= primaryFrameCount())
        return append(path, in, out, speed, false, videoOnly, retime);

    edit::Producer producer =
        makeClipProducer(path, -1, -1, speed, false, videoOnly, retime);
    if (!producer.is_valid())
        return false;
    if (!retime.isEmpty() && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return false;

    // Carve out exactly the span the new clip will occupy, then drop it in.
    // The clip stops at its source's last frame whatever the bounds ask for,
    // so the span is counted to there: V1 gets back what it gives up.
    if (in >= 0) {
        if (in >= producer.get_length())
            return false;
        out = qMin(out, producer.get_length() - 1);
    }
    const int span = in >= 0 ? out - in + 1 : producer.get_playtime();
    if (span < 1)
        return false;
    const int end = position + span;

    // Overwriting across (or right up against) a dissolve would orphan or
    // corrupt its mix region — clear the dissolve first.
    for (int i = 0; i < primary()->count(); ++i) {
        if (!primary()->is_mix(i))
            continue;
        const int mixStart = primary()->clip_start(i);
        const int mixEnd = mixStart + primary()->clip_length(i);
        if (mixStart <= end && mixEnd >= position)
            return false;
    }

    if (!ensureBoundaryAt(position) || !ensureBoundaryAt(end))
        return false;

    const int index = primary()->get_clip_index_at(position);
    int covered = 0;
    for (int i = index; i < primary()->count(); ++i) {
        edit::ClipInfo *info = primary()->clip_info(i);
        if (!info)
            break;
        const bool inside = info->start >= position && info->start < end;
        delete info;
        if (!inside)
            break;
        ++covered;
    }
    for (int i = 0; i < covered; ++i)
        primary()->remove(index);

    return (index >= primary()->count()
                ? primary()->append(producer, in, out)
                : primary()->insert(producer, index, in, out)) == 0;
}

bool Timeline::trim(int index, int in, int out) {
    const int entry = entryIndexFor(index);
    if (entry < 0)
        return false;
    // The edge a transition sits on ripples with the transition kept across it.
    if (const std::optional<bool> under = trimUnderMixOn(0, index, in, out))
        return *under;

    edit::ClipInfo *info = primary()->clip_info(entry);
    if (!info)
        return false;
    const int length = info->length;
    const int rawIn = info->frame_in;
    const int rawOut = info->frame_out;
    delete info;

    // entries() reports restored (pre-dissolve) bounds, so compare against
    // those — and refuse a trim on an edge a dissolve is riding.
    const bool mixBefore = entry > 0 && primary()->is_mix(entry - 1);
    const bool mixAfter = entry + 1 < primary()->count()
                          && primary()->is_mix(entry + 1);
    const int mixInLen = mixBefore ? primary()->clip_length(entry - 1) : 0;
    const int mixOutLen = mixAfter ? primary()->clip_length(entry + 1) : 0;
    if ((in != rawIn - mixInLen && mixBefore)
        || (out != rawOut + mixOutLen && mixAfter))
        return false;
    const int newIn = mixBefore ? rawIn : in;
    const int newOut = mixAfter ? rawOut : out;

    // At least one frame, and never outside the source material. Everything
    // after the clip ripples with the resize — playlist starts are derived.
    if (newIn < 0 || newOut < newIn || newOut >= length)
        return false;

    const QVariantMap animatedEntry=entryMapOn(0,index);
    // Fades anchor to the old edges; remember them so they follow the trim.
    // Volume keyframes are edge-anchored the same way — capture and shift.
    int fadeIn = 0, fadeOut = 0;
    QVariantList volumeKf;
    if (edit::ClipInfo *fadeInfo = primary()->clip_info(entry)) {
        fadeLengths(fadeInfo->cut, rawIn, rawOut, &fadeIn, &fadeOut);
        volumeKf = volumeKfOf(fadeInfo->cut);
        delete fadeInfo;
    }

    const QVariantList before = entries();
    const int primaryBefore = primaryFrameCount();
    if (primary()->resize_clip(entry, newIn, newOut) != 0)
        return false;
    if (fadeIn > 0 || fadeOut > 0 || !volumeKf.isEmpty()) {
        edit::ClipInfo *after = primary()->clip_info(entry);
        if (after && after->cut) {
            if (fadeIn > 0 || fadeOut > 0)
                applyFades(after->cut, after->frame_in, after->frame_out,
                           qMin(fadeIn, after->frame_count),
                           qMin(fadeOut, after->frame_count));
            if (!volumeKf.isEmpty()) {
                // Keyframes are clip-relative: an in-trim shifts the
                // content under them, so shift the keys to match.
                QVariantList shifted;
                for (const QVariant &v : std::as_const(volumeKf)) {
                    QVariantMap key = v.toMap();
                    key.insert(QStringLiteral("frame"),
                               key.value(QStringLiteral("frame")).toInt()
                                   + (rawIn - newIn));
                    shifted.append(key);
                }
                applyVolumeKf(*m_profile, after->cut, shifted,
                              after->frame_in, after->frame_out);
            }
        }
        delete after;
    }
    if (!animatedEntry.value(ClipKey::opacityKf).toList().isEmpty())
        setOpacityKfAt(0, index, EditEngine::shiftedCurve(animatedEntry.value(ClipKey::opacityKf).toList(), newIn-rawIn));
    if(!animatedEntry.value(ClipKey::editKeys).toMap().isEmpty()){
        auto shifted=animatedEntry;shifted[ClipKey::editKeys]=EditEngine::shiftedKeys(animatedEntry.value(ClipKey::editKeys).toMap(),newIn-rawIn,newOut-newIn+1);shifted[ClipKey::opacityKf]=EditEngine::shiftedCurve(animatedEntry.value(ClipKey::opacityKf).toList(),newIn-rawIn);applyEditExtras(0,index,ClipEntry::read(shifted));
    }
    if(render::effectsAnimated(animatedEntry.value(ClipKey::videoEffects).toList()))
        setVideoEffectsAt(0,index,EditEngine::shiftedEffects(animatedEntry.value(ClipKey::videoEffects).toList(),newIn-rawIn));
    // Downstream ripples from the clip's OLD end; track clips over the
    // clip hold position (sync-lock).
    rippleOthersFrom(0, storylineEndIn(before, index),
                       primaryFrameCount() - primaryBefore);
    return true;
}

bool Timeline::trimNormal(int index, int in, int out) {
    if (trackLocked(0) || entryIndexFor(index) < 0)
        return false;
    const QVariantList before = entries();
    QVariantList clips = before;
    int clip = 0, at = -1, nextAt = -1;
    for (int i = 0; i < clips.size(); ++i) {
        if (clips.at(i).toMap().value(ClipKey::lane).toInt() >= 1)
            continue;
        if (clip == index)
            at = i;
        else if (clip == index + 1)
            nextAt = i;
        ++clip;
    }
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    const int oldIn = map.value(ClipKey::in).toInt();
    const int oldOut = map.value(ClipKey::out).toInt();
    const int length = map.value(ClipKey::length).toInt();
    if ((in != oldIn && map.value(ClipKey::mixIn).toInt() > 0)
        || (out != oldOut && map.value(ClipKey::mixOut).toInt() > 0))
        return false; // a dissolve owns that edge
    // The in edge moves the START: empty space before the clip grows or
    // shrinks by the same amount, never below zero (the previous clip).
    const int gapBefore = map.value(ClipKey::gapBefore).toInt();
    in = qMax(in, oldIn - gapBefore);
    in = qBound(0, in, length - 1);
    // The out edge moves the END into the space after it, never past the
    // next clip's start.
    QVariantMap next = nextAt >= 0 ? clips.at(nextAt).toMap() : QVariantMap();
    if (nextAt >= 0)
        out = qMin(out, oldOut + next.value(ClipKey::gapBefore).toInt());
    out = qBound(in, out, length - 1);
    if (in == oldIn && out == oldOut)
        return false;
    map.insert(ClipKey::in, in);
    map.insert(ClipKey::out, out);
    map.insert(ClipKey::gapBefore, gapBefore + (in - oldIn));
    map[ClipKey::editKeys]=EditEngine::shiftedKeys(map.value(ClipKey::editKeys).toMap(),in-oldIn,out-in+1);
    if(map.contains(ClipKey::videoEffects))map[ClipKey::videoEffects]=EditEngine::shiftedEffects(map.value(ClipKey::videoEffects).toList(),in-oldIn);
    // Clip-relative keyframes ride the content, not the edge.
    for (const char *key : {"volumeKf", "opacityKf"}) {
        QVariantList shifted;
        for (const QVariant &v : map.value(QLatin1String(key)).toList()) {
            QVariantMap k = v.toMap();
            k.insert(QStringLiteral("frame"),
                     k.value(QStringLiteral("frame")).toInt() + (oldIn - in));
            shifted.append(k);
        }
        if (!shifted.isEmpty())
            map.insert(QLatin1String(key), shifted);
    }
    clips[at] = map;
    if (nextAt >= 0) {
        next.insert(ClipKey::gapBefore,
                    next.value(ClipKey::gapBefore).toInt() - (out - oldOut));
        clips[nextAt] = next;
    }
    return rebuild(clips, before);
}

bool Timeline::move(int from, int to) {
    if (!primary() || from == to || from < 0 || to < 0 || from >= clipCount() ||
        to >= clipCount())
        return false;
    // A transition made of handles whose pair the new order parts goes first.
    QList<int> order, parted;
    const QList<HandleMix> mixes = handleMixesOn(0);
    if (!mixes.isEmpty()) {
        for (int i = 0; i < trackClipCount(0); ++i)
            order.append(i);
        if (from < order.size() && to < order.size())
            order.insert(to, order.takeAt(from));
        for (const HandleMix &mix : mixes) {
            const int at = order.indexOf(mix.index);
            if (at < 0 || at + 1 >= order.size() || order.at(at + 1) != mix.index + 1)
                parted.append(mix.index);
        }
    }
    return withMixesReleased(0, parted, 0, [&] { return movePlain(from, to); });
}

bool Timeline::movePlain(int from, int to) {
    if (!primary() || from == to || from < 0 || to < 0 || from >= clipCount() ||
        to >= clipCount())
        return false;
    if (trackLocked(0)) // the no-mix path refuses through entryIndexFor; the rebuild path must too
        return false;
    // A reorder keeps the storyline's length: the tracks above hold still
    // (sync-lock), whatever now plays beneath them.
    const QVariantList before = entries();
    if (!hasMixes()) { // playlist move speaks entry indices — blanks count
        const int entryFrom = entryIndexFor(from);
        const int entryTo = entryIndexFor(to);
        if (entryFrom < 0 || entryTo < 0)
            return false;
        const bool ok = primary()->move(entryFrom, entryTo) == 0;
        tidyPrimary();
        return ok;
    }

    // With dissolves in play, rebuild from a reordered snapshot; a dissolve
    // survives only where the pair it sat between is still adjacent.
    const QVariantList all = entries();
    QVariantList lane0, lane1;
    for (const QVariant &v : all) {
        QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1) {
            lane1.append(map);
        } else {
            map.insert(QStringLiteral("origIndex"), lane0.size());
            lane0.append(map);
        }
    }
    lane0.insert(to, lane0.takeAt(from));
    for (int i = 0; i < lane0.size(); ++i) {
        QVariantMap map = lane0.at(i).toMap();
        if (map.value(ClipKey::mixOut).toInt() < 1)
            continue;
        const bool sameNeighbour =
            i + 1 < lane0.size()
            && lane0.at(i + 1).toMap().value(QStringLiteral("origIndex")).toInt()
                   == map.value(QStringLiteral("origIndex")).toInt() + 1;
        if (!sameNeighbour) {
            map.insert(ClipKey::mixOut, 0);
            lane0[i] = map;
        }
    }
    return rebuild(lane0 + lane1, before);
}
