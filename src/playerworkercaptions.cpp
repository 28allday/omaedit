#include "playerworker_p.h"

void PlayerWorker::importCaptions(const QVariantList &cues, const QVariantMap &style) {
    placeCaptions(cues, style, {}, tr("Import captions"));
}

void PlayerWorker::placeCaptions(const QVariantList &cues, const QVariantMap &style,
                                 const QVariantList &remove, const QString &label) {
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
    const QVariantList before = m_timeline.entries();
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
            const int candidate = n <= tracks.size() ? tracks.at(n - 1) : captionTrack(n);
            if (candidate < 0) break;
            if (n > tracks.size()) tracks.append(candidate);
            if (free(candidate, start, start + frames)) track = candidate;
        }
        if (track < 0)
            break;
        if (m_timeline.connectTitleAt(cue.value(QStringLiteral("text")).toString(),
                                      start, frames, captionStyle, track)) {
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

// A caption's look, with what it leaves unset as the title reads it back, so
// a caption made with a style compares equal to that style.
static QVariantMap captionLook(const QVariantMap &style) {
    const auto text = [&](const char *key, const QString &fallback) {
        const QString v = style.value(QLatin1String(key)).toString();
        return v.isEmpty() ? fallback : v;
    };
    const auto number = [&](const char *key, double fallback) {
        const QVariant v = style.value(QLatin1String(key));
        return v.isValid() && !v.isNull() ? qRound(v.toDouble() * 100) / 100. : fallback;
    };
    const QString place = text("place", QStringLiteral("centre"));
    const bool edge = place == QStringLiteral("top") || place == QStringLiteral("bottom");
    return {{"place", place}, {"size", text("size", QStringLiteral("medium"))}, {"font", text("font", {})},
            {"color", text("color", QStringLiteral("#ffffff")).toLower()}, {"align", text("align", QStringLiteral("centre"))},
            {"weight", text("weight", {})}, {"italic", style.value("italic").toBool()}, {"bg", text("bg", {}).toLower()},
            {"outline", !style.contains("outline") || style.value("outline").isNull() || style.value("outline").toBool()},
            {"pixelSize", number("pixelSize", 0)}, {"tracking", number("tracking", 0)}, {"lineSpacing", number("lineSpacing", 1)},
            {"margin", edge ? number("margin", 8) : 0.}, {"width", edge ? number("width", 90) : 0.}};
}

void PlayerWorker::restyleCaptions(const QVariantMap &style, bool everyCaption) {
    if (staleEdit() || exportActive())
        return;
    const QVariantList before = snapshotNow();
    // The look the captions were given: the one the tracks keep, or, on a
    // timeline from before caption styles, the lower third every caption had.
    QVariantMap previous{{"place", "lower"}, {"size", "small"}, {"bg", "#99000000"}};
    QList<int> captionTracks;
    bool stored = false;
    for (const QVariant &v : m_timeline.tracks()) {
        const QVariantMap t = v.toMap(), mix = t.value("mix").toMap();
        if (mix.value("editKind") != "caption") continue;
        captionTracks.append(t.value("index").toInt());
        if (!stored && mix.contains("captionStyle")) { previous = mix.value("captionStyle").toMap(); stored = true; }
    }
    if (captionTracks.isEmpty()) {
        // Nothing to hold the style yet: the track captions will go on.
        if (!m_timeline.addTrack(false)) return;
        const int track = m_timeline.trackCount() - 1;
        m_timeline.setTrackName(track, QStringLiteral("Captions"));
        m_timeline.setTrackMix(track, "editKind", "caption");
        m_timeline.setTrackMix(track, "captionBurnIn", true);
        captionTracks.append(track);
    }
    QVariantMap drawn = style;
    for (const char *key : {"maxChars", "maxLines", "minSeconds"}) drawn.remove(QLatin1String(key));
    drawn.insert(QStringLiteral("caption"), true);
    const QVariantMap was = captionLook(previous);
    int changed = 0, kept = 0;
    for (int i = 0; i < m_timeline.connectedCount(); ++i) {
        int track = -1, clip = -1;
        if (!m_timeline.connectedAddress(i, &track, &clip) || !captionTracks.contains(track)) continue;
        const QVariantMap e = m_timeline.entryMapOn(track, clip);
        const QString text = e.value(ClipKey::title).toString();
        if (!e.value(ClipKey::caption).toBool() || text.isEmpty()) continue;
        if (m_timeline.trackLocked(track)) { ++kept; continue; }
        const bool moved = !qFuzzyIsNull(e.value(ClipKey::rectX, 0.).toDouble()) || !qFuzzyIsNull(e.value(ClipKey::rectY, 0.).toDouble())
                           || !qFuzzyCompare(e.value(ClipKey::rectW, 1.).toDouble(), 1.) || !qFuzzyCompare(e.value(ClipKey::rectH, 1.).toDouble(), 1.);
        // A caption given its own look or dragged elsewhere keeps it,
        // unless every caption is to take the style.
        if (!everyCaption && (moved || captionLook(Timeline::titleStyleOf(e)) != was)) { ++kept; continue; }
        if (!m_timeline.updateTitleAt(i, text, drawn)) { ++kept; continue; }
        if (moved) m_timeline.setConnectedRect(i, 0, 0, 1, 1);
        ++changed;
    }
    for (int track : std::as_const(captionTracks))
        m_timeline.setTrackMix(track, "captionStyle", style);
    emit captionsRestyled(changed, kept);
    recordEdit(before, false, tr("Caption style"));
    emit tracksChanged(m_timeline.tracks());
    setSpeed(0.0);
    announceTimeline();
    refreshFrame();
}
