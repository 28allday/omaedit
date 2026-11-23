#include "playerworker_p.h"

void PlayerWorker::importCaptions(const QVariantList &cues, const QVariantMap &style, const QVariantMap &language) {
    placeCaptions(cues, style, {}, tr("Import captions"), language);
}

void PlayerWorker::placeCaptions(const QVariantList &cues, const QVariantMap &style,
                                 const QVariantList &remove, const QString &label, const QVariantMap &language) {
    if (exportActive() || (cues.isEmpty() && remove.isEmpty()))
        return;
    // "Captions", then "Captions 2", … — overlapping cues (two speakers)
    // go on the next track rather than truncate each other, since a lane
    // placement overwrites whatever it lands on.
    // The caption style the cues were made with (Backend::captionStyle carries
    // it whole), for the tracks made for them.
    const QVariantMap captionLook = style.value(QStringLiteral("captionStyle")).toMap();
    const auto captionTrack = [this, &captionLook](int n) {
        const QString wanted=n==1?QStringLiteral("Captions"):QStringLiteral("Captions %1").arg(n);
        int found=0;
        for(const auto &v:m_timeline.tracks()){
            const auto t=v.toMap();const int index=t.value("index").toInt();
            if(m_timeline.trackLocked(index))continue;
            const bool caption=t.value("mix").toMap().value("editKind")=="caption";
            if((caption && ++found==n) || (!caption && t.value("name")==wanted && t.value("kind")!="audio")){
                m_timeline.setTrackMix(index,"editKind","caption");return index;
            }
        }
        if(!m_timeline.addTrack(false))return -1;
        const int track=m_timeline.trackCount()-1;m_timeline.setTrackName(track,wanted);
        m_timeline.setTrackMix(track,"editKind","caption");m_timeline.setTrackMix(track,"captionBurnIn",true);
        // A new caption track keeps the timeline's caption style with it.
        if(!captionLook.isEmpty())m_timeline.setTrackMix(track,"captionStyle",captionLook);
        emit tracksChanged(m_timeline.tracks());return track;
    };
    // Captions in a language: the caption tracks of that language, an empty
    // caption track with none set, or new ones that take it.
    const QString code = language.value(QStringLiteral("code")).toString();
    const auto languageTrack = [this, &captionLook, &language, &code](int n) {
        const auto holdsCaptions = [this](int index) {
            for (const QVariant &v : m_timeline.entries())
                if (v.toMap().value(ClipKey::track).toInt() == index && v.toMap().value(ClipKey::lane).toInt() >= 1) return true;
            return false;
        };
        int found = 0, captionTracks = 0, empty = -1;
        QVariantMap stored = language;
        stored.remove(QStringLiteral("trackName"));
        for (const QVariant &v : m_timeline.tracks()) {
            const QVariantMap t = v.toMap(), mix = t.value(QStringLiteral("mix")).toMap();
            const int index = t.value(QStringLiteral("index")).toInt();
            if (mix.value(QStringLiteral("editKind")) != QStringLiteral("caption")) continue;
            ++captionTracks;
            if (m_timeline.trackLocked(index)) continue;
            const QString has = mix.value(QStringLiteral("language")).toMap().value(QStringLiteral("code")).toString();
            if (has.compare(code, Qt::CaseInsensitive) == 0 && ++found == n) return index;
            if (empty < 0 && (has.isEmpty() || has == QStringLiteral("und")) && !holdsCaptions(index)) empty = index;
        }
        if (n == 1 && empty >= 0) {
            m_timeline.setTrackMix(empty, QStringLiteral("language"), stored);
            emit tracksChanged(m_timeline.tracks());
            return empty;
        }
        if (!m_timeline.addTrack(false)) return -1;
        const int track = m_timeline.trackCount() - 1;
        const QString name = language.value(QStringLiteral("trackName")).toString();
        m_timeline.setTrackName(track, n == 1 ? name : QStringLiteral("%1 %2").arg(name).arg(n));
        m_timeline.setTrackMix(track, QStringLiteral("editKind"), QStringLiteral("caption"));
        // The first caption track is burnt in, as ever; another language's is not.
        m_timeline.setTrackMix(track, QStringLiteral("captionBurnIn"), captionTracks == 0);
        m_timeline.setTrackMix(track, QStringLiteral("language"), stored);
        if (!captionLook.isEmpty()) m_timeline.setTrackMix(track, QStringLiteral("captionStyle"), captionLook);
        emit tracksChanged(m_timeline.tracks());
        return track;
    };
    // (A language set on a track, or a track made for one, is undone with the captions.)
    const QVariantList before = code.isEmpty() ? m_timeline.entries() : snapshotNow();
    // Captions being replaced (split, joined) go first: flat connected
    // indices, highest first so the others keep theirs.
    QList<int> removing;
    for (const QVariant &v : remove) removing.append(v.toInt());
    std::sort(removing.begin(), removing.end(), std::greater<int>());
    int removed = 0;
    for (int index : std::as_const(removing))
        if (m_timeline.removeConnected(index)) ++removed;
    QVariantMap captionStyle = style;
    captionStyle.remove(QStringLiteral("captionStyle"));
    captionStyle.insert(QStringLiteral("caption"), true);
    // What each track already holds, so a new caption never lands on one.
    QHash<int, QList<QPair<int, int>>> taken;
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::lane).toInt() < 1) continue;
        const int start = e.value(ClipKey::start).toInt();
        taken[e.value(ClipKey::track).toInt()].append({start, start + e.value(ClipKey::frames).toInt()});
    }
    const auto free = [&taken](int track, int start, int end) {
        for (const auto &span : std::as_const(taken[track]))
            if (span.first < end && start < span.second) return false;
        return true;
    };
    QList<int> tracks;   // caption tracks in use, by their engine index
    int placed = 0;
    for (const QVariant &v : cues) {
        const QVariantMap cue = v.toMap();
        const int start = cue.value(QStringLiteral("start")).toInt();
        const int frames = cue.value(QStringLiteral("frames")).toInt();
        if (frames < 1 || start < 0) continue;
        int track = -1;
        for (int n = 1; track < 0; ++n) {
            const int candidate = n <= tracks.size() ? tracks.at(n - 1) : code.isEmpty() ? captionTrack(n) : languageTrack(n);
            if (candidate < 0) break;
            if (n > tracks.size()) tracks.append(candidate);
            if (free(candidate, start, start + frames)) track = candidate;
        }
        if (track < 0)
            break;
        // Made in the timeline's caption style: in the style of the track
        // it lands on, when that track keeps one of its own.
        QVariantMap drawn = captionStyle;
        if (!captionLook.isEmpty()) {
            const QVariantMap own = m_timeline.trackMix(track).value(QStringLiteral("captionStyle")).toMap();
            if (!own.isEmpty()) {
                drawn = own;
                for (const char *key : {"maxChars", "maxLines", "minSeconds"}) drawn.remove(QLatin1String(key));
                if (drawn.value(QStringLiteral("pixelSize")).toDouble() <= 0) drawn.remove(QStringLiteral("pixelSize"));
                drawn.insert(QStringLiteral("caption"), true);
            }
        }
        if (cue.contains(QStringLiteral("speaker"))) drawn.insert(QStringLiteral("speaker"), cue.value(QStringLiteral("speaker")));
        if (m_timeline.connectTitleAt(cue.value(QStringLiteral("text")).toString(),
                                      start, frames, drawn, track)) {
            ++placed;
            taken[track].append({start, start + frames});
        }
    }
    emit captionsImported(placed, tracks.size());
    if (placed == 0 && removed == 0)
        return;
    recordEdit(before, true, label);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::restyleCaptions(const QVariantMap &styles, bool everyCaption) {
    if (staleEdit() || exportActive() || styles.isEmpty())
        return;
    const QVariantList before = snapshotNow();
    // Each caption track's style (its own, or on one without, the first
    // kept on another, or the lower third every caption had before caption
    // styles), and the new style for the tracks being restyled.
    QList<int> captionTracks;
    QHash<int, QVariantMap> previous;
    QVariantMap first;
    for (const QVariant &v : m_timeline.tracks()) {
        const QVariantMap t = v.toMap(), mix = t.value("mix").toMap();
        if (mix.value("editKind") != "caption") continue;
        const int index = t.value("index").toInt();
        captionTracks.append(index);
        if (mix.contains("captionStyle")) {
            previous.insert(index, mix.value("captionStyle").toMap());
            if (first.isEmpty()) first = mix.value("captionStyle").toMap();
        }
    }
    QHash<int, QVariantMap> wanted;
    for (int track : std::as_const(captionTracks)) {
        if (!previous.contains(track)) previous.insert(track, first.isEmpty() ? Timeline::legacyCaptionStyle() : first);
        if (styles.contains(QString::number(track))) wanted.insert(track, styles.value(QString::number(track)).toMap());
    }
    if (captionTracks.isEmpty() && styles.contains(QStringLiteral("-1"))) {
        // Nothing to hold the style yet: the track captions will go on.
        if (!m_timeline.addTrack(false)) return;
        const int track = m_timeline.trackCount() - 1;
        m_timeline.setTrackName(track, QStringLiteral("Captions"));
        m_timeline.setTrackMix(track, "editKind", "caption");
        m_timeline.setTrackMix(track, "captionBurnIn", true);
        captionTracks.append(track);
        previous.insert(track, Timeline::legacyCaptionStyle());
        wanted.insert(track, styles.value(QStringLiteral("-1")).toMap());
    }
    if (wanted.isEmpty())
        return;
    int changed = 0, kept = 0;
    for (int i = 0; i < m_timeline.connectedCount(); ++i) {
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(i, &track, &clip) || !wanted.contains(track)) continue;
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        const QString text = e.value(ClipKey::title).toString();
        if (!e.value(ClipKey::caption).toBool() || text.isEmpty()) continue;
        if (m_timeline.trackLocked(track)) { ++kept; continue; }
        // A caption given its own look or dragged elsewhere keeps it,
        // unless every caption is to take the style.
        if (!everyCaption && Timeline::captionHasOwnLook(e, previous.value(track))) { ++kept; continue; }
        QVariantMap drawn = wanted.value(track);
        for (const char *key : {"maxChars", "maxLines", "minSeconds"}) drawn.remove(QLatin1String(key));
        drawn.insert(QStringLiteral("caption"), true);
        drawn.insert(QStringLiteral("ownStyle"), false);
        const bool moved = !qFuzzyIsNull(e.value(ClipKey::rectX, 0.).toDouble()) || !qFuzzyIsNull(e.value(ClipKey::rectY, 0.).toDouble())
                           || !qFuzzyCompare(e.value(ClipKey::rectW, 1.).toDouble(), 1.) || !qFuzzyCompare(e.value(ClipKey::rectH, 1.).toDouble(), 1.);
        if (!m_timeline.updateTitleAt(i, text, drawn)) { ++kept; continue; }
        if (moved) m_timeline.setConnectedRect(i, 0, 0, 1, 1);
        ++changed;
    }
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
        m_timeline.setTrackMix(it.key(), "captionStyle", it.value());
    emit captionsRestyled(changed, kept);
    recordEdit(before, false, tr("Caption style"));
    emit tracksChanged(m_timeline.tracks());
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::restyleCaption(int index, const QVariantMap &style, const QVariantList &place, const QString &label) {
    if (staleEdit() || exportActive())
        return;
    int track = -1, clip = -1;
    if (!m_timeline.connectedAddress(index, &track, &clip) || m_timeline.trackLocked(track)) return;
    const QVariantMap e = m_timeline.entryMapOn(track, clip);
    const QString text = e.value(ClipKey::title).toString();
    if (!e.value(ClipKey::caption).toBool() || text.isEmpty()) return;
    const QVariantList before = snapshotNow();
    bool done = m_timeline.updateTitleAt(index, text, style);
    // Its own place: offsets from where the style sets it, in parts of the frame.
    // [x, y] moves it; [x, y, w, h] sizes it too.
    if (done && (place.size() == 2 || place.size() == 4)) {
        const double x = place.at(0).toDouble(), y = place.at(1).toDouble();
        const double w = place.size() == 4 ? place.at(2).toDouble() : e.value(ClipKey::rectW, 1.).toDouble();
        const double h = place.size() == 4 ? place.at(3).toDouble() : e.value(ClipKey::rectH, 1.).toDouble();
        if (!qFuzzyCompare(1. + x, 1. + e.value(ClipKey::rectX, 0.).toDouble())
            || !qFuzzyCompare(1. + y, 1. + e.value(ClipKey::rectY, 0.).toDouble())
            || !qFuzzyCompare(w, e.value(ClipKey::rectW, 1.).toDouble()) || !qFuzzyCompare(h, e.value(ClipKey::rectH, 1.).toDouble()))
            m_timeline.setConnectedRect(index, x, y, w, h);
    }
    if (!done) {
        restoreSnapshot(before);
        return;
    }
    recordEdit(before, false, label);
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::setCaptionSpeaker(const QVariantList &indices, const QString &speaker, const QVariantMap &settings) {
    if (staleEdit() || exportActive() || indices.isEmpty())
        return;
    const QVariantList before = snapshotNow();
    int changed = 0;
    for (const QVariant &v : indices) {
        const int index = v.toInt();
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(index, &track, &clip) || m_timeline.trackLocked(track)) continue;
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        if (!e.value(ClipKey::caption).toBool() || e.value(ClipKey::speaker).toString() == speaker) continue;
        QVariantMap style = Timeline::titleStyleOf(e);
        style.insert(QStringLiteral("speaker"), speaker);
        if (m_timeline.updateTitleAt(index, e.value(ClipKey::title).toString(), style)) ++changed;
    }
    // A new name joins the timeline's speakers in the same step.
    const bool listed = !settings.isEmpty() && settings != m_timeline.captionSettings();
    if (changed == 0 && !listed)
        return;
    if (listed) m_timeline.setCaptionSettings(settings);
    recordEdit(before, false, tr("Caption speaker"));
    announceTimeline();
}

void PlayerWorker::duplicateCaptionTrack(int track, const QVariantMap &language, const QString &name) {
    if (staleEdit() || exportActive() || track < 1 || track >= m_timeline.trackCount()
        || m_timeline.trackMix(track).value("editKind") != "caption")
        return;
    const QVariantList before = snapshotNow();
    QVariantList captions;
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::caption).toBool()
            && !e.value(ClipKey::title).toString().isEmpty())
            captions.append(e);
    }
    if (!m_timeline.addTrack(false)) return;
    const int copy = m_timeline.trackCount() - 1;
    m_timeline.setTrackName(copy, name);
    m_timeline.setTrackMix(copy, "editKind", "caption");
    // Another language over the first would cover it: the copy is not
    // burnt in until it is chosen to be.
    m_timeline.setTrackMix(copy, "captionBurnIn", false);
    const QVariant style = m_timeline.trackMix(track).value("captionStyle");
    if (style.isValid()) m_timeline.setTrackMix(copy, "captionStyle", style);
    m_timeline.setTrackMix(copy, "language", language);
    int placed = 0;
    for (const QVariant &v : std::as_const(captions)) {
        QVariantMap e = v.toMap();
        const int start = e.value(ClipKey::start).toInt();
        if (!m_timeline.connectTitleAt(e.value(ClipKey::title).toString(), start, e.value(ClipKey::frames).toInt() + e.value(ClipKey::mixIn).toInt(),
                                       Timeline::titleStyleOf(e), copy))
            continue;
        e.remove(ClipKey::link);
        const int at = m_timeline.clipIndexAt(copy, start);
        if (at >= 0) m_timeline.applyEntryAttributes(copy, at, e);
        ++placed;
    }
    emit captionTrackDuplicated(copy, placed);
    recordEdit(before, false, tr("Duplicate caption track"));
    emit tracksChanged(m_timeline.tracks());
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}
