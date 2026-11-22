#include "timeline_p.h"

#define OMAVID_TRACK_FLAG(Name, field)                                       \
    bool Timeline::setTrack##Name(int track, bool value) {                \
        if (track < 0 || track >= trackCount())                              \
            return false;                                                    \
        m_tracks[size_t(track)].field = value;                               \
        refreshTrackFilters();                                               \
        return true;                                                         \
    }

// ── Track header state ──────────────────────────────────────────────────
Timeline::Track::Kind Timeline::trackKind(int track) const {
    if (track < 0 || track >= trackCount())
        return Track::Video;
    if (track == 0)
        return Track::Video;
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    for (int i = 0; i < pl->count(); ++i) {
        if (pl->is_blank(i))
            continue;
        std::unique_ptr<edit::ClipInfo> info(pl->clip_info(i));
        if (!info || !info->producer)
            continue;
        return info->producer->get_int("omavid:audioOnly") != 0
                   ? Track::Audio
                   : Track::Video;
    }
    return m_tracks[size_t(track)].kind;
}

QString Timeline::trackName(int track) const {
    if (track < 0 || track >= trackCount())
        return QString();
    if (!m_tracks[size_t(track)].name.isEmpty())
        return m_tracks[size_t(track)].name;
    const Track::Kind kind = trackKind(track);
    int ordinal = 0;
    for (int i = 0; i <= track; ++i)
        if (trackKind(i) == kind)
            ++ordinal;
    return (kind == Track::Audio ? QStringLiteral("A%1")
                                 : QStringLiteral("V%1"))
        .arg(ordinal);
}

bool Timeline::trackLocked(int track) const {
    return track >= 0 && track < trackCount() && m_tracks[size_t(track)].locked;
}

int Timeline::trackClipCount(int track) const {
    if (track < 0 || track >= trackCount())
        return 0;
    if (!m_tracks[size_t(track)].audioEntries.isEmpty()) return m_tracks[size_t(track)].audioEntries.size();
    edit::Playlist *pl = m_tracks[size_t(track)].playlist.get();
    int clips = 0;
    for (int i = 0; i < pl->count(); ++i)
        if (!pl->is_mix(i) && !pl->is_blank(i))
            ++clips;
    return clips;
}

QVariantList Timeline::tracks() const {
    QVariantList list;
    for (int i = 0; i < trackCount(); ++i) {
        const Track &t = m_tracks[size_t(i)];
        QVariantMap map;
        map.insert(QStringLiteral("index"), i);
        map.insert(QStringLiteral("name"), trackName(i));
        map.insert(QStringLiteral("kind"),
                   trackKind(i) == Track::Audio ? QStringLiteral("audio")
                                                : QStringLiteral("video"));
        map.insert(QStringLiteral("locked"), t.locked);
        map.insert(QStringLiteral("disabled"), t.disabled);
        map.insert(QStringLiteral("muted"), t.muted);
        map.insert(QStringLiteral("solo"), t.solo);
        map.insert(QStringLiteral("autoSelect"), t.autoSelect);
        map.insert(QStringLiteral("clips"), trackClipCount(i));
        map.insert(QStringLiteral("mix"), t.mix);
        list.append(map);
    }
    return list;
}

// Header flags land as filters ON THE TRACK PLAYLIST (marked
// omavid:trackFlag): brightness 0 hides the picture, volume -60 dB mutes.
// A multitrack "hide" would do neither for the bottom track (nothing
// consumes it as a B-frame — probed 7.40), and playlist FILTERS serialize
// into project XML where playlist properties never do.
void Timeline::refreshTrackFilters() {
    // The meters' slots follow the tracks (the mixer applies the flags itself).
    if (m_trackPeaks.size() != size_t(trackCount()) * 6)
        m_trackPeaks.assign(size_t(trackCount()) * 6, 0.0f);
}
OMAVID_TRACK_FLAG(Locked, locked)
OMAVID_TRACK_FLAG(Disabled, disabled)
OMAVID_TRACK_FLAG(Muted, muted)
OMAVID_TRACK_FLAG(Solo, solo)
OMAVID_TRACK_FLAG(AutoSelect, autoSelect)
#undef OMAVID_TRACK_FLAG

bool Timeline::setTrackName(int track, const QString &name) {
    if (track < 0 || track >= trackCount())
        return false;
    m_tracks[size_t(track)].name = name.trimmed();
    return true;
}

bool Timeline::addTrack(bool audio) {
    if (!primary())
        return false; // a profile (first clip) comes first
    edit::Playlist *pl = ensureLane(laneTotal());
    if (!pl)
        return false;
    m_tracks.back().kind = audio ? Track::Audio : Track::Video;
    return true;
}

bool Timeline::removeTrack(int track) {
    if (track < 1 || track >= trackCount() || trackClipCount(track) > 0)
        return false;
    m_tracks.erase(m_tracks.begin() + track);
    return true;
}

bool Timeline::swapTracks(int a, int b) {
    if (a < 1 || b < 1 || a >= trackCount() || b >= trackCount() || a == b)
        return false;
    std::swap(m_tracks[size_t(a)], m_tracks[size_t(b)]);
    return true;
}

int Timeline::trackNeighbour(int track, bool up) const {
    if (track < 1 || track >= trackCount())
        return -1;
    const Track::Kind kind = trackKind(track);
    // Video rows climb with the index, audio rows descend with it.
    const int step = (kind == Track::Audio) == up ? -1 : 1;
    for (int i = track + step; i >= 1 && i < trackCount(); i += step)
        if (trackKind(i) == kind)
            return i;
    return -1;
}

QVariantList Timeline::trackFlagsVariant() const {
    QVariantList list;
    for (const TrackFlags &f : trackFlags()) {
        QVariantMap map;
        map.insert(QStringLiteral("audio"), f.kind == Track::Audio);
        map.insert(QStringLiteral("locked"), f.locked);
        map.insert(QStringLiteral("disabled"), f.disabled);
        map.insert(QStringLiteral("muted"), f.muted);
        map.insert(QStringLiteral("solo"), f.solo);
        map.insert(QStringLiteral("autoSelect"), f.autoSelect);
        map.insert(QStringLiteral("name"), f.name);
        map.insert(QStringLiteral("mix"), f.mix);
        list.append(map);
    }
    return list;
}

std::vector<Timeline::TrackFlags>
Timeline::trackFlagsFromVariant(const QVariantList &list) {
    std::vector<TrackFlags> flags;
    for (const QVariant &v : list) {
        const QVariantMap m = v.toMap();
        flags.push_back({m.value(QStringLiteral("audio")).toBool() ? Track::Audio
                                                                    : Track::Video,
                         m.value(QStringLiteral("locked")).toBool(),
                         m.value(QStringLiteral("disabled")).toBool(),
                         m.value(QStringLiteral("muted")).toBool(),
                         m.value(QStringLiteral("solo")).toBool(),
                         m.value(QStringLiteral("autoSelect"), true).toBool(),
                         m.value(QStringLiteral("name")).toString(),
                         m.value(QStringLiteral("mix")).toMap()});
    }
    return flags;
}

int Timeline::trackFrameCount(int track) const {
    if (track < 0 || track >= trackCount())
        return 0;
    return m_tracks[size_t(track)].playlist->get_playtime();
}

std::vector<Timeline::TrackFlags> Timeline::trackFlags() const {
    std::vector<TrackFlags> flags;
    for (int i = 0; i < trackCount(); ++i) {
        const Track &t = m_tracks[size_t(i)];
        flags.push_back({trackKind(i), t.locked, t.disabled, t.muted, t.solo,
                         t.autoSelect, t.name, t.mix});
    }
    return flags;
}

void Timeline::applyTrackFlags(const std::vector<TrackFlags> &flags) {
    // Empty tracks hold no clips, so a rebuild from entries never recreates
    // them — the flag snapshot does (persistent tracks, Phase C).
    while (m_profile && primary() && int(m_tracks.size()) < int(flags.size()))
        ensureLane(laneTotal());
    for (size_t i = 0; i < flags.size() && i < m_tracks.size(); ++i) {
        Track &t = m_tracks[i];
        t.kind = flags[i].kind;
        t.locked = flags[i].locked;
        t.disabled = flags[i].disabled;
        t.muted = flags[i].muted;
        t.solo = flags[i].solo;
        t.autoSelect = flags[i].autoSelect;
        t.name = flags[i].name;
        t.mix = flags[i].mix;
    }
    refreshTrackFilters();
}
