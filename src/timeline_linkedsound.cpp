#include "timeline_p.h"

static std::atomic<quint64> narrowConformCount{0}, wholeConformCount{0};

bool Timeline::pathHasAudio(const QString &path) {
    if (path.isEmpty() || path.startsWith(QStringLiteral("color:")) || !m_profile)
        return false;
    const auto known = m_audioProbe.constFind(path);
    if (known != m_audioProbe.constEnd())
        return known.value();
    bool has = false;
    if (QFileInfo(path).isFile() && !ProjectFile::isProject(path)) {
        has = media::probeCached(path).hasAudio;
    } else if (edit::Producer probe(*m_profile, AnimatedWebP::resource(path).toUtf8().constData()); probe.is_valid()) {
        const int streams = probe.get_int("meta.media.nb_streams");
        for (int i = 0; i < streams && !has; ++i) {
            const char *type = probe.get(QStringLiteral("meta.media.%1.stream.type").arg(i).toUtf8().constData());
            has = type && qstrcmp(type, "audio") == 0;
        }
        if (streams == 0) // a non-avformat source: trust its audio index
            has = probe.get_int("audio_index") >= 0 && probe.get("audio_index");
    }
    if (m_audioProbe.size() > 512)
        m_audioProbe.clear();
    m_audioProbe.insert(path, has);
    return has;
}

// A video that was OVERWRITTEN by a new clip (replace, fit to fill,
// a paste over it) takes its sound with it — an overwrite replaces A1
// too. A video merely deleted leaves its audio (Alt-delete keeps it).
// The blade that makes room for the new clip splits the old sound
// too, so the piece under the new picture carries a link made by
// this very edit and matching no picture: an orphan as well. Sound
// that already had no picture before the edit is the editor's own.
bool Timeline::removeOverwrittenSound(const QVariantList &before) {
    bool changed = false;
    QSet<QString> beforeLinks, beforeAny, pictureLinks;
    for (const QVariant &v : before) {
        const QVariantMap m = v.toMap();
        beforeAny.insert(m.value(ClipKey::link).toString());
        if (m.value(ClipKey::track).toInt() == 0)
            beforeLinks.insert(m.value(ClipKey::link).toString());
    }
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        if (!m.value(ClipKey::audioOnly).toBool())
            pictureLinks.insert(m.value(ClipKey::link).toString());
    }
    QSet<QString> nowLinks;
    QList<QPair<int, int>> fresh; // [start, end) of V1 clips new since `before`
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        if (m.value(ClipKey::track).toInt() != 0 || m.value(ClipKey::gap).toBool())
            continue;
        const QString link = m.value(ClipKey::link).toString();
        nowLinks.insert(link);
        if (link.isEmpty() || !beforeLinks.contains(link))
            fresh.append({m.value(ClipKey::start).toInt(),
                          m.value(ClipKey::start).toInt() + m.value(ClipKey::frames).toInt()});
    }
    QList<QPair<int, int>> orphans; // (track, index), removed last-first
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        const QString link = m.value(ClipKey::link).toString();
        if (!m.value(ClipKey::audioOnly).toBool() || link.isEmpty()
            || nowLinks.contains(link) || pictureLinks.contains(link)
            || (beforeAny.contains(link) && !beforeLinks.contains(link)))
            continue;
        const int s = m.value(ClipKey::start).toInt();
        const int e = s + m.value(ClipKey::frames).toInt();
        for (const auto &span : fresh)
            if (span.first < e && s < span.second) {
                orphans.append({m.value(ClipKey::track).toInt(), m.value(ClipKey::index).toInt()});
                break;
            }
    }
    std::sort(orphans.begin(), orphans.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    for (const auto &o : orphans)
        if (!trackLocked(o.first) && removeOn(o.first, o.second, false))
            changed = true;
    return changed;
}

// Embedded sound leaves V1 for an audio track, linked to its video
// (a clip inside a dissolve keeps it — a detach there doubles frames).
bool Timeline::detachPrimarySound() {
    bool changed = false;
    // The main track's clips by index, read once. Asking for each clip by
    // its index rebuilt the whole list every time, and this runs after every
    // edit: an edit then took time with the square of the timeline's length.
    const auto primaryClips = [this] {
        QHash<int, QVariantMap> byIndex;
        for (const QVariant &v : entries()) {
            const QVariantMap e = v.toMap();
            if (e.value(ClipKey::track).toInt() == 0)
                byIndex.insert(e.value(ClipKey::index).toInt(), e);
        }
        return byIndex;
    };
    const QHash<int, QVariantMap> clips = primaryClips();
    for (int c = 0;; ++c) {
        const QVariantMap m = clips.value(c);
        if (m.isEmpty())
            break;
        if (m.value(ClipKey::gap).toBool() || m.value(ClipKey::videoOnly).toBool()
            || m.value(ClipKey::audioOnly).toBool()
            || !m.value(ClipKey::title).toString().isEmpty()
            || m.contains(ClipKey::compoundJson)
            || m.value(ClipKey::mixIn).toInt() > 0 || m.value(ClipKey::mixOut).toInt() > 0)
            continue;
        const QString path = m.value(ClipKey::path).toString();
        if (path.startsWith(QStringLiteral("color:")) || ProjectFile::isProject(path)
            || !pathHasAudio(path))
            continue;
        // A detach changes this clip alone on the main track: the others
        // read before it are still what they were.
        if (detachAudio(c))
            changed = true;
    }
    return changed;
}

// The same for a picture on a video track above V1: its sound goes
// to an audio track below the video, linked to it, so every sound
// in the timeline stacks under the pictures.
bool Timeline::detachUpperTrackSound() {
    bool changed = false;
    for (bool again = true; again;) {
        again = false;
        for (const QVariant &v : entries()) {
            const QVariantMap m = v.toMap();
            const int track = m.value(ClipKey::track).toInt();
            if (track <= 0 || m.value(ClipKey::gap).toBool() || m.value(ClipKey::videoOnly).toBool()
                || m.value(ClipKey::audioOnly).toBool() || trackLocked(track)
                || !m.value(ClipKey::title).toString().isEmpty()
                || m.contains(ClipKey::compoundJson) || m.value(ClipKey::adjust).toBool()
                || m.value(ClipKey::mixIn).toInt() > 0 || m.value(ClipKey::mixOut).toInt() > 0)
                continue;
            const QString path = m.value(ClipKey::path).toString();
            if (path.startsWith(QStringLiteral("color:")) || ProjectFile::isProject(path) || !pathHasAudio(path))
                continue;
            if (detachAudioOn(track, m.value(ClipKey::index).toInt())) {
                changed = true;
                again = true; // the entries moved on: look again
                break;
            }
        }
    }
    return changed;
}
quint64 Timeline::narrowConforms() { return narrowConformCount.load(); }
quint64 Timeline::wholeConforms() { return wholeConformCount.load(); }
void noteSettle(bool narrow) { ++(narrow ? narrowConformCount : wholeConformCount); }

// Linked audio follows its video. Only a video that CHANGED since
// `before` pulls its audio — an audio clip edited on its own stays
// where the editor put it. The audio maps are rewritten to mirror
// the video (span, material, rate, fades, dissolve) and put back as a
// rebuild from the snapshot places them, the way undo does: no lane op
// ripples anything — V1 already carried the other tracks along. Only
// the changed sound is put back where that is enough; otherwise the
// whole timeline is rebuilt.
bool Timeline::conformLinkedSound(const QVariantList &before) {
    bool changed = false;
    auto geo = [](const QVariantMap &m) {
        return QVariantList{m.value(ClipKey::start), m.value(ClipKey::in),
                            m.value(ClipKey::out), m.value(ClipKey::speed, 1.0), m.value(ClipKey::retime),
                            m.value(ClipKey::path), m.value(ClipKey::fadeIn, 0),
                            m.value(ClipKey::fadeOut, 0), m.value(ClipKey::mixOut, 0),
                            m.value(ClipKey::mixAlign, QString()), m.value(ClipKey::mixExtA, 0),
                            m.value(ClipKey::mixExtB, 0), m.value(ClipKey::mixIn, 0)};
    };
    // A compound on V1 holds pictures whose sound stayed outside on A1,
    // still carrying their links. Each visible piece counts as a V1 picture
    // here, where the compound shows it, so trimming the compound conforms
    // that sound like any clip's. A link a real V1 clip also carries (a
    // compound made over part of a clip) is left alone: it has no single
    // picture to follow.
    const auto withCompoundPictures = [](const QVariantList &list) {
        QVariantList out = list;
        QVariantList pieces;
        QHash<QString, int> uses;
        for (const QVariant &v : list) {
            const QVariantMap c = v.toMap();
            if (c.value(ClipKey::track).toInt() != 0)
                continue;
            ++uses[c.value(ClipKey::link).toString()];
            const QString json = c.value(ClipKey::compoundJson).toString();
            if (json.isEmpty())
                continue;
            const int wIn = c.value(ClipKey::in).toInt();
            const int wOut = c.value(ClipKey::out).toInt();
            int pos = 0;
            for (const QVariant &s : QJsonDocument::fromJson(json.toUtf8()).toVariant().toList()) {
                const QVariantMap sub = s.toMap();
                const int sIn = sub.value(ClipKey::in).toInt();
                const int len = sub.value(ClipKey::out).toInt() - sIn + 1;
                pos += qMax(0, sub.value(ClipKey::gapBefore).toInt()); // empty space inside, as buildCompound lays it
                const int from = qMax(pos, wIn), to = qMin(pos + len - 1, wOut);
                const QString link = sub.value(ClipKey::link).toString();
                if (from <= to && !link.isEmpty() && sub.value(ClipKey::videoOnly).toBool()) {
                    QVariantMap p = sub;
                    p.insert(ClipKey::track, 0);
                    p.insert(ClipKey::start, c.value(ClipKey::start).toInt() + from - wIn);
                    p.insert(ClipKey::in, sIn + from - pos);
                    p.insert(ClipKey::out, sIn + to - pos);
                    p.insert(ClipKey::frames, to - from + 1);
                    p.remove(ClipKey::fadeIn);
                    p.remove(ClipKey::fadeOut);
                    pieces.append(p);
                    ++uses[link];
                }
                pos += len;
            }
        }
        for (const QVariant &p : std::as_const(pieces))
            if (uses.value(p.toMap().value(ClipKey::link).toString()) == 1)
                out.append(p);
        return out;
    };
    QHash<QString, QVariantMap> was, wasAudio;
    for (const QVariant &v : withCompoundPictures(before)) {
        const QVariantMap m = v.toMap();
        const QString link = m.value(ClipKey::link).toString();
        // A picture on any video track: V1's, or one on a track above it.
        if (!link.isEmpty() && !m.value(ClipKey::audioOnly).toBool()
            && (m.value(ClipKey::track).toInt() == 0 || !was.contains(link)))
            was.insert(link, m);
        else if (m.value(ClipKey::audioOnly).toBool() && !link.isEmpty() && !wasAudio.contains(link))
            wasAudio.insert(link, m);
    }
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    QHash<QString, int> audioAt; // link → index in `clips`
    for (int i = 0; i < clips.size(); ++i) {
        const QVariantMap m = clips.at(i).toMap();
        const QString link = m.value(ClipKey::link).toString();
        if (m.value(ClipKey::audioOnly).toBool() && !link.isEmpty() && !audioAt.contains(link))
            audioAt.insert(link, i);
    }
    bool dirty = false;
    QList<int> moved; // places in `clips` of the sound that has to change
    // A multicam clip with a sound source of its own (fixedSoundOf): its
    // sound is that source where it lies under the picture, not the picture's
    // own. The sound may be shorter than its picture, so a blade outside it
    // cuts the picture alone and the sound may then lie under the other
    // part: a sound is the picture's it lies under most, which takes its link
    // (the sound keeps its level and its other settings); a part left with
    // none has no link to one.
    QSet<QString> shared; // links whose sound changed hands or lost a picture: conformed to its picture, changed or not
    QSet<int> loose;      // pictures left with no sound and no link to one
    QSet<int> dropped;    // sounds whose source has nothing under their picture
    bool reshaped = false; // a picture changed, a clip came or went: the whole rebuild's
    const auto plainPicture = [](const QVariantMap &m) {
        return !m.contains(ClipKey::retime) && !m.contains(ClipKey::freeze) && !m.contains(ClipKey::compoundJson)
               && qFuzzyCompare(m.value(ClipKey::speed, 1.0).toDouble(), 1.0);
    };
    {
        QHash<QString, QList<int>> linked; // link → the pictures of such clips that carry it, where a sound does too
        QList<int> unlinked;               // pictures of such clips with no link
        for (int i = 0; i < clips.size(); ++i) {
            const QVariantMap m = clips.at(i).toMap();
            if (m.value(ClipKey::audioOnly).toBool())
                continue;
            const QString link = m.value(ClipKey::link).toString();
            if (link.isEmpty() ? plainPicture(m) && !fixedSoundOf(m).isEmpty() : audioAt.contains(link) && !fixedSoundOf(m).isEmpty())
                (link.isEmpty() ? unlinked : linked[link]).append(i);
        }
        for (auto it = linked.cbegin(); it != linked.cend(); ++it) {
            const QVariantMap sound = clips.at(audioAt.value(it.key())).toMap();
            if (trackLocked(sound.value(ClipKey::track).toInt()))
                continue;
            const int from = sound.value(ClipKey::start).toInt(), to = from + sound.value(ClipKey::frames).toInt();
            const auto under = [&](int i) { // how much of the sound, as it stands, lies under what picture `i` would play
                QVariantMap wanted = sound;
                if (!fixedSoundFor(clips.at(i).toMap(), wanted))
                    return 0;
                const int start = wanted.value(ClipKey::start).toInt();
                return qMax(0, qMin(to, start + wanted.value(ClipKey::frames).toInt()) - qMax(from, start));
            };
            int owner = it.value().first(), most = -1;
            if (it.value().size() == 1) {
                // Its one picture as it was, or all of the sound under it
                // still (every edit but a blade outside the sound, or a
                // trim): the main pass's, as any linked sound.
                const QVariantMap picture = clips.at(owner).toMap(), bv = was.value(it.key());
                if (unlinked.isEmpty() || (!bv.isEmpty() && geo(picture) == geo(bv)))
                    continue;
                most = under(owner);
                if (most == to - from)
                    continue;
            }
            for (const int i : it.value())
                if (const int lies = i == owner && most >= 0 ? most : under(i); lies > most || (lies == most && i == owner)) { most = lies; owner = i; }
            for (const int i : std::as_const(unlinked))
                if (const int lies = under(i); lies > most) { most = lies; owner = i; }
            if (unlinked.removeOne(owner)) {
                QVariantMap m = clips.at(owner).toMap();
                m.insert(ClipKey::link, it.key());
                m.insert(ClipKey::videoOnly, true);
                clips[owner] = m;
                reshaped = dirty = true;
            }
            shared.insert(it.key());
            for (const int i : it.value()) {
                if (i == owner) continue;
                QVariantMap m = clips.at(i).toMap();
                m.remove(ClipKey::link);
                clips[i] = m;
                loose.insert(i);
                reshaped = dirty = true;
            }
        }
    }
    const QVariantList pictures = withCompoundPictures(clips);
    for (int place = 0; place < pictures.size(); ++place) {
        const QVariantMap v = pictures.at(place).toMap();
        const QString link = v.value(ClipKey::link).toString();
        if (v.value(ClipKey::audioOnly).toBool() || !v.value(ClipKey::videoOnly).toBool()
            || link.isEmpty() || !audioAt.contains(link))
            continue;
        const QVariantMap bv = was.value(link);
        if (!bv.isEmpty() && geo(v) == geo(bv) && !shared.contains(link))
            continue;
        QVariantMap a = clips.at(audioAt.value(link)).toMap();
        if (trackLocked(a.value(ClipKey::track).toInt()))
            continue;
        const QString path = v.value(ClipKey::path).toString();
        const bool otherAngle = a.value(ClipKey::path).toString() != path
                                && !v.value(ClipKey::mcam).toString().isEmpty()
                                && !m_audioFollowsVideo;
        a.insert(ClipKey::start, v.value(ClipKey::start));
        const bool ownSource = !fixedSoundOf(v).isEmpty();
        if (ownSource) {
            if (!fixedSoundFor(v, a)) {
                // The source has nothing under this picture: it has no sound,
                // and no link to one (a compound's piece keeps what it has).
                if (place < clips.size()) {
                    QVariantMap picture = v;
                    picture.remove(ClipKey::link);
                    clips[place] = picture;
                    dropped.insert(audioAt.value(link));
                    reshaped = dirty = true;
                }
                continue;
            }
        } else if (otherAngle) {
            // The sound keeps its own angle: the video's trims by delta,
            // measured from where the sound was BEFORE the edit — a blade
            // already cut the sound with the picture, so adding the delta
            // to the bladed sound would trim it twice.
            int dIn = bv.isEmpty() ? 0 : v.value(ClipKey::in).toInt() - bv.value(ClipKey::in).toInt();
            int dOut = bv.isEmpty() ? 0 : v.value(ClipKey::out).toInt() - bv.value(ClipKey::out).toInt();
            if(!bv.isEmpty() && v.value(ClipKey::mcamAngle)!=bv.value(ClipKey::mcamAngle)
                && multicamDefinition(v).value("version").toInt()==2) {
                // Changing angles changes producer coordinates, not the sound's
                // virtual time. Retained sound follows only an actual trim.
                const auto from=multicamPoint(bv,multicamDefinition(bv),0,fps());
                const auto to=multicamPoint(v,multicamDefinition(v),0,fps());
                const double speed=bv.value(ClipKey::speed,1.).toDouble();
                dIn=from.valid && to.valid && std::isfinite(speed) && speed!=0.
                    ? qRound((to.seconds-from.seconds)*fps()/speed)
                    :v.value(ClipKey::start).toInt()-bv.value(ClipKey::start).toInt();
                dOut=dIn+(v.value(ClipKey::out).toInt()-v.value(ClipKey::in).toInt())
                    -(bv.value(ClipKey::out).toInt()-bv.value(ClipKey::in).toInt());
            }
            const QVariantMap base = !bv.isEmpty() && wasAudio.contains(link) ? wasAudio.value(link) : a;
            a.insert(ClipKey::in, qMax(0, base.value(ClipKey::in).toInt() + dIn));
            a.insert(ClipKey::out, qMax(a.value(ClipKey::in).toInt(), base.value(ClipKey::out).toInt() + dOut));
        } else {
            if (a.value(ClipKey::path).toString() != path) {
                // Source interpretation belongs to the replacement media too.
                // Retaining the old audio hint would import it under the new
                // path during rebuild and retime an already baked render again.
                if (v.contains(ClipKey::sourceMeta))
                    a.insert(ClipKey::sourceMeta, v.value(ClipKey::sourceMeta));
                else
                    a.remove(ClipKey::sourceMeta);
            }
            // Volume automation is counted from the clip's first frame. A new
            // head moves the material under it, so the keys move the other
            // way and stay on the sound they were set on, as a picture's own
            // keys do when it is trimmed. (A new source or speed is not a trim.)
            const int headMoved = v.value(ClipKey::in).toInt() - a.value(ClipKey::in).toInt();
            if (headMoved != 0 && a.value(ClipKey::path).toString() == path
                && qFuzzyCompare(a.value(ClipKey::speed, 1.0).toDouble(), v.value(ClipKey::speed, 1.0).toDouble())
                && !v.contains(ClipKey::retime) && !a.contains(ClipKey::retime)
                && !a.value(ClipKey::volumeKf).toList().isEmpty()) {
                QVariantList keys;
                for (const QVariant &k : a.value(ClipKey::volumeKf).toList()) {
                    QVariantMap key = k.toMap();
                    key.insert(QStringLiteral("frame"), key.value(QStringLiteral("frame")).toInt() - headMoved);
                    keys.append(key);
                }
                a[ClipKey::volumeKf] = keys;
            }
            a.insert(ClipKey::path, path);
            a.insert(ClipKey::in, v.value(ClipKey::in));
            a.insert(ClipKey::out, v.value(ClipKey::out));
            {
                // Its volume automation is in clip frames: a new speed rescales
                // it as the picture's own keys are.
                const double was = qAbs(a.value(ClipKey::speed, 1.0).toDouble());
                const double now = qAbs(v.value(ClipKey::speed, 1.0).toDouble());
                if (was > 0 && now > 0 && !qFuzzyCompare(was, now) && !v.contains(ClipKey::retime) && !a.contains(ClipKey::retime)
                    && !a.value(ClipKey::volumeKf).toList().isEmpty())
                    a[ClipKey::volumeKf] = EditEngine::scaledCurve(a.value(ClipKey::volumeKf).toList(), was / now);
            }
            a.insert(ClipKey::speed, v.value(ClipKey::speed, 1.0));
            if(v.contains(ClipKey::sourceOrigin))a[ClipKey::sourceOrigin]=v.value(ClipKey::sourceOrigin);
            else a.remove(ClipKey::sourceOrigin);
            if (v.contains(ClipKey::retime)) a[ClipKey::retime] = v.value(ClipKey::retime);
            else a.remove(ClipKey::retime);
            a.insert(ClipKey::length, v.value(ClipKey::length));
            a.insert(ClipKey::frames, v.value(ClipKey::frames));
        }
        // (The sound of a multicam clip with a sound source of its own takes
        // the fade of an edge it shares with its picture, and no transition:
        // the sound each side of one is the same source, and plays through.)
        const bool sameHead = a.value(ClipKey::start).toInt() == v.value(ClipKey::start).toInt();
        const bool sameTail = a.value(ClipKey::start).toInt() + a.value(ClipKey::frames).toInt()
                              == v.value(ClipKey::start).toInt() + v.value(ClipKey::frames).toInt();
        for (const QString &key : {ClipKey::fadeIn, ClipKey::fadeOut, ClipKey::mixOut, ClipKey::mixIn, ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB})
            if (v.contains(key) && (!ownSource || (sameHead && key == ClipKey::fadeIn) || (sameTail && key == ClipKey::fadeOut)))
                a.insert(key, v.value(key));
            else
                a.remove(key);
        a.remove(ClipKey::mixStyle); // a crossfade has no wipe
        auto oldAudio = clips.at(audioAt.value(link)).toMap();
        // entries() supplies an empty style even when no marker is attached.
        if (oldAudio.value(ClipKey::mixStyle).toString().isEmpty()) oldAudio.remove(ClipKey::mixStyle);
        auto withoutFades = a;
        for (const QString &key : {ClipKey::fadeIn, ClipKey::fadeOut}) {
            oldAudio.remove(key); withoutFades.remove(key);
        }
        // Already where its picture is, fades and all (a ripple carried it
        // there): nothing to do. Every clip downstream of a ripple comes
        // through here.
        const QVariantMap standing = clips.at(audioAt.value(link)).toMap();
        if (oldAudio == withoutFades && standing.value(ClipKey::fadeIn, 0).toInt() == a.value(ClipKey::fadeIn, 0).toInt()
            && standing.value(ClipKey::fadeOut, 0).toInt() == a.value(ClipKey::fadeOut, 0).toInt())
            continue;
        if (oldAudio == withoutFades
            && setFadesOn(a.value(ClipKey::track).toInt(), a.value(ClipKey::index).toInt(),
                          a.value(ClipKey::fadeIn).toInt(), a.value(ClipKey::fadeOut).toInt())) {
            // A linked fade needs only its audio filters updated. Reopening
            // every clip here made even a short fade handle release expensive.
            clips[audioAt.value(link)] = a;
            changed = true;
            continue;
        }
        clips[audioAt.value(link)] = a;
        moved.append(audioAt.value(link));
        dirty = true;
    }
    // A sound given its picture's transition crosses over with the sound
    // beside it on its own track. Where that is not the other picture's
    // sound (it lies on another track, as the sound of a clip put in a
    // picture's place on an upper track does), there is nothing to cross
    // over with: the sound keeps its plain cut, in step with its picture,
    // instead of frames counted from a crossfade that is not there.
    bool crossing = false; // a conformed sound is in a transition: the usual edit has none, and looks no further
    for (const int place : std::as_const(moved)) {
        const QVariantMap m = clips.at(place).toMap();
        crossing = crossing || m.value(ClipKey::mixOut).toInt() > 0 || m.value(ClipKey::mixIn).toInt() > 0;
    }
    if (dirty && crossing) {
        // (Found by where they lie, as conformed: the sounds' places in their
        // track's order are those of before, and a ripple may have changed it.)
        QHash<QPair<int, int>, int> startsAt, endsAt, pictureAt; // (track, frame) and (track, index) → place in `clips`
        for (int i = 0; i < clips.size(); ++i) {
            const QVariantMap m = clips.at(i).toMap();
            const int track = m.value(ClipKey::track).toInt(), start = m.value(ClipKey::start).toInt();
            if (m.value(ClipKey::audioOnly).toBool()) {
                startsAt.insert({track, start}, i);
                endsAt.insert({track, start + m.value(ClipKey::frames).toInt()}, i);
            } else {
                pictureAt.insert({track, m.value(ClipKey::index).toInt()}, i);
            }
        }
        const auto crosses = [&](int first, int second) {
            if (first < 0 || second < 0)
                return false;
            const QVariantMap a = clips.at(first).toMap(), b = clips.at(second).toMap();
            return a.value(ClipKey::mixOut).toInt() > 0 && b.value(ClipKey::mixIn).toInt() == a.value(ClipKey::mixOut).toInt();
        };
        for (const int place : QList<int>(moved)) {
            QVariantMap a = clips.at(place).toMap();
            if (a.contains(ClipKey::audioTiming))
                continue;
            const int track = a.value(ClipKey::track).toInt(), start = a.value(ClipKey::start).toInt();
            if (const MixParts parts = mixParts(a); parts.extA + parts.extB > 0 && parts.overlap == 0
                                                    && !crosses(place, startsAt.value({track, start + a.value(ClipKey::frames).toInt()}, -1))) {
                a.insert(ClipKey::out, a.value(ClipKey::out).toInt() - parts.extA);
                a.insert(ClipKey::frames, a.value(ClipKey::frames).toInt() - parts.extA);
                for (const QString &key : {ClipKey::mixOut, ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB})
                    a.remove(key);
            }
            if (a.value(ClipKey::mixIn).toInt() > 0 && !crosses(endsAt.value({track, start}, -1), place)) {
                // The transition's sides, from the picture before this sound's picture.
                MixParts parts;
                for (const QVariant &pv : pictures) {
                    const QVariantMap picture = pv.toMap();
                    if (picture.value(ClipKey::audioOnly).toBool() || picture.value(ClipKey::link) != a.value(ClipKey::link))
                        continue;
                    const int before = pictureAt.value({picture.value(ClipKey::track).toInt(), picture.value(ClipKey::index).toInt() - 1}, -1);
                    if (before >= 0)
                        parts = mixParts(clips.at(before).toMap());
                    break;
                }
                if (parts.extA + parts.extB == a.value(ClipKey::mixIn).toInt() && parts.overlap == 0) {
                    a.insert(ClipKey::start, a.value(ClipKey::start).toInt() - parts.extA);
                    a.insert(ClipKey::in, a.value(ClipKey::in).toInt() + parts.extB);
                    rebaseEntryKeys(a, parts.extB); // its level keys stay on the sound they were set on
                    a.insert(ClipKey::frames, a.value(ClipKey::frames).toInt() + parts.extA);
                    a.remove(ClipKey::mixIn);
                }
            }
            clips[place] = a;
        }
    }
    // A crossfade is two sounds' business. When the picture of one of them
    // has gone and its sound stays (a picture deleted alone), only the other
    // sound was given its picture's plain cut above: the one left would keep
    // half a crossfade, its frames counted from the crossfade's edge, and be
    // laid a crossfade's length away from where it played. It keeps the
    // frames it played alone, where it played them.
    if (dirty) {
        QHash<QPair<int, int>, int> at; // (track, index) → place in `clips`
        for (int i = 0; i < pristine.size(); ++i) {
            const QVariantMap m = pristine.at(i).toMap();
            if (m.value(ClipKey::audioOnly).toBool())
                at.insert({m.value(ClipKey::track).toInt(), m.value(ClipKey::index).toInt()}, i);
        }
        for (auto it = at.cbegin(); it != at.cend(); ++it) {
            const int first = it.value(), second = at.value({it.key().first, it.key().second + 1}, -1);
            if (second < 0)
                continue;
            const QVariantMap wasA = pristine.at(first).toMap(), wasB = pristine.at(second).toMap();
            const MixParts parts = mixParts(wasA);
            if (parts.extA + parts.extB + parts.overlap < 1 || wasB.value(ClipKey::mixIn).toInt() < 1)
                continue;
            QVariantMap a = clips.at(first).toMap(), b = clips.at(second).toMap();
            const bool aPlain = a.value(ClipKey::mixOut).toInt() < 1, bPlain = b.value(ClipKey::mixIn).toInt() < 1;
            if (aPlain == bPlain)
                continue; // both followed their pictures, or neither had to
            if (aPlain) {
                if (b != wasB || trackLocked(b.value(ClipKey::track).toInt()))
                    continue;
                b.insert(ClipKey::start, b.value(ClipKey::start).toInt() - parts.extA);
                b.insert(ClipKey::in, b.value(ClipKey::in).toInt() + parts.extB + parts.overlap);
                rebaseEntryKeys(b, parts.extB + parts.overlap); // its level keys stay on the sound they were set on
                b.insert(ClipKey::frames, b.value(ClipKey::frames).toInt() + parts.extA);
                b.remove(ClipKey::mixIn);
                clips[second] = b;
                moved.append(second);
            } else {
                if (a != wasA || trackLocked(a.value(ClipKey::track).toInt()))
                    continue;
                a.insert(ClipKey::out, a.value(ClipKey::out).toInt() - parts.extA);
                a.insert(ClipKey::frames, a.value(ClipKey::frames).toInt() - parts.extA);
                for (const QString &key : {ClipKey::mixOut, ClipKey::mixAlign, ClipKey::mixExtA, ClipKey::mixExtB})
                    a.remove(key);
                clips[first] = a;
                moved.append(first);
            }
        }
    }
    // A picture of such a clip that has no sound and no link to one (its
    // first angle had none to detach, or its source had nothing under it
    // until now) is given the source's sound where that lies under it. Only
    // a picture new or changed in what it shows: one moved is left as it is.
    // (A picture whose sound the editor removed keeps its link to it, and
    // so do the parts it is bladed into: none of them is given another.)
    struct NewSound { QVariantMap sound; QString link; int sampling; };
    QList<NewSound> wanted;
    {
        const auto shows = [](const QVariantMap &m) {
            return QVariantList{m.value(ClipKey::in), m.value(ClipKey::out), m.value(ClipKey::path), m.value(ClipKey::mcamAngle)};
        };
        QList<QVariantList> stood;
        bool looked = false;
        for (int i = 0; i < clips.size(); ++i) {
            QVariantMap m = clips.at(i).toMap();
            if (m.value(ClipKey::audioOnly).toBool() || !m.value(ClipKey::link).toString().isEmpty()
                || !plainPicture(m) || fixedSoundOf(m).isEmpty())
                continue;
            if (!std::exchange(looked, true))
                for (const QVariant &b : before) {
                    const QVariantMap stoodAs = b.toMap();
                    if (!stoodAs.value(ClipKey::audioOnly).toBool() && stoodAs.value(ClipKey::link).toString().isEmpty() && !fixedSoundOf(stoodAs).isEmpty())
                        stood.append(shows(stoodAs));
                }
            QVariantMap sound;
            if (!fixedSoundFor(m, sound))
                continue;
            const int from = sound.value(ClipKey::start).toInt(), to = from + sound.value(ClipKey::frames).toInt();
            const auto sounding = [&](const QVariantList &list, bool now) {
                for (int j = 0; j < list.size(); ++j) {
                    const QVariantMap other = list.at(j).toMap();
                    const int start = other.value(ClipKey::start).toInt();
                    if (!(now && dropped.contains(j)) && other.value(ClipKey::audioOnly).toBool() && other.value(ClipKey::path) == sound.value(ClipKey::path)
                        && start < to && from < start + other.value(ClipKey::frames).toInt())
                        return true;
                }
                return false;
            };
            // (Its source already sounding there: the editor's own, unlinked,
            // or a sound on a locked track that plays on under several parts.)
            if (sounding(clips, true))
                continue;
            // As it stood, with no sound: left so, unless the source sounded
            // under it until this edit (a sound that lay under several parts
            // was cut back to the one it belongs to).
            if (!loose.contains(i) && stood.contains(shows(m)) && !sounding(before, false))
                continue;
            const QString link = newLinkId();
            m.insert(ClipKey::link, link);
            m.insert(ClipKey::videoOnly, true);
            clips[i] = m;
            wanted.append({sound, link, m.value(ClipKey::frameBlend).toInt()});
            reshaped = dirty = true;
        }
    }
    if (dirty) {
        // The restore decorates "the clip just placed": lane entries must
        // come in placement order (lane, then start) once starts moved.
        QVariantList ordered, lanes;
        for (int i = 0; i < clips.size(); ++i)
            if (!dropped.contains(i))
                (clips.at(i).toMap().value(ClipKey::lane).toInt() >= 1 ? lanes : ordered).append(clips.at(i));
        std::stable_sort(lanes.begin(), lanes.end(), [](const QVariant &x, const QVariant &y) {
            const QVariantMap a = x.toMap(), b = y.toMap();
            const int la = a.value(ClipKey::lane).toInt(), lb = b.value(ClipKey::lane).toInt();
            return la != lb ? la < lb
                            : a.value(ClipKey::start).toInt() < b.value(ClipKey::start).toInt();
        });
        ordered += lanes;
        // Only the sound that changed is put back when that gives what the
        // whole rebuild would: a trim then costs the same on a long timeline
        // as on a short one. OMAEDIT_NARROW_CONFORM=0 always rebuilds;
        // =verify does both and stops if they differ.
        const QByteArray mode = qgetenv("OMAEDIT_NARROW_CONFORM");
        const bool narrow = !reshaped && mode != "0" && replaceLaneEntries(pristine, clips, moved);
        if (narrow && mode == "verify") {
            const QVariantList narrowed = entries();
            if (rebuild(ordered, pristine) && entries() != narrowed)
                qFatal("the narrow sound conform differs from the whole rebuild");
        }
        if (narrow) noteSettle(true);
        if (narrow || rebuild(ordered, pristine))
            changed = true;
        else
            wanted.clear();
        for (const NewSound &fresh : std::as_const(wanted)) {
            const QString path = fresh.sound.value(ClipKey::path).toString();
            const int start = fresh.sound.value(ClipKey::start).toInt();
            QScopedValueRollback<int> sampling(m_restoreSampling, fresh.sampling);
            int t = -1, c = -1;
            if (withSourceOrigin(fresh.sound, [&] {
                    return connectAt(path, start, fresh.sound.value(ClipKey::in).toInt(), fresh.sound.value(ClipKey::out).toInt(), true);
                }) && placedAudioAt(path, start, &t, &c))
                setLinkAt(t, c, fresh.link);
        }
    }
    return changed;
}

void Timeline::noteRestoreSources(const QVariantList &clips, const QSet<QString> &paths) {
    std::function<void(const QVariantList &, bool)> remember = [&](const QVariantList &list, bool top) {
        for (const QVariant &v : list) {
            const QVariantMap m = v.toMap();
            const QString path = m.value(ClipKey::path).toString();
            if (paths.contains(path)) {
                if (top) {
                    const int sourceLength = m.contains(ClipKey::retime)
                        ? qCeil(m.value(ClipKey::retime).toMap().value(QStringLiteral("sourceLength")).toDouble())
                        : qRound(m.value(ClipKey::length).toInt() * qAbs(m.value(ClipKey::speed, 1.0).toDouble()));
                    m_offlineLengths.insert(path, qMax(m_offlineLengths.value(path), sourceLength));
                }
                if (!m_mediaSources.contains(path) && m.contains(ClipKey::sourceMeta))
                    m_mediaSources.insert(path, m.value(ClipKey::sourceMeta));
            }
            const QString compound = m.value(ClipKey::compoundJson).toString();
            if (!compound.isEmpty())
                remember(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(), false);
        }
    };
    remember(clips, true);
}

// One clip of the main track put back in its own place: taken out, made again
// from `entry` and dressed, by the code a whole restore appends a clip with.
bool Timeline::replacePrimaryEntry(int index, const QVariantMap &entry) {
    if (!m_profile || !primary() || trackLocked(0))
        return false;
    const int at = entryIndexFor(index);
    const ClipEntry e = ClipEntry::read(entry);
    // The whole rebuild's: a compound, a gap, a dissolve at either edge, a
    // change of length (the rest of the track would have to move).
    if (at < 0 || e.gap || e.lane != 0 || !ClipEntry::orDefault(e.compoundJson).isEmpty()
        || e.mixIn != 0 || e.mixOut != 0 || e.in < 0 || e.out < e.in
        || primary()->is_blank(at) || primary()->clip_length(at) != e.out - e.in + 1
        || (at > 0 && primary()->is_mix(at - 1))
        || (at + 1 < primary()->count() && primary()->is_mix(at + 1)))
        return false;
    QScopedValueRollback<bool> restoring(m_restoring, true);
    QScopedValueRollback<QVariantMap> sourceScope(m_mediaSources);
    QScopedValueRollback<QHash<QString, int>> lengthScope(m_offlineLengths);
    noteRestoreSources(entries(), {e.path});
    QScopedValueRollback<int> sampling(m_restoreSampling, ClipEntry::orDefault(e.frameBlend));
    std::optional<double> origin;
    if (!SourceTiming::readOrigin(e.raw, origin))
        return false;
    QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin, origin);
    const QVariantMap retime = e.retimeMap();
    edit::Producer producer = makeClipProducer(e.path, e.in, e.out, e.speed, e.gap, e.videoOnly, retime);
    if (!producer.is_valid() || (!retime.isEmpty() && e.out >= producer.get_length()))
        return false;
    if (primary()->remove(at) != 0 || primary()->insert(producer, at, e.in, e.out) != 0)
        return false;
    return dressPrimaryEntry(e, index);
}

bool Timeline::replaceLaneEntries(const QVariantList &was, const QVariantList &now, const QList<int> &moved) {
    if (moved.isEmpty() || !m_profile)
        return false;
    struct Change { int lane, index, start, place; QVariantMap entry; };
    QList<Change> changes;
    QSet<int> lanes, places;
    QSet<QString> paths;
    for (const int at : moved) {
        const QVariantMap old = was.at(at).toMap(), a = now.at(at).toMap();
        const int lane = old.value(ClipKey::lane).toInt();
        // What the whole rebuild is still needed for: a changed source or
        // speed, a speed map, a dissolve, a multicam sound on its own angle
        // (its length is not the picture's), a precise sound track.
        if (lane < 1 || lane >= trackCount() || a.value(ClipKey::lane).toInt() != lane
            || a.value(ClipKey::track).toInt() != lane || places.contains(at)
            || trackLocked(lane) || !m_tracks[size_t(lane)].audioEntries.isEmpty()
            || old.value(ClipKey::audioOnly).toBool() != a.value(ClipKey::audioOnly).toBool()
            || old.value(ClipKey::path) != a.value(ClipKey::path)
            || old.value(ClipKey::speed, 1.0) != a.value(ClipKey::speed, 1.0)
            || old.contains(ClipKey::retime) || a.contains(ClipKey::retime)
            || old.contains(ClipKey::audioTiming) || a.contains(ClipKey::audioTiming)
            || !a.value(ClipKey::mcam).toString().isEmpty()
            || !a.value(ClipKey::title).toString().isEmpty() || a.contains(ClipKey::compoundJson)
            || old.value(ClipKey::mixIn).toInt() != 0 || old.value(ClipKey::mixOut).toInt() != 0
            || a.value(ClipKey::mixIn).toInt() != 0 || a.value(ClipKey::mixOut).toInt() != 0)
            return false;
        const int in = a.value(ClipKey::in).toInt(), out = a.value(ClipKey::out).toInt();
        const int start = a.value(ClipKey::start).toInt();
        if (in < 0 || out < in || start < 0 || a.value(ClipKey::frames).toInt() != out - in + 1)
            return false;
        places.insert(at);
        lanes.insert(lane);
        paths.insert(a.value(ClipKey::path).toString());
        changes.append({lane, old.value(ClipKey::index).toInt(), start, -1, a});
    }
    // A dissolve anywhere on the track moves what follows it, and a clip put
    // back onto another would cut into it where the rebuild's order decides
    // which of the two gives way: both are the rebuild's.
    for (const int lane : std::as_const(lanes)) {
        const edit::Playlist *pl = laneAt(lane - 1);
        for (int i = 0; i < pl->count(); ++i)
            if (pl->is_mix(i))
                return false;
    }
    QHash<int, QList<QPair<int, int>>> spans; // lane → [start, end)
    for (int i = 0; i < now.size(); ++i) {
        const QVariantMap m = now.at(i).toMap();
        const int lane = m.value(ClipKey::lane).toInt();
        if (!lanes.contains(lane))
            continue;
        if (m.value(ClipKey::mixIn).toInt() != 0 || m.value(ClipKey::mixOut).toInt() != 0)
            return false;
        const int start = m.value(ClipKey::start).toInt();
        spans[lane].append({start, start + m.value(ClipKey::frames).toInt()});
    }
    for (auto &list : spans) {
        std::sort(list.begin(), list.end());
        for (int i = 1; i < list.size(); ++i)
            if (list.at(i).first < list.at(i - 1).second)
                return false;
    }
    // Where each will stand among its track's clips once all are back. They
    // go back in order, so it is also where each stands as it lands.
    for (Change &c : changes) {
        const auto &list = spans[c.lane];
        for (int i = 0; i < list.size() && c.place < 0; ++i)
            if (list.at(i).first == c.start)
                c.place = i;
        if (c.place < 0)
            return false;
    }
    // As a restore sets itself up, for the sources these clips read.
    QScopedValueRollback<bool> restoring(m_restoring, true);
    QScopedValueRollback<QVariantMap> sourceScope(m_mediaSources);
    QScopedValueRollback<QHash<QString, int>> lengthScope(m_offlineLengths);
    noteRestoreSources(now, paths);
    // Every one comes off before any goes back: none can land on another's
    // old place. Highest index first, so the ones still to go keep theirs.
    std::sort(changes.begin(), changes.end(), [](const Change &x, const Change &y) {
        return x.lane != y.lane ? x.lane < y.lane : x.index > y.index;
    });
    for (const Change &c : std::as_const(changes)) {
        const int entry = entryIndexOn(c.lane, c.index);
        if (entry < 0)
            return false;
        delete laneAt(c.lane - 1)->replace_with_blank(entry);
    }
    std::sort(changes.begin(), changes.end(), [](const Change &x, const Change &y) {
        return x.lane != y.lane ? x.lane < y.lane : x.start < y.start;
    });
    for (const Change &c : std::as_const(changes)) {
        const ClipEntry e = ClipEntry::read(c.entry);
        QScopedValueRollback<int> sampling(m_restoreSampling, ClipEntry::orDefault(e.frameBlend));
        std::optional<double> origin;
        if (!SourceTiming::readOrigin(e.raw, origin))
            return false;
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin, origin);
        bool landed = false;
        if (!restoreLaneEntry(e, c.start, c.place, false, &landed))
            return false;
    }
    return true;
}

bool Timeline::settleLinkedAudio(const QVariantList &before, bool keepOverwritten) {
    if (!m_profile || !primary())
        return false;
    bool changed = false;
    // Each step runs whatever the one before it found.
    if (!keepOverwritten)
        changed = removeOverwrittenSound(before) || changed;
    changed = detachPrimarySound() || changed;
    changed = detachUpperTrackSound() || changed;
    changed = conformLinkedSound(before) || changed;
    return changed;
}

bool Timeline::detachAudioOn(int track, int index) {
    if (track == 0)
        return detachAudio(index);
    if (trackLocked(track))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    if (map.value(ClipKey::audioOnly).toBool()
        || map.value(ClipKey::videoOnly).toBool()
        || !map.value(ClipKey::title).toString().isEmpty()
        || map.value(ClipKey::mixOut).toInt() > 0
        || map.value(ClipKey::mixIn).toInt() > 0)
        return false;
    const QString path = map.value(ClipKey::path).toString();
    if (path.startsWith(QStringLiteral("color:")) || !pathHasAudio(path))
        return false; // nothing to detach from a silent file
    const int start = map.value(ClipKey::start).toInt();
    const int in = map.value(ClipKey::in).toInt();
    const int out = map.value(ClipKey::out).toInt();
    const double volume = map.value(ClipKey::volume).toDouble();
    const double speed = map.value(ClipKey::speed, 1.0).toDouble();
    map.insert(ClipKey::videoOnly, true);
    map.insert(ClipKey::volume, 0.0);
    clips[at] = map;
    if (!rebuild(clips, pristine))
        return false;
    QScopedValueRollback<int> sampling(m_restoreSampling,map.value(ClipKey::frameBlend).toInt());
    if (!withSourceOrigin(map,[&]{return connectAt(path,start,in,out,true,-1,speed,false,map.value(ClipKey::retime).toMap());})) {
        restore(pristine);
        return false;
    }
    int t = -1, c = -1;
    if (volume != 0.0 && placedAudioAt(path, start, &t, &c))
        setVolumeAt(t, c, volume);
    linkDetached(track, index, map.value(ClipKey::link).toString());
    return true;
}

bool Timeline::placedAudioAt(const QString &path, int start, int *track, int *index) const {
    for (const QVariant &v : entries()) {
        const QVariantMap m = v.toMap();
        if (m.value(ClipKey::audioOnly).toBool()
            && m.value(ClipKey::path).toString() == path
            && m.value(ClipKey::start).toInt() == start
            && m.value(ClipKey::link).toString().isEmpty()) {
            *track = m.value(ClipKey::track).toInt();
            *index = m.value(ClipKey::index).toInt();
            return true;
        }
    }
    return connectedAddress(connectedCount() - 1, track, index);
}

// After a detach: the video clip at (track, index) and the audio just
// placed for it share one link id — the clip's own if it already had
// one, else a fresh one.
void Timeline::linkDetached(int track, int index, const QString &existing) {
    const QVariantMap v = entryMapOn(track, index);
    const QString link = existing.isEmpty() ? newLinkId() : existing;
    setLinkAt(track, index, link);
    int t = -1, c = -1;
    if (placedAudioAt(v.value(ClipKey::path).toString(),
                      v.value(ClipKey::start).toInt(), &t, &c))
        setLinkAt(t, c, link);
}

bool Timeline::reattachAudioOn(int track, int index) {
    if (trackLocked(track))
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    const int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap map = clips.at(at).toMap();
    if (!map.value(ClipKey::videoOnly).toBool()
        || map.contains(ClipKey::freeze))
        return false;
    int mate = -1;
    const QString link = map.value(ClipKey::link).toString();
    for (int i = 0; i < clips.size() && mate < 0 && !link.isEmpty(); ++i) {
        const QVariantMap m = clips.at(i).toMap();
        // The link only breaks ties: a mate that has been moved or trimmed
        // no longer mirrors the picture and is refused like any other.
        if (i != at && m.value(ClipKey::audioOnly).toBool()
            && m.value(ClipKey::link).toString() == link
            && m.value(ClipKey::path) == map.value(ClipKey::path)
            && m.value(ClipKey::in).toInt() == map.value(ClipKey::in).toInt()
            && m.value(ClipKey::out).toInt() == map.value(ClipKey::out).toInt()
            && m.value(ClipKey::start).toInt() == map.value(ClipKey::start).toInt()
            && qFuzzyCompare(m.value(ClipKey::speed, 1.0).toDouble(),
                             map.value(ClipKey::speed, 1.0).toDouble())
            && !trackLocked(m.value(ClipKey::track).toInt()))
            mate = i;
    }
    for (int i = 0; i < clips.size() && mate < 0; ++i) {
        const QVariantMap m = clips.at(i).toMap();
        if (i != at && m.value(ClipKey::audioOnly).toBool()
            && m.value(ClipKey::path) == map.value(ClipKey::path)
            && m.value(ClipKey::in).toInt() == map.value(ClipKey::in).toInt()
            && m.value(ClipKey::out).toInt() == map.value(ClipKey::out).toInt()
            && m.value(ClipKey::start).toInt() == map.value(ClipKey::start).toInt()
            && qFuzzyCompare(m.value(ClipKey::speed, 1.0).toDouble(),
                             map.value(ClipKey::speed, 1.0).toDouble())
            && !trackLocked(m.value(ClipKey::track).toInt()))
            mate = i;
    }
    if (mate < 0)
        return false;
    const QVariantMap audio = clips.at(mate).toMap();
    map.insert(ClipKey::videoOnly, false);
    map.insert(ClipKey::volume, audio.value(ClipKey::volume));
    if (audio.contains(ClipKey::volumeKf))
        map.insert(ClipKey::volumeKf, audio.value(ClipKey::volumeKf));
    map.remove(ClipKey::link); // whole again — nothing to link to
    clips[at] = map;
    clips.removeAt(mate);
    return rebuild(clips, pristine);
}

bool Timeline::detachAudio(int index) {
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    int clip = 0;
    for (int i = 0; i < clips.size(); ++i) {
        QVariantMap map = clips.at(i).toMap();
        if (map.value(ClipKey::lane).toInt() >= 1)
            continue;
        if (clip == index) {
            if (map.value(ClipKey::gap).toBool()
                || map.value(ClipKey::videoOnly).toBool()
                || map.contains(ClipKey::compoundJson))
                return false;
            // A dissolved edge shares frames with its neighbour — the
            // detached copy would double that audio. Refused, like blades.
            if (map.value(ClipKey::mixOut).toInt() > 0
                || map.value(ClipKey::mixIn).toInt() > 0)
                return false;
            const QString path = map.value(ClipKey::path).toString();
            if (path.startsWith(QStringLiteral("color:")) || !pathHasAudio(path))
                return false; // synthetic clips and silent files carry no audio
            const int start = map.value(ClipKey::start).toInt();
            const int in = map.value(ClipKey::in).toInt();
            const int out = map.value(ClipKey::out).toInt();
            const double speed =
                map.value(ClipKey::speed, 1.0).toDouble();
            // Every sound setting moves with the audio.
            QVariantMap sound;
            for (const char *key : {"volume", "pan", "eq", "audioMap", "normalize", "volumeKf"})
                if (map.contains(QLatin1String(key))) {
                    sound.insert(QLatin1String(key), map.value(QLatin1String(key)));
                    map.remove(QLatin1String(key));
                }
            map.insert(ClipKey::videoOnly, true);
            clips[i] = map;
            // Only this clip changes, and not in length: it alone is put back
            // where that gives what the whole rebuild would (see
            // conformLinkedSound for the switch).
            const QByteArray mode = qgetenv("OMAEDIT_NARROW_CONFORM");
            const bool narrow = mode != "0" && replacePrimaryEntry(clip, map);
            if (narrow && mode == "verify") {
                const QVariantList narrowed = entries();
                if (rebuild(clips, pristine) && entries() != narrowed)
                    qFatal("the narrow sound detach differs from the whole rebuild");
            }
            if (narrow) noteSettle(true);
            if (!narrow && !rebuild(clips, pristine))
                return false;
            QScopedValueRollback<int> sampling(m_restoreSampling,map.value(ClipKey::frameBlend).toInt());
            if (!withSourceOrigin(map,[&]{return connectAt(path,start,in,out,true,-1,speed,false,map.value(ClipKey::retime).toMap());})) {
                restore(pristine); // roll the whole detach back
                return false;
            }
            // Where the sound landed, found once: finding it reads the whole
            // clip list, and neither its settings nor the link move it.
            int t = -1, c = -1;
            const bool placed = placedAudioAt(path, start, &t, &c);
            if (!sound.isEmpty() && placed)
                applyEntryAttributes(t, c, sound);
            const QString existing = map.value(ClipKey::link).toString();
            const QString link = existing.isEmpty() ? newLinkId() : existing;
            setLinkAt(0, clip, link);
            if (placed)
                setLinkAt(t, c, link);
            return true;
        }
        ++clip;
    }
    return false;
}

// The clips of the main track from the first that differs to the last, taken
// out with the space before each and put back as `wanted` has them. The run
// must fill the same frames as before: nothing after it may move.
bool Timeline::replacePrimaryRun(const QVariantList &now, const QVariantList &wanted, const QList<int> &run) {
    if (run.isEmpty() || !m_profile || !primary() || trackLocked(0))
        return false;
    int before = 0, after = 0;
    QSet<QString> paths;
    for (const int i : run) {
        const QVariantMap cur = now.at(i).toMap(), w = wanted.at(i).toMap();
        for (const QVariantMap &m : {cur, w})
            if (m.value(ClipKey::gap).toBool() || m.contains(ClipKey::compoundJson)
                || m.value(ClipKey::mixIn).toInt() != 0 || m.value(ClipKey::mixOut).toInt() != 0
                || !m.value(ClipKey::title).toString().isEmpty() || m.value(ClipKey::path).toString().isEmpty()
                || m.value(ClipKey::gapBefore).toInt() < 0)
                return false;
        const int in = w.value(ClipKey::in).toInt(), out = w.value(ClipKey::out).toInt();
        if (in < 0 || out < in)
            return false;
        before += cur.value(ClipKey::gapBefore).toInt() + cur.value(ClipKey::frames).toInt();
        after += w.value(ClipKey::gapBefore).toInt() + out - in + 1;
        paths.insert(w.value(ClipKey::path).toString());
    }
    if (before != after)
        return false;
    // Where the run sits in the track: its clips, and the space before each.
    edit::Playlist *pl = primary();
    const int firstIndex = now.at(run.first()).toMap().value(ClipKey::index).toInt();
    const int first = entryIndexFor(firstIndex), last = entryIndexFor(firstIndex + int(run.size()) - 1);
    if (first < 0 || last < first)
        return false;
    const bool leadingSpace = now.at(run.first()).toMap().value(ClipKey::gapBefore).toInt() > 0;
    if (leadingSpace != (first > 0 && pl->is_blank(first - 1)))
        return false;
    const int from = leadingSpace ? first - 1 : first;
    int clipsThere = 0, framesThere = 0;
    for (int at = from; at <= last; ++at) {
        if (pl->is_mix(at))
            return false;
        clipsThere += !pl->is_blank(at);
        framesThere += pl->clip_length(at);
    }
    if (clipsThere != run.size() || framesThere != before
        || (from > 0 && pl->is_mix(from - 1)) || (last + 1 < pl->count() && pl->is_mix(last + 1)))
        return false;

    QScopedValueRollback<bool> restoring(m_restoring, true);
    QScopedValueRollback<QVariantMap> sourceScope(m_mediaSources);
    QScopedValueRollback<QHash<QString, int>> lengthScope(m_offlineLengths);
    noteRestoreSources(wanted, paths);
    for (int at = last; at >= from; --at)
        if (pl->remove(at) != 0)
            return false;
    bool ok = true;
    int at = from;
    for (int k = 0; k < run.size(); ++k) {
        const ClipEntry e = ClipEntry::read(wanted.at(run.at(k)).toMap());
        if (e.gapBefore > 0) {
            if (at >= pl->count()) pl->blank(e.gapBefore - 1);
            else pl->insert_blank(at, e.gapBefore - 1);
            ++at;
        }
        QScopedValueRollback<int> sampling(m_restoreSampling, ClipEntry::orDefault(e.frameBlend));
        std::optional<double> origin;
        if (!SourceTiming::readOrigin(e.raw, origin))
            return false;
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin, origin);
        const QVariantMap retime = e.retimeMap();
        edit::Producer producer = makeClipProducer(e.path, e.in, e.out, e.speed, e.gap, e.videoOnly, retime);
        if (!producer.is_valid() || (!retime.isEmpty() && e.out >= producer.get_length()))
            return false;
        if ((at >= pl->count() ? pl->append(producer, e.in, e.out) : pl->insert(producer, at, e.in, e.out)) != 0)
            return false;
        ++at;
        ok = dressPrimaryEntry(e, firstIndex + k) && ok;
    }
    tidyPrimary();
    return ok;
}

bool Timeline::rebuildNarrowly(const QVariantList &wanted) {
    if (!m_profile || !primary() || m_restoring)
        return false;
    const QVariantList now = entries();
    if (now.isEmpty() || now.size() != wanted.size())
        return false; // a clip more or fewer: the tracks themselves change
    QList<int> primaryClips, primaryChanged, laneChanged;
    for (int i = 0; i < now.size(); ++i) {
        const QVariantMap cur = now.at(i).toMap(), w = wanted.at(i).toMap();
        const int lane = cur.value(ClipKey::lane).toInt();
        if (lane != w.value(ClipKey::lane).toInt())
            return false;
        if (lane == 0)
            primaryClips.append(i);
        if (now.at(i) == wanted.at(i))
            continue;
        (lane == 0 ? primaryChanged : laneChanged).append(i);
    }
    // Nothing differs: whoever asked wants the rebuild itself (a dissolve
    // planted again, say).
    if (primaryChanged.isEmpty() && laneChanged.isEmpty())
        return false;
    QList<int> run;
    if (!primaryChanged.isEmpty()) {
        const int from = int(primaryClips.indexOf(primaryChanged.first())), to = int(primaryClips.indexOf(primaryChanged.last()));
        run = primaryClips.mid(from, to - from + 1);
    }
    // The main track first: it refuses before it touches anything, and what a
    // track clip refuses after that is the whole rebuild's to put right.
    if (!run.isEmpty() && !replacePrimaryRun(now, wanted, run))
        return false;
    return laneChanged.isEmpty() || replaceLaneEntries(now, wanted, laneChanged);
}
