#include "playerworker.h"
#include "sourcerateedit.h"
#include "mediarefs.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {
const QStringList attributeKeys{"fps","sourceFps","forceFps","duration","sourceDuration","startTimecodeFrames"};
QVariantMap attributes(const QVariantMap &source) {
    QVariantMap result;
    for(const auto &key:attributeKeys)if(source.contains(key))result[key]=source.value(key);
    return result;
}
QVariantMap mergeAttributes(QVariantMap sources,const QVariantMap &saved) {
    for(auto it=saved.begin();it!=saved.end();++it) {
        auto source=sources.value(it.key()).toMap();
        const auto savedSource=it.value().toMap();
        for(const auto &key:attributeKeys) {
            if(savedSource.contains(key))source[key]=savedSource.value(key);
            else source.remove(key);
        }
        sources[it.key()]=source;
    }
    return sources;
}
QVariantMap snapshotState(QVariantList *clips) {
    if(!clips->isEmpty() && clips->last().toMap().contains("omavid:markers"))return clips->takeLast().toMap();
    return {};
}
bool keepsTrims(QVariantList expected,const QVariantList &actual) {
    snapshotState(&expected);
    if(expected.size()!=actual.size())return false;
    for(int index=0;index<expected.size();++index) {
        const auto before=expected[index].toMap(),after=actual[index].toMap();
        for(const auto *key:{"lane","start","in","out","frames"})
            if(before.value(key).toInt()!=after.value(key).toInt())return false;
    }
    return true;
}
bool restoreTimeline(Timeline *timeline,const QVariantList &snapshot) {
    auto clips=snapshot;const auto state=snapshotState(&clips);
    const bool restored=state.contains("omavid:tracks")
        ? timeline->restore(clips,Timeline::trackFlagsFromVariant(state.value("omavid:tracks").toList()))
        : timeline->restore(clips);
    if(restored) {
        timeline->setMasterMixMap(state.value("omavid:master").toMap());
        timeline->setAdrCues(state.value("omavid:adr").toList());
    }
    return restored;
}
QVariantMap lockedSettings(Timeline &timeline) {
    auto settings=timeline.projectSettings();
    if(const auto profile=timeline.profile()) {
        settings["custom"]=true;settings["width"]=profile->width();settings["height"]=profile->height();
        settings["fpsNum"]=profile->frame_rate_num();settings["fpsDen"]=profile->frame_rate_den();
        settings["matchedProfile"]=QVariantMap{{"width",profile->width()},{"height",profile->height()},
            {"sarNum",profile->sample_aspect_num()},{"sarDen",profile->sample_aspect_den()},
            {"progressive",bool(profile->progressive())}};
    }
    return settings;
}
}

QVariantMap PlayerWorker::sourceAttributeState(const QStringList &paths) {
    QVariantMap sources;
    for(const auto &path:paths)sources[path]=attributes(m_timeline.mediaSources().value(path).toMap());
    QVariantList sequences;
    for(int index=0;index<m_sequences.size();++index) {
        const auto &sequence=m_sequences[index];
        sequences<<QVariantMap{{"id",sequence.id},
            {"name",sequence.name},{"binId",sequence.binId},{"synced",sequence.synced},{"configured",sequence.configured},{"lockFormat",sequence.lockFormat},
            {"snapshot",index==m_activeSequence?snapshotNow():sequence.snapshot},
            {"settings",index==m_activeSequence?resolvedTimelineSettings():sequence.settings},
            {"position",index==m_activeSequence?m_position:sequence.position},
            {"audioPosition",index==m_activeSequence?parkedAudioPosition():sequence.audioPosition}};
    }
    return {{"sources",sources},{"sequences",sequences},{"active",m_activeSequence},
        {"activeId",m_sequences.value(m_activeSequence).id}};
}

QVariantList PlayerWorker::sourceAttributeCounterpart(const QVariantList &target) {
    auto snapshot=colorUndoSnapshot();
    if(!target.isEmpty() && target.last().toMap().contains("omavid:sequenceState")) {
        auto sentinel=snapshot.last().toMap();sentinel["omavid:sequenceState"]=sourceAttributeState({});
        snapshot.last()=sentinel;return snapshot;
    }
    const auto state=target.isEmpty()?QVariantMap{}:target.last().toMap().value("omavid:sourceState").toMap();
    if(!state.isEmpty()) {
        auto sentinel=snapshot.last().toMap();
        sentinel["omavid:sourceState"]=sourceAttributeState(state.value("sources").toMap().keys());
        snapshot.last()=sentinel;
    }
    return snapshot;
}

void PlayerWorker::recordSequenceEdit(const QVariantMap &before,const QString &label) {
    auto snapshot=snapshotNow();auto sentinel=snapshot.last().toMap();
    sentinel["omavid:sequenceState"]=before;sentinel["omavid:undoLabel"]=label;
    snapshot.last()=sentinel;m_undoStack.append(snapshot);m_redoStack.clear();
    m_lastEditGesture=0;++m_editRevision;
    while(m_undoStack.size()>100)m_undoStack.removeFirst();
}

QString PlayerWorker::applyClipAttributes(const QVariantMap &sources,bool preserve) {
    if(exportActive())return tr("Wait for the export before changing clip attributes.");
    const auto previous=m_timeline.mediaSources();
    QStringList changedPaths;
    for(auto it=sources.begin();it!=sources.end();++it)
        if(attributes(previous.value(it.key()).toMap())!=attributes(it.value().toMap()))changedPaths<<it.key();
    if(changedPaths.isEmpty())return {};
    if(!preserve) {
        const auto problem=mediaRateChangeProblem(sources);
        if(!problem.isEmpty())return problem;
    }
    const auto beforeState=sourceAttributeState(changedPaths);
    auto afterState=beforeState;
    QVariantMap updated;
    for(const auto &path:changedPaths)updated[path]=attributes(sources.value(path).toMap());
    afterState["sources"]=updated;
    auto sequences=afterState.value("sequences").toList();
    for(auto &value:sequences) {
        auto sequence=value.toMap();
        const auto snapshot=sequence.value("snapshot").toList();
        if(preserve) {
            // Resolve automatic/inactive sequence clocks from their existing
            // media before changing interpretation. Every timeline keeps its rate.
            Timeline existing;existing.setAllowOffline(true);
            existing.setProjectSettings(sequence.value("settings").toMap());
            existing.setMediaSources(previous,m_timeline.proxyMode());
            if(!restoreTimeline(&existing,snapshot))return tr("Could not load a timeline before changing source attributes.");
            const auto prepared=SourceRateEdit::prepare(snapshot,previous,sources,existing.fps(),existing.trackFlagsVariant());
            if(!prepared.value("valid").toBool())return prepared.value("error").toString();
            sequence["snapshot"]=prepared.value("entries");
            sequence["settings"]=lockedSettings(existing);
        } else sequence["snapshot"]=MediaRefs::withSources(snapshot,sources);
        value=sequence;
    }
    afterState["sequences"]=sequences;
    auto before=snapshotNow();auto sentinel=before.last().toMap();
    sentinel["omavid:sourceState"]=beforeState;
    sentinel["omavid:undoLabel"]=tr("Change Clip Attributes");before.last()=sentinel;
    // Install the history only after all preparation succeeds. It is removed
    // again if the live transaction cannot commit; redo remains available.
    const auto redo=m_redoStack;
    m_undoStack.append(before);m_redoStack.clear();
    const auto problem=restoreSourceAttributeState(afterState,true,false);
    if(!problem.isEmpty()) {
        m_undoStack.removeLast();m_redoStack=redo;
        return problem;
    }
    m_lastEditGesture=0;++m_editRevision;
    while(m_undoStack.size()>100)m_undoStack.removeFirst();
    return {};
}

QString PlayerWorker::restoreSourceAttributeState(const QVariantMap &state,bool contentEdit,bool notifyLibrary,bool restoreSequences) {
    if(exportActive())return tr("Wait for the export before restoring project edits.");
    auto beforeSequences=m_sequences;
    const int beforeActive=m_activeSequence;
    const auto beforeSnapshot=snapshotNow();
    const auto beforeSources=m_timeline.mediaSources();
    const auto beforePreferences=m_timeline.projectSettings();
    const auto beforeSettings=resolvedTimelineSettings();
    const auto beforeMarkers=m_markers;
    const int beforePosition=m_position;
    const auto saved=state.value("sequences").toList();
    if(beforeActive<0 || beforeActive>=m_sequences.size() || saved.isEmpty())
        return tr("The project timelines changed before this edit could be restored.");
    beforeSequences[beforeActive].snapshot=beforeSnapshot;
    beforeSequences[beforeActive].settings=beforeSettings;
    beforeSequences[beforeActive].position=beforePosition;
    beforeSequences[beforeActive].audioPosition=parkedAudioPosition();
    const auto sources=mergeAttributes(beforeSources,state.value("sources").toMap());
    const auto activeId=state.value("activeId").toString();
    if(!restoreSequences && (saved.size()!=m_sequences.size() || activeId!=m_sequences[beforeActive].id))
        return tr("The project timelines changed before this edit could be restored.");
    QList<Sequence> prepared;
    QSet<QString> ids;
    int active=-1;
    for(int index=0;index<saved.size();++index) {
        const auto sequence=saved[index].toMap();
        const auto id=sequence.value("id").toString();
        if(id.isEmpty() || ids.contains(id))return tr("The project timelines changed before this edit could be restored.");
        ids.insert(id);if(id==activeId)active=index;
        int existing=-1;
        for(int i=0;i<beforeSequences.size();++i)if(beforeSequences[i].id==id)existing=i;
        if(!restoreSequences && existing!=index)
            return tr("The project timelines changed before this edit could be restored.");
        auto target=existing>=0?beforeSequences[existing]:Sequence{};
        target.id=id;
        if(restoreSequences) {
            target.name=sequence.value("name").toString();
            target.binId=sequence.value("binId").toString();
            target.synced=sequence.value("synced").toBool();
            // Timeline structure edits own membership and names, not later
            // changes to the clips, headers or preferences of surviving IDs.
            if(existing>=0) {prepared.append(target);continue;}
            target.configured=sequence.value("configured").toBool();
            target.lockFormat=sequence.value("lockFormat").toBool();
        }
        const auto currentSettings=target.settings;
        target.snapshot=sequence.value("snapshot").toList();
        target.settings=sequence.value("settings").toMap();target.position=sequence.value("position").toInt();
        target.audioPosition=sequence.value("audioPosition").toMap();
        if(notifyLibrary && existing>=0) {
            // Source history owns only the saved frame clock. Resolution,
            // colour, audio and project preferences keep their current values.
            const auto clock=target.settings;
            target.settings=currentSettings;
            for(const auto &key:QStringList{"fpsNum","fpsDen"})
                if(clock.contains(key))target.settings[key]=clock.value(key);
        }
        Timeline trial;trial.setAllowOffline(true);trial.setProjectSettings(target.settings);
        trial.setMediaSources(sources,m_timeline.proxyMode());
        if(!restoreTimeline(&trial,target.snapshot))return tr("The edit could not preserve every timeline. No changes were applied.");
        // The edit graph may accept an ordinary cut while clamping its out point.
        if(!keepsTrims(target.snapshot,trial.entries()))
            return restoreSequences
                ?tr("The timeline history cannot retain every requested clip trim. Check the source media before trying again. No changes were applied.")
                :tr("The new source rate cannot retain every requested clip trim. Shorten the affected trims or enable Keep selected footage to adjust durations. No changes were applied.");
        target.position=qBound(0,target.position,qMax(0,trial.frameCount()-1));
        prepared.append(target);
    }
    if(active<0)return tr("The project timelines changed before this edit could be restored.");
    // Capture both current and resurrected cache paths before the live mutation.
    // Recreating a closed timeline may reuse a file that still serves a nest.
    QMap<QString,QByteArray> nestBackups;
    QSet<QString> cachePaths;
    for(const auto &sequence:beforeSequences+prepared)cachePaths.insert(nestPath(sequence.id));
    for(const auto &path:cachePaths) {
        if(!QFileInfo::exists(path))continue;
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly))return tr("Could not preserve a nested sequence before restoring this edit.");
        nestBackups[path]=file.readAll();
        if(file.error()!=QFileDevice::NoError)return tr("Could not preserve a nested sequence before restoring this edit.");
    }
    const double speed=m_speed;const bool viewing=m_sourceMode;
    const auto preview=m_previewPath;const int previewPosition=m_previewPos;
    const bool rebuildActive=prepared[active].id!=beforeSequences[beforeActive].id
        || prepared[active].snapshot!=beforeSnapshot || prepared[active].settings!=beforeSettings || sources!=beforeSources;
    setSpeed(0.);
    if(restoreSequences) {
        dropStabJob();dropTrackJob();m_sourceMode=false;
        if(rebuildActive)while(!m_stripJobs.isEmpty())finishStripWork(0,QStringLiteral("cancelled"));
    }
    m_activeSequence=active;
    bool committed=true;
    if(rebuildActive) {
        m_timeline.setProjectSettings(prepared[active].settings,false);
        m_timeline.setMediaSources(sources,m_timeline.proxyMode());
        committed=restoreTimeline(&m_timeline,prepared[active].snapshot)
            && keepsTrims(prepared[active].snapshot,m_timeline.entries());
    }
    if(committed) {m_sequences=prepared;committed=refreshNests();}
    if(!committed) {
        bool recovered=true;
        for(const auto &path:cachePaths)
            if(!nestBackups.contains(path) && QFileInfo::exists(path))recovered &= QFile::remove(path);
        for(auto it=nestBackups.begin();it!=nestBackups.end();++it) {
            QFile current(it.key());
            if(current.open(QIODevice::ReadOnly) && current.readAll()==it.value() && current.error()==QFileDevice::NoError)
                continue; // an atomic write refusal left this backup intact
            current.close();
            QSaveFile file(it.key());
            recovered &= file.open(QIODevice::WriteOnly) && file.write(it.value())==it.value().size() && file.commit();
        }
        m_sequences=std::move(beforeSequences);m_activeSequence=beforeActive;
        if(rebuildActive) {
            m_timeline.setProjectSettings(beforeSettings,false);
            m_timeline.setMediaSources(beforeSources,m_timeline.proxyMode());
            recovered &= restoreTimeline(&m_timeline,beforeSnapshot);
            recovered &= keepsTrims(beforeSnapshot,m_timeline.entries());
            m_timeline.setProjectSettings(beforePreferences);
        }
        m_markers=beforeMarkers;m_position=beforePosition;m_sourceMode=viewing;
        if(viewing){openPreview(preview);seek(previewPosition);}else refreshFrame();
        setSpeed(speed);
        return recovered?tr("The edit could not be applied. The previous project state was restored.")
            :tr("The edit could not be applied, and some media could not be restored. The previous timeline snapshots are retained.");
    }
    auto activeSnapshot=prepared[active].snapshot;
    m_markers=snapshotState(&activeSnapshot).value("omavid:markers").toList();
    m_position=prepared[active].position;
    // The tiles and the picture are the open timeline's. An edit of a
    // timeline that is not open (Collapse Multicam there, its undo) leaves
    // them as they were, unless a nested sequence's file changed with it:
    // the open timeline may show that one.
    bool openChanged=rebuildActive;
    for(const auto &path:cachePaths) {
        if(openChanged)break;
        QFile file(path);
        if(file.exists()!=nestBackups.contains(path))openChanged=true;
        else if(file.exists())openChanged=!file.open(QIODevice::ReadOnly) || file.readAll()!=nestBackups.value(path);
    }
    if(openChanged)m_stripCache.clear();
    if(notifyLibrary && !state.value("sources").toMap().isEmpty())emit clipAttributesRestored(state.value("sources").toMap());
    emit markersChanged(m_markers);
    announceTimeline(contentEdit && (!restoreSequences || rebuildActive));
    restoreAudioPosition(m_sequences[m_activeSequence].audioPosition);
    announceSequences();
    if(viewing && !restoreSequences){openPreview(preview);seek(previewPosition);}
    else if(openChanged || viewing)refreshFrame(); // (out of the source viewer: the timeline's picture)
    setSpeed(speed);
    return {};
}
