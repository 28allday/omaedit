#include "backend_p.h"
#include "media/mediaprobe.h"
#include "sound/mixer.h"


// ── Multicam ─────────────────────────────────────────────────────────────

QString Backend::multicamJson(const QVariantMap &entry) const {
    if(entry.value("multicamVersion").toInt()==2) {
        QVariantMap camera{{"version",2},{"id",entry.value("id")},{"name",entry.value("name")}};
        auto angles=entry.value("angles").toList();
        for(auto &value:angles) {
            const auto angle=value.toMap();
            const auto path=angle.value("path").toString();
            if(m_lastMediaSources.contains(path))value=MulticamTiming::withMetadata(angle,m_lastMediaSources.value(path).toMap());
        }
        camera["angles"]=angles;
        // A sound source of its own rides the description as an angle does
        // (Timeline::fixedSoundOf); `soundAngle` says which angle's it is.
        auto sound=entry.value("sound").toMap();
        if(!sound.value("path").toString().isEmpty()) {
            const auto path=sound.value("path").toString();
            if(m_lastMediaSources.contains(path))sound=MulticamTiming::withMetadata(sound,m_lastMediaSources.value(path).toMap());
            camera["sound"]=sound;
            if(entry.contains("soundAngle"))camera["soundAngle"]=entry.value("soundAngle");
        }
        // The recordings lined up with it that are no angle, in use or not:
        // the sound can be chosen again on the placed clip.
        if(auto recordings=entry.value("recordings").toList();!recordings.isEmpty()) {
            for(auto &value:recordings) {
                const auto path=value.toMap().value("path").toString();
                if(m_lastMediaSources.contains(path))value=MulticamTiming::withMetadata(value.toMap(),m_lastMediaSources.value(path).toMap());
            }
            camera["recordings"]=recordings;
        }
        return QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
    }
    QJsonObject o;
    o.insert(QStringLiteral("id"), entry.value(QStringLiteral("id")).toString());
    o.insert(QStringLiteral("name"), entry.value(QStringLiteral("name")).toString());
    QJsonArray angles;
    for (const QVariant &a : entry.value(QStringLiteral("angles")).toList()) {
        const QVariantMap m = a.toMap();
        QJsonObject j;
        j.insert(QStringLiteral("path"), m.value(QStringLiteral("path")).toString());
        j.insert(QStringLiteral("name"), m.value(QStringLiteral("name")).toString());
        j.insert(QStringLiteral("offset"), m.value(QStringLiteral("offset")).toInt());
        j.insert(QStringLiteral("frames"), m.value(QStringLiteral("frames")).toInt());
        angles.append(j);
    }
    o.insert(QStringLiteral("angles"), angles);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Version 2 browser marks address the selected angle's recorded pictures.
// Older definitions retain their virtual-frame bounds and offset conversion.
QVariantMap Backend::angleSource(const QVariantMap &entry, int angle) const {
    if(entry.value("multicamVersion").toInt()>2)return {};
    if(entry.value("multicamVersion").toInt()==2) {
        auto camera=entry;auto angles=entry.value("angles").toList();
        for(auto &value:angles) {
            const auto item=value.toMap();
            const auto path=item.value("path").toString();
            if(m_lastMediaSources.contains(path))value=MulticamTiming::withMetadata(item,m_lastMediaSources.value(path).toMap());
        }
        camera["angles"]=angles;return MulticamTiming::nativeSource(camera,angle);
    }
    const QVariantList angles = entry.value(QStringLiteral("angles")).toList();
    if (angles.isEmpty())
        return entry;
    angle = qBound(0, angle, int(angles.size()) - 1);
    const QVariantMap a = angles.at(angle).toMap();
    const int offset = a.value(QStringLiteral("offset")).toInt();
    const int frames = a.value(QStringLiteral("frames")).toInt();
    QVariantMap source = entry;
    source.insert(QStringLiteral("path"), a.value(QStringLiteral("path")));
    source.insert(QStringLiteral("duration"), frames);
    source.remove(QStringLiteral("multicam"));
    int in = entry.value(QStringLiteral("rangeIn"), -1).toInt();
    int out = entry.value(QStringLiteral("rangeOut"), -1).toInt();
    if (in >= 0)
        in = qBound(0, in - offset, qMax(0, frames - 1));
    if (out >= 0)
        out = qBound(0, out - offset, qMax(0, frames - 1));
    if (in >= 0 && out >= 0 && out < in)
        out = in;
    source.insert(QStringLiteral("rangeIn"), in);
    source.insert(QStringLiteral("rangeOut"), out);
    return source;
}

double Backend::placementRate(const QVariantMap &source) const {
    if (!m_timeline.isEmpty() || !m_connected.isEmpty()) return m_fps;
    const int num = m_projectSettings.value("fpsNum").toInt();
    const int den = m_projectSettings.value("fpsDen").toInt();
    return m_projectSettings.value("custom").toBool() && num > 0 && den > 0
        ? double(num) / den : source.value("sourceFps", source.value("fps")).toDouble();
}

QVariantMap Backend::libraryPlacementRange(int libraryIndex) const {
    if (libraryIndex < 0 || libraryIndex >= m_library.size()) return {};
    const auto entry = m_library[libraryIndex].toMap();
    const auto source = entry.value("multicam").toBool()
        ? angleSource(entry, entry.value("activeAngle").toInt()) : entry;
    return SourceTiming::resolve(source, placementRate(source));
}

QVariantMap Backend::placementSource(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size()) return {};
    const auto entry = m_library[libraryIndex].toMap();
    const bool multicam = entry.value("multicam").toBool();
    const int angle = entry.value("activeAngle").toInt();
    const auto native = multicam ? angleSource(entry, angle) : entry;
    QVariantMap playback;
    const auto capture=[&](const QString &path) {
        if(!path.isEmpty())playback[path]=m_lastMediaSources.value(path).toMap();
    };
    capture(native.value("path").toString());
    if(multicam)for(const auto &value:entry.value("angles").toList())capture(value.toMap().value("path").toString());
    if(multicam)capture(entry.value("sound").toMap().value("path").toString());
    if(multicam)for(const auto &value:entry.value("recordings").toList())capture(value.toMap().value("path").toString());
    QVariantMap snapshot{{"source",native},{"markers",entry.value("clipMarkers").toList()},
        {"angle",angle},{"playbackSources",playback}};
    if(multicam)snapshot["mcam"]=multicamJson(entry);
    auto source=SourceTiming::placement(snapshot,placementRate(native));
    if (source.isEmpty()) {
        const auto reason = tr("The selected source range has no available pictures or sound.");
        setStatus(reason); emit failed(reason); return {};
    }
    auto tag=source.value("placementTag").toMap();
    tag["_sourceSnapshot"]=snapshot;
    // Validation is side-effect free. A caller queues this tag only immediately
    // before dispatching the accepted placement, so a refusal cannot leak it.
    source["placementTag"] = tag;
    return source;
}

void Backend::createMulticam(const QString &name, const QVariantList &libraryIndices,
                             const QString &method, int soundFrom) {
    startSync(name, libraryIndices, method, {}, soundFrom);
}

void Backend::createSyncedClip(const QString &name, const QVariantList &libraryIndices,
                               const QString &method, bool silenceCameras) {
    // Pictures first: the first sets the clip's format and clock.
    QVariantList pictures, recordings;
    for (const QVariant &v : libraryIndices) {
        const int i = v.toInt();
        const QVariantMap e = m_library.value(i).toMap();
        const bool sound = e.value(QStringLiteral("audioOnly")).toBool() || !e.value(QStringLiteral("hasVideo"), true).toBool();
        (sound ? recordings : pictures).append(i);
    }
    if (pictures.isEmpty()) {
        setStatus(tr("Sync Clips needs at least one clip with a picture."));
        emit failed(m_status);
        return;
    }
    startSync(name, pictures + recordings, method, {{QStringLiteral("synced"), true},
                                                    {QStringLiteral("silenceCameras"), silenceCameras}});
}

void Backend::setClipCategory(const QVariantList &addrs, const QString &category) {
    if (m_exporting || addrs.isEmpty())
        return;
    emit setCategoriesRequested(plainVariant(addrs).toList(), category);
}

QVariantList Backend::audioCategoriesInUse() const {
    const auto categories = m_mediaCatalog->categories();
    QStringList used;
    // Every clip the mix plays sound from, as a category stem takes it: a
    // picture that sounds (a nested sequence, a compound clip, one still
    // holding its own sound) is in its stem too, and that is the Uncategorised one.
    for (const QVariantList *list : {&m_timeline, &m_connected})
        for (const QVariant &v : *list) {
            const auto entry = v.toMap();
            if (sound::Mixer::sounds(entry))
                used.append(ClipCategories::stemOf(entry, categories));
        }
    QVariantList rows;
    for (const QVariant &v : categories)
        if (used.contains(v.toMap().value("id").toString()))
            rows.append(v);
    // A sound with no category has a stem of its own, made only when there
    // is such a sound. (Only here: elsewhere it reads as Dialogue.)
    if (used.contains(ClipCategories::uncategorisedStem()))
        rows.append(QVariantMap{{"id", ClipCategories::uncategorisedStem()}, {"name", tr("Uncategorised")}, {"kind", "audio"}});
    return rows;
}

void Backend::startSync(const QString &name, const QVariantList &libraryIndices,
                        const QString &method, const QVariantMap &synced, int soundFrom) {
    const bool makingSynced = !synced.isEmpty();
    if (m_syncing) {
        setStatus(tr("A sync is already running."));
        return;
    }
    // The sound source: a recording (no picture) is lined up with the angles,
    // last of them, and taken out again when the clip is made (it is no
    // picture angle); a clip with a picture is an angle like the others.
    QVariantList angles;
    QVariantMap recording;
    QStringList paths;
    int soundAngle = -1;
    bool soundFound = soundFrom < 0;
    for (const QVariant &v : libraryIndices) {
        const int i = v.toInt();
        if (i < 0 || i >= m_library.size())
            continue;
        const QVariantMap e = m_library.at(i).toMap();
        if (e.value(QStringLiteral("multicam")).toBool()
            || e.value(QStringLiteral("offline")).toBool()
            || paths.contains(e.value(QStringLiteral("path")).toString()))
            continue;
        const bool picked = !makingSynced && i == soundFrom;
        if (picked && !e.value(QStringLiteral("hasAudio"), true).toBool()) {
            setStatus(tr("%1 has no sound to take the multicam clip's sound from.").arg(e.value(QStringLiteral("name")).toString()));
            emit failed(m_status);
            return;
        }
        soundFound = soundFound || picked;
        const bool apart = picked && (e.value(QStringLiteral("audioOnly")).toBool() || !e.value(QStringLiteral("hasVideo"), true).toBool());
        if (picked && !apart) soundAngle = int(angles.size());
        QVariantMap a;
        a.insert(QStringLiteral("path"), e.value(QStringLiteral("path")));
        a.insert(QStringLiteral("name"), e.value(QStringLiteral("name")));
        const auto native=MulticamTiming::withMetadata(a,m_lastMediaSources.value(e.value("path").toString()).toMap());
        a=native;
        a.insert(QStringLiteral("frames"), e.value(QStringLiteral("duration")));
        a.insert(QStringLiteral("library"), i);
        // A waveform sync answers later, when the row numbers may name other
        // clips: the clip itself goes along, as it was chosen.
        a.insert(QStringLiteral("clip"), e);
        a.insert(QStringLiteral("offset"), 0);
        if (apart) recording = a;
        else angles.append(a);
        paths.append(e.value(QStringLiteral("path")).toString());
    }
    if (!soundFound) {
        setStatus(tr("The clip to take the sound from must be one of the clips chosen."));
        emit failed(m_status);
        return;
    }
    if (angles.size() < 2) {
        setStatus(makingSynced ? tr("Sync Clips needs at least two library clips.")
                  : !recording.isEmpty() ? tr("A multicam clip needs at least two angles besides the recording its sound comes from. For one camera and a recording, use Sync Clips.")
                               : tr("A multicam clip needs at least two library clips."));
        emit failed(m_status);
        return;
    }
    if (!recording.isEmpty()) {
        paths.removeAll(recording.value(QStringLiteral("path")).toString());
        paths.append(recording.value(QStringLiteral("path")).toString());
        angles.append(recording);
    }
    QVariantMap pending;
    pending.insert(QStringLiteral("name"),
                   !name.trimmed().isEmpty() ? name.trimmed()
                   : makingSynced ? tr("Synced %1").arg(m_sequences.size() + 1)
                                  : QStringLiteral("Multicam %1").arg(m_library.size() + 1));
    for (auto it = synced.cbegin(); it != synced.cend(); ++it)
        pending.insert(it.key(), it.value());
    pending.insert(QStringLiteral("angles"), angles);
    if (!recording.isEmpty()) pending.insert(QStringLiteral("soundApart"), true);
    else if (soundAngle >= 0) pending.insert(QStringLiteral("soundAngle"), soundAngle);
    pending["multicamFps"]=MulticamTiming::effectiveRate(angles.first().toMap());
    if(pending.value("multicamFps").toDouble()<=0)return;
    pending.insert(QStringLiteral("syncMethod"), method);
    QList<int> offsets;
    if (method == QStringLiteral("audio")) {
        m_pendingMulticam = pending;
        m_syncing = true;
        emit syncingChanged();
        setBusy(true);
        setStatus(tr("Syncing %1 clips by waveform…").arg(angles.size()));
        emit syncByAudioRequested(paths,pending.value("multicamFps").toDouble(),m_lastMediaSources);
        return;
    }
    if (method == QStringLiteral("timecode")) {
        // The container's timecode track when every angle has one, else
        // their creation times; both give "starts N frames after the first".
        QList<double> starts;
        bool allTc = true, allCreated = true;
        qint64 createdOrigin=0;
        bool hasCreatedOrigin=false;
        for (const QVariant &a : angles) {
            const QVariantMap e = m_library.at(a.toMap().value(QStringLiteral("library")).toInt()).toMap();
            allTc = allTc && clipTimecodeFrames(e) >= 0;
            const auto when=QDateTime::fromString(e.value("created").toString(),Qt::ISODateWithMs);
            allCreated=allCreated && when.isValid();
            if(!hasCreatedOrigin && when.isValid()) {createdOrigin=when.toMSecsSinceEpoch();hasCreatedOrigin=true;}
        }
        for (const QVariant &a : angles) {
            const QVariantMap e = m_library.at(a.toMap().value(QStringLiteral("library")).toInt()).toMap();
            if (allTc) {
                starts.append(clipTimecodeFrames(e)/MulticamTiming::nativeRate(a.toMap()));
            } else if (allCreated) {
                const QDateTime when = QDateTime::fromString(
                    e.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
                // Subtract integer timestamps before converting: large epoch
                // seconds lose enough precision to move a Floor-sampled frame.
                starts.append((when.toMSecsSinceEpoch()-createdOrigin)/1000.0);
            } else {
                setStatus(tr("No timecode or creation time on every angle — sync by waveform instead."));
                emit failed(m_status);
                return;
            }
        }
        const double first = *std::min_element(starts.cbegin(), starts.cend());
        // Creation times years apart (a recording copied to this disk today
        // beside ones made long ago) are no way to line clips up, and their
        // offset in frames is more than a clip can hold.
        if (*std::max_element(starts.cbegin(), starts.cend()) - first > 24 * 3600.) {
            setStatus(tr("These clips were made more than a day apart, so their creation times cannot line them up — sync by waveform instead."));
            emit failed(m_status);
            return;
        }
        QVariantList seconds;
        for (double start : starts) {
            seconds.append(start-first);
            offsets.append(qRound((start-first)*pending.value("multicamFps").toDouble()));
        }
        pending["offsetSeconds"]=seconds;
        pending.insert(QStringLiteral("syncMethod"),
                       allTc ? QStringLiteral("timecode") : QStringLiteral("created"));
    } else if (method == QStringLiteral("markers")) {
        QList<double> marks;
        for (const QVariant &a : angles) {
            const QVariantMap e = m_library.at(a.toMap().value(QStringLiteral("library")).toInt()).toMap();
            const QVariantList markers = e.value(QStringLiteral("clipMarkers")).toList();
            if (markers.isEmpty()) {
                setStatus(tr("%1 has no marker to sync on.")
                              .arg(e.value(QStringLiteral("name")).toString()));
                emit failed(m_status);
                return;
            }
            int first = markers.first().toMap().value(QStringLiteral("frame")).toInt();
            for (const QVariant &m : markers)
                first = qMin(first, m.toMap().value(QStringLiteral("frame")).toInt());
            marks.append(first/MulticamTiming::effectiveRate(a.toMap()));
        }
        const double latest = *std::max_element(marks.cbegin(), marks.cend());
        QVariantList seconds;
        for (double mark : marks) {
            seconds.append(latest-mark);
            offsets.append(qRound((latest-mark)*pending.value("multicamFps").toDouble()));
        }
        pending["offsetSeconds"]=seconds; // the latest marker's clip starts first
    } else {
        for (int i = 0; i < angles.size(); ++i)
            offsets.append(0);
        pending.insert(QStringLiteral("syncMethod"), QStringLiteral("start"));
    }
    finishMulticam(pending, offsets);
}

void Backend::onClipsSynced(const QStringList &paths, const QList<int> &offsets,
                            const QString &message) {
    Q_UNUSED(paths);
    if (!m_syncing)
        return;
    m_syncing = false;
    emit syncingChanged();
    setBusy(false);
    const QVariantMap pending = m_pendingMulticam;
    m_pendingMulticam.clear();
    if (!message.isEmpty()) {
        setStatus(message);
        emit failed(message);
        return;
    }
    finishMulticam(pending, offsets);
}

void Backend::finishSyncedClip(const QVariantMap &pending, const QList<int> &offsets) {
    const QVariantList angles = pending.value(QStringLiteral("angles")).toList();
    QVariantList items;
    QVariantMap first;
    for (int i = 0; i < angles.size(); ++i) {
        const QVariantMap a = angles.at(i).toMap();
        const QVariantMap e = a.value(QStringLiteral("clip")).toMap();
        if (first.isEmpty())
            first = e;
        items.append(QVariantMap{{QStringLiteral("path"), a.value(QStringLiteral("path"))},
                                 {QStringLiteral("audioOnly"), e.value(QStringLiteral("audioOnly")).toBool()
                                                                   || !e.value(QStringLiteral("hasVideo"), true).toBool()},
                                 {QStringLiteral("offset"), offsets.value(i, 0)}});
    }
    const double fps = pending.value("multicamFps").toDouble();
    const int nominal = qRound(fps);
    int fpsNum = nominal, fpsDen = 1;
    if (qAbs(fps - nominal) >= 0.005) {
        const bool drop = qAbs(fps - nominal * 1000.0 / 1001.0) < 0.005;
        fpsNum = drop ? nominal * 1000 : qRound(fps * 1000);
        fpsDen = drop ? 1001 : 1000;
    }
    emit addSyncedClipRequested(pending.value(QStringLiteral("name")).toString(), items,
                                pending.value(QStringLiteral("silenceCameras")).toBool(),
                                {{"custom", true}, {"width", first.value("width")}, {"height", first.value("height")},
                                 {"fpsNum", fpsNum}, {"fpsDen", fpsDen}});
}

void Backend::finishMulticam(const QVariantMap &pending, const QList<int> &offsets) {
    if (pending.value(QStringLiteral("synced")).toBool()) {
        finishSyncedClip(pending, offsets);
        return;
    }
    QVariantList angles = pending.value(QStringLiteral("angles")).toList();
    const double clock=pending.value("multicamFps").toDouble();
    if(clock<=0 || angles.isEmpty())return;
    int duration = 0;
    bool hasAudio = false;
    QVariantMap first;
    for (int i = 0; i < angles.size(); ++i) {
        QVariantMap a = angles.at(i).toMap();
        a.insert(QStringLiteral("offset"), offsets.value(i, 0));
        a["offsetSeconds"]=pending.value("offsetSeconds").toList().value(i,offsets.value(i,0)/clock);
        a["frames"]=qMax(1,qRound(MulticamTiming::nativeFrames(a)*clock/MulticamTiming::effectiveRate(a)));
        const QVariantMap e = a.value(QStringLiteral("clip")).toMap();
        if (i == 0)
            first = e;
        hasAudio = hasAudio || e.value(QStringLiteral("hasAudio"), true).toBool();
        a.remove(QStringLiteral("library"));
        a.remove(QStringLiteral("clip"));
        // (A recording the sound comes from is no angle: the clip is as long
        // as its pictures are.)
        if (i + 1 < angles.size() || !pending.value(QStringLiteral("soundApart")).toBool())
            duration = qMax(duration, offsets.value(i, 0) + a.value(QStringLiteral("frames")).toInt());
        angles[i] = a;
    }
    QVariantMap sound;
    if (pending.value(QStringLiteral("soundApart")).toBool())
        sound = angles.takeLast().toMap();
    else if (pending.contains(QStringLiteral("soundAngle")))
        sound = angles.value(pending.value(QStringLiteral("soundAngle")).toInt()).toMap();
    QVariantMap entry;
    const QString id = QString::number(QDateTime::currentMSecsSinceEpoch(), 36)
                       + QString::number(m_library.size());
    entry.insert(QStringLiteral("multicam"), true);
    entry["multicamVersion"]=2;entry["multicamFps"]=clock;
    const auto chosen=angles.first().toMap();
    entry["sourceFps"]=MulticamTiming::nativeRate(chosen);
    entry["sourceDuration"]=MulticamTiming::nativeFrames(chosen);
    entry["forceFps"]=chosen.value("forceFps");
    entry.insert(QStringLiteral("id"), id);
    entry.insert(QStringLiteral("path"), QStringLiteral("multicam:") + id);
    entry.insert(QStringLiteral("name"), pending.value(QStringLiteral("name")));
    entry.insert(QStringLiteral("angles"), angles);
    if (!sound.isEmpty()) {
        entry.insert(QStringLiteral("sound"), sound);
        if (pending.contains(QStringLiteral("soundAngle")))
            entry.insert(QStringLiteral("soundAngle"), pending.value(QStringLiteral("soundAngle")));
        else // a recording that is no angle: kept by the clip, whatever its sound is changed to later
            entry.insert(QStringLiteral("recordings"), QVariantList{sound});
    }
    entry.insert(QStringLiteral("syncMethod"), pending.value(QStringLiteral("syncMethod")));
    entry.insert(QStringLiteral("activeAngle"), 0);
    entry.insert(QStringLiteral("duration"), duration);
    entry.insert(QStringLiteral("fps"), clock);
    entry.insert(QStringLiteral("width"), first.value(QStringLiteral("width")));
    entry.insert(QStringLiteral("height"), first.value(QStringLiteral("height")));
    entry.insert(QStringLiteral("hasVideo"), true);
    entry.insert(QStringLiteral("hasAudio"), hasAudio);
    entry.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    if (first.contains(QStringLiteral("thumb")))
        entry.insert(QStringLiteral("thumb"), first.value(QStringLiteral("thumb")));
    m_library.append(entry);
    emit libraryChanged();
    setDirty(true);
    setStatus(sound.isEmpty()
              ? tr("Multicam clip “%1”: %2 angles, synced by %3")
                  .arg(entry.value(QStringLiteral("name")).toString())
                  .arg(angles.size())
                  .arg(entry.value(QStringLiteral("syncMethod")).toString())
              : tr("Multicam clip “%1”: %2 angles, synced by %3, sound from %4")
                  .arg(entry.value(QStringLiteral("name")).toString())
                  .arg(angles.size())
                  .arg(entry.value(QStringLiteral("syncMethod")).toString(), sound.value(QStringLiteral("name")).toString()));
    // Lined up by the cameras' clocks, an angle that starts after another has
    // ended shares no moment with it: clocks that were not set alike. Said,
    // because every cut between the two will be refused.
    const QString method = entry.value(QStringLiteral("syncMethod")).toString();
    if (method == QLatin1String("timecode") || method == QLatin1String("created")) {
        int last = 0, soonest = 0;
        const auto start = [&](int i) { return angles.at(i).toMap().value(QStringLiteral("offset")).toInt(); };
        const auto end = [&](int i) { return start(i) + angles.at(i).toMap().value(QStringLiteral("frames")).toInt(); };
        for (int i = 1; i < angles.size(); ++i) {
            if (start(i) > start(last)) last = i;
            if (end(i) < end(soonest)) soonest = i;
        }
        if (start(last) >= end(soonest)) {
            const QString late = angles.at(last).toMap().value(QStringLiteral("name")).toString();
            const QString early = angles.at(soonest).toMap().value(QStringLiteral("name")).toString();
            m_backgroundTasks->post(QStringLiteral("warning"), tr("Angles share no moment"),
                method == QLatin1String("timecode")
                    ? tr("By their timecode, %1 starts after %2 has ended: the cameras' clocks were not set alike. Sync by audio waveform instead.").arg(late, early)
                    : tr("By their creation time, %1 starts after %2 has ended. Sync by audio waveform instead.").arg(late, early));
        }
    }
}

// A restored multicam entry has no poster until its first angle's import
// lands; borrow that angle's.
void Backend::refreshMulticamThumbs() {
    for (int i = 0; i < m_library.size(); ++i) {
        QVariantMap entry = m_library.at(i).toMap();
        if (!entry.value(QStringLiteral("multicam")).toBool()
            || entry.contains(QStringLiteral("thumb")))
            continue;
        const QVariantList angles = entry.value(QStringLiteral("angles")).toList();
        if (angles.isEmpty())
            continue;
        const QString path = angles.first().toMap().value(QStringLiteral("path")).toString();
        for (const QVariant &v : std::as_const(m_library)) {
            const QVariantMap e = v.toMap();
            if (e.value(QStringLiteral("path")).toString() == path
                && e.contains(QStringLiteral("thumb"))) {
                entry.insert(QStringLiteral("thumb"), e.value(QStringLiteral("thumb")));
                m_library[i] = entry;
                break;
            }
        }
    }
}

void Backend::setMulticamMode(bool on) {
    if (m_multicamMode == on)
        return;
    m_multicamMode = on;
    emit multicamChanged();
    if (on)
        requestAngleFrames();
    else if (!m_angleFrames.isEmpty()) {
        m_angleFrames.clear();
        emit angleFramesChanged();
    }
}

void Backend::setMulticamAudioFollows(bool on) {
    QMetaObject::invokeMethod(m_worker, [this, on] { m_worker->setAudioFollowsVideo(on); }, Qt::QueuedConnection);
    if (m_multicamAudioFollows == on)
        return;
    m_multicamAudioFollows = on;
    emit multicamChanged();
}

// The multicam clip under the playhead: the highest track wins, like the
// composite. Changes ripple to the viewer's grid.
void Backend::updateMulticamClip() {
    QVariantMap found;
    auto consider = [&](const QVariantMap &e, int track, int index) {
        const QString json = e.value(ClipKey::mcam).toString();
        if (json.isEmpty())
            return;
        const int start = e.value(ClipKey::start).toInt();
        const int frames = e.value(ClipKey::frames).toInt();
        if (m_position < start || m_position >= start + frames)
            return;
        if (!found.isEmpty() && found.value(QStringLiteral("track")).toInt() > track)
            return;
        found.clear();
        found["entry"]=e;
        found.insert(QStringLiteral("has"), true);
        found.insert(QStringLiteral("track"), track);
        found.insert(QStringLiteral("index"), index);
        found.insert(QStringLiteral("angle"), e.value(ClipKey::mcamAngle).toInt());
        found.insert(QStringLiteral("start"), start);
        found.insert(QStringLiteral("frames"), frames);
        found.insert(QStringLiteral("in"), e.value(ClipKey::in).toInt());
        found.insert(QStringLiteral("speed"), e.value(ClipKey::speed, 1.0).toDouble());
        found.insert(QStringLiteral("videoOnly"), e.value(ClipKey::videoOnly).toBool());
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        found.insert(QStringLiteral("name"), o.value(QStringLiteral("name")).toString());
        found.insert(QStringLiteral("id"), o.value(QStringLiteral("id")).toString());
        QVariantList angles;
        for (const auto &v : o.value(QStringLiteral("angles")).toArray()) {
            QVariantMap a=v.toObject().toVariantMap();
            a.insert(QStringLiteral("name"), v.toObject().value(QStringLiteral("name")).toString());
            a.insert(QStringLiteral("path"), v.toObject().value(QStringLiteral("path")).toString());
            a.insert(QStringLiteral("offset"), v.toObject().value(QStringLiteral("offset")).toInt());
            a.insert(QStringLiteral("frames"), v.toObject().value(QStringLiteral("frames")).toInt());
            angles.append(a);
        }
        found.insert(QStringLiteral("angles"), angles);
        // Where its sound comes from, when not from the angle shown.
        const QString sound = o.value(QStringLiteral("sound")).toObject().value(QStringLiteral("name")).toString();
        if (!sound.isEmpty()) {
            found.insert(QStringLiteral("soundFrom"), sound);
            found.insert(QStringLiteral("soundAngle"), o.value(QStringLiteral("soundAngle")).toInt(-1));
        }
    };
    for (int i = 0; i < m_timeline.size(); ++i)
        consider(m_timeline.at(i).toMap(), 0, i);
    for (const QVariant &v : std::as_const(m_connected)) {
        const QVariantMap e = v.toMap();
        consider(e, e.value(ClipKey::track).toInt(),
                 e.value(ClipKey::index).toInt());
    }
    if (found.isEmpty())
        found.insert(QStringLiteral("has"), false);
    const bool changed = found != m_multicamClip;
    m_multicamClip = found;
    if (changed)
        emit multicamChanged();
    if (m_multicamMode)
        requestAngleFrames();
}

void Backend::requestAngleFrames() {
    if (!m_multicamClip.value(QStringLiteral("has")).toBool()) {
        if (!m_angleFrames.isEmpty()) {
            m_angleFrames.clear();
            emit angleFramesChanged();
        }
        return;
    }
    if (m_anglesPending) {
        m_anglesDirty = true; // one decode in flight; ask again when it lands
        return;
    }
    const QVariantList angles = m_multicamClip.value(QStringLiteral("angles")).toList();
    auto shownEntry=m_multicamClip.value("entry").toMap();
    auto camera=QJsonDocument::fromJson(shownEntry.value(ClipKey::mcam).toString().toUtf8()).toVariant().toMap();
    if(camera.value("version").toInt()==2) {
        auto definitions=camera.value("angles").toList();QStringList paths;
        for(auto &value:definitions) {
            const auto angle=value.toMap();const auto path=angle.value("path").toString();
            paths.append(path);if(m_lastMediaSources.contains(path))value=MulticamTiming::withMetadata(angle,m_lastMediaSources.value(path).toMap());
        }
        camera["angles"]=definitions;
        shownEntry[ClipKey::mcam]=QString::fromUtf8(QJsonDocument::fromVariant(camera).toJson(QJsonDocument::Compact));
        const auto frames=Timeline::multicamNativeFrames(shownEntry,m_position-shownEntry.value(ClipKey::start).toInt(),m_fps);
        m_anglesPending=true;m_anglesDirty=false;
        emit decodeAnglesRequested(paths,frames,++m_angleSeq);return;
    }
    const int shown = m_multicamClip.value(QStringLiteral("angle")).toInt();
    const int start = m_multicamClip.value(QStringLiteral("start")).toInt();
    const int in = m_multicamClip.value(QStringLiteral("in")).toInt();
    const double speed = qAbs(m_multicamClip.value(QStringLiteral("speed"), 1.0).toDouble());
    const int source = in + qRound((m_position - start) * (speed > 0 ? speed : 1.0));
    const int virtualFrame = source + (Timeline::angleWithoutFootage(shownEntry) ? 0
                                       : angles.value(shown).toMap().value(QStringLiteral("offset")).toInt());
    QStringList paths;
    QList<int> frames;
    for (const QVariant &v : angles) {
        const QVariantMap a = v.toMap();
        const int f = virtualFrame - a.value(QStringLiteral("offset")).toInt();
        const int n = a.value(QStringLiteral("frames")).toInt();
        paths.append(a.value(QStringLiteral("path")).toString());
        frames.append(f >= 0 && (n <= 0 || f < n) ? f : -1);
    }
    m_anglesPending = true;
    m_anglesDirty = false;
    emit decodeAnglesRequested(paths, frames, ++m_angleSeq);
}

void Backend::onAnglesReady(const QList<QImage> &frames, int seq) {
    if (seq != m_angleSeq) // a newer request superseded this one
        return;
    m_anglesPending = false;
    if (!m_multicamMode) {
        // A decode already in flight can finish after the grid closes.
        m_anglesDirty = false;
        return;
    }
    QStringList urls;
    QStringList keys;
    for (int i = 0; i < frames.size(); ++i) {
        if (frames.at(i).isNull()) {
            urls.append(QString());
            continue;
        }
        const QString key = QStringLiteral("angle-%1-%2").arg(i).arg(seq);
        if (m_thumbs) // headless (tests) still reports what decoded
            m_thumbs->insert(key, frames.at(i));
        keys.append(key);
        urls.append(QStringLiteral("image://thumbs/") + key);
    }
    // The pictures of the grid before last go. Those of the last grid stay
    // one turn more: a cell loads its picture behind the scenes, and one
    // asked for as the next grid arrived would find it gone and draw nothing.
    if (m_thumbs)
        for (const QString &old : std::as_const(m_angleKeysBefore))
            m_thumbs->remove(old);
    m_angleKeysBefore = m_angleKeys;
    m_angleKeys = keys;
    m_angleFrames = urls;
    emit angleFramesChanged();
    if (m_anglesDirty && m_multicamMode)
        requestAngleFrames();
}

void Backend::cutToAngle(int angle, bool cut) {
    if (m_exporting)
        return;
    // A key that changes nothing says why: the viewer's keys are live with no
    // multicam clip under the playhead, and for angles the clip has not got.
    if (!m_multicamClip.value(QStringLiteral("has")).toBool()) {
        sayRefused(tr("The playhead is not on a multicam clip; no angle was changed."));
        return;
    }
    const QVariantList angles = m_multicamClip.value(QStringLiteral("angles")).toList();
    if (angle < 0 || angle >= angles.size()) {
        if (angle >= 0)
            sayRefused(tr("This multicam clip has no angle %1; it has %2.").arg(angle + 1).arg(angles.size()));
        return;
    }
    if (angle == m_multicamClip.value(QStringLiteral("angle")).toInt()) {
        sayRefused(tr("Already on %1")
                       .arg(angles.at(angle).toMap().value(QStringLiteral("name")).toString()));
        return;
    }
    const int start = m_multicamClip.value(QStringLiteral("start")).toInt();
    // (No frame: the whole segment under the playhead, with no cut.)
    emit cutToAngleRequested(m_multicamClip.value(QStringLiteral("track")).toInt(),
                             m_multicamClip.value(QStringLiteral("index")).toInt(),
                             cut ? m_position : -1, angle, !m_multicamAudioFollows);
    // (Said where it is read: in place of a finished task's notice, as a
    // refused edit's reason is.)
    sayRefused(tr("%1 %2")
                   .arg(cut && m_position > start ? tr("Cut to") : tr("Switched to"),
                        angles.at(angle).toMap().value(QStringLiteral("name")).toString()));
}

void Backend::switchAngle(int track, int index, int angle) {
    if (m_exporting)
        return;
    QVariantMap e;
    if (track == 0) {
        if (index >= 0 && index < m_timeline.size())
            e = m_timeline.at(index).toMap();
    } else {
        for (const QVariant &v : std::as_const(m_connected)) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::track).toInt() == track
                && m.value(ClipKey::index).toInt() == index)
                e = m;
        }
    }
    if (e.isEmpty() || e.value(ClipKey::mcam).toString().isEmpty())
        return;
    const QList<Timeline::Angle> angles =
        Timeline::anglesOf(e.value(ClipKey::mcam).toString());
    if (angle < 0 || angle >= angles.size())
        return;
    // The angle it shows already (its own line of the Switch Angle menu):
    // nothing to do, and nothing to refuse.
    if (angle == e.value(ClipKey::mcamAngle).toInt()) {
        sayRefused(tr("Already on %1").arg(angles.at(angle).name));
        return;
    }
    emit cutToAngleRequested(track, index, -1, angle, !m_multicamAudioFollows);
    sayRefused(tr("Switched to %1").arg(angles.at(angle).name));
}

// ── The sound source, chosen after the clip is made ──────────────────────
// The rows of a "Sound From" menu for a multicam description (a placed
// clip's tag, or a library item): "Follows the picture", then every source
// lined up when the clip was made that has sound: each angle by its number
// and name, then a recording that is no angle.
QVariantList Backend::soundSourceRows(const QVariantMap &camera) const {
    QVariantList rows;
    if (camera.value(QStringLiteral("angles")).toList().isEmpty())
        return rows;
    const int current = Timeline::soundSourceIn(camera);
    rows.append(QVariantMap{{QStringLiteral("source"), -1}, {QStringLiteral("text"), tr("Follows the picture")},
                            {QStringLiteral("current"), current == -1}, {QStringLiteral("name"), QString()}});
    const auto hasSound = [this](const QString &path) {
        for (const QVariant &v : std::as_const(m_library)) {
            const QVariantMap e = v.toMap();
            if (!e.value(QStringLiteral("multicam")).toBool() && e.value(QStringLiteral("path")).toString() == path)
                return e.value(QStringLiteral("hasAudio"), true).toBool();
        }
        // (No longer in the library: its file says. One gone missing is
        // still listed; choosing it says what is wrong.)
        return !QFileInfo::exists(path) || media::probeCached(path).hasAudio;
    };
    for (const QVariant &v : Timeline::soundSourcesIn(camera)) {
        const QVariantMap source = v.toMap();
        const int number = source.value(QStringLiteral("source")).toInt();
        const QString name = source.value(QStringLiteral("name")).toString();
        if (number != current && !hasSound(source.value(QStringLiteral("path")).toString()))
            continue;
        rows.append(QVariantMap{{QStringLiteral("source"), number},
                                {QStringLiteral("text"), source.value(QStringLiteral("recording")).toBool()
                                                             ? QStringLiteral("♪  ") + name : QString::number(number + 1) + QStringLiteral("  ") + name},
                                {QStringLiteral("current"), number == current}, {QStringLiteral("name"), name}});
    }
    return rows;
}

QVariantList Backend::multicamSoundSources(const QString &mcam) const {
    const QVariantMap camera = QJsonDocument::fromJson(mcam.toUtf8()).toVariant().toMap();
    return camera.value(QStringLiteral("version")).toInt() == 2 ? soundSourceRows(camera) : QVariantList();
}

QVariantList Backend::libraryMulticamSoundSources(int libraryIndex) const {
    const QVariantMap entry = m_library.value(libraryIndex).toMap();
    return entry.value(QStringLiteral("multicam")).toBool() && entry.value(QStringLiteral("multicamVersion")).toInt() == 2
               ? soundSourceRows(entry) : QVariantList();
}

void Backend::setMulticamSound(int track, int index, int source) {
    if (m_exporting)
        return;
    QVariantMap e;
    if (track == 0) {
        if (index >= 0 && index < m_timeline.size())
            e = m_timeline.at(index).toMap();
    } else {
        for (const QVariant &v : std::as_const(m_connected)) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::track).toInt() == track && m.value(ClipKey::index).toInt() == index)
                e = m;
        }
    }
    const QString mcam = e.value(ClipKey::mcam).toString();
    if (mcam.isEmpty() || e.value(ClipKey::audioOnly).toBool()) {
        sayRefused(tr("That is not a multicam clip; no sound was changed."));
        emit failed(m_status);
        return;
    }
    const QVariantMap camera = QJsonDocument::fromJson(mcam.toUtf8()).toVariant().toMap();
    if (camera.value(QStringLiteral("version")).toInt() != 2) {
        sayRefused(tr("This multicam clip was made by an older version and has one sound only; make it again to choose its sound."));
        emit failed(m_status);
        return;
    }
    // Offered only what was lined up when the clip was made and has sound.
    bool offered = false;
    for (const QVariant &row : soundSourceRows(camera))
        offered = offered || row.toMap().value(QStringLiteral("source")).toInt() == source;
    if (!offered) {
        const QString name = Timeline::soundSourcesIn(camera).value(source).toMap().value(QStringLiteral("name")).toString();
        sayRefused(source < 0 || name.isEmpty() ? tr("This multicam clip has no such sound source; the sound was not changed.")
                                               : tr("%1 has no sound to take the multicam clip's sound from.").arg(name));
        emit failed(m_status);
        return;
    }
    emit setMulticamSoundRequested(track, index, source);
}

void Backend::setLibraryMulticamSound(int libraryIndex, int source) {
    if (m_exporting || libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    QVariantMap entry = m_library.at(libraryIndex).toMap();
    const auto refuse = [this](const QString &why) { sayRefused(why); emit failed(m_status); };
    if (!entry.value(QStringLiteral("multicam")).toBool())
        return refuse(tr("That is not a multicam clip; no sound was changed."));
    if (entry.value(QStringLiteral("multicamVersion")).toInt() != 2)
        return refuse(tr("This multicam clip was made by an older version and has one sound only; make it again to choose its sound."));
    QString name;
    bool offered = false;
    for (const QVariant &row : soundSourceRows(entry))
        if (row.toMap().value(QStringLiteral("source")).toInt() == source) {
            offered = true;
            name = row.toMap().value(QStringLiteral("name")).toString();
        }
    if (!offered) {
        const QString named = Timeline::soundSourcesIn(entry).value(source).toMap().value(QStringLiteral("name")).toString();
        return refuse(source < 0 || named.isEmpty() ? tr("This multicam clip has no such sound source; the sound was not changed.")
                                                   : tr("%1 has no sound to take the multicam clip's sound from.").arg(named));
    }
    if (Timeline::soundSourceIn(entry) == source) {
        sayRefused(source < 0 ? tr("The sound follows the picture already") : tr("The sound is from %1 already").arg(name));
        return;
    }
    if (!Timeline::chooseSoundSourceIn(entry, source))
        return;
    m_library[libraryIndex] = entry;
    emit libraryChanged();
    setDirty(true);
    const QString clip = entry.value(QStringLiteral("name")).toString();
    sayRefused(source < 0
                   ? tr("“%1”: the sound follows the picture when it is placed from now on. Clips already on a timeline keep their sound.").arg(clip)
                   : tr("“%1”: sound from %2 when it is placed from now on. Clips already on a timeline keep their sound.").arg(clip, name));
}

// ── Collapse Multicam ────────────────────────────────────────────────────
int Backend::multicamPiecesIn(int index) const {
    if (index < 0 || index >= m_sequences.size())
        return 0;
    // The open timeline as it is shown; another as it is kept.
    return Timeline::multicamPiecesIn(index == m_activeSequence ? m_timeline + m_connected
                                                               : m_sequences.at(index).toMap().value(QStringLiteral("snapshot")).toList());
}

void Backend::collapseMulticam(int index) {
    if (m_exporting || index < 0 || index >= m_sequences.size())
        return;
    const QVariantMap timeline = m_sequences.at(index).toMap();
    if (multicamPiecesIn(index) == 0) {
        sayRefused(tr("“%1” has no multicam clips; nothing was collapsed.").arg(timeline.value(QStringLiteral("name")).toString()));
        emit failed(m_status);
        return;
    }
    emit collapseMulticamRequested(timeline.value(QStringLiteral("id")).toString());
}
