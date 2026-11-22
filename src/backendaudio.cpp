#include "backend_p.h"

// ── Audio page: mix state, meters, library, recording ────────────────────

void Backend::setupAudio() {
    m_soundLibrary = new SoundLibrary(this);
    // The Audio page offers no sound library: sound is what the editor
    // imports. The catalogue stays for the setting that brings the panel back.
    m_soundLibrary->setFolderScanning(QSettings().value(QStringLiteral("audio/soundLibrary"), false).toBool());
    m_recorder = new Recorder(this);
    m_adrStopTimer = new QTimer(this);m_adrStopTimer->setSingleShot(true);
    connect(m_adrStopTimer,&QTimer::timeout,this,[this]{if(recordingTake())stopRecording();});
    connect(m_worker,&PlayerWorker::adrRecordReady,this,[this](const QString &id,int track,int start,int stop,const QString &prefix){
        if(recordingTake())return;
        bool present=false;for(const auto &v:m_adrCues)present=present||v.toMap().value("id").toString()==id;
        if(!present)return;
        m_recordCue=id;
        if(startRecording(track,prefix)){m_recordStart=start;m_adrStopTimer->start(qMax(1,qRound((stop-start+1)*1000./qMax(1.,m_fps))));}
        else m_recordCue.clear();
    });
    connect(m_worker, &PlayerWorker::audioOperationFinished, this, &Backend::setStatus);
    connect(this, &Backend::trackMixRequested, m_worker, &PlayerWorker::setTrackMix);
    connect(this, &Backend::trackMixMapRequested, m_worker, &PlayerWorker::setTrackMixMap);
    connect(this, &Backend::masterMixRequested, m_worker, &PlayerWorker::setMasterMix);
    connect(this, &Backend::adrCuesRequested, m_worker, &PlayerWorker::setAdrCues);
    connect(this, &Backend::loopRequested, m_worker, &PlayerWorker::setLoop);
    connect(this, &Backend::scrubRequested, m_worker, &PlayerWorker::scrubTo);
    connect(this, &Backend::resetLoudnessRequested, m_worker, &PlayerWorker::resetLoudness);
    connect(m_worker, &PlayerWorker::trackLevels, m_audioMeters, &AudioMeters::setTrackPeaks);
    connect(m_worker, &PlayerWorker::audioRoutingLevels, m_audioMeters, &AudioMeters::setRoutingLevels);
    connect(m_worker, &PlayerWorker::loudnessChanged, m_audioMeters, &AudioMeters::setLoudness);
    connect(m_worker, &PlayerWorker::spectrumChanged, m_audioMeters, &AudioMeters::setSpectrum);
    connect(m_worker, &PlayerWorker::masterChanged, this, [this](const QVariantMap &master) {
        if (master == m_master)
            return;
        m_loudnessPass->invalidate();
        m_master = master;
        emit masterChanged();
    });
    connect(m_worker, &PlayerWorker::adrCuesChanged, this, [this](const QVariantList &cues) {
        if (cues == m_adrCues)
            return;
        m_adrCues = cues;
        emit adrCuesChanged();
    });
    connect(m_recorder, &Recorder::errorChanged, this, [this] {
        if (!m_recorder->error().isEmpty())
            setStatus(m_recorder->error());
    });
    connect(m_recorder, &Recorder::recordingFailed, this, [this](const QString &, const QString &message) {
        m_adrStopTimer->stop();
        pause();
        m_recordTrack = -1;
        m_recordCue.clear();
        emit recordingChanged();
        setStatus(message);
        emit failed(message);
    });
    // Audio the project library holds joins the sound library too.
    connect(this, &Backend::libraryChanged, this, [this] {
        QStringList paths;
        for (const QVariant &v : std::as_const(m_library)) {
            const QVariantMap e = v.toMap();
            if (!e.value(QStringLiteral("hasVideo")).toBool() && e.value(QStringLiteral("hasAudio")).toBool())
                paths.append(e.value(QStringLiteral("path")).toString());
        }
        if (!paths.isEmpty())
            m_soundLibrary->addPaths(paths);
    });
}

QVariantMap Backend::audioServices() const { return AudioEngine::serviceCapabilities(); }
QVariantList Backend::mixEffects() const { return Timeline::mixEffectCatalogue(); }


void Backend::setTrackMix(int track, const QString &key, const QVariant &value) {
    if (track < 0 || track >= m_tracks.size())
        return;
    if (key == "monitor") {
        if (value.toBool()) {
            const auto mix=m_tracks[track].toMap().value("mix").toMap();
            m_recorder->setInputDevice(mix.value("input").toString());
            for(int i=0;i<m_tracks.size();++i) if(i!=track && m_tracks[i].toMap().value("mix").toMap().value("monitor").toBool())
                emit trackMixRequested(i,"monitor",false);
        }
        m_recorder->setMonitoring(value.toBool());
    }
    emit trackMixRequested(track, key, plainVariant(value));
    // The Audio page's own height for a track is a way of looking at it, not
    // a change to the project (PlayerWorker::setTrackMix).
    if (key != "audioHeight")
        setDirty(true);
}

void Backend::setTrackMixMap(int track, const QVariantMap &mix) {
    if (track < 0 || track >= m_tracks.size())
        return;
    emit trackMixMapRequested(track, plainVariant(mix).toMap());
    setDirty(true);
}

void Backend::setMasterMix(const QString &key, const QVariant &value) {
    if (!hasVideo())
        return;
    if (key == "output") setAudioOutput(value.toString());
    emit masterMixRequested(key, plainVariant(value));
    setDirty(true);
}

void Backend::setAdrCues(const QVariantList &cues) {
    if (!hasVideo())
        return;
    emit adrCuesRequested(plainVariant(cues).toList());
    setDirty(true);
}

void Backend::setLoop(bool on) {
    if (on == m_loop)
        return;
    m_loop = on;
    emit loopRequested(m_loop, m_loopIn, m_loopOut);
    emit loopChanged();
}

void Backend::setLoopRange(int in, int out) {
    if (in == m_loopIn && out == m_loopOut)
        return;
    m_loopIn = in;
    m_loopOut = out;
    emit loopRequested(m_loop, m_loopIn, m_loopOut);
}

void Backend::scrubTo(int frame) {
    if (m_exporting || frame < 0)
        return;
    if (m_viewingSource)
        viewTimeline();
    emit scrubRequested(frame);
}

void Backend::resetLoudness() { emit resetLoudnessRequested(); }

void Backend::auditionSound(const QString &path) {
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    pause();
    m_previewLibraryIndex = -1;
    for (int i = 0; i < m_library.size(); ++i)
        if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == path)
            m_previewLibraryIndex = i;
    m_auditionPath = path;
    emit previewRequested(path);
    emit sourceChanged();
}

void Backend::stopAudition() {
    if (m_auditionPath.isEmpty())
        return;
    m_auditionPath.clear();
    viewTimeline();
    emit sourceChanged();
}

void Backend::dropSoundAt(const QString &path, int track, int frame) {
    if (m_exporting || path.isEmpty() || !QFileInfo::exists(path) || track < 0
        || track > m_tracks.size() || frame < 0)
        return;
    bool known = false;
    for (const QVariant &v : std::as_const(m_library))
        known = known || v.toMap().value(QStringLiteral("path")).toString() == path;
    if (!known)
        open(QUrl::fromLocalFile(path), false);
    const QString name = QFileInfo(path).fileName();
    if (!hasVideo()) {
        emit insertRequested(path, 0, -1, -1);
        setStatus(tr("Added %1").arg(name));
        return;
    }
    emit placeOnTrackRequested(track, 2, path, frame, -1, -1, true);
    setStatus(tr("Placed %1 on %2")
                  .arg(name, track < m_tracks.size()
                                 ? m_tracks.value(track).toMap().value(QStringLiteral("name")).toString()
                                 : QStringLiteral("a new audio track")));
}

QString Backend::recordingsDir() const {
    return StoragePreferences::recordingsRoot(m_projectPath);
}

bool Backend::startRecording(int track, const QString &prefix) {
    if (m_exporting || m_recordTrack >= 0 || track < 0 || track > m_tracks.size())
        return false;
    if (m_viewingSource)
        viewTimeline();
    if (track < m_tracks.size()) {
        const auto t=m_tracks[track].toMap();if(t.value("locked").toBool())return false;
        m_recorder->setInputDevice(t.value("mix").toMap().value("input").toString());
    }
    const QString path = m_recorder->start(recordingsDir(), prefix.isEmpty() ? QStringLiteral("Recording") : prefix);
    if (path.isEmpty())
        return false;
    m_recordTrack = track;
    m_recordStart = m_position;
    emit recordingChanged();
    if (hasVideo())
        play();
    setStatus(tr("Recording %1").arg(QFileInfo(path).fileName()));
    return true;
}

int Backend::trackForTake(int track, int start, int frames) const {
    const int end = start + qMax(1, frames);
    for (const QVariantList &list : {m_timeline, m_connected})
        for (const QVariant &v : list) {
            const QVariantMap e = v.toMap();
            if (e.value(QStringLiteral("gap")).toBool() || e.value(QStringLiteral("track")).toInt() != track)
                continue;
            const int from = e.value(QStringLiteral("start")).toInt();
            if (from < end && from + e.value(QStringLiteral("frames")).toInt() > start)
                return int(m_tracks.size()); // one past the last: a new audio track
        }
    return track;
}

void Backend::placeTake(const QString &path, int track, int start, int frames) {
    dropSoundAt(path, trackForTake(track, start, frames), start);
}

QString Backend::stopRecording() {
    if (m_recordTrack < 0)
        return QString();
    m_adrStopTimer->stop();
    pause();
    const int track = m_recordTrack;
    const int start = m_recordStart;
    const QString path = m_recorder->stop();
    m_recordTrack = -1;
    emit recordingChanged();
    if (path.isEmpty() || !QFileInfo::exists(path) || QFileInfo(path).size() <= 44) {
        m_recordCue.clear();
        setStatus(m_recorder->error().isEmpty() ? tr("Nothing was recorded.") : m_recorder->error());
        return QString();
    }
    if(m_recordCue.isEmpty())placeTake(path, track, start, qMax(1, m_position - start));
    else {
        auto cues=m_adrCues;
        for(int i=0;i<cues.size();++i){auto cue=cues[i].toMap();if(cue.value("id").toString()!=m_recordCue)continue;
            auto takes=cue.value("takes").toList();takes.append(QVariantMap{{"path",path},{"start",start},{"rating",0},{"promoted",false}});
            cue["takes"]=takes;cue["status"]="recorded";cues[i]=cue;break;}
        setAdrCues(cues);m_soundLibrary->addPaths({path});m_recordCue.clear();
    }
    emit takeRecorded(path, track, start);
    return path;
}

QVariantList Backend::audioOutputs() const {
    QVariantList out;
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    for (const QAudioDevice &d : QMediaDevices::audioOutputs()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromUtf8(d.id()));
        m.insert(QStringLiteral("name"), d.description());
        m.insert(QStringLiteral("default"), d.id() == def.id());
        out.append(m);
    }
    return out;
}

void Backend::chooseSoundFolder() {
    m_chooserMode = ChooserMode::SoundFolder;
    m_chooser->openDirectory(QStringLiteral("Sound Folder"));
}

void Backend::endAudioGesture(int track,const QString &key) {
    QMetaObject::invokeMethod(m_worker,[this,track,key]{m_worker->endAudioGesture(track,key);},Qt::QueuedConnection);
}

void Backend::beginAudioGesture(int track,const QString &key) {
    QMetaObject::invokeMethod(m_worker,[this,track,key]{m_worker->beginAudioGesture(track,key);},Qt::QueuedConnection);
}

void Backend::setAudioFadeCurve(int track,int index,const QString &curve) {
    QMetaObject::invokeMethod(m_worker,[this,track,index,curve]{m_worker->setAudioFadeCurve(track,index,curve);},Qt::QueuedConnection);
}

void Backend::startAdrRecording(int cueIndex) {
    if(recordingTake()||m_exporting)return;
    QMetaObject::invokeMethod(m_worker,[this,cueIndex]{m_worker->prepareAdrRecording(cueIndex);},Qt::QueuedConnection);
}
void Backend::promoteAdrTake(int cueIndex,int takeIndex,int in,int out) {
    QMetaObject::invokeMethod(m_worker,[this,cueIndex,takeIndex,in,out]{m_worker->promoteAdrTake(cueIndex,takeIndex,in,out);},Qt::QueuedConnection);
}

QVariantList Backend::audioRenderTracks() const {
    QVariantList out;
    for(const auto &v:m_tracks){auto track=v.toMap();const int index=track.value("index").toInt();bool sound=track.value("kind")=="audio";
        const auto entries=index==0?m_timeline:m_connected;
        for(const auto &e:entries){auto clip=e.toMap();if(index!=0&&clip.value(ClipKey::track).toInt()!=index)continue;
            if(!clip.value(ClipKey::videoOnly).toBool()&&!clip.value(ClipKey::gap).toBool()&&clip.value(ClipKey::title).toString().isEmpty()
                && !clip.value(ClipKey::path).toString().startsWith("color:"))sound=true;}
        if(sound){track["hasAudio"]=true;out.append(track);}
    }
    return out;
}
