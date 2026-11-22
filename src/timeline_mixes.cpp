#include "timeline_p.h"

namespace {

// Wipe maps for the luma transition: we
// render our own greyscale gradients on demand (low luma transitions
// first). Cached per user; project files never depend on them — the style
// marker is what persists, and restore() regenerates the map locally.
QString lumaResource(const QString &styled) {
    const QString style = render::transitionBase(styled);  // its settings play no part in the carrier's map
    if (style.isEmpty() || style == QStringLiteral("dissolve"))
        return QString();
    const QDir dir(StoragePreferences::cacheRoot()
                   + QStringLiteral("/omaedit-lumas"));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    const QString file = dir.absoluteFilePath(style + QStringLiteral(".png"));
    if (QFileInfo::exists(file))
        return file;
    const int size = 256;
    QImage map(size, size, QImage::Format_Grayscale8);
    for (int y = 0; y < size; ++y) {
        uchar *line = map.scanLine(y);
        for (int x = 0; x < size; ++x) {
            double v = 0.5;
            if (style == QStringLiteral("wipe-right"))
                v = double(x) / (size - 1);
            else if (style == QStringLiteral("wipe-left"))
                v = 1.0 - double(x) / (size - 1);
            else if (style == QStringLiteral("wipe-down"))
                v = double(y) / (size - 1);
            else if (style == QStringLiteral("iris"))
                v = qMin(1.0,
                         qSqrt(qPow(x - size / 2.0, 2)
                               + qPow(y - size / 2.0, 2))
                             / (size * 0.7071));
            line[x] = uchar(qBound(0.0, v, 1.0) * 255);
        }
    }
    return map.save(file) ? file : QString();
}

// An entry's in point `moved` frames later (earlier when negative). Its keys
// stay on the frames they were set on. So do its effects that run as it
// plays: each counts from the clip's first frame, which is now another one,
// and its clock makes up the difference (as a cut does for the right half).
// Every frame the clip already showed is then the picture it was.
void moveEntryIn(QVariantMap &entry, int moved) {
    entry.insert(ClipKey::in, entry.value(ClipKey::in).toInt() + moved);
    rebaseEntryKeys(entry, moved);
    rebaseEntryClocks(entry, moved);
}
}

void rebaseEntryClocks(QVariantMap &entry, int moved) {
    QVariantList effects = entry.value(ClipKey::videoEffects).toList();
    if (moved == 0 || effects.isEmpty())
        return;
    for (QVariant &v : effects) {
        QVariantMap state = v.toMap();
        const int clock = state.value(QStringLiteral("clock")).toInt() + moved;
        if (clock != 0)
            state.insert(QStringLiteral("clock"), clock);
        else
            state.remove(QStringLiteral("clock"));
        v = state;
    }
    entry.insert(ClipKey::videoEffects, effects);
}

bool Timeline::hasMixes() const {
    if (!primary())
        return false;
    for (int i = 0; i < primary()->count(); ++i)
        if (primary()->is_mix(i))
            return true;
    return false;
}

bool Timeline::mixAt(int index, int length, bool followLane,
                        const QString &style, const QString &align, int extA, int extB) {
    return mixOn(0, index, length, followLane, style, align, extA, extB);
}

bool Timeline::mixOn(int track, int index, int length, bool followLane,
                        const QString &style, const QString &align, int extA, int extB) {
    const int entry = entryIndexOn(track, index);
    if (entry < 0 || length < 1)
        return false;
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    const int next = entry + 1;
    if (next >= pl->count() || pl->is_mix(next) || pl->is_blank(next))
        return false; // no next clip (or a gap), or already dissolved

    // The mix overlaps existing frames — it can't consume more than either
    // neighbour has (and must leave each at least one frame of its own).
    length = qMin(length, qMin(pl->clip_length(entry),
                               pl->clip_length(next)) - 1);
    if (length < 1)
        return false;

    // The mix consumes the seam's overlap: the NEXT clip's entry start stays
    // put (its head is inside the mix region) — only content downstream of
    // the pair moves left, on this track by playlist arithmetic and on the
    // auto-selected others by the ripple.
    const int boundary = pl->clip_start(next) + pl->clip_length(next);
    edit::Filter luma(*m_profile, "luma");
    const QString map = lumaResource(style);
    if (luma.is_valid() && !map.isEmpty()) {
        luma.set("resource", map.toUtf8().constData());
        luma.set("softness", 0.05);
    }
    if (pl->mix(entry, length, luma.is_valid() ? &luma : nullptr) != 0)
        return false;
    // The style persists as a marker filter on clip A's cut (cut filters
    // serialize; the map path itself never needs to).
    if (edit::ClipInfo *styleInfo = pl->clip_info(entry)) {
        if (styleInfo->cut) {
            edit::Producer *cut = styleInfo->cut;
            for (int i = cut->filter_count() - 1; i >= 0; --i) {
                std::unique_ptr<edit::Filter> f(cut->filter(i));
                if (f && f->is_valid() && (f->get("omavid:mixStyle") || f->get("omavid:mixAlign")))
                    cut->detach(*f);
            }
            if (!map.isEmpty() || !align.isEmpty()) {
                edit::Filter marker(*m_profile, "brightness"); // identity
                if (marker.is_valid()) {
                    marker.set("level", 1);
                    if (!map.isEmpty())
                        marker.set("omavid:mixStyle", style.toUtf8().constData());
                    if (!align.isEmpty()) {
                        marker.set("omavid:mixAlign", align.toUtf8().constData());
                        marker.set("omavid:mixExtA", extA);
                        marker.set("omavid:mixExtB", extB);
                    }
                    cut->attach(marker);
                }
            }
        }
        delete styleInfo;
    }
    edit::Filter crossfade(*m_profile, "mix");
    if (crossfade.is_valid()) {
        const auto clip = entryMapOn(track,index);
        crossfade.set("start", clip.value(ClipKey::audioCurve).toString()=="equalPower" ? -2 : -1);
        pl->mix_add(entry + 1, &crossfade);
    }
    // The overlap shortens everything after the seam by `length`.
    if (followLane)
        rippleOthersFrom(track, boundary, -length, false);
    return true;
}

bool Timeline::setFades(int index, int fadeIn, int fadeOut) {
    return setFadesOn(0, index, fadeIn, fadeOut);
}

bool Timeline::setFadesOn(int track, int index, int fadeIn, int fadeOut) {
    const int entry = entryIndexOn(track, index);
    if (entry < 0 || fadeIn < 0 || fadeOut < 0)
        return false;
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    // An edge carries either a dissolve or a fade, not both.
    const bool mixBefore = entry > 0 && pl->is_mix(entry - 1);
    const bool mixAfter = entry + 1 < pl->count() && pl->is_mix(entry + 1);
    if ((fadeIn > 0 && mixBefore) || (fadeOut > 0 && mixAfter))
        return false;
    edit::ClipInfo *info = pl->clip_info(entry);
    if (!info || !info->cut) {
        delete info;
        return false;
    }
    fadeIn = qMin(fadeIn, info->frame_count);
    fadeOut = qMin(fadeOut, info->frame_count);
    applyFades(info->cut, info->frame_in, info->frame_out, fadeIn, fadeOut);
    delete info;
    return true;
}


bool Timeline::setAudioFadeCurveAt(int track,int index,const QString &curve) {
    if(trackLocked(track)||!QStringList{"log","linear","equalPower","smooth"}.contains(curve))return false;
    auto info=clipInfoAt(track,index);if(!info||!info->cut)return false;
    const auto before=entries(); // the fallback if the dissolve's rebuild fails part way
    int a=0,b=0;fadeLengths(info->cut,info->frame_in,info->frame_out,&a,&b);
    for(int i=info->cut->filter_count()-1;i>=0;--i){std::unique_ptr<edit::Filter> f(info->cut->filter(i));
        if(f&&f->get("omavid:audioCurve"))info->cut->detach(*f);}
    edit::Filter meta(*m_profile,"volume");meta.set("level",0);meta.set("omavid:audioCurve",curve.toUtf8().constData());info->cut->attach(meta);
    applyFades(info->cut,info->frame_in,info->frame_out,a,b);
    const auto snapshot=entries();
    if(entryMapOn(track,index).value(ClipKey::mixOut).toInt()>0 && !m_restoring) return rebuild(snapshot,before);
    return true;
}
void Timeline::applyFades(edit::Producer *cut, int in, int out, int fadeIn,
                             int fadeOut) {
    QString curve;
    for(int i=0;i<cut->filter_count();++i){std::unique_ptr<edit::Filter> f(cut->filter(i));if(f && f->get("omavid:audioCurve"))curve=QString::fromUtf8(f->get("omavid:audioCurve"));}
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:fade"))
            cut->detach(*f);
    }
    const auto addFade = [this, cut, curve](const char *service, int a, int b,
                                     const char *anim, const char *marker) {
        const bool shaped=std::strcmp(service,"volume")==0 && !curve.isEmpty();
        edit::Filter f(*m_profile, shaped ? "omaedit.fade" : service);
        if (!f.is_valid()) return;
        if(shaped)f.set("config",QJsonDocument::fromVariant(QVariantMap{{"shape",curve},{"outgoing",std::strcmp(marker,"out")==0}}).toJson(QJsonDocument::Compact).constData());
        f.set("level", anim);
        f.set_in_and_out(a, b);
        f.set("omavid:fade", marker);
        cut->attach(f);
        reorderAttributeFilters(cut);
    };
    // ⚠️ The two filters read "level" in DIFFERENT units (verified against
    // 7.40 with a tone probe): brightness takes a 0..1 gain, but the volume
    // filter takes DECIBELS — a 0→1 ramp there is a 1 dB nudge, not a fade.
    // -60 dB is silence for all practical purposes.
    if (fadeIn > 0) {
        addFade("brightness", in, in + fadeIn - 1, "0=0;-1=1", "in");
        addFade("volume", in, in + fadeIn - 1, "0=-60;-1=0", "in");
    }
    if (fadeOut > 0) {
        addFade("brightness", out - fadeOut + 1, out, "0=1;-1=0", "out");
        addFade("volume", out - fadeOut + 1, out, "0=0;-1=-60", "out");
    }
}

// Re-plant an existing dissolve with a new overlap (via snapshot rebuild,
// like unmixAt — a mix can't be resized in place). The overlap change moves
// content downstream of the pair; the tracks above follow by the ACTUAL applied
// length, since mixAt clamps against the neighbours.
bool Timeline::setMixLength(int index, int length) {
    return setMixLengthOn(0, index, length);
}

bool Timeline::setMixLengthOn(int track, int index, int length) {
    if (length < 1)
        return unmixOn(track, index);
    if (trackLocked(track))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    int boundary = -1; // where downstream content begins (see unmixOn)
    const int at = snapshotIndexOn(clips, track, index, &boundary);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    const int overlap = map.value(ClipKey::mixOut).toInt();
    if (overlap < 1 || overlap == length)
        return false;
    const int nextAt = snapshotIndexOn(clips, track, index + 1, nullptr);
    // A transition made of the clips' handles keeps its cut and the edit its
    // length: the two sides take the new length in the shares they had (a
    // centred one stays centred), each as far as its handle goes.
    if (const MixParts parts = mixParts(map); nextAt >= 0 && parts.extA + parts.extB > 0) {
        const QVariantMap next = clips.at(nextAt).toMap();
        const int mostAfter = mixSideLimit(map, next, true), mostBefore = mixSideLimit(map, next, false);
        int after = map.value(ClipKey::mixAlign).toString() == QStringLiteral("centre")
                        ? length / 2 : qRound(double(length) * parts.extA / overlap);
        after = qBound(0, after, mostAfter);
        const int before = qMin(length - after, mostBefore);
        after = qBound(0, length - before, mostAfter);
        return setMixSidesOn(track, index, before, after);
    }
    // The new overlap can't exceed either neighbour (restore clamps the
    // same way) — clamp here too so the lane shift matches what lands.
    int applied = length;
    if (nextAt >= 0) {
        const int aFrames = map.value(ClipKey::frames).toInt();
        const int bFrames = clips.at(nextAt).toMap().value(ClipKey::frames).toInt()
                            + overlap; // restored span of B
        applied = qMin(applied, qMin(aFrames, bFrames) - 1);
    }
    if (applied < 1 || applied == overlap)
        return false;
    map.insert(ClipKey::mixOut, applied);
    clips[at] = map;
    if (track >= 1 && boundary >= 0)
        shiftSnapshotLane(&clips, track, boundary, overlap - applied);
    if (!rebuild(clips, pristine))
        return false;
    if (boundary >= 0)
        rippleOthersFrom(track, boundary, overlap - applied, false);
    return true;
}

// Restyle an existing dissolve (same overlap, different wipe) — snapshot
// rebuild, like every other mix mutation.
bool Timeline::setMixStyle(int index, const QString &style) {
    return setMixStyleOn(0, index, style);
}

bool Timeline::setMixStyleOn(int track, int index, const QString &style) {
    if (trackLocked(track))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    if (map.value(ClipKey::mixOut).toInt() < 1 || !render::transitionStyle(style))
        return false;
    if (map.value(ClipKey::mixStyle).toString() == style)
        return false;
    map.insert(ClipKey::mixStyle, style);
    clips[at] = map;
    return rebuild(clips, pristine); // overlap unchanged — nothing moves
}

// The name that is true of a transition's parts: "" a plain overlap, "start"
// / "end" all of it after / before the cut, "centre" the two sides equal to
// within the odd frame, and "custom" anything else.
QString Timeline::mixAlignFor(int extA, int extB, int overlap) {
    if (extA < 1 && extB < 1)
        return QString();
    if (overlap > 0)
        return QStringLiteral("custom");
    if (extB < 1)
        return ClipKey::start;
    if (extA < 1)
        return QStringLiteral("end");
    return qAbs(extA - extB) <= 1 ? QStringLiteral("centre") : QStringLiteral("custom");
}

// One rebuild gives the transition on a cut its three parts: A's out reaches
// extA frames past the cut, B's in extB frames before it, and `overlap`
// frames of the pair are shared (none of the three removes the transition and
// hands the handles back). Only a change of the overlap changes the track's
// length, so only then does anything after the pair move.
bool Timeline::shapeMixOn(int track, int index, int extA, int extB, int overlap,
                          const QString &align, const QString *style) {
    if (trackLocked(track) || extA < 0 || extB < 0 || overlap < 0)
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    int boundary = -1; // where downstream content begins (see unmixOn)
    const int at = snapshotIndexOn(clips, track, index, &boundary);
    const int nextAt = snapshotIndexOn(clips, track, index + 1, nullptr);
    if (at < 0 || nextAt < 0)
        return false;
    QVariantMap a = clips.at(at).toMap();
    QVariantMap b = clips.at(nextAt).toMap();
    const MixParts old = mixParts(a);
    const int was = old.extA + old.extB + old.overlap;
    const int n = extA + extB + overlap;
    if (was < 1 && n < 1)
        return false;
    // A sound cut to the sample carries its timing beside these frames.
    if (a.contains(ClipKey::audioTiming) || b.contains(ClipKey::audioTiming))
        return false;
    // A new transition needs the pair to meet: no empty space on the cut.
    if (was < 1 && (a.value(ClipKey::gap).toBool() || b.value(ClipKey::gap).toBool()
                    || b.value(ClipKey::start).toInt() != a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt()))
        return false;
    const int dA = extA - old.extA, dB = extB - old.extB;
    const int aOut = a.value(ClipKey::out).toInt() + dA;
    const int bIn = b.value(ClipKey::in).toInt() - dB;
    // A side that grows must find the frames in its source.
    if (aOut < a.value(ClipKey::in).toInt() || (dA > 0 && aOut > a.value(ClipKey::length).toInt() - 1)
        || (dB > 0 && bIn < 0) || bIn > b.value(ClipKey::out).toInt())
        return false;
    // The pair as the rebuild lays it before the transition takes its frames.
    const int aFrames = a.value(ClipKey::frames).toInt() + dA;
    const int bOwnOut = b.value(ClipKey::mixOut).toInt();
    const int bFrames = b.value(ClipKey::frames).toInt() - bOwnOut + was + dB;
    if (n > 0 && n > qMin(aFrames, bFrames) - 1)
        return false; // each clip keeps a frame of its own
    const QString name = n > 0 ? align : QString();
    if (n == was && dA == 0 && dB == 0 && name == a.value(ClipKey::mixAlign).toString()
        && (!style || *style == a.value(ClipKey::mixStyle).toString()))
        return false;
    a.insert(ClipKey::out, aOut);
    a.insert(ClipKey::frames, aFrames);
    a.insert(ClipKey::mixOut, n);
    if (name.isEmpty()) {
        for (const QString &key : {ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB})
            a.remove(key);
    } else {
        a.insert(ClipKey::mixAlign, name);
        a.insert(ClipKey::mixExtA, extA);
        a.insert(ClipKey::mixExtB, extB);
    }
    if (style)
        a.insert(ClipKey::mixStyle, *style);
    moveEntryIn(b, -dB);
    b.insert(ClipKey::mixIn, n);
    b.insert(ClipKey::frames, bFrames - n + bOwnOut);
    b.insert(ClipKey::start, a.value(ClipKey::start).toInt() + aFrames);
    const int delta = dA + dB - (n - was); // the track's change of length
    if (track >= 1 && boundary >= 0 && delta != 0)
        shiftSnapshotLane(&clips, track, boundary, delta);
    clips[at] = a;
    clips[nextAt] = b;
    if (!rebuild(clips, pristine))
        return false;
    if (boundary >= 0 && delta != 0)
        rippleOthersFrom(track, boundary, delta, false);
    return true;
}

// A new transition sits across the cut: half each side, made of the frames
// the two sources have past it, so the edit keeps its length. A side with no
// spare frames gives its share to the other, and only what neither can give
// overlaps the clips' own frames.
bool Timeline::addMixAcrossOn(int track, int index, int length, const QString &style) {
    if (trackLocked(track) || length < 1 || !render::transitionStyle(style))
        return false;
    const QVariantList clips = entries();
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    const int nextAt = snapshotIndexOn(clips, track, index + 1, nullptr);
    if (at < 0 || nextAt < 0)
        return false;
    const QVariantMap a = clips.at(at).toMap(), b = clips.at(nextAt).toMap();
    if (a.value(ClipKey::mixOut).toInt() > 0)
        return false;
    const MixRoom room = mixRoom(a, b);
    for (int n = length; n >= 1; --n) {
        int extA = qMin(n / 2, room.handleA);
        const int extB = qMin(n - extA, room.handleB);
        extA = qMin(n - extB, room.handleA);
        if (n > qMin(room.framesA + extA, room.framesB + extB) - 1)
            continue; // a short clip: a shorter transition
        const int overlap = n - extA - extB;
        return shapeMixOn(track, index, extA, extB, overlap, mixAlignFor(extA, extB, overlap), &style);
    }
    return false;
}

// One side of a transition set from the cut: `before` frames ahead of it and
// `after` frames past it. The other side and the cut stay where they are and
// the edit keeps its length: a side grows out of its clip's handle and no
// further, and shrinks by handing handle back (shared frames go last, which
// alone lengthens the edit). Nothing left on either side removes it.
bool Timeline::setMixSidesOn(int track, int index, int before, int after) {
    if (trackLocked(track))
        return false;
    const QVariantList clips = entries();
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    const int nextAt = snapshotIndexOn(clips, track, index + 1, nullptr);
    if (at < 0 || nextAt < 0)
        return false;
    const QVariantMap a = clips.at(at).toMap(), b = clips.at(nextAt).toMap();
    const MixParts parts = mixParts(a);
    if (parts.extA + parts.extB + parts.overlap < 1)
        return false;
    // Neither side's room depends on the other's handle frames, so the two
    // are bounded independently.
    const int extA = qBound(0, after, mixSideLimit(a, b, true));
    before = qBound(0, before, mixSideLimit(a, b, false));
    int extB = parts.extB, overlap = parts.overlap;
    const int had = extB + overlap;
    if (before >= had) {
        extB += before - had;
    } else {
        const int back = qMin(had - before, extB);
        extB -= back;
        overlap -= had - before - back;
    }
    return shapeMixOn(track, index, extA, extB, overlap, mixAlignFor(extA, extB, overlap), nullptr);
}

bool Timeline::setMixAlignOn(int track, int index, const QString &align) {
    if (trackLocked(track))
        return false;
    if (align != QString() && align != QStringLiteral("centre")
        && align != ClipKey::start && align != QStringLiteral("end"))
        return false;
    const QVariantList clips = entries();
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    const int nextAt = snapshotIndexOn(clips, track, index + 1, nullptr);
    if (at < 0 || nextAt < 0)
        return false;
    const QVariantMap a = clips.at(at).toMap(), b = clips.at(nextAt).toMap();
    int n = a.value(ClipKey::mixOut).toInt();
    if (n < 1 || a.value(ClipKey::mixAlign).toString() == align)
        return false;
    // The new extensions, clamped to the handles each source has; what a
    // handle cannot give shifts to the other side, and the rest overlaps
    // the visible frames as a plain dissolve would.
    const MixRoom room = mixRoom(a, b);
    for (; n >= 1; --n) {
        int extA = 0, extB = 0;
        if (align == ClipKey::start)
            extA = n;
        else if (align == QStringLiteral("end"))
            extB = n;
        else if (align == QStringLiteral("centre")) {
            extA = n / 2;
            extB = n - extA;
        }
        if (extA > room.handleA) {
            extB += extA - room.handleA;
            extA = room.handleA;
        }
        if (extB > room.handleB) {
            const int spill = qMin(extB - room.handleB, room.handleA - extA);
            extA += spill;
            extB = room.handleB;
        }
        if (n > qMin(room.framesA + extA, room.framesB + extB) - 1)
            continue; // the clips are too short to share this much
        return shapeMixOn(track, index, extA, extB, n - extA - extB, align, nullptr);
    }
    return false;
}

QVariantList Timeline::clipsApart(const QVariantList &copies, const QVariantList &all) {
    QVariantList out;
    for (const QVariant &v : copies) {
        QVariantMap e = v.toMap();
        const int track = e.value(ClipKey::track).toInt(), index = e.value(ClipKey::index).toInt();
        // A transition both of whose clips are copied goes with the copies:
        // the clip it leaves carries a note of its sides and kind, for the
        // paste to put it back across the two where they land (one that
        // shares frames of the pair, an overlap, is not carried).
        QVariantMap carried;
        if (const MixParts parts = mixParts(e); parts.extA + parts.extB > 0 && parts.overlap == 0) {
            for (const QVariant &o : copies) {
                const QVariantMap next = o.toMap();
                if (next.value(ClipKey::track).toInt() == track && next.value(ClipKey::index).toInt() == index + 1
                    && next.value(ClipKey::mixIn).toInt() == parts.extA + parts.extB
                    && next.value(ClipKey::audioOnly).toBool() == e.value(ClipKey::audioOnly).toBool())
                    carried = {{QStringLiteral("after"), parts.extA}, {QStringLiteral("before"), parts.extB},
                               {QStringLiteral("style"), e.value(ClipKey::mixStyle).toString()},
                               {QStringLiteral("align"), e.value(ClipKey::mixAlign).toString()},
                               {QStringLiteral("into"), next.value(ClipKey::path).toString()}};
            }
        }
        if (const int mixed = e.value(ClipKey::mixIn).toInt(); mixed > 0) {
            QVariantMap before;
            bool standing = false; // the copy is still the clip in the list
            for (const QVariant &o : all) {
                const QVariantMap m = o.toMap();
                if (m.value(ClipKey::track).toInt() != track)
                    continue;
                if (m.value(ClipKey::index).toInt() == index - 1) {
                    before = m;
                } else if (m.value(ClipKey::index).toInt() == index) {
                    standing = true;
                    for (const QString &key : {ClipKey::path, ClipKey::start, ClipKey::frames, ClipKey::in, ClipKey::out, ClipKey::mixIn})
                        standing = standing && m.value(key) == e.value(key);
                }
            }
            const MixParts parts = mixParts(before);
            if (standing && parts.extA + parts.extB + parts.overlap == mixed) {
                e.insert(ClipKey::start, e.value(ClipKey::start).toInt() - parts.extA);
                e.insert(ClipKey::frames, e.value(ClipKey::frames).toInt() + parts.extA);
                moveEntryIn(e, parts.extB + parts.overlap);
                e.remove(ClipKey::mixIn);
            }
        }
        if (const MixParts parts = mixParts(e); parts.extA + parts.extB + parts.overlap > 0) {
            e.insert(ClipKey::out, e.value(ClipKey::out).toInt() - parts.extA);
            e.insert(ClipKey::frames, e.value(ClipKey::frames).toInt() - parts.extA);
            for (const QString &key : {ClipKey::mixOut, ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB, ClipKey::mixStyle})
                e.remove(key);
        }
        if (!carried.isEmpty())
            e.insert(ClipKey::mixCarry, carried);
        out.append(e);
    }
    return out;
}

QList<Timeline::HandleMix> Timeline::handleMixesOn(int track) const {
    QList<HandleMix> out;
    const edit::Playlist *pl = track >= 0 && track < trackCount() ? playlistOn(track) : nullptr;
    bool any = false;
    for (int i = 0; pl && !any && i < pl->count(); ++i)
        any = pl->is_mix(i);
    if (!any)
        return out; // the usual case costs a walk of the track, not a reading of the edit
    QVariantMap before;
    int lead = 0; // the frames `before` is drawn ahead of its entry's start
    for (const QVariant &v : entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() != track)
            continue;
        const MixParts parts = mixParts(before);
        const bool mixed = e.value(ClipKey::mixIn).toInt() > 0;
        if (mixed && parts.extA + parts.extB > 0) {
            const int to = e.value(ClipKey::start).toInt();
            out.append({before.value(ClipKey::index).toInt(), before.value(ClipKey::start).toInt() - lead,
                        to - before.value(ClipKey::mixOut).toInt(), to, to - parts.extA, to + e.value(ClipKey::frames).toInt()});
        }
        lead = mixed ? parts.extA : 0;
        before = e;
    }
    return out;
}

bool Timeline::withMixesReleased(int track, const QList<int> &outgoing, int said, const std::function<bool()> &edit) {
    if (m_restoring || outgoing.isEmpty())
        return edit();
    QList<int> taken;
    for (const HandleMix &mix : handleMixesOn(track))
        if (outgoing.contains(mix.index))
            taken.append(mix.index);
    if (taken.isEmpty())
        return edit();
    const QVariantList pristine = entries();
    for (const int index : std::as_const(taken)) {
        if (!shapeMixOn(track, index, 0, 0, 0, QString(), nullptr)) {
            restore(pristine);
            return false;
        }
    }
    if (!edit()) {
        restore(pristine);
        return false;
    }
    m_brokenDissolves += qMin(said, int(taken.size()));
    return true;
}

// The transitions made of spare frames on the two cuts of the clip at (track,
// index) are taken off, `edit` is made on the clips as they are drawn, and
// each transition is put back across its cut with the sides it had, as far as
// the clips then have frames for them (one that no longer fits is counted as
// removed, for takeBrokenDissolves). False, with the timeline as it was, when
// the edit fails. The edit must leave the clips' indices as they are.
bool Timeline::withMixesHeld(int track, int index, const std::function<bool()> &edit) {
    struct Held { int index, before, after; QString style; };
    QList<Held> held;
    if (!m_restoring)
        for (const HandleMix &mix : handleMixesOn(track))
            if (mix.index == index - 1 || mix.index == index) {
                const QVariantMap a = entryMapOn(track, mix.index);
                const MixParts parts = mixParts(a);
                held.append({mix.index, parts.extB + parts.overlap, parts.extA, a.value(ClipKey::mixStyle).toString()});
            }
    if (held.isEmpty())
        return edit();
    const QVariantList pristine = entries();
    const auto back = [&] {
        restore(pristine);
        return false;
    };
    for (const Held &mix : std::as_const(held))
        if (!shapeMixOn(track, mix.index, 0, 0, 0, QString(), nullptr))
            return back();
    if (!edit())
        return back();
    for (const Held &mix : std::as_const(held)) {
        if (!addMixAcrossOn(track, mix.index, mix.before + mix.after, mix.style)) {
            ++m_brokenDissolves;
            continue;
        }
        setMixSidesOn(track, mix.index, mix.before, mix.after); // (false when it already sits so)
    }
    return true;
}

// As withMixesHeld, for an edit that puts another clip in the place of the
// one at (track, index) and leaves the cuts where they are (a Replace): each
// transition goes back across its cut with the sides and kind it had, a side
// no longer than the spare frames its clip now has. `kept` says what became
// of each: as long as it was, shorter, or gone (no frames on either side).
// One that shares frames of the pair (an overlap) is not held: the edit
// meets it as it always has. The pictures' linked sound crosses over the same
// frames: its crossfades come off with the transitions, and `settled` (run
// after the edit, before the transitions go back) is where the caller lays
// the new clip's sound, so that a conform to the pictures afterwards gives
// the sound its crossfades again.
bool Timeline::withMixesKept(int track, int index, const std::function<bool()> &edit, QList<KeptMix> *kept,
                             const std::function<void()> &settled) {
    struct Held { int index; MixParts parts; QString style, align; };
    QList<Held> held;
    if (!m_restoring)
        for (const HandleMix &mix : handleMixesOn(track))
            if (mix.index == index - 1 || mix.index == index) {
                const QVariantMap a = entryMapOn(track, mix.index);
                const MixParts parts = mixParts(a);
                if (parts.overlap == 0)
                    held.append({mix.index, parts, a.value(ClipKey::mixStyle).toString(), a.value(ClipKey::mixAlign).toString()});
            }
    if (held.isEmpty())
        return edit();
    const QVariantList pristine = entries();
    const auto back = [&] {
        restore(pristine);
        return false;
    };
    for (const Held &mix : std::as_const(held)) {
        const QVariantMap a = entryMapOn(track, mix.index);
        if (const QString link = a.value(ClipKey::link).toString(); !link.isEmpty() && !a.value(ClipKey::audioOnly).toBool())
            for (const QVariant &v : std::as_const(pristine)) {
                const QVariantMap mate = v.toMap();
                if (mate.value(ClipKey::link).toString() == link && mate.value(ClipKey::audioOnly).toBool()
                    && mate.value(ClipKey::mixOut).toInt() > 0
                    && !shapeMixOn(mate.value(ClipKey::track).toInt(), mate.value(ClipKey::index).toInt(), 0, 0, 0, QString(), nullptr))
                    return back();
            }
        if (!shapeMixOn(track, mix.index, 0, 0, 0, QString(), nullptr))
            return back();
    }
    if (!edit())
        return back();
    if (settled)
        settled();
    for (const Held &mix : std::as_const(held)) {
        const int now = putMixAcrossOn(track, mix.index, mix.parts.extA, mix.parts.extB, mix.style, mix.align);
        if (kept)
            kept->append({mix.index < index, mix.parts.extA + mix.parts.extB, now});
    }
    return true;
}

// A transition with `after` frames past the cut that follows clip `index` of
// `track` and `before` frames ahead of it, of this kind, put on a cut that
// has none: each side as long as asked or as its clip's spare frames allow,
// each clip keeping a frame of its own outside it. The frames it then has:
// none when neither clip has any to spare, or the two do not meet.
int Timeline::putMixAcrossOn(int track, int index, int after, int before, const QString &style, const QString &align) {
    const QVariantMap a = entryMapOn(track, index), b = entryMapOn(track, index + 1);
    if (a.isEmpty() || b.isEmpty() || a.value(ClipKey::gap).toBool() || b.value(ClipKey::gap).toBool()
        || a.value(ClipKey::mixOut).toInt() > 0)
        return 0;
    const MixRoom room = mixRoom(a, b);
    const int extA = qMax(0, qMin(qMin(after, room.handleA), room.framesB - 1));
    const int extB = qMax(0, qMin(qMin(before, room.handleB), room.framesA - 1));
    if (extA + extB < 1)
        return 0;
    const QString name = extA == after && extB == before && !align.isEmpty() ? align : mixAlignFor(extA, extB, 0);
    return shapeMixOn(track, index, extA, extB, 0, name, &style) ? extA + extB : 0;
}

// A clip on an upper track that is in a transition made of spare frames is
// trimmed at its free edge, as one on the main track is: asked for in the
// clip's own in, out and start (which count the transition's frames), it is
// trimmed as the clip drawn with the transition held aside. The edge the
// transition sits on stays: a trim of it that does not ripple is refused (one
// that does is trimUnderMixOn's). Nothing to say (no value) for a clip in no
// such transition.
std::optional<bool> Timeline::trimLaneClipInTransition(int track, int index, int in, int out, int newStart, bool ripple) {
    if (m_restoring || track < 1)
        return std::nullopt;
    int head = 0, lead = 0, tail = 0;
    bool any = false;
    for (const HandleMix &mix : handleMixesOn(track)) {
        if (mix.index != index && mix.index != index - 1)
            continue;
        const MixParts parts = mixParts(entryMapOn(track, mix.index));
        if (parts.overlap > 0)
            return false; // frames the pair shares would have to move the track
        any = true;
        if (mix.index == index) {
            tail = parts.extA;
        } else {
            head = parts.extB;
            lead = parts.extA;
        }
    }
    if (!any)
        return std::nullopt;
    const QVariantMap clip = entryMapOn(track, index);
    if ((clip.value(ClipKey::mixOut).toInt() > 0 && out != clip.value(ClipKey::out).toInt())
        || (clip.value(ClipKey::mixIn).toInt() > 0
            && (in != clip.value(ClipKey::in).toInt() || newStart != clip.value(ClipKey::start).toInt())))
        return false;
    const int plainIn = in + head, plainOut = out - tail, plainStart = newStart - lead;
    return withMixesHeld(track, index, [&] {
        if (ripple)
            return trimOn(track, index, plainIn, plainOut, plainStart, true);
        const int entry = entryIndexOn(track, index);
        return entry >= 0 && trimLaneEntry(track - 1, entry, plainIn, plainOut, plainStart);
    });
}

// A ripple trim of an edge a transition sits on. The clip's out point (or in
// point) moves by the frames asked, the clips after it ride as they do for a
// plain ripple trim, and the transition stays across the cut with the sides,
// length and kind it has. It is made of the two clips' spare frames, so the
// trim goes as far as the clip still covers its half of it (and keeps a frame
// of its own outside it, and outside a transition on its other edge) and
// stops there. `in` and `out` are asked for as the clip's entry counts them.
// Nothing to say (no value) when neither edge asked to move has a transition.
std::optional<bool> Timeline::trimUnderMixOn(int track, int index, int in, int out) {
    if (m_restoring || track < 0 || track >= trackCount())
        return std::nullopt;
    const QVariantMap clip = entryMapOn(track, index);
    if (clip.isEmpty())
        return std::nullopt;
    const int askedIn = in - clip.value(ClipKey::in).toInt(), askedOut = out - clip.value(ClipKey::out).toInt();
    const bool heldIn = clip.value(ClipKey::mixIn).toInt() > 0 && askedIn != 0;
    const bool heldOut = clip.value(ClipKey::mixOut).toInt() > 0 && askedOut != 0;
    // A transition whose clips share frames (an overlap) has no cut of its
    // own to keep: its edges stay held, as they always were.
    if (heldOut && mixParts(clip).overlap > 0)
        return std::nullopt;
    if (heldIn && mixParts(entryMapOn(track, index - 1)).overlap > 0)
        return std::nullopt;
    if (!heldIn && !heldOut)
        return std::nullopt;
    // Both edges by the same frames is a slip: the transition's frames would
    // become other frames of the clip, and that stays refused.
    if (trackLocked(track) || askedIn == askedOut)
        return false;
    const QVariantList pristine = entries();
    const auto back = [&] {
        restore(pristine);
        return false;
    };
    // The clip's free edge, when it was asked to move too: a plain trim.
    bool moved = false;
    if ((askedIn != 0 && !heldIn) || (askedOut != 0 && !heldOut)) {
        const int freeIn = heldIn ? clip.value(ClipKey::in).toInt() : in, freeOut = heldOut ? clip.value(ClipKey::out).toInt() : out;
        if (!trimOn(track, index, freeIn, freeOut, clip.value(ClipKey::start).toInt(), true))
            return false;
        moved = true;
    }
    for (const bool outgoing : {true, false}) {
        if (outgoing ? !heldOut : !heldIn)
            continue;
        QVariantList clips = entries();
        const QVariantList standing = clips;
        // The clip the transition leaves, the one it enters, and where the
        // clips after the pair begin.
        const int first = outgoing ? index : index - 1;
        int boundary = -1;
        const int at = snapshotIndexOn(clips, track, first, &boundary);
        const int nextAt = snapshotIndexOn(clips, track, first + 1, nullptr);
        if (at < 0 || nextAt < 0)
            return back();
        QVariantMap a = clips.at(at).toMap(), b = clips.at(nextAt).toMap();
        if (a.contains(ClipKey::audioTiming) || b.contains(ClipKey::audioTiming))
            return back(); // a sound cut to the sample carries its timing beside these frames
        const int n = a.value(ClipKey::mixOut).toInt();
        int delta = 0;  // the track's change of length
        int from = 0;   // the clips of the other tracks that start here or later ride
        if (outgoing) {
            const int most = a.value(ClipKey::length).toInt() - 1 - a.value(ClipKey::out).toInt();
            const int least = n + 1 - a.value(ClipKey::frames).toInt();
            delta = qBound(qMin(least, 0), askedOut, qMax(most, 0));
            if (delta == 0)
                continue;
            from = a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt() - mixParts(a).extA; // the cut
            // The pictures' linked sound crosses over the same frames. The
            // ripple moves the incoming sound and not the outgoing one, which
            // would part them mid-crossfade: their crossfade comes off first,
            // and the sound takes it again when it is conformed to its
            // pictures (both changed), as it does after any picture edit.
            if (const QString link = a.value(ClipKey::link).toString(); !link.isEmpty() && !a.value(ClipKey::audioOnly).toBool()) {
                bool parted = false;
                for (const QVariant &v : std::as_const(standing)) {
                    const QVariantMap mate = v.toMap();
                    if (mate.value(ClipKey::link).toString() != link || !mate.value(ClipKey::audioOnly).toBool()
                        || mate.value(ClipKey::mixOut).toInt() < 1)
                        continue;
                    if (!shapeMixOn(mate.value(ClipKey::track).toInt(), mate.value(ClipKey::index).toInt(), 0, 0, 0, QString(), nullptr))
                        return back();
                    parted = true;
                }
                if (parted)
                    clips = entries(); // (the pictures stand as they did)
            }
            a.insert(ClipKey::out, a.value(ClipKey::out).toInt() + delta);
            a.insert(ClipKey::frames, a.value(ClipKey::frames).toInt() + delta);
            b.insert(ClipKey::start, b.value(ClipKey::start).toInt() + delta);
        } else {
            const int most = b.value(ClipKey::frames).toInt() - b.value(ClipKey::mixOut).toInt() - 1;
            const int trimmed = qBound(qMin(-b.value(ClipKey::in).toInt(), 0), askedIn, qMax(most, 0));
            if (trimmed == 0)
                continue;
            from = b.value(ClipKey::start).toInt() + b.value(ClipKey::frames).toInt()
                   - (b.value(ClipKey::mixOut).toInt() > 0 ? mixParts(b).extA : 0); // the clip's own end
            moveEntryIn(b, trimmed);
            b.insert(ClipKey::frames, b.value(ClipKey::frames).toInt() - trimmed);
            delta = -trimmed;
        }
        clips[at] = a;
        clips[nextAt] = b;
        // An upper track's clips are placed by their starts: those after the
        // pair move with the edit (the main track's follow from its order).
        if (track >= 1 && boundary >= 0)
            shiftSnapshotLane(&clips, track, boundary, delta);
        if (!rebuild(clips, entries()))
            return back();
        rippleOthersFrom(track, from, delta);
        moved = true;
    }
    return moved; // (false at its limit already: nothing changed)
}

bool Timeline::unmixAt(int index) {
    return unmixOn(0, index);
}

// Snapshot index of storyline/lane clip `index` on `track`, and the
// downstream boundary (the NEXT clip's end) a dissolve change pivots on.
int Timeline::snapshotIndexOn(const QVariantList &clips, int track, int index,
                                 int *boundary) {
    int clip = 0, at = -1;
    if (boundary)
        *boundary = -1;
    for (int i = 0; i < clips.size(); ++i) {
        const QVariantMap m = clips.at(i).toMap();
        if (m.value(ClipKey::lane).toInt() != track)
            continue;
        if (clip == index)
            at = i;
        else if (clip == index + 1 && boundary)
            *boundary = m.value(ClipKey::start).toInt()
                        + m.value(ClipKey::frames).toInt();
        ++clip;
    }
    return at;
}

bool Timeline::unmixOn(int track, int index) {
    // There is no unmix — rebuild from a snapshot with the dissolve cleared,
    // exactly the way undo would. Removing the mix restores the overlap: the
    // pair's own spans stay anchored — only content downstream of the pair
    // (the next clip's end onward) grows back right. Mirror of mixOn. The
    // handle frames an aligned transition was made of go back to the
    // sources, so the cut is the one the transition was put on.
    return shapeMixOn(track, index, 0, 0, 0, QString(), nullptr);
}

// An insert, overwrite or move landing inside a lane dissolve would split its
// mix entry and leave half a transition behind. The dissolve goes first, the
// way Remove Transition takes it, and the edit then cuts plain clips; the
// caller's undo snapshot predates both. Runs BEFORE the caller opens producers
// or takes playlist pointers: removing a mix rebuilds the timeline.
// Returns the number removed, or -1 when one could not be.
bool Timeline::hasLaneDissolve() const {
    for (size_t t = 1; t < m_tracks.size(); ++t)
        if (const auto &pl = m_tracks[t].playlist)
            for (int i = 0; i < pl->count(); ++i)
                if (pl->is_mix(i))
                    return true;
    return false;
}

int Timeline::breakLaneDissolves(int track, int from, int to) {
    if (m_restoring || track < 0 || track >= trackCount())
        return 0;
    int broken = 0;
    for (bool again = true; again;) {
        again = false;
        for (const QVariant &v : entries()) {
            const QVariantMap m = v.toMap();
            const int overlap = m.value(ClipKey::mixOut).toInt();
            if (m.value(ClipKey::track).toInt() != track || overlap < 1)
                continue;
            const int end = m.value(ClipKey::start).toInt() + m.value(ClipKey::frames).toInt();
            if (end - overlap > to || end < from)
                continue;
            if (!unmixOn(track, m.value(ClipKey::index).toInt()))
                return -1;
            ++broken;
            again = true;
            break;
        }
    }
    m_brokenDissolves += broken;
    return broken;
}

// A ripple opening space at `position` moves the lane clips that start at or
// after it and leaves the rest where they are: a dissolve whose outgoing
// clip starts before the point while its incoming clip rides would be torn
// in two, and the ripple could not happen at all. On every lane that rides
// (unlocked, auto-selected, not the edited track) such a dissolve goes
// first, as breakLaneDissolves does. Returns the number removed, or -1.
int Timeline::breakRippledLaneDissolves(int sourceTrack, int position) {
    if (m_restoring)
        return 0;
    int broken = 0;
    for (int track = 1; track < trackCount(); ++track) {
        const Track &t = m_tracks[size_t(track)];
        if (track == sourceTrack || t.locked || !t.autoSelect)
            continue;
        for (bool again = true; again;) {
            again = false;
            for (const QVariant &v : entries()) {
                const QVariantMap m = v.toMap();
                if (m.value(ClipKey::track).toInt() != track
                    || m.value(ClipKey::mixOut).toInt() < 1)
                    continue;
                const int start = m.value(ClipKey::start).toInt();
                const int end = start + m.value(ClipKey::frames).toInt();
                if (start >= position || end < position)
                    continue; // both halves ride, or neither does
                if (!unmixOn(track, m.value(ClipKey::index).toInt()))
                    return -1;
                ++broken;
                again = true;
                break;
            }
        }
    }
    m_brokenDissolves += broken;
    return broken;
}

bool Timeline::takeRippleFailure() {
    return std::exchange(m_rippleFailed, false);
}

int Timeline::takeBrokenDissolves() {
    const int n = m_brokenDissolves;
    m_brokenDissolves = 0;
    return n;
}

// The frames a placement will cover, without keeping a producer alive.
int Timeline::placementSpan(const QString &path, int in, int out, bool audioOnly,
                               double speed, bool videoOnly) {
    if (in >= 0 && out >= in)
        return out - in + 1;
    edit::Producer producer = makeLaneProducer(path, audioOnly, speed, videoOnly);
    if (!producer.is_valid())
        return 0;
    std::unique_ptr<edit::Producer> cut = boundedCut(producer, in, out);
    return cut ? cut->get_playtime() : 0;
}

// A snapshot entry given a new start. A sample-precise sound cut carries
// its exact timing beside the frame numbers, and a snapshot whose two
// disagree is refused whole: its timing moves by the same frames. False when
// that timing cannot be moved there (the entry is left as it was).
bool moveEntryStart(QVariantMap &entry, int start, const edit::Profile &profile) {
    if (!entry.contains(ClipKey::audioTiming)) {
        entry.insert(ClipKey::start, start);
        return true;
    }
    const QVariantMap moved = AudioTimeline::shifted(
        entry, {qint64(start - entry.value(ClipKey::start).toInt()) * profile.frame_rate_den(), profile.frame_rate_num()},
        profile.frame_rate_num(), profile.frame_rate_den());
    if (moved.isEmpty())
        return false;
    entry = moved;
    return true;
}

// Move every lane-`track` entry starting at/after `position` by delta,
// inside a snapshot (lanes place by absolute start).
void Timeline::shiftSnapshotLane(QVariantList *clips, int track, int position,
                                    int delta) const {
    for (int i = 0; i < clips->size(); ++i) {
        QVariantMap m = clips->at(i).toMap();
        if (m.value(ClipKey::lane).toInt() != track)
            continue;
        const int start = m.value(ClipKey::start).toInt();
        if (start >= position && m_profile && moveEntryStart(m, qMax(0, start + delta), *m_profile))
            (*clips)[i] = m;
    }
}
