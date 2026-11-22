#include "playerworker_p.h"

QString PlayerWorker::newSequenceId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString PlayerWorker::nestFolder() const {
    return StoragePreferences::cacheRoot() + QStringLiteral("/omaedit-nests/") + m_nestSessionId;
}

QString PlayerWorker::nestPath(const QString &id) const {
    return QDir(nestFolder()).absoluteFilePath(id + ProjectFile::extension());
}

bool PlayerWorker::writeNest(const Sequence &seq) {
    if (seq.id.isEmpty())
        return false;
    m_nestsWritten.remove(seq.id); // until this write is known to have landed
    QDir().mkpath(nestFolder());   // made with its first file, not for every look
    QVariantList clips = seq.snapshot;
    std::vector<Timeline::TrackFlags> tracks;
    bool hasTracks = false;
    QVariantMap master;
    if (!clips.isEmpty() && clips.last().toMap().contains(QStringLiteral("omavid:markers"))) {
        const QVariantMap last = clips.takeLast().toMap();
        master = last.value("omavid:master").toMap();
        if (last.contains(QStringLiteral("omavid:tracks"))) {
            hasTracks = true;
            tracks = Timeline::trackFlagsFromVariant(last.value(QStringLiteral("omavid:tracks")).toList());
        }
    }
    Timeline scratch;
    scratch.setAllowOffline(true);
    const QVariantMap settings = seq.settings.isEmpty() ? m_timeline.projectSettings() : seq.settings;
    scratch.setProjectSettings(settings);
    scratch.setMediaSources(m_timeline.mediaSources(), QStringLiteral("original"));
    if (!(hasTracks ? scratch.restore(clips, tracks) : scratch.restore(clips)))
        return false;
    scratch.setMasterMixMap(master);
    if (!scratch.saveDocument(nestPath(seq.id), QStringList()))
        return false;
    // What the file was written from, and the nested files it embeds as they
    // stood: refreshNests() leaves it alone while all of that is unchanged.
    const auto facts = [](const QString &path) {
        const QFileInfo file(path);
        return QVariantList{file.size(), file.lastModified().toMSecsSinceEpoch()};
    };
    NestWritten written{seq.snapshot, settings, m_timeline.mediaSources(), {}};
    written.children.insert(nestPath(seq.id), facts(nestPath(seq.id))); // the file itself, as written
    for (const auto &v : ProjectSafety::clipEntries(seq.snapshot)) {
        const QString path = v.toMap().value(ClipKey::path).toString();
        if (!ProjectFile::isProject(path) || written.children.contains(path)) continue;
        written.children.insert(path, facts(path));
    }
    m_nestsWritten.insert(seq.id, written);
    return true;
}

// Whether the sequence's file already holds what writeNest() would write.
bool PlayerWorker::nestCurrent(const Sequence &seq) const {
    const auto it = m_nestsWritten.constFind(seq.id);
    if (it == m_nestsWritten.constEnd())
        return false;
    if (it->snapshot != seq.snapshot || it->sources != m_timeline.mediaSources()
        || it->settings != (seq.settings.isEmpty() ? m_timeline.projectSettings() : seq.settings))
        return false;
    for (auto child = it->children.constBegin(); child != it->children.constEnd(); ++child) {
        const QFileInfo file(child.key());
        if (child.value() != QVariant(QVariantList{file.size(), file.lastModified().toMSecsSinceEpoch()}))
            return false;
    }
    return true;
}

bool PlayerWorker::refreshNests() {
    QHash<QString, int> indexes;
    for (int i = 0; i < m_sequences.size(); ++i) indexes.insert(m_sequences[i].id, i);
    QHash<QString, QSet<QString>> children;
    QSet<QString> referenced;
    for (const auto &seq : std::as_const(m_sequences)) {
        for (const auto &v : ProjectSafety::clipEntries(seq.snapshot)) {
            const auto entry = v.toMap();
            const QString path = entry.value(ClipKey::path).toString();
            const QString id = entry.value(ClipKey::nestId, QFileInfo(path).completeBaseName()).toString();
            if (ProjectFile::isProject(path) && indexes.contains(id)) {
                children[seq.id].insert(id);
                referenced.insert(id);
            }
        }
        if (QFileInfo::exists(nestPath(seq.id))) referenced.insert(seq.id);
    }
    // A parent embeds its children for recovery and queued renders. Refresh
    // them in dependency order so a deep child edit reaches every ancestor.
    QList<int> order;
    QSet<QString> visiting, done;
    std::function<bool(const QString &)> visit = [&](const QString &id) {
        if (done.contains(id)) return true;
        if (visiting.contains(id)) return false;
        visiting.insert(id);
        for (const auto &child : children.value(id)) if (!visit(child)) return false;
        visiting.remove(id);
        done.insert(id);
        order.append(indexes.value(id));
        return true;
    };
    for (const auto &id : referenced) if (!visit(id)) return false;
    // Children come first, so a parent sees its child's new file. Each save,
    // recovery copy and switch comes through here: a file whose timeline has
    // not changed is not rebuilt and written again.
    for (int index : order)
        if (!nestCurrent(m_sequences.at(index)) && !writeNest(m_sequences.at(index))) return false;
    return true;
}

void PlayerWorker::announceSequences() {
    QVariantList list;
    for (int i = 0; i < m_sequences.size(); ++i) {
        const Sequence &seq = m_sequences.at(i);
        QVariantMap m;
        m.insert(QStringLiteral("name"), seq.name);
        m.insert(QStringLiteral("id"), seq.id);
        m.insert(QStringLiteral("settings"), i == m_activeSequence ? m_timeline.projectSettings() : seq.settings);
        m.insert(QStringLiteral("configured"), seq.configured);
        m.insert(QStringLiteral("lockFormat"), seq.lockFormat);
        m.insert(QStringLiteral("binId"), seq.binId);
        m.insert(QStringLiteral("synced"), seq.synced);
        m.insert(QStringLiteral("position"), i == m_activeSequence ? m_position : seq.position);
        m.insert(QStringLiteral("audioPosition"), i == m_activeSequence ? parkedAudioPosition() : seq.audioPosition);
        m.insert(QStringLiteral("snapshot"), i == m_activeSequence ? snapshotNow() : seq.snapshot);
        list.append(m);
    }
    emit sequencesChanged(list, m_activeSequence);
}

void PlayerWorker::stashActiveSequence() {
    if (m_activeSequence < 0 || m_activeSequence >= m_sequences.size())
        return;
    // Always the full snapshot — an empty sequence still owns its markers
    // and track headers.
    m_sequences[m_activeSequence].snapshot = snapshotNow();
    m_sequences[m_activeSequence].position = m_position;
    m_sequences[m_activeSequence].audioPosition = parkedAudioPosition();
    m_sequences[m_activeSequence].settings = m_timeline.projectSettings();
    // Nested elsewhere? Its file plays the latest edit.
    if (!m_sequences[m_activeSequence].id.isEmpty()
        && QFileInfo::exists(nestPath(m_sequences[m_activeSequence].id))) {
        if (!refreshNests()) emit editRefused(tr("Could not refresh the nested sequences. Check their media and cache storage."));
    }
}

void PlayerWorker::createSequence(int request, const QString &anchorId, const QString &name, const QVariantMap &settings) {
    const auto fail=[this,request](const QString &error){emit sequenceCreateFinished(request,false,error);};
    if (exportActive()) { fail(tr("Wait for the current export before creating a sequence.")); return; }
    bool found=false; QStringList names;
    for (const auto &sequence : m_sequences) { found|=!anchorId.isEmpty() && sequence.id==anchorId; if (sequence.configured) names.append(sequence.name); }
    if (!found) { fail(tr("The project changed. Reopen New Sequence to continue.")); return; }
    const auto error=CreationNames::problem(name,names);
    if (!error.isEmpty()) { fail(error); return; }
    if (!settings.isEmpty()) {
        auto validated = settings; validated["title"] = QStringLiteral("Timeline");
        if (const auto problem=CreationNames::projectProblem(validated); !problem.isEmpty()) { fail(problem); return; }
    }
    const auto container = m_timeline.projectSettings();
    addSequence(name.trimmed());
    if (!settings.isEmpty()) {
        auto next = settings;
        for (const char *key : {"title", "proxyFolder", "importSpace", "importLut"}) next[key]=container.value(key);
        m_timeline.setProjectSettings(next);
        m_sequences[m_activeSequence].lockFormat = true;
        announceTimeline();
        announceSequences();
    }
    emit sequenceCreateFinished(request,true,{});
}

void PlayerWorker::addSequence(const QString &name) {
    if (exportActive())
        return;
    const auto before=sourceAttributeState({});
    setSpeed(0.0);
    stashActiveSequence();
    Sequence seq;
    seq.name = name.trimmed().isEmpty() ? QStringLiteral("Sequence %1").arg(m_sequences.size() + 1)
                                        : name.trimmed();
    seq.id = newSequenceId();
    seq.settings = m_timeline.projectSettings();
    seq.configured = true;
    if (m_sequences.size() == 1 && !m_sequences.first().configured)
        m_sequences[0] = std::move(seq);
    else
        m_sequences.append(seq);
    m_activeSequence = m_sequences.size() - 1;
    m_sourceMode = false;
    endTimelineDecorJobs();
    m_position = 0;
    m_timeline.clear();
    m_timeline.setMasterMixMap({});
    m_timeline.setAdrCues({});
    if (!m_markers.isEmpty()) {
        m_markers.clear();
        emit markersChanged(m_markers);
    }
    recordSequenceEdit(before,tr("Create Timeline"));
    announceTimeline();
    emit frameReady(QImage(), 0);
    announceSequences();
}

void PlayerWorker::importSequence(const QString &name, const QVariantList &snapshot,
                                  const QStringList &trackNames, const QVariantList &markers,
                                  const QVariantMap &format, const QStringList &problems) {
    const auto fail = [&](const QString &error) { emit sequenceImportFinished(name, problems, false, error); };
    if (exportActive()) { fail(tr("Wait for the current export before importing a sequence.")); return; }
    // The GUI converted placement in the announced format. A queued sequence
    // switch may already have changed the live profile; use the captured format.
    const auto previousSettings = m_timeline.projectSettings();
    const auto previousFormat = resolvedTimelineSettings();
    auto settings = previousFormat;
    for (const char *key : {"custom","width","height","fpsNum","fpsDen","matchedProfile",
                           "colorspace","sampleRate","channels","startFrame","startRate"})
        if (format.contains(key)) settings[key] = format.value(key);
    // Validate against real media before touching the active edit or its history.
    // A structurally valid map can still exceed its producer's available frames.
    {
        Timeline trial;
        trial.setAllowOffline(true);
        trial.setProjectSettings(settings);
        trial.setMediaSources(m_timeline.mediaSources(), m_timeline.proxyMode());
        if (!trial.restore(snapshot)) {
            fail(tr("Could not import “%1”. Its media or timing could not be loaded.").arg(name));
            return;
        }
    }
    const auto beforeHistory=sourceAttributeState({});
    setSpeed(0.0);
    stashActiveSequence();
    const auto previous = m_timeline.entries();
    const auto flags = Timeline::trackFlagsFromVariant(m_timeline.trackFlagsVariant());
    m_timeline.setProjectSettings(settings, false);
    if (!m_timeline.restore(snapshot, {})) {
        // Media can disappear between validation and placement. The active
        // sequence can have a different format from the import request.
        m_timeline.setProjectSettings(previousFormat, false);
        const bool recovered = m_timeline.restore(previous, flags);
        m_timeline.setProjectSettings(previousSettings);
        announceTimeline(false);
        refreshFrame();
        fail(recovered ? tr("Could not import “%1”. Its media or timing could not be loaded.").arg(name)
                       : tr("Could not import “%1” or reload the previous media. The previous sequence snapshot is retained.").arg(name));
        return;
    }
    dropStabJob();
    dropTrackJob();
    endTimelineDecorJobs();
    Sequence seq;
    seq.name = name.trimmed().isEmpty() ? QStringLiteral("Sequence %1").arg(m_sequences.size() + 1) : name.trimmed();
    seq.id = newSequenceId(); seq.settings = settings; seq.configured = true; seq.lockFormat = true;
    if (m_sequences.size() == 1 && !m_sequences.first().configured) m_sequences[0] = seq;
    else m_sequences.append(seq);
    m_activeSequence = m_sequences.size() - 1;
    m_sourceMode = false;
    m_lastEditGesture = 0;
    m_timeline.setMasterMixMap({}); m_timeline.setAdrCues({});
    for (int lane = 0; lane < trackNames.size(); ++lane)
        if (!trackNames.at(lane).isEmpty()) m_timeline.setTrackName(lane, trackNames.at(lane));
    if (m_markers != markers) { m_markers = markers; emit markersChanged(m_markers); }
    m_position = 0;
    recordSequenceEdit(beforeHistory,tr("Import Timeline"));
    // The new timeline is named before its clips are shown (see loadProject).
    seek(0);
    stashActiveSequence();
    announceSequences();
    announceTimeline();
    emit sequenceImportFinished(seq.name, problems, true, {});
}

void PlayerWorker::addSyncedClip(const QString &name, const QVariantList &items, bool silenceCameras,
                                 const QVariantMap &format) {
    const auto fail = [&](const QString &error) { emit syncedClipFinished(name, error); };
    if (exportActive()) { fail(tr("Wait for the current export before syncing clips.")); return; }
    Timeline scratch;
    scratch.setAllowOffline(true);
    auto settings = resolvedTimelineSettings();
    for (auto it = format.cbegin(); it != format.cend(); ++it)
        settings[it.key()] = it.value();
    scratch.setProjectSettings(settings);
    scratch.setMediaSources(m_timeline.mediaSources(), m_timeline.proxyMode());
    // Each clip on a track of its own, the first picture on V1, every clip at
    // the point it was found to start. Pictures come first in `items`.
    QSet<int> used;
    const auto trackFor = [&](bool audio) {
        if (!audio && !used.contains(0))
            return 0;
        for (int t = 1; t < scratch.trackCount(); ++t)
            if (!used.contains(t) && (scratch.trackKind(t) == Timeline::Track::Audio) == audio)
                return t;
        return scratch.trackCount();
    };
    int firstPicture = -1;
    QList<int> recordings;
    QHash<int, int> placedAt;
    for (const QVariant &v : items) {
        const auto item = v.toMap();
        const bool audio = item.value("audioOnly").toBool();
        const int track = trackFor(audio);
        if (!scratch.overwriteOn(track, item.value("path").toString(), item.value("offset").toInt(), -1, -1, audio,
                                 1.0, !audio && silenceCameras)) {
            fail(tr("Could not place %1 in the synced clip.").arg(QFileInfo(item.value("path").toString()).fileName()));
            return;
        }
        // An empty V1 takes its first clip at the start, wherever it was asked for.
        if (track == 0 && item.value("offset").toInt() > 0
            && !scratch.insertGapAt(0, item.value("offset").toInt())) {
            fail(tr("Could not place %1 in the synced clip.").arg(QFileInfo(item.value("path").toString()).fileName()));
            return;
        }
        used.insert(track);
        if (audio) recordings.append(track);
        else if (firstPicture < 0) firstPicture = track;
        placedAt[track] = item.value("offset").toInt();
    }
    if (!silenceCameras)
        scratch.settleLinkedAudio({});
    // A recording moves with its picture when the clip is opened and edited.
    if (firstPicture >= 0 && recordings.size() == 1) {
        const QString link = QUuid::createUuid().toString(QUuid::Id128).left(16);
        const auto indexOn = [&](int track) { return scratch.clipIndexContaining(track, placedAt.value(track)); };
        scratch.setLinkAt(firstPicture, indexOn(firstPicture), link);
        scratch.setLinkAt(recordings.first(), indexOn(recordings.first()), link);
    }
    const auto before = sourceAttributeState({});
    stashActiveSequence();
    Sequence seq;
    seq.name = name;
    seq.id = newSequenceId();
    seq.settings = scratch.projectSettings();
    seq.configured = true;
    seq.synced = true;
    seq.snapshot = scratch.entries();
    QVariantMap sentinel{{QStringLiteral("omavid:markers"), QVariantList{}}};
    sentinel.insert(QStringLiteral("omavid:tracks"), scratch.trackFlagsVariant());
    sentinel.insert(QStringLiteral("omavid:master"), scratch.masterMix());
    sentinel.insert(QStringLiteral("omavid:adr"), scratch.adrCues());
    seq.snapshot.append(sentinel);
    m_sequences.append(seq);
    recordSequenceEdit(before, tr("Sync Clips"));
    announceSequences();
    emit syncedClipFinished(name, {});
}

void PlayerWorker::switchSequence(int index) {
    m_lastEditGesture = 0;
    if (exportActive() || index < 0 || index >= m_sequences.size() || index == m_activeSequence)
        return;
    m_replaying = true; // another clip list altogether (see staleEdit)
    ++m_editRevision; // a mixer gesture must not coalesce across timelines
    setSpeed(0.0);
    dropStabJob();
    dropTrackJob(); // its clip belongs to the sequence being left
    stashActiveSequence();
    const int previous = m_activeSequence;
    m_activeSequence = index;
    const Sequence &target = m_sequences.at(index);
    if (!target.settings.isEmpty()) m_timeline.setProjectSettings(target.settings, false);
    m_sourceMode = false;
    endTimelineDecorJobs();
    if (target.snapshot.isEmpty()) {
        m_timeline.clear();
        m_timeline.setMasterMixMap({});
        m_timeline.setAdrCues({});
        if (!m_markers.isEmpty()) {
            m_markers.clear();
            emit markersChanged(m_markers);
        }
        m_position = 0;
        announceTimeline();
        emit frameReady(QImage(), 0);
    } else {
        m_position = target.position;
        if (!restoreSnapshot(target.snapshot)) { // announces + refreshes
            // The target no longer rebuilds (media gone): the engine now
            // holds the PREVIOUS arrangement, so stay on that tab rather
            // than report it under the target's name — the next stash
            // would have overwritten the target with it for good.
            const QString name = target.name;
            m_activeSequence = previous >= 0 ? previous : 0;
            m_position = m_sequences.at(m_activeSequence).position;
            m_timeline.setProjectSettings(m_sequences.at(m_activeSequence).settings, false);
            restoreSnapshot(m_sequences.at(m_activeSequence).snapshot);
            emit sequenceOpenFailed(name);
        }
    }
    restoreAudioPosition(m_sequences[m_activeSequence].audioPosition);
    announceSequences();
}

void PlayerWorker::renameSequence(int index, const QString &name) {
    if (exportActive() || index < 0 || index >= m_sequences.size() || name.trimmed().isEmpty()
        || m_sequences[index].name==name.trimmed())
        return;
    QStringList names;
    for (int i=0; i<m_sequences.size(); ++i) if (i!=index) names.append(m_sequences[i].name);
    if (!CreationNames::problem(name, names).isEmpty()) return;
    const auto before=sourceAttributeState({});
    m_sequences[index].name = name.trimmed();
    recordSequenceEdit(before,tr("Rename Timeline"));
    announceTimeline(false);
    announceSequences();
}

void PlayerWorker::moveSequenceToBin(int index, const QString &binId) {
    if (exportActive() || index < 0 || index >= m_sequences.size() || m_sequences[index].binId == binId)
        return;
    const auto before=sourceAttributeState({});
    m_sequences[index].binId = binId;
    recordSequenceEdit(before,tr("Move Timeline to Bin"));
    announceTimeline(false);
    announceSequences();
}

void PlayerWorker::duplicateSequence(int index) {
    if (exportActive() || index < 0 || index >= m_sequences.size())
        return;
    const auto before=sourceAttributeState({});
    stashActiveSequence();
    Sequence copy = m_sequences.at(index);
    copy.id = newSequenceId(); // a copy is its own sequence, nests apart
    // Keep grade groups within the copy, but give them new identities so a
    // grading change in an alternate cut cannot modify the original cut.
    QHash<QString, QString> gradeIds;
    std::function<QVariant(const QVariant &)> detach = [&](const QVariant &value) -> QVariant {
        if (value.metaType().id() == QMetaType::QVariantList) {
            auto list=value.toList(); for (auto &v : list) v=detach(v); return list;
        }
        if (value.metaType().id() != QMetaType::QVariantMap) return value;
        auto map=value.toMap();
        for (auto it=map.begin(); it!=map.end(); ++it) it.value()=detach(it.value());
        if (map.contains("sharing")) {
            auto sharing=map.value("sharing").toMap(); const auto id=sharing.value("id").toString();
            if (!id.isEmpty()) {
                if (!gradeIds.contains(id)) gradeIds[id]=newSequenceId();
                sharing["id"]=gradeIds.value(id); map["sharing"]=sharing;
            }
        }
        // A copied edit must not carry cross-timeline undo side effects.
        map.remove("omavid:colorSequences");
        return map;
    };
    copy.snapshot = detach(copy.snapshot).toList();
    const QString base = copy.name.left(108) + QStringLiteral(" copy");
    copy.name = base;
    for (int n = 2; std::any_of(m_sequences.cbegin(), m_sequences.cend(),
                                [&](const Sequence &s) { return s.name.compare(copy.name, Qt::CaseInsensitive) == 0; }); ++n)
        copy.name = base + QStringLiteral(" %1").arg(n);
    m_sequences.append(copy);
    switchSequence(m_sequences.size() - 1);
    recordSequenceEdit(before,tr("Duplicate Timeline"));
    announceTimeline();announceSequences();
}

void PlayerWorker::closeSequence(int index) {
    if (exportActive() || index < 0 || index >= m_sequences.size() || m_sequences.size() < 2)
        return;
    const auto before=sourceAttributeState({});
    if (index == m_activeSequence) {
        switchSequence(index > 0 ? index - 1 : 1);
        if (index == m_activeSequence) return; // failed switch: keep the live edit
    }
    m_sequences.removeAt(index);
    if (m_activeSequence > index)
        --m_activeSequence;
    recordSequenceEdit(before,tr("Close Timeline"));
    announceTimeline(false);
    announceSequences();
}

void PlayerWorker::setSequences(const QVariantList &sequences, int active) {
    if (sequences.isEmpty())
        return;
    // A list saved without ids names its timelines here. The active one is
    // the timeline already loaded and shown: it keeps the id it has.
    const QString shownId = m_sequences.value(m_activeSequence).id;
    const int shown = qBound(0, active, int(sequences.size()) - 1);
    m_sequences.clear();
    for (const QVariant &v : sequences) {
        const QVariantMap m = v.toMap();
        Sequence seq;
        seq.name = m.value(QStringLiteral("name")).toString();
        seq.snapshot = m.value(QStringLiteral("snapshot")).toList();
        seq.position = m.value(QStringLiteral("position")).toInt();
        seq.audioPosition = m.value(QStringLiteral("audioPosition")).toMap();
        seq.id = m.value(QStringLiteral("id")).toString();
        seq.settings = m.value("settings", m_timeline.projectSettings()).toMap();
        seq.configured = m.value("configured", true).toBool();
        seq.lockFormat = m.value("lockFormat", false).toBool();
        seq.binId = m.value(QStringLiteral("binId")).toString();
        seq.synced = m.value(QStringLiteral("synced")).toBool();
        if (seq.id.isEmpty())
            seq.id = m_sequences.size() == shown && !shownId.isEmpty() ? shownId : newSequenceId();
        m_sequences.append(seq);
    }
    m_activeSequence = qBound(0, active, m_sequences.size() - 1);
    m_sequences[m_activeSequence].snapshot = snapshotNow();
    m_sequences[m_activeSequence].settings = m_timeline.projectSettings();
    // Saved/recovered nests are frozen files. Only references to sequences
    // in this project reconnect to their editable cache; external nests keep
    // the saved picture. The stable id survives recovery path materialization.
    QSet<QString> sequenceIds;
    for (const Sequence &seq : std::as_const(m_sequences)) sequenceIds.insert(seq.id);
    QVariantMap paths;
    for (const Sequence &seq : std::as_const(m_sequences)) {
        for (const QVariant &v : ProjectSafety::clipEntries(seq.snapshot)) {
            const auto entry = v.toMap();
            const QString path = entry.value("path").toString();
            const QString id = entry.value("nestId", QFileInfo(path).completeBaseName()).toString();
            if (ProjectFile::isProject(path) && sequenceIds.contains(id)) {
                if (path != nestPath(id)) paths.insert(path, nestPath(id));
            }
        }
    }
    const auto savedSequences = m_sequences;
    for (Sequence &seq : m_sequences)
        seq.snapshot = MediaRefs::rewrite(seq.snapshot, paths).toList();
    if (!refreshNests() || (!paths.isEmpty() && !restoreSnapshot(m_sequences.at(m_activeSequence).snapshot, false))) {
        m_sequences = savedSequences;
        emit editRefused(tr("Could not refresh the nested sequences. Check their media and cache storage."));
    }
    // The saved sequence IDs replace the temporary load ID. Publish those IDs
    // atomically with the entries before accepting a new captured edit target.
    announceTimeline(false);
    m_position=m_sequences[m_activeSequence].position;
    restoreAudioPosition(m_sequences[m_activeSequence].audioPosition);
    refreshFrame();
    announceSequences();
}

// The nested file for a sequence, refreshed and checked for cycles, ready to
// be placed in the active one; empty when it cannot be.
QString PlayerWorker::preparedNest(int index) {
    if (exportActive() || index < 0 || index >= m_sequences.size() || index == m_activeSequence)
        return {};
    Sequence &seq = m_sequences[index];
    if (seq.id.isEmpty())
        seq.id = newSequenceId();
    QSet<QString> visited;
    QStringList pending{seq.id};
    while (!pending.isEmpty()) {
        const QString id = pending.takeLast();
        if (id == m_sequences.at(m_activeSequence).id) {
            emit editRefused(tr("A sequence cannot contain itself, including through other nested sequences."));
            return {};
        }
        if (visited.contains(id)) continue;
        visited.insert(id);
        for (const auto &candidate : std::as_const(m_sequences)) if (candidate.id == id)
            for (const auto &v : ProjectSafety::clipEntries(candidate.snapshot)) {
                const auto entry = v.toMap();
                const QString path = entry.value(ClipKey::path).toString();
                if (ProjectFile::isProject(path))
                    pending.append(entry.value(ClipKey::nestId, QFileInfo(path).completeBaseName()).toString());
            }
    }
    if (!refreshNests() || !writeNest(seq)) {
        emit editRefused(tr("Could not refresh the nested sequences. Check their media and cache storage."));
        return {};
    }
    const QString path = nestPath(seq.id);
    return QFileInfo::exists(path) ? path : QString();
}

void PlayerWorker::nestSequenceOnTrack(int index, int track, int position, bool audioHint) {
    if (position < 0 || track < 0)
        return;
    const QString path = preparedNest(index);
    if (path.isEmpty())
        return;
    m_pendingTag.clear();
    placeOnTrack(track, 2, path, position, -1, -1, audioHint);
    announceSequences(); // the id may be new
}

void PlayerWorker::nestSequence(int index, int position) {
    if (position < 0)
        return;
    const QString path = preparedNest(index);
    if (path.isEmpty())
        return;
    const bool wasEmpty = m_timeline.isEmpty();
    const QVariantList before = m_timeline.entries();
    const int primaryBefore = m_timeline.primaryFrameCount();
    if (!m_timeline.insertAt(path, position, -1, -1))
        return;
    recordEdit(before, true, tr("Nest sequence"));
    rippleMarkers(position, m_timeline.primaryFrameCount() - primaryBefore);
    setSpeed(0.0);
    announceTimeline();
    if (wasEmpty)
        m_position = 0;
    refreshFrame();
    announceSequences(); // the id may be new
}
