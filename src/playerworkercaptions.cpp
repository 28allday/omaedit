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
    const auto captionTrack = [this](int n) {
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
