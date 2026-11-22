#include "playerworker_p.h"

void PlayerWorker::queueMediaDecor(const QString &path, int frames, double fps, bool audio, bool picture) {
    if (picture) queueLibraryStrip(path, frames, fps);
    if (audio) queueLibraryWave(path, frames);
}

QString PlayerWorker::mediaRateChangeProblem(const QVariantMap &sources) {
    if(exportActive())return tr("Wait for the export before changing clip attributes.");
    const auto previous=m_timeline.mediaSources();
    bool rateChanged = false;
    for (auto it = sources.begin(); it != sources.end(); ++it)
        rateChanged |= !qFuzzyCompare(Timeline::interpretationFactor(previous.value(it.key()).toMap()),
                                     Timeline::interpretationFactor(it.value().toMap()));
    if (!rateChanged) return {};
    QHash<QByteArray,double> lengths;
    QSet<QString> checkedNests;
    std::function<QString(const QVariant &,const QVariantMap &)> check=[&](const QVariant &value,const QVariantMap &settings)->QString {
        if(value.metaType().id()==QMetaType::QVariantList) {
            const auto list=value.toList();
            const auto format=list.isEmpty()?QVariantMap{}:list.last().toMap().value("omavid:sequenceFormat").toMap();
            for(const auto &v:list){const auto problem=check(v,format.isEmpty()?settings:format);if(!problem.isEmpty())return problem;}
        } else if(value.metaType().id()==QMetaType::QVariantMap) {
            const auto entry=value.toMap();const auto path=entry.value(ClipKey::path).toString();
            if(entry.contains(ClipKey::retime) && sources.contains(path)
                && !qFuzzyCompare(Timeline::interpretationFactor(previous.value(path,entry.value(ClipKey::sourceMeta)).toMap()),
                                  Timeline::interpretationFactor(sources.value(path).toMap()))) {
                const auto key=QJsonDocument::fromVariant(QVariantList{path,settings,entry.value(ClipKey::audioOnly),entry.value(ClipKey::videoOnly)}).toJson(QJsonDocument::Compact);
                if(!lengths.contains(key)) {
                    Timeline trial;trial.setProjectSettings(settings);trial.setMediaSources(sources,"original");
                    lengths[key]=trial.speedSourceLength(entry);
                }
                if(entry.value(ClipKey::retime).toMap().value("sourceLength").toDouble()>lengths.value(key)+1e-6)
                    return tr("Changing the frame rate of %1 would invalidate speed segments in the project or undo history. Reset or render those clips in place, then save and reopen before changing the source rate.").arg(QFileInfo(path).fileName());
            }
            if (ProjectFile::isProject(path) && !checkedNests.contains(path)) {
                checkedNests.insert(path);
                QString error;
                const auto document = ProjectSafety::readDocument(path, &error);
                if (document.isEmpty() || checkedNests.size() > 256)
                    return tr("Cannot inspect nested sequence %1 before changing source frame rates.").arg(QFileInfo(path).fileName());
                const auto resolved = ProjectSafety::resolveDocument(document, QFileInfo(path).absolutePath(), &error);
                if (resolved.isEmpty())
                    return tr("Cannot inspect nested sequence %1 before changing source frame rates.").arg(QFileInfo(path).fileName());
                auto nestedSettings = resolved.value("project").toMap();
                const auto profile = resolved.value("profile").toMap();
                if (!profile.isEmpty()) {
                    nestedSettings["custom"] = true;
                    for (const char *key : {"width", "height", "fpsNum", "fpsDen"})
                        nestedSettings[key] = profile.value(key);
                }
                const auto problem = check(resolved.value("entries"), nestedSettings);
                if (!problem.isEmpty()) return problem;
            }
            for(auto it=entry.begin();it!=entry.end();++it) {
                const QVariant nested=it.key()=="compoundJson"?QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant():it.value();
                const auto nestedSettings=it.key()=="snapshot" && entry.contains("settings")?entry.value("settings").toMap():settings;
                const auto problem=check(nested,nestedSettings);if(!problem.isEmpty())return problem;
            }
        }
        return {};
    };
    auto settings=m_timeline.projectSettings();
    // Automatic sequences also need their actual locked timebase for probes.
    if(m_timeline.profile()) {
        settings["custom"]=true;settings["width"]=m_timeline.width();settings["height"]=m_timeline.height();
        settings["fpsNum"]=m_timeline.profile()->frame_rate_num();settings["fpsDen"]=m_timeline.profile()->frame_rate_den();
    }
    auto problem=check(m_timeline.entries(),settings);if(!problem.isEmpty())return problem;
    for(const auto &sequence:m_sequences){problem=check(sequence.snapshot,sequence.settings);if(!problem.isEmpty())return problem;}
    for(const auto &history:m_undoStack){problem=check(history,settings);if(!problem.isEmpty())return problem;}
    for(const auto &history:m_redoStack){problem=check(history,settings);if(!problem.isEmpty())return problem;}
    return {};
}

void PlayerWorker::setMediaSources(const QVariantMap &sources, const QString &mode) {
    if (exportActive()) { m_mediaSourcesDeferred = true; m_deferredMediaSources = sources; m_deferredMediaMode = mode; return; }
    const QVariantMap previous = m_timeline.mediaSources();
    const QString oldMode = m_timeline.proxyMode();
    bool changed = oldMode != mode;
    QSet<QString> used = MediaRefs::paths(m_timeline.entries());
    for (const Sequence &sequence : m_sequences) used.unite(MediaRefs::paths(sequence.snapshot));
    for (const QString &path : used) if (previous.value(path) != sources.value(path)) changed = true;
    const bool previewChanged = !m_previewPath.isEmpty()
        && (oldMode != mode || previous.value(m_previewPath) != sources.value(m_previewPath));
    const double speed = m_speed;
    const int sourcePos = m_previewPos;
    const QString source = m_previewPath;
    const bool viewing = m_sourceMode;
    const QVariantList snapshot = changed ? snapshotNow() : QVariantList();
    m_timeline.setMediaSources(sources, mode);
    if(changed && !m_timeline.entries().isEmpty() && !restoreSnapshot(snapshot,false)) {
        m_timeline.setMediaSources(previous,oldMode);restoreSnapshot(snapshot,false);
        emit editRefused(tr("The source settings could not preserve the timeline. The previous playback settings were restored."));
        if(viewing){openPreview(source);seek(sourcePos);}setSpeed(speed);return;
    }
    for (Sequence &sequence : m_sequences) sequence.snapshot = MediaRefs::withSources(sequence.snapshot, sources).toList();
    for (QVariantList &history : m_undoStack) history = MediaRefs::withSources(history, sources).toList();
    for (QVariantList &history : m_redoStack) history = MediaRefs::withSources(history, sources).toList();
    if (changed) {
        m_stripCache.clear();
        if (m_activeSequence >= 0 && m_activeSequence < m_sequences.size())
            m_sequences[m_activeSequence].snapshot = snapshotNow();
        if (!refreshNests()) emit editRefused(tr("Could not refresh the nested sequences. Check their media and cache storage."));
    }
    if (previewChanged && viewing) { openPreview(source); seek(sourcePos); }
    if (changed || previewChanged) setSpeed(speed);
}
void PlayerWorker::relinkMedia(const QVariantMap &mapping, const QString &token) {
    if (exportActive() || mapping.isEmpty()) { emit mediaRelinked(false, mapping, token, tr("Relinking is unavailable while exporting.")); return; }
    const QVariantList before = snapshotNow();
    const QVariantList replacement = MediaRefs::rewrite(before, mapping).toList();
    QVariantMap sources = m_timeline.mediaSources();
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        if (sources.contains(it.key())) sources.insert(it.value().toString(), sources.take(it.key()));
    }
    Timeline trial;
    trial.setAllowOffline(true);
    trial.setProjectSettings(m_timeline.projectSettings());
    trial.setMediaSources(sources, m_timeline.proxyMode());
    QVariantList clips = replacement;
    if (!clips.isEmpty() && clips.last().toMap().contains(QStringLiteral("omavid:markers"))) clips.removeLast();
    if (!trial.restore(clips, Timeline::trackFlagsFromVariant(m_timeline.trackFlagsVariant()))
        || trial.frameCount() != m_timeline.frameCount()) {
        emit mediaRelinked(false, mapping, token, tr("The replacements could not preserve the timeline's frame ranges.")); return;
    }
    for (const Sequence &sequence : m_sequences) {
        QVariantList original = sequence.snapshot;
        if (!original.isEmpty() && original.last().toMap().contains(QStringLiteral("omavid:markers"))) original.removeLast();
        if (original.isEmpty()) continue;
        Timeline check; check.setAllowOffline(true); check.setProjectSettings(m_timeline.projectSettings());
        check.setMediaSources(m_timeline.mediaSources(), m_timeline.proxyMode());
        if (!check.restore(original)) { emit mediaRelinked(false, mapping, token, tr("A stored sequence could not be restored.")); return; }
        const int length = check.frameCount(); check.setMediaSources(sources, m_timeline.proxyMode());
        if (!check.restore(MediaRefs::rewrite(original, mapping).toList()) || check.frameCount() != length) {
            emit mediaRelinked(false, mapping, token, tr("A replacement would change the duration of a stored sequence.")); return;
        }
    }
    const QVariantMap oldSources = m_timeline.mediaSources();
    m_timeline.setMediaSources(sources, m_timeline.proxyMode());
    if (!restoreSnapshot(replacement)) {
        m_timeline.setMediaSources(oldSources, m_timeline.proxyMode()); restoreSnapshot(before);
        emit mediaRelinked(false, mapping, token, tr("The timeline could not be relinked.")); return;
    }
    for (Sequence &sequence : m_sequences) sequence.snapshot = MediaRefs::rewrite(sequence.snapshot, mapping).toList();
    for (QVariantList &snapshot : m_undoStack) snapshot = MediaRefs::rewrite(snapshot, mapping).toList();
    for (QVariantList &snapshot : m_redoStack) snapshot = MediaRefs::rewrite(snapshot, mapping).toList();
    stashActiveSequence();
    if (!refreshNests()) emit editRefused(tr("Could not refresh the nested sequences. Check their media and cache storage."));
    announceSequences();
    const bool viewing = m_sourceMode; const int pos = m_previewPos;
    const QString source = mapping.value(m_previewPath, m_previewPath).toString();
    emit mediaRelinked(true, mapping, token, tr("Media relinked."));
    if (viewing && source != m_previewPath) { openPreview(source); seek(pos); }
}

void PlayerWorker::previewMedia(const QString &path, int frame, bool audio, const QString &purpose, const QString &token) {
    if (exportActive() || (purpose == QStringLiteral("skim") && m_speed != 0.0)) { emit mediaPreviewReady(purpose, token, {}, frame); return; }
    const QString resolved = m_timeline.playbackPath(path);
    const QVariantMap settings = m_timeline.mediaSources().value(path).toMap();
    auto player = m_mediaPreviewPlayers.value(purpose);
    if (!player || m_mediaPreviewPaths.value(purpose) != resolved || m_mediaPreviewSettings.value(purpose) != settings) {
        player = std::make_shared<MediaPlayer>();
        if (!player->open(resolved)) { emit mediaPreviewReady(purpose, token, {}, frame); return; }
        player->setInputTransform(settings); m_mediaPreviewPlayers.insert(purpose, player);
        m_mediaPreviewPaths.insert(purpose, resolved); m_mediaPreviewSettings.insert(purpose, settings);
    }
    const int position = qBound(0, frame, qMax(0, player->frameCount()-1));
    player->setWorkingManagement(m_timeline.linearSourceManagement(path));
    const MediaPlayer::Decoded decoded = player->decodeAt(position, audio && m_speed == 0.0, kFrequency, kChannels);
    if(!decoded.error.isEmpty())emit frameError(decoded.error);
    if (audio && m_speed == 0.0 && !decoded.audio.isEmpty() && ensureAudio()) {
        flushAudio(); queueAudition(decoded.audio);
    }
    emit mediaPreviewReady(purpose, token, decoded.image.isNull() ? QImage() : decoded.image.scaledToWidth(purpose == QStringLiteral("skim") ? 480 : 960, Qt::SmoothTransformation), position);
}
void PlayerWorker::setMediaGang(bool on, const QVariantMap &anchors) {
    m_mediaGang = on; resetMediaGangAnchors();
    // A tagged fine reply may have advanced the worker just before the UI
    // unlinked its viewers and discarded the paired source picture. Link from
    // the displayed positions, provided they still belong to these viewers.
    if (!on || anchors.isEmpty() || !m_preview.isOpen()
        || anchors.value("sourcePath").toString()!=m_previewPath
        || !AudioTimeline::sameEditContext(anchors.value("context").toMap(),audioEditContext(),false)) return;
    m_previewPos=m_gangSourceAnchor=qBound(0,anchors.value("sourceFrame").toInt(),m_preview.frameCount()-1);
    m_gangTimelineAnchor=qBound(0,anchors.value("frame").toInt(),qMax(0,m_timeline.frameCount()-1));
    m_gangFineAnchor.clear();
    AudioTimeline::Time time;
    const auto position=anchors.value("position").toMap();const auto *profile=m_timeline.profile();
    if (AudioTimeline::readTime(position,&time) && time.ticks>=0
        && __int128(time.ticks)*profile->frame_rate_num()/(__int128(time.rate)*profile->frame_rate_den())==m_gangTimelineAnchor)
        m_gangFineAnchor=position;
}
void PlayerWorker::resetMediaGangAnchors() {
    m_gangSourceAnchor=m_previewPos; m_gangTimelineAnchor=m_position;
    m_gangFineAnchor=parkedAudioPosition(); m_gangContext=audioEditContext();
    const auto source=m_preview.metadata();
    m_gangSourceNum=source.value("fpsNum").toInt();m_gangSourceDen=source.value("fpsDen",1).toInt();
}
bool PlayerWorker::mediaGangReady() {
    if (!m_mediaGang || !m_preview.isOpen() || m_preview.frameCount()<1 || m_timeline.frameCount()<1
        || m_preview.fps()<=0 || m_timeline.fps()<=0) return false;
    if (!AudioTimeline::sameEditContext(m_gangContext,audioEditContext(),false)) resetMediaGangAnchors();
    return m_gangSourceNum>0 && m_gangSourceDen>0;
}
PlayerWorker::GangedSourceFrame PlayerWorker::decodeGangedSource() {
    GangedSourceFrame result;
    if (!mediaGangReady()) return result;
    const auto *profile=m_timeline.profile();
    AudioTimeline::Time anchor{qint64(m_gangTimelineAnchor)*profile->frame_rate_den(),profile->frame_rate_num()};
    AudioTimeline::Time current{qint64(m_position)*profile->frame_rate_den(),profile->frame_rate_num()};
    AudioTimeline::Time fine;
    if (AudioTimeline::readTime(m_gangFineAnchor,&fine)) anchor=fine;
    if (AudioTimeline::readTime(parkedAudioPosition(),&fine)) current=fine;
    const auto delta=AudioTimeline::add(current,{-anchor.ticks,anchor.rate});
    if (!delta) {result.decoded.error=tr("The linked sound position is outside the supported time range.");return result;}
    const __int128 n=__int128(delta->ticks)*m_gangSourceNum,d=__int128(delta->rate)*m_gangSourceDen;
    const auto rounded=n>=0?(2*n+d)/(2*d):-((-2*n+d)/(2*d));
    result.position=int(std::clamp<__int128>(m_gangSourceAnchor+rounded,0,m_preview.frameCount()-1));
    result.decoded=m_preview.decodeAt(result.position,false);
    return result;
}
void PlayerWorker::refreshMediaGang() {
    if (!mediaGangReady()) return;
    RenderScope render(RenderScope::current()?RenderScope::current():frameRequest());
    if (!m_sourceMode) {
        const auto linked=decodeGangedSource();
        if (linked.decoded.cancelled || RenderScope::cancelled()) return;
        if (!linked.decoded.error.isEmpty()) {emit frameError(linked.decoded.error);return;}
        if (linked.position>=0) {
            m_previewPos=linked.position;
            emit previewFrameReady(linked.decoded.image,m_previewPos);
        }
        return;
    }
    const auto *profile=m_timeline.profile();
    AudioTimeline::Time anchor;QVariantMap position;
    int frame=qBound(0,m_gangTimelineAnchor+qRound((m_previewPos-m_gangSourceAnchor)*m_timeline.fps()/m_preview.fps()),m_timeline.frameCount()-1);
    if (AudioTimeline::readTime(m_gangFineAnchor,&anchor)) {
        const auto moved=AudioTimeline::add(anchor,{qint64(m_previewPos-m_gangSourceAnchor)*m_gangSourceDen,m_gangSourceNum});
        if (!moved) {emit frameError(tr("The linked sound position is outside the supported time range."));return;}
        auto time=*moved;
        const qint64 last=qint64((__int128(m_timeline.frameCount())*profile->frame_rate_den()*time.rate+profile->frame_rate_num()-1)/profile->frame_rate_num())-1;
        time.ticks=qBound<qint64>(0,time.ticks,last);
        position=AudioTimeline::writeTime(time);
        frame=int(__int128(time.ticks)*profile->frame_rate_num()/(__int128(time.rate)*profile->frame_rate_den()));
    }
    const auto decoded=decodePlaybackFrame(frame,false);
    if (decoded.cancelled || RenderScope::cancelled()) return;
    if (!decoded.error.isEmpty()) {emit frameError(decoded.error);return;}
    m_position=frame;
    restoreAudioPosition(position,false);
    emit frameReady(decoded.image,m_position);
}

void PlayerWorker::setMonitorVolume(double volume) {
    m_monitorVolume = qBound(0.0, volume, 1.0);
    m_audioOutput->setVolume(m_monitorMuted ? 0. : m_monitorVolume * (m_monitorDim ? .1 : 1.));
}

void PlayerWorker::setMonitorMapping(const QString &mode) {
    const bool changed = mode != m_monitorMapping;
    m_monitorMapping = mode;
    if (changed) soundChanged(); // the channels sound made ahead was folded to
}
void PlayerWorker::setMonitorOptions(bool muted, bool dim, bool mono) {
    const bool changed = mono != m_monitorMono;
    m_monitorMuted=muted; m_monitorDim=dim; m_monitorMono=mono;
    if (changed) soundChanged();
    setMonitorVolume(m_monitorVolume);
}
