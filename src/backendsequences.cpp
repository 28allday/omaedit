#include "backend_p.h"

// The project's timelines as the media panel shows them. The poster is the
// library's picture of the first clip with one, V1 before the other tracks.
QVariantList Backend::timelineItems() const {
    QHash<QString, QString> thumbs;
    for (const QVariant &v : m_library) {
        const QVariantMap e = v.toMap();
        if (!e.value(ClipKey::audioOnly).toBool() && e.contains(QStringLiteral("thumb")))
            thumbs.insert(e.value(ClipKey::path).toString(), e.value(QStringLiteral("thumb")).toString());
    }
    QVariantList items;
    for (int i = 0; i < m_sequences.size(); ++i) {
        const QVariantMap sequence = m_sequences.at(i).toMap();
        if (!sequence.value(QStringLiteral("configured"), true).toBool())
            continue;
        const bool active = i == m_activeSequence;
        const QVariantMap settings = sequence.value(QStringLiteral("settings")).toMap();
        const QVariantList entries = active ? m_timeline + connected()
                                            : sequence.value(QStringLiteral("snapshot")).toList();
        int frames = 0, posterTrack = 0, posterStart = 0;
        QString thumb;
        for (const QVariant &v : entries) {
            const QVariantMap e = v.toMap();
            const int track = e.value(ClipKey::track).toInt() > 0 ? 1 : 0;
            const int start = e.value(ClipKey::start).toInt();
            frames = qMax(frames, start + e.value(ClipKey::frames).toInt());
            const auto found = thumbs.constFind(e.value(ClipKey::path).toString());
            if (found != thumbs.constEnd()
                && (thumb.isEmpty() || track < posterTrack || (track == posterTrack && start < posterStart))) {
                thumb = found.value();
                posterTrack = track;
                posterStart = start;
            }
        }
        if (active)
            frames = m_duration;
        const int fpsDen = settings.value(QStringLiteral("fpsDen")).toInt();
        const double fps = active ? m_fps : fpsDen > 0 ? settings.value(QStringLiteral("fpsNum")).toDouble() / fpsDen : 0;
        // A bin deleted since returns its timelines to the library root, as it does clips.
        QString binId = sequence.value(QStringLiteral("binId")).toString();
        if (m_mediaCatalog->bin(binId).isEmpty())
            binId.clear();
        items.append(QVariantMap{{QStringLiteral("index"), i},
                                 {QStringLiteral("id"), sequence.value(QStringLiteral("id"))},
                                 {QStringLiteral("name"), sequence.value(QStringLiteral("name"))},
                                 {QStringLiteral("active"), active},
                                 {QStringLiteral("binId"), binId},
                                 {QStringLiteral("synced"), sequence.value(QStringLiteral("synced"), false)},
                                 {QStringLiteral("frames"), frames},
                                 {QStringLiteral("dur"), fps > 0 ? Interchange::durationText(frames, fps) : QString()},
                                 // A timeline that takes its format from its clips keeps
                                 // placeholder settings: the open one says what it plays at.
                                 {QStringLiteral("height"), active && m_videoHeight > 0 ? QVariant(m_videoHeight)
                                                            : settings.value(QStringLiteral("custom")).toBool()
                                                                  ? settings.value(QStringLiteral("height")) : QVariant(0)},
                                 {QStringLiteral("thumb"), thumb}});
    }
    return items;
}

QVariantList Backend::sequences() const {
    QVariantList names;
    for (const QVariant &v : m_sequences) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), v.toMap().value(QStringLiteral("name")).toString());
        m.insert(QStringLiteral("id"), v.toMap().value(QStringLiteral("id")).toString());
        m.insert(QStringLiteral("synced"), v.toMap().value(QStringLiteral("synced")).toBool());
        names.append(m);
    }
    return names;
}

void Backend::setStackedSequence(int index) {
    if (index < 0 || index >= m_sequences.size() || index == m_activeSequence)
        index = -1;
    const QString id = index < 0 ? QString()
                                 : m_sequences.at(index).toMap().value(QStringLiteral("id")).toString();
    if (index == m_stackedSequence && id == m_stackedId)
        return;
    m_stackedSequence = index;
    m_stackedId = id;
    if (index >= 0)
        setStatus(tr("Stacked “%1” above the timeline — drag its clips down")
                      .arg(m_sequences.at(index).toMap().value(QStringLiteral("name")).toString()));
    emit stackedSequenceChanged();
}

QVariantList Backend::sequenceEntries(int index) const {
    if (index < 0 || index >= m_sequences.size())
        return {};
    QVariantList out;
    for (const QVariant &v : m_sequences.at(index).toMap().value(QStringLiteral("snapshot")).toList()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::gap).toBool() || e.value(ClipKey::frames).toInt() < 1)
            continue;
        out.append(e);
    }
    return out;
}

void Backend::placeEntryAt(const QVariantMap &source, int track, int position) {
    if (m_exporting || track < 0 || position < 0 || source.isEmpty()
        || source.value(ClipKey::gap).toBool()
        || source.value(ClipKey::frames).toInt() < 1)
        return;
    if (!hasVideo() && track > 0)
        return;
    QVariantMap entry = source;
    // Its place in the other sequence, and ties that only mean something
    // there, stay behind; the material and its attributes come along.
    for (const char *key : {"link", "original", "start", "gapBefore", "lane", "track", "index",
                            "mixIn", "mixOut", "mixStyle", "mixAlign"})
        entry.remove(QLatin1String(key));
    emit placeEntryRequested(track, position, entry, false);
    setStatus(tr("Placed %1 from the stacked sequence")
                  .arg(entry.value(ClipKey::name).toString()));
}


QString Backend::sequenceNameForId(const QString &id) const {
    const int i = sequenceIndexForId(id);
    return i < 0 ? QString() : m_sequences.at(i).toMap().value(QStringLiteral("name")).toString();
}

int Backend::sequenceIndexForId(const QString &id) const {
    for (int i = 0; i < m_sequences.size(); ++i)
        if (m_sequences.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    return -1;
}

void Backend::nestSequence(int index) {
    if (m_exporting || index < 0 || index >= m_sequences.size() || index == m_activeSequence)
        return;
    const QString name = m_sequences.at(index).toMap().value(QStringLiteral("name")).toString();
    setStatus(tr("Sequence “%1” inserted as a clip").arg(name));
    emit nestSequenceRequested(index, m_viewingSource ? 0 : m_position);
}

// A sequence placed on a track as one clip, as a library clip drops there.
void Backend::dropSequenceAt(int index, int track, int frame, bool audio) {
    if (m_exporting || index < 0 || index >= m_sequences.size() || index == m_activeSequence
        || track < 0 || track > m_tracks.size() || frame < 0)
        return;
    const QString name = m_sequences.at(index).toMap().value(QStringLiteral("name")).toString();
    setStatus(tr("Sequence “%1” inserted as a clip").arg(name));
    if (!hasVideo()) // nothing to overwrite yet — this starts the edit
        emit nestSequenceRequested(index, 0);
    else
        emit nestSequenceOnTrackRequested(index, track, frame, audio);
}

void Backend::renderAndReplace(int track, int index) {
    if (m_exporting || track < 0 || index < 0 || !m_renderJob.isEmpty())
        return;
    emit renderAndReplaceRequested(track, index);
}

void Backend::restoreOriginal(int track, int index) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Original restored"));
    emit restoreOriginalRequested(track, index);
}

void Backend::cancelTimelinePlacement() {
    auto pending=std::move(m_pendingTimelinePlacements);
    m_pendingTimelinePlacements.clear();
    m_nextTimelinePlacementTag.clear();
    m_pendingTimelineSource.clear();
    m_pendingTimelineAnchor.clear();
    m_pendingTimelineGeneration=-1;
    m_pendingTimelineCreateRequest=0;
    m_pendingTimelinePrompted=false;
    for(auto &placement:pending)if(placement.cancel)placement.cancel({});
}

QVariantMap Backend::timelinePlacementTarget() const {
    return {{"context",m_audioEditContext},{"trackCount",m_tracks.size()},
        {"sampleRate",m_projectSettings.value("sampleRate",48000)}};
}

void Backend::queueTimelinePlacement(const QString &path, PendingTimelinePlacement placement) {
    const bool first=m_pendingTimelinePlacements.isEmpty();
    if(first) {
        m_pendingTimelineSource=path;
        m_pendingTimelineAnchor=creationContext().value("anchorId").toString();
        m_pendingTimelineGeneration=m_mediaGeneration;
        // A placement can arrive after Create was queued but before its reply.
        // Join that creation instead of opening a second dialog.
        for(auto it=m_sequenceCreateContexts.cbegin();it!=m_sequenceCreateContexts.cend();++it)
            if(it.value().value("generation").toInt()==m_mediaGeneration
                && it.value().value("anchorId").toString()==m_pendingTimelineAnchor
                && (!m_pendingTimelineCreateRequest || it.key()<m_pendingTimelineCreateRequest))
                m_pendingTimelineCreateRequest=it.key();
    }
    m_pendingTimelinePlacements.append(std::move(placement));
    if(first && !m_pendingTimelineCreateRequest) {
        m_pendingTimelinePrompted=true;emit timelineCreationRequested();
    }
}

void Backend::dispatchTimelinePlacement(const QString &path, std::function<void()> placement, const QVariantMap &sourceAction) {
    auto tag=m_nextTimelinePlacementTag;
    m_nextTimelinePlacementTag.clear();
    const auto snapshot=tag.take("_sourceSnapshot").toMap();
    auto action=sourceAction;
    if(tag.contains("_sourceFrames"))action["sourceFrames"]=tag.take("_sourceFrames");
    if (m_exporting || m_projectLoadPending) return;
    // Keep source markers, timing origin and multicam metadata with their own
    // placement, including while several clips wait for timeline creation.
    auto tagged=[worker=m_worker,tag,placement=std::move(placement)] {
        worker->tagNextPlacement(tag);
        placement();
    };
    if (!m_timelineCreationPrompt || hasTimeline()) {
        // Saving an existing timeline may run in the background; queued edits
        // must still land and leave that saved revision marked dirty.
        QMetaObject::invokeMethod(m_worker, std::move(tagged), Qt::QueuedConnection);
        return;
    }
    if (projectBusy()) return;
    queueTimelinePlacement(path,{[worker=m_worker,snapshot,action,placement=std::move(tagged)](const QVariantMap &target) {
        if(target.isEmpty())return;
        if(!snapshot.isEmpty() && !action.isEmpty())worker->placeSourceSnapshot(target,snapshot,action);
        else placement();
    },{}});
}

QVariantMap Backend::newTimelineDefaults() const {
    auto settings = defaultProjectSettings();
    for (const char *key : {"title", "proxyFolder", "importSpace", "importLut"}) settings[key] = m_projectSettings.value(key);
    // The preferred start, counted at a rate with room for any frame number;
    // the timeline recounts it at its own rate and keeps the timecode.
    const auto parts = m_newTimelineStart.split(QLatin1Char(':'));
    if (const int start = ((parts.value(0).toInt() * 60 + parts.value(1).toInt()) * 60 + parts.value(2).toInt()) * kNewTimelineStartRate
                          + parts.value(3).toInt(); start > 0) {
        settings["startFrame"] = start; settings["startRate"] = kNewTimelineStartRate;
    }
    for (const auto &value : m_library) {
        const auto clip = value.toMap();
        if (clip.value("path").toString() != m_pendingTimelineSource) continue;
        if (clip.value("width").toInt() >= 16 && clip.value("height").toInt() >= 16) {
            settings["width"] = clip.value("width"); settings["height"] = clip.value("height");
            const double fps = clip.value("fps").toDouble();
            if (fps > 0) {
                settings["fpsNum"] = qRound(fps*1000); settings["fpsDen"] = 1000;
                for (const auto &rate : QList<QPair<int,int>>{{24000,1001},{24,1},{25,1},{30000,1001},{30,1},{50,1},{60000,1001},{60,1},{120,1}})
                    if (qAbs(fps-double(rate.first)/rate.second) < .001) {
                        settings["fpsNum"]=rate.first; settings["fpsDen"]=rate.second; break;
                    }
            }
        }
        break;
    }
    return settings;
}

QVariantMap Backend::creationContext() const {
    return {{"generation",m_mediaGeneration},{"anchorId",m_sequences.value(m_activeSequence).toMap().value("id")},
            {"projectName",projectName()},{"settings",m_projectSettings}};
}
QString Backend::nextSequenceName() const {
    QStringList names;
    for (const auto &v : m_sequences) if (v.toMap().value("configured", true).toBool()) names.append(v.toMap().value("name").toString());
    return CreationNames::nextSequence(names);
}
QString Backend::sequenceCreationProblem(const QVariantMap &input, const QString &name, const QVariantMap &settings) const {
    const auto context=plainVariant(input).toMap();
    if (projectBusy() || m_exporting) return tr("Wait for the current project operation before creating a timeline.");
    bool found=false;
    for (const auto &v : m_sequences) found|=!context.value("anchorId").toString().isEmpty() && v.toMap().value("id")==context.value("anchorId");
    if (context.isEmpty() || context.value("generation").toInt()!=m_mediaGeneration || !found)
        return tr("The project changed. Close this dialog and reopen New Timeline.");
    QStringList names;
    for (const auto &v : m_sequences) if (v.toMap().value("configured", true).toBool()) names.append(v.toMap().value("name").toString());
    if (const auto error = CreationNames::problem(name,names); !error.isEmpty()) return error;
    if (!settings.isEmpty()) {
        auto validated = plainVariant(settings).toMap(); validated["title"] = QStringLiteral("Timeline");
        return CreationNames::projectProblem(validated);
    }
    return {};
}
int Backend::createSequence(const QVariantMap &input, const QString &name, const QVariantMap &settings) {
    const int request=++m_sequenceCreateRequest;
    const auto context=plainVariant(input).toMap();
    const auto error=sequenceCreationProblem(context,name,settings);
    if (!error.isEmpty()) {
        QTimer::singleShot(0,this,[this,request,error]{setStatus(error);emit sequenceCreateFinished(request,false,error);});
    } else {
        m_sequenceCreateContexts.insert(request,context);
        if(!m_pendingTimelinePlacements.isEmpty() && !m_pendingTimelineCreateRequest
            && context.value("generation").toInt()==m_pendingTimelineGeneration
            && context.value("anchorId").toString()==m_pendingTimelineAnchor)
            m_pendingTimelineCreateRequest=request;
        emit sequenceCreateRequested(request,context.value("anchorId").toString(),name.trimmed(),plainVariant(settings).toMap());
    }
    return request;
}

void Backend::addSequence(const QString &name) {
    if (m_exporting)
        return;
    QString title = name.trimmed();
    if (title.isEmpty()) {
        QStringList taken;
        for (const QVariant &v : m_sequences)
            taken.append(v.toMap().value(QStringLiteral("name")).toString());
        for (int n = m_sequences.size() + 1;; ++n) {
            title = QStringLiteral("Sequence %1").arg(n);
            if (!taken.contains(title))
                break;
        }
    }
    setStatus(tr("New timeline “%1”").arg(title));
    emit addSequenceRequested(title);
}

void Backend::switchSequence(int index) {
    if (m_exporting || index < 0 || index >= m_sequences.size() || index == m_activeSequence)
        return;
    setStatus(tr("Sequence “%1”")
                  .arg(m_sequences.at(index).toMap().value(QStringLiteral("name")).toString()));
    emit switchSequenceRequested(index);
}

void Backend::moveSequenceToBin(int index, const QString &binId) {
    const QVariantMap bin = m_mediaCatalog->bin(binId);
    if (index < 0 || index >= m_sequences.size()
        || (!binId.isEmpty() && (bin.isEmpty() || bin.value(QStringLiteral("smart")).toBool())))
        return;
    emit moveSequenceToBinRequested(index, binId);
}

void Backend::renameSequence(int index, const QString &name) {
    if (index < 0 || index >= m_sequences.size() || name.trimmed().isEmpty())
        return;
    QStringList names;
    for (int i=0; i<m_sequences.size(); ++i) if (i!=index) names.append(m_sequences[i].toMap().value("name").toString());
    if (const auto error=CreationNames::problem(name,names); !error.isEmpty()) { setStatus(error); return; }
    emit renameSequenceRequested(index, name.trimmed());
}

void Backend::duplicateSequence(int index) {
    if (m_exporting || index < 0 || index >= m_sequences.size())
        return;
    setStatus(tr("Timeline duplicated"));
    emit duplicateSequenceRequested(index);
}

void Backend::closeSequence(int index) {
    if (m_exporting || index < 0 || index >= m_sequences.size() || m_sequences.size() < 2)
        return;
    setStatus(tr("Timeline “%1” deleted")
                  .arg(m_sequences.at(index).toMap().value(QStringLiteral("name")).toString()));
    emit closeSequenceRequested(index);
}

void Backend::onSequences(const QVariantList &sequences, int active) {
    const bool activated = m_sequences.value(m_activeSequence).toMap().value("id")
                           != sequences.value(active).toMap().value("id");
    // The worker returns to the timeline on create/duplicate/switch. The
    // viewer and seek routing must leave the source preview at the same time.
    if (activated && m_viewingSource) viewTimeline();
    // Names, count and the active tab are what the project file records
    // beyond the timeline itself (snapshots dirty through the timeline).
    const auto names = [](const QVariantList &list) {
        QStringList out;
        for (const QVariant &v : list)
            out.append(v.toMap().value(QStringLiteral("name")).toString());
        return out;
    };
    const bool changed = m_activeSequence != active || names(m_sequences) != names(sequences)
        || (sequences.value(active).toMap().value("configured", true).toBool()
            && m_sequences.value(m_activeSequence).toMap().value("id") != sequences.value(active).toMap().value("id"))
        || m_sequences.value(m_activeSequence).toMap().value("configured") != sequences.value(active).toMap().value("configured");
    const bool echo = m_sequencesEchoPending;
    m_sequencesEchoPending = false;
    m_sequences = sequences;
    m_activeSequence = active;
    bool clipsRenamed = false;
    for (QVariantList *clips : {&m_timeline, &m_connected})
        for (QVariant &clip : *clips)
            clipsRenamed = nameNestedClip(clip, m_sequences) || clipsRenamed;
    emit sequencesChanged();
    if (clipsRenamed)
        emit timelineEntriesChanged();
    // The stacked sequence follows its id; it unstacks when it becomes the
    // active one or goes away.
    {
        const int was = m_stackedSequence;
        m_stackedSequence = m_stackedId.isEmpty() ? -1 : sequenceIndexForId(m_stackedId);
        if (m_stackedSequence == m_activeSequence)
            m_stackedSequence = -1;
        if (m_stackedSequence < 0)
            m_stackedId.clear();
        if (was != m_stackedSequence || m_stackedSequence >= 0)
            emit stackedSequenceChanged();
    }
    if (changed && !echo && !m_restoringLibrary)
        setDirty(true, true);
}
