#include "timeline_p.h"

// ── Compound clips (guide p. 505) ───────────────────────────────────────
// A compound's producer is a nested edit::Playlist rebuilt from the JSON
// entries list riding it as omavid:compound — the omavid:path restore
// trick, scaled up to a whole sub-timeline. Blade/trim on the compound are
// ordinary cut operations; both halves of a blade share the JSON.

std::unique_ptr<edit::Playlist> Timeline::buildCompound(const QString &json) {
    const QVariantList subs =
        QJsonDocument::fromJson(json.toUtf8()).toVariant().toList();
    if (subs.isEmpty() || !m_profile)
        return nullptr;
    auto sub = std::make_unique<edit::Playlist>(*m_profile);
    for (const QVariant &v : subs) {
        const ClipEntry e = ClipEntry::read(v.toMap());
        QScopedValueRollback<int> sampling(m_restoreSampling,ClipEntry::orDefault(e.frameBlend));
        std::optional<double> origin;if(!SourceTiming::readOrigin(e.raw,origin))return nullptr;
        QScopedValueRollback<std::optional<double>> originScope(m_sourceOrigin,origin);
        if (e.gapBefore > 0)
            sub->blank(e.gapBefore - 1); // empty space inside the compound
        const int in = e.in;
        const int out = e.out;
        edit::Producer producer = makeClipProducer(e.path, in, out, e.speed, e.gap, e.videoOnly, e.retimeMap());
        if (!producer.is_valid())
            return nullptr;
        if ((in >= 0 ? sub->append(producer, in, out)
                     : sub->append(producer)) != 0)
            return nullptr;
        // Decorations ride each inner cut, exactly like the storyline
        // restore pass.
        edit::ClipInfo *info = sub->clip_info(sub->count() - 1);
        if (info && info->cut) {
            if(e.audioCurve){edit::Filter marker(*m_profile,"volume");marker.set("level",0);marker.set("omavid:audioCurve",e.audioCurve->toUtf8().constData());info->cut->attach(marker);}
            if (e.fadeIn > 0 || e.fadeOut > 0)
                applyFades(info->cut, info->frame_in, info->frame_out, e.fadeIn, e.fadeOut);
            if (e.volume != 0.0)
                applyVolumeDb(*m_profile, info->cut, e.volume);
            if (!e.volumeKf.isEmpty())
                applyVolumeKf(*m_profile, info->cut, e.volumeKf, info->frame_in, info->frame_out);
            if (e.pan != 0.0)
                applyPanValue(*m_profile, info->cut, e.pan);
            if (!e.eq.isEmpty())
                applyEqPreset(*m_profile, info->cut, e.eq);
            const double cb = e.colorB, cc = e.colorC, cs = e.colorS;
            if (cb != 0.0 || cc != 1.0 || cs != 1.0) {
                edit::Filter eqf(*m_profile, "avfilter.eq");
                if (eqf.is_valid()) {
                    eqf.set("av.brightness", cb);
                    eqf.set("av.contrast", cc);
                    eqf.set("av.saturation", cs);
                    eqf.set("omavid:color",
                            QStringLiteral("%1 %2 %3")
                                .arg(QString::number(cb), QString::number(cc),
                                     QString::number(cs))
                                .toUtf8().constData());
                    info->cut->attach(eqf);
                }
            }
            if (e.disabled) {
                edit::Filter dark(*m_profile, "brightness");
                edit::Filter mute(*m_profile, "volume");
                if (dark.is_valid() && mute.is_valid()) {
                    dark.set("level", 0);
                    dark.set("omavid:disabled", 1);
                    info->cut->attach(dark);
                    mute.set("level", -60);
                    mute.set("omavid:disabled", 1);
                    info->cut->attach(mute);
                }
            }
            if (e.opacity != 1.)
                applyOpacityValue(*m_profile, info->cut, e.opacity);
            if (!e.opacityKf.isEmpty())
                applyOpacityKf(*m_profile, info->cut, e.opacityKf, info->frame_in, info->frame_out);
            EditEngine::attach(*m_profile, *info->cut, e.raw, info->frame_in, info->frame_out, true);
            const QStringList bypass = ClipEntry::orDefault(e.bypass);
            for (int i = 0; i < info->cut->filter_count(); ++i) {
                std::unique_ptr<edit::Filter> filter(info->cut->filter(i));
                if (filter && bypass.contains(bypassKeyOf(*filter)))
                    filter->set("disable", 1);
            }
            reorderAttributeFilters(info->cut);
        }
        delete info;
    }
    sub->set("omavid:compound", json.toUtf8().constData());
    sub->set("omavid:compoundCount", sub->count());
    sub->set("omavid:path", "compound:");
    // PLAYLIST properties don't survive the xml consumer (the round-6 fact
    // that created omavid:library) — so the JSON also rides the first inner
    // clip's PRODUCER, which does serialize. Each compound builds its own
    // producer copies, so the marker never leaks to an unrelated clip.
    if (edit::ClipInfo *first = sub->clip_info(0)) {
        if (first->producer)
            first->producer->set("omavid:compoundOf",
                                 json.toUtf8().constData());
        delete first;
    }
    return sub;
}

bool Timeline::appendCompound(const QString &json, int in, int out) {
    // Restore pre-locks from real footage when present. A timeline made
    // entirely of a synthetic compound still needs a profile and V1.
    if (!primary()) {
        const QVariantList subs = QJsonDocument::fromJson(json.toUtf8()).toVariant().toList();
        if (subs.isEmpty()
            || !lockProfileTo(subs.first().toMap().value(ClipKey::path).toString()))
            return false;
    }
    auto comp = buildCompound(json);
    if (!comp)
        return false;
    // append refs the underlying playlist service — `comp` may go.
    return (in >= 0 ? primary()->append(*comp, in, out)
                    : primary()->append(*comp)) == 0;
}

bool Timeline::makeCompound(int start, int end, QString *error) {
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();
    if (trackLocked(0))
        return fail(QStringLiteral("Cannot make a nested clip: V1 is locked."));
    if (!primary() || start < 0 || end < start
        || start >= primaryFrameCount())
        return fail(QStringLiteral("Cannot make a nested clip: select a range on V1."));
    end = qMin(end, primaryFrameCount() - 1);

    const QVariantList before = entries();
    QVariantList result;
    QVariantList sub;
    int compoundPos = -1; // where the compound entry lands in `result`
    int compoundGap = 0;  // empty space that stays before it on V1
    int inside = 0;       // frames of the compound filled so far
    for (const QVariant &v : before) {
        QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1) {
            result.append(map); // lane clips stay attached above
            continue;
        }
        const int clipStart = map.value(ClipKey::start).toInt();
        const int frames = map.value(ClipKey::frames).toInt();
        const int clipEnd = clipStart + frames - 1;
        if (clipEnd < start || clipStart > end) {
            result.append(map);
            continue;
        }
        // v1 guard rails: no dissolves inside or touching the span, and no
        // compounds of compounds.
        if (map.value(ClipKey::mixIn).toInt() > 0
            || map.value(ClipKey::mixOut).toInt() > 0)
            return fail(QStringLiteral("Cannot make a nested clip: remove dissolves touching the range first."));
        if (map.contains(ClipKey::compoundJson))
            return fail(QStringLiteral("Cannot make a nested clip: ungroup the nested clips in the range first."));
        const int in = map.value(ClipKey::in).toInt();
        if (clipStart < start) { // straddles the left edge — blade it
            QVariantMap head = map;
            head.insert(ClipKey::out,
                        in + (start - clipStart) - 1);
            head.insert(ClipKey::fadeOut, 0);
            result.append(head);
        }
        QVariantMap piece = map;
        if (clipStart < start) {
            piece.insert(ClipKey::in, in + (start - clipStart));
            piece.insert(ClipKey::fadeIn, 0);
            piece[ClipKey::editKeys] = EditEngine::shiftedKeys(map.value(ClipKey::editKeys).toMap(), start - clipStart, end - start + 1);
            if (map.contains(ClipKey::videoEffects)) piece[ClipKey::videoEffects] = EditEngine::shiftedEffects(map.value(ClipKey::videoEffects).toList(), start - clipStart);
            piece[ClipKey::opacityKf] = EditEngine::shiftedCurve(map.value(ClipKey::opacityKf).toList(), start - clipStart);
            if (const QVariantList level = map.value(ClipKey::volumeKf).toList(); !level.isEmpty())
                piece[ClipKey::volumeKf] = EditEngine::shiftedCurve(level, start - clipStart);
        }
        if (clipEnd > end) {
            piece.insert(ClipKey::out, in + (end - clipStart));
            piece.insert(ClipKey::fadeOut, 0);
        }
        // Where it sits inside the compound, and how long it is there: the
        // compound's clips are drawn from these, as any timeline's are.
        piece.insert(ClipKey::start, qMax(clipStart, start) - start);
        piece.insert(ClipKey::frames, piece.value(ClipKey::out).toInt() - piece.value(ClipKey::in).toInt() + 1);
        // Empty space before the clip: what the range holds of it goes in
        // with the clip, the rest (before the first one) stays on V1 in
        // front of the compound, which then stands where its clips stood.
        const int place = piece.value(ClipKey::start).toInt();
        if (compoundPos < 0 && clipStart >= start)
            compoundGap = qMax(0, map.value(ClipKey::gapBefore).toInt() - place);
        piece.insert(ClipKey::gapBefore, place - inside);
        inside = place + piece.value(ClipKey::frames).toInt();
        sub.append(piece);
        if (compoundPos < 0) {
            compoundPos = result.size();
            result.append(QVariantMap()); // patched with the compound below
        }
        if (clipEnd > end) { // straddles the right edge
            QVariantMap tail = map;
            tail.insert(ClipKey::gapBefore, 0); // it follows the compound directly
            tail.insert(ClipKey::in, in + (end - clipStart) + 1);
            tail.insert(ClipKey::fadeIn, 0);
            tail[ClipKey::editKeys] = EditEngine::shiftedKeys(map.value(ClipKey::editKeys).toMap(), end - clipStart + 1, clipEnd - end);
            if (map.contains(ClipKey::videoEffects)) tail[ClipKey::videoEffects] = EditEngine::shiftedEffects(map.value(ClipKey::videoEffects).toList(), end - clipStart + 1);
            tail[ClipKey::opacityKf] = EditEngine::shiftedCurve(map.value(ClipKey::opacityKf).toList(), end - clipStart + 1);
            if (const QVariantList level = map.value(ClipKey::volumeKf).toList(); !level.isEmpty())
                tail[ClipKey::volumeKf] = EditEngine::shiftedCurve(level, end - clipStart + 1);
            result.append(tail);
        }
    }
    if (sub.isEmpty() || compoundPos < 0)
        return fail(QStringLiteral("Cannot make a nested clip: the range contains no V1 clips."));
    QVariantMap comp;
    comp.insert(ClipKey::path, QStringLiteral("compound:"));
    comp.insert(ClipKey::lane, 0);
    comp.insert(ClipKey::in, 0);
    comp.insert(ClipKey::out, inside - 1); // a range ending in empty space ends at its last clip
    comp.insert(ClipKey::gapBefore, compoundGap);
    comp.insert(ClipKey::compoundJson,
                QString::fromUtf8(QJsonDocument::fromVariant(sub)
                                      .toJson(QJsonDocument::Compact)));
    result[compoundPos] = comp;
    return rebuild(result, before)
        || fail(QStringLiteral("Could not make a nested clip: check that its media is available."));
}

bool Timeline::breakApart(int index, QString *error) {
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();
    if (trackLocked(0))
        return fail(QStringLiteral("Cannot ungroup the nested clip: V1 is locked."));
    if (!primary())
        return fail(QStringLiteral("Cannot ungroup: select a nested clip on V1."));
    const QVariantList before = entries();
    QVariantList result;
    int clip = 0;
    bool exploded = false;
    int trailing = 0; // empty space the compound's window ended in
    for (const QVariant &v : before) {
        QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1) {
            result.append(map);
            continue;
        }
        if (clip++ != index) {
            if (trailing > 0) { // it stays, before the clip that followed the compound
                map.insert(ClipKey::gapBefore, map.value(ClipKey::gapBefore).toInt() + trailing);
                trailing = 0;
            }
            result.append(map);
            continue;
        }
        const QString json =
            map.value(ClipKey::compoundJson).toString();
        if (json.isEmpty())
            return fail(QStringLiteral("Cannot ungroup: the selected clip is not a nested clip."));
        // v1: shed any dissolve on the compound's edges first.
        if (map.value(ClipKey::mixIn).toInt() > 0
            || map.value(ClipKey::mixOut).toInt() > 0)
            return fail(QStringLiteral("Cannot ungroup the nested clip: remove its edge dissolves first."));
        const int wIn = map.value(ClipKey::in).toInt();
        const int wOut = map.value(ClipKey::out).toInt();
        const QVariantList subs =
            QJsonDocument::fromJson(json.toUtf8()).toVariant().toList();
        int pos = 0;
        // The first piece out stands where the compound stood: it takes
        // the compound's own gap (a normal trim of its in point opens one),
        // not the gap it had before it went in. Empty space inside the
        // compound comes out as it shows: each piece keeps its place in the
        // window, counted as the compound itself is built (buildCompound).
        bool first = true;
        int shown = wIn; // the window's frames given back so far
        for (const QVariant &s : subs) {
            QVariantMap piece = s.toMap();
            const int sIn = piece.value(ClipKey::in).toInt();
            const int sOut = piece.value(ClipKey::out).toInt();
            const int len = sOut - sIn + 1;
            pos += qMax(0, piece.value(ClipKey::gapBefore).toInt());
            const int segStart = pos;
            const int segEnd = pos + len - 1;
            pos += len;
            // The compound may be trimmed — only its visible window comes
            // back out (the contents come back; a trim is honoured).
            if (segEnd < wIn || segStart > wOut)
                continue;
            if (segStart < wIn) {
                piece.insert(ClipKey::in, sIn + (wIn - segStart));
                piece[ClipKey::editKeys] = EditEngine::shiftedKeys(piece.value(ClipKey::editKeys).toMap(), wIn - segStart, wOut - wIn + 1);
                if (piece.contains(ClipKey::videoEffects)) piece[ClipKey::videoEffects] = EditEngine::shiftedEffects(piece.value(ClipKey::videoEffects).toList(), wIn - segStart);
                piece[ClipKey::opacityKf] = EditEngine::shiftedCurve(piece.value(ClipKey::opacityKf).toList(), wIn - segStart);
                if (const QVariantList level = piece.value(ClipKey::volumeKf).toList(); !level.isEmpty())
                    piece[ClipKey::volumeKf] = EditEngine::shiftedCurve(level, wIn - segStart);
            }
            if (segEnd > wOut)
                piece.insert(ClipKey::out, sIn + (wOut - segStart));
            piece.insert(ClipKey::lane, 0);
            piece.insert(ClipKey::gapBefore, qMax(segStart, wIn) - shown
                                                 + (first ? map.value(ClipKey::gapBefore).toInt() : 0));
            first = false;
            shown = qMin(segEnd, wOut) + 1;
            result.append(piece);
            exploded = true;
        }
        trailing = qMax(0, wOut + 1 - shown);
    }
    if (!exploded)
        return fail(QStringLiteral("Cannot ungroup the nested clip: no clip contents were found."));
    return rebuild(result, before)
        || fail(QStringLiteral("Could not ungroup the nested clip: check that its media is available."));
}
