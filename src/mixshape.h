#pragma once
#include "clipentry.h"
#include <QHash>
#include <QList>
#include <QPair>

// Where a transition sits on its cut, read from the clip entries.
//
// The entries lay a transition out by its END: clip A's span runs to the
// transition's last frame and clip B's starts after it. When the transition
// is made of the clips' handles the cut itself is earlier: extA frames of the
// transition follow it (A's out extended) and the rest come before it (B's in
// pulled back by extB, and any frames the pair shares). The timeline draws
// the two blocks meeting at the cut, so everything that draws or decorates a
// block reads its place from here.
namespace MixShape {

struct Parts { int extA = 0, extB = 0, overlap = 0; };

// The transition out of `a`, from A's entry. A length once set on an aligned
// transition without moving its sides could leave more handle on record than
// the transition is long: the surplus plays as the clips' own frames and is
// counted as that.
inline Parts parts(const QVariantMap &a) {
    Parts p;
    const int n = qMax(0, a.value(ClipKey::mixOut).toInt());
    if (!a.value(ClipKey::mixAlign).toString().isEmpty()) {
        p.extA = qBound(0, a.value(ClipKey::mixExtA).toInt(), n);
        p.extB = qBound(0, a.value(ClipKey::mixExtB).toInt(), n - p.extA);
    }
    p.overlap = n - p.extA - p.extB;
    return p;
}

// What the pair on a cut can give its transition: each source's handle past
// the cut (in all, what the transition already uses included) and each clip's
// own frames on its side of the cut (A from its first frame of its own, past
// a transition into it; B to its last frame of its own).
struct Room { int handleA = 0, handleB = 0, framesA = 0, framesB = 0; };

inline Room room(const QVariantMap &a, const QVariantMap &b) {
    Room r;
    const Parts p = parts(a);
    const int n = p.extA + p.extB + p.overlap;
    r.handleA = qMax(0, a.value(ClipKey::length).toInt() - 1 - a.value(ClipKey::out).toInt()) + p.extA;
    r.handleB = qMax(0, b.value(ClipKey::in).toInt()) + p.extB;
    r.framesA = a.value(ClipKey::frames).toInt() - p.extA;
    r.framesB = b.value(ClipKey::frames).toInt() - b.value(ClipKey::mixOut).toInt() + n - p.extB;
    return r;
}

// The most frames the transition on A's cut can have before the cut
// (after = false) or after it while the other side, the cut and the edit's
// length stay as they are: a side grows out of its clip's handle, and each
// clip keeps a frame of its own outside the transition.
inline int sideLimit(const QVariantMap &a, const QVariantMap &b, bool after) {
    const Parts p = parts(a);
    const Room r = room(a, b);
    const int n = p.extA + p.extB + p.overlap;
    if (n < 1)
        return 0;
    if (after)
        return p.extA + qMax(0, qMin(r.handleA - p.extA, r.framesB + p.extB - 1 - n));
    return p.extB + p.overlap + qMax(0, qMin(r.handleB - p.extB, r.framesA + p.extA - 1 - n));
}

// A block's place against its entry's: it starts `lead` frames before the
// entry's start (the frames after the cut of the transition into the clip)
// and ends `tail` frames before the entry's end (the same of the transition
// out of it). `before` is the rest of the transition into the clip: its
// frames ahead of the cut, which lie over the end of the block before.
// mostBefore (on the block the transition leaves) and mostAfter (on the one
// it enters) are sideLimit's two answers, for the grips on the box's edges.
struct Block { int lead = 0, tail = 0, before = 0, mostBefore = 0, mostAfter = 0; };

// One per entry of a clip list (any mix of tracks, each track's clips in
// order of index or not).
inline QList<Block> blocks(const QVariantList &entries) {
    QList<Block> out(entries.size());
    QHash<QPair<int, int>, int> at; // (track, index) → place in the list
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap e = entries.at(i).toMap();
        at.insert({e.value(ClipKey::track).toInt(), e.value(ClipKey::index).toInt()}, i);
    }
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap e = entries.at(i).toMap();
        const Parts p = parts(e);
        if (p.extA + p.extB + p.overlap < 1)
            continue;
        out[i].tail = p.extA;
        const int next = at.value({e.value(ClipKey::track).toInt(), e.value(ClipKey::index).toInt() + 1}, -1);
        if (next >= 0 && entries.at(next).toMap().value(ClipKey::mixIn).toInt() > 0) {
            const QVariantMap b = entries.at(next).toMap();
            out[next].lead = p.extA;
            out[next].before = p.extB + p.overlap;
            out[next].mostAfter = sideLimit(e, b, true);
            out[i].mostBefore = sideLimit(e, b, false);
        }
    }
    return out;
}

} // namespace MixShape
