#include "backend_p.h"

Backend::Backend(QObject *parent)
    : QObject(parent), m_chooser(new FileChooser(this)), m_worker(new PlayerWorker) {
    // In this order: several of them answer the same signal, and the
    // answers run in the order the connections were made.
    createParts();
    wireFileChooser();
    m_worker->moveToThread(&m_thread);
    wireClipPlacement();
    wireEdits();
    wireTimelines();
    wireViewer();
    wireProjectFiles();
    wirePictures();
    setupMedia();
    setupHardware();
    setupDeliver();
    setupAudio();
    setupBeats();
    setupColor();
    setupProject();
    setupProjectSafety();
    setupFeedback();
    setupPlayback();
    setupAudioCursor();
    // Keep missing projects so users can locate or remove them explicitly.
    const QStringList stored =
        QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"))
            .value(QStringLiteral("recentProjects")).toStringList();
    for (const QString &path : stored)
        if (!path.isEmpty() && !m_recent.contains(path) && m_recent.size() < 8)
            m_recent.append(path);
    m_presets = QJsonDocument::fromJson(
                    QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"))
                        .value(QStringLiteral("attributePresets")).toString().toUtf8())
                    .toVariant().toList();
    // Before anything else the worker does: no project is open yet, so no
    // clip is looking at a kept file while it moves.
    connect(&m_thread, &QThread::started, m_worker, [worker = m_worker] { worker->adoptKeptMedia(); });
    connect(&m_thread, &QThread::started, m_worker, &PlayerWorker::refreshSequences);
    m_thread.start();
}

// The objects Backend owns and hands to the window, and what they report back.
void Backend::createParts() {
    m_backgroundTasks = new BackgroundTasks(this);
    m_interchangePool = new QThreadPool(this);
    m_interchangePool->setMaxThreadCount(1);
    m_workspaceLayouts = new WorkspaceLayouts(this);
    m_effectShelf = new EffectShelf(this);
    connect(m_effectShelf, &EffectShelf::status, this, &Backend::setStatus);
    m_audioMeters = new AudioMeters(this);
    m_loudnessPass = new LoudnessPass(m_worker, this);
    m_loudnessPass->hasTimeline = [this] { return hasVideo(); };
    connect(m_loudnessPass, &LoudnessPass::message, this, &Backend::setStatus);
    m_transcriber = new Transcriber(this);
    m_transcriber->locate = [this](const QVariantMap &job) { return transcriptClip(job); };
    m_transcriber->store = [this](int index, const QVariantList &segments, const QString &language) {
        storeTranscript(index, segments, language);
    };
    connect(m_transcriber, &Transcriber::message, this, &Backend::setStatus);
    connect(m_transcriber, &Transcriber::finished, this, &Backend::transcriptionFinished);
    m_commands = new CommandRegistry(this);
    m_mediaCatalog = new MediaCatalog(this);
    connect(m_mediaCatalog, &MediaCatalog::changed, this, [this] {
        if (!m_restoringLibrary) setDirty(true);
    });
    connect(m_mediaCatalog, &MediaCatalog::changed, this, &Backend::timelineItemsChanged);
    connect(m_mediaCatalog, &MediaCatalog::error, this, [this](const QString &message) {
        setStatus(message); emit failed(message);
    });
    connect(m_mediaCatalog, &MediaCatalog::removed, this, [this](const QStringList &ids) {
        QVariantList indexes;
        for (int i = 0; i < m_library.size(); ++i)
            if (ids.contains(m_library.at(i).toMap().value(QStringLiteral("binId")).toString()))
                indexes.append(i);
        moveMediaToBin(indexes, QString());
    });
    m_volumes = new Volumes(this);
    connect(this, &Backend::projectChanged, this, [this] { m_volumes->setProjectPath(m_projectPath); });
    connect(m_volumes, &Volumes::becameLow, this, [this](const QString &free) {
        m_backgroundTasks->post("warning", tr("Low disk space"), tr("%1 on the project volume. Free space before importing or rendering.").arg(free));
    });
    m_volumes->setProjectPath(m_projectPath);
    // A drive holding the project, its media or their proxies stays mounted.
    m_volumes->setInUse([this](const QString &mountPoint) -> QString {
        const QString root = mountPoint.endsWith(QLatin1Char('/')) ? mountPoint : mountPoint + QLatin1Char('/');
        const auto on = [&](const QString &path) { return !path.isEmpty() && (path.startsWith(root) || path == mountPoint); };
        if (on(m_projectPath))
            return m_projectPath;
        for (const QVariantList *list : {&m_library, &m_timeline, &m_connected})
            for (const QVariant &value : *list) {
                const QVariantMap entry = value.toMap();
                for (const char *key : {"path", "proxyPath"})
                    if (const QString path = entry.value(QLatin1String(key)).toString(); on(path))
                        return path;
            }
        return QString();
    });
    connect(m_volumes, &Volumes::diskActionFinished, this,
            [this](const QString &, const QString &, bool, const QString &message, const QString &) {
                if (!message.isEmpty())
                    setStatus(message);
            });
}

// What a file dialog's answer is for depends on what asked (m_chooserMode).
void Backend::wireFileChooser() {
    connect(m_chooser, &FileChooser::selected, this, [this](const QUrl &url) {
        switch (m_chooserMode) {
        case ChooserMode::Export:
            exportTo(url, m_exportPreset);
            return;
        case ChooserMode::SaveProject: {
            saveProjectAs(QUrl::fromLocalFile(ProjectFile::withExtension(url.toLocalFile())));
            return;
        }
        case ChooserMode::ExportFrame:
            saveFrameImage(url.toLocalFile(), m_exportFrameSource);
            return;
        case ChooserMode::LocateProject:
            m_replacementRecentPath = url.toLocalFile();
            loadProject(url);
            return;
        case ChooserMode::OpenProject:
            loadProject(url);
            return;
        case ChooserMode::ArchiveFolder:
            emit archiveFolderChosen(url.toLocalFile());
            return;
        case ChooserMode::ImportCaptions:
            importCaptions(url);
            return;
        case ChooserMode::ExportCaptions:
            exportCaptionFiles(url, m_captionExportFormat, m_captionExportTrack);
            return;
        case ChooserMode::ImportTimeline:
            importTimeline(url);
            return;
        case ChooserMode::ExportTimeline:
            exportTimeline(url, m_exportTimelineFormat);
            return;
        case ChooserMode::RenderFolder:
            emit renderFolderChosen(url.toLocalFile());
            return;
        case ChooserMode::SoundFolder:
            if (m_soundLibrary)
                m_soundLibrary->addFolder(url.toLocalFile());
            return;
        case ChooserMode::LutFolder:
            m_lutLibrary->setFolder(url.toLocalFile());
            return;
        case ChooserMode::ColorAsset:
            if(m_colorAssetAction=="matte") {
                QImageReader reader(url.toLocalFile());const QSize size=reader.size();
                if(!reader.canRead()||!size.isValid()||qint64(size.width())*size.height()>32*1024*1024){setStatus(tr("Choose a readable matte image up to 32 megapixels."));return;}
            }
            emit colorAssetChosen(m_colorAssetAction, url.toLocalFile());
            return;
        case ChooserMode::LutFile:
            m_lutLibrary->addFile(url.toLocalFile());
            emit lutFileChosen(url.toLocalFile());
            return;
        case ChooserMode::Import:
        case ChooserMode::ImportFolder:
            return; // handled below, with every URL the picker returned
        }
    });
    connect(m_chooser, &FileChooser::selectedMany, this, [this](const QList<QUrl> &urls) {
        if (m_chooserMode == ChooserMode::Import || m_chooserMode == ChooserMode::ImportFolder)
            importUrls(urls);
    });
    connect(m_chooser, &FileChooser::failed, this, [this](const QString &message) {
        setStatus(message);
        emit failed(message);
        emit chooserCanceled(); // abandon pending Save-then-close/new/open intents too
    });
    connect(m_chooser, &FileChooser::canceled, this, &Backend::chooserCanceled);
}

// Placing clips, titles and generators on the timeline.
void Backend::wireClipPlacement() {
    // What an edit takes is the editor's standing choice, kept between runs.
    // A script's edits never follow it: they say what they take.
    if (!qApp || !qApp->property("omaedit.agent").toBool())
        m_editTakes = qMax(0, QStringList{QStringLiteral("both"), QStringLiteral("picture"), QStringLiteral("sound")}
                                  .indexOf(QSettings().value(QStringLiteral("edit/takes")).toString()));
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &Backend::importRequested, m_worker, &PlayerWorker::importClip);
    connect(this, &Backend::previewRequested, m_worker, &PlayerWorker::openPreview);
    connect(this, &Backend::closePreviewRequested, m_worker, &PlayerWorker::closePreview);
    connect(this, &Backend::appendRequested, this, [this](const QString &path, int in, int out, int takes) {
        dispatchTimelinePlacement(path, [this, path, in, out, takes] { m_worker->appendClip(path, in, out, takes); },
            {{"op","append"},{"takes",takes}});
    });
    connect(this, &Backend::insertRequested, this, [this](const QString &path, int position, int in, int out, int takes) {
        dispatchTimelinePlacement(path, [this, path, position, in, out, takes] { m_worker->insertClip(path, position, in, out, takes); },
            {{"op","insert"},{"position",position},{"takes",takes}});
    });
    connect(this, &Backend::overwriteRequested, this, [this](const QString &path, int position, int in, int out, int takes) {
        dispatchTimelinePlacement(path, [this, path, position, in, out, takes] { m_worker->overwriteClip(path, position, in, out, takes); },
            {{"op","overwrite"},{"position",position},{"takes",takes}});
    });
    connect(this, &Backend::removeRequested, m_worker, &PlayerWorker::removeClip);
    connect(this, &Backend::liftRequested, m_worker, &PlayerWorker::liftClip);
    connect(this, &Backend::removeRangeRequested, m_worker,
            &PlayerWorker::removeRange);
    connect(this, &Backend::insertGapRequested, this, [this](int position, int frames) {
        dispatchTimelinePlacement(QString(), [this, position, frames] { m_worker->insertGap(position, frames); });
    });
    connect(this, &Backend::closeGapRequested, m_worker, &PlayerWorker::closeGap);
    connect(this, &Backend::deleteGapsRequested, m_worker, &PlayerWorker::deleteGaps);
    connect(this, &Backend::rollRequested, m_worker,
            &PlayerWorker::rollEdit);
    connect(this, &Backend::setVolumeKfRequested, m_worker,
            &PlayerWorker::setClipVolumeKf);
    connect(this, &Backend::setConnectedVolumeKfRequested, m_worker,
            &PlayerWorker::setConnectedVolumeKf);
    connect(this, &Backend::setOpacityRequested, m_worker,
            &PlayerWorker::setConnectedOpacity);
    connect(this, &Backend::setOpacityKfRequested, m_worker,
            &PlayerWorker::setConnectedOpacityKf);
    connect(this, &Backend::makeCompoundRequested, m_worker,
            &PlayerWorker::makeCompound);
    connect(this, &Backend::breakApartRequested, m_worker,
            &PlayerWorker::breakApartClip);
    connect(m_worker, &PlayerWorker::compoundEditFinished, this,
            [this](bool breakingApart, int index, const QString &error) {
        if (!error.isEmpty()) {
            setStatus(error);
            return;
        }
        setStatus(breakingApart ? tr("Nested clip broken apart")
                               : tr("Nested clip created"));
        if (!breakingApart)
            emit compoundCreated(index);
    });
    connect(this, &Backend::setDisabledRequested, m_worker,
            &PlayerWorker::setClipDisabled);
    connect(this, &Backend::connectRequested, this, [this](const QString &path, int position, int in, int out, bool audio) {
        dispatchTimelinePlacement(path, [this, path, position, in, out, audio] { m_worker->connectClip(path, position, in, out, audio); },
            {{"op","connect"},{"position",position},{"audio",audio}});
    });
    connect(this, &Backend::tagNextPlacementRequested, this, [this](const QVariantMap &tag) {
        m_nextTimelinePlacementTag=tag;
    });
    connect(this, &Backend::cutToAngleRequested, m_worker, &PlayerWorker::cutToAngle);
    connect(this, &Backend::setMulticamSoundRequested, m_worker, &PlayerWorker::setMulticamSound);
    connect(this, &Backend::collapseMulticamRequested, m_worker, &PlayerWorker::collapseMulticam);
    connect(this, &Backend::detachAudioOnRequested, m_worker,
            &PlayerWorker::detachAudioOn);
    connect(this, &Backend::reattachAudioRequested, m_worker, &PlayerWorker::reattachAudio);
    connect(this, &Backend::setFlipRequested, m_worker, &PlayerWorker::setFlip);
    connect(this, &Backend::setAudioMapRequested, m_worker, &PlayerWorker::setAudioMap);
    connect(this, &Backend::setNormalizeRequested, m_worker, &PlayerWorker::setNormalize);
    connect(this, &Backend::freezeFrameRequested, m_worker, &PlayerWorker::freezeFrame);
    connect(this, &Backend::syncByAudioRequested, m_worker, &PlayerWorker::syncByAudio);
    connect(this, &Backend::addSyncedClipRequested, m_worker, &PlayerWorker::addSyncedClip);
    connect(this, &Backend::setCategoriesRequested, m_worker, &PlayerWorker::setCategories);
    connect(m_worker, &PlayerWorker::syncedClipFinished, this, [this](const QString &name, const QString &error) {
        setStatus(error.isEmpty() ? tr("Synced clip “%1” is in the library").arg(name) : error);
        if (!error.isEmpty())
            emit failed(error);
    });
    connect(this, &Backend::decodeAnglesRequested, m_worker, &PlayerWorker::decodeAngles);
    connect(m_worker, &PlayerWorker::clipsSynced, this, &Backend::onClipsSynced);
    connect(m_worker, &PlayerWorker::anglesReady, this, &Backend::onAnglesReady);
    connect(this, &Backend::connectTitleRequested, this, [this](const QString &text, int position, int length, const QVariantMap &style) {
        dispatchTimelinePlacement(QString(), [this, text, position, length, style] { m_worker->connectTitle(text, position, length, style); });
    });
    connect(this, &Backend::updateTitleRequested, m_worker,
            &PlayerWorker::updateTitle);
    connect(this, &Backend::trimConnectedRequested, m_worker,
            &PlayerWorker::trimConnectedClip);
    connect(this, &Backend::setConnectedRectRequested, m_worker,
            &PlayerWorker::setConnectedRect);
    connect(this, &Backend::removeConnectedRequested, m_worker,
            &PlayerWorker::removeConnectedClip);
    connect(this, &Backend::moveConnectedRequested, m_worker,
            &PlayerWorker::moveConnectedClip);
    connect(this, &Backend::splitRequested, m_worker, &PlayerWorker::splitAt);
    connect(this, &Backend::splitAllRequested, m_worker, &PlayerWorker::splitAllAt);
    connect(this, &Backend::trimToFrameRequested, m_worker, &PlayerWorker::trimToFrame);
    connect(this, &Backend::splitOnRequested, m_worker, &PlayerWorker::splitOn);
    connect(this, &Backend::addTransitionRequested, m_worker, &PlayerWorker::addTransition);
    connect(this, &Backend::setTransitionSidesRequested, m_worker, &PlayerWorker::setTransitionSides);
    connect(this, &Backend::resizeDissolveRequested, m_worker,
            &PlayerWorker::resizeDissolve);
    connect(this, &Backend::restyleDissolveRequested, m_worker,
            &PlayerWorker::restyleDissolve);
    connect(this, &Backend::removeDissolveRequested, m_worker,
            &PlayerWorker::removeDissolve);
    connect(this, &Backend::setFadesRequested, m_worker,
            &PlayerWorker::setClipFades);
    connect(this, &Backend::toggleFadeEdgeRequested, m_worker, &PlayerWorker::toggleFadeEdge);
    connect(this, &Backend::setVolumeRequested, m_worker,
            &PlayerWorker::setClipVolume);
    connect(this, &Backend::setConnectedVolumeRequested, m_worker,
            &PlayerWorker::setConnectedVolume);
    connect(this, &Backend::setPanRequested, m_worker,
            &PlayerWorker::setClipPan);
    connect(this, &Backend::setConnectedPanRequested, m_worker,
            &PlayerWorker::setConnectedPan);
    connect(this, &Backend::setEqRequested, m_worker,
            &PlayerWorker::setClipEq);
    connect(this, &Backend::setConnectedEqRequested, m_worker,
            &PlayerWorker::setConnectedEq);
    connect(this, &Backend::detachAudioRequested, m_worker,
            &PlayerWorker::detachAudio);
}

// Edits on placed clips and what the worker says came of them.
void Backend::wireEdits() {
    connect(m_worker, &PlayerWorker::audioLevels, m_audioMeters, &AudioMeters::setLevels);
    connect(this, &Backend::setSpeedClipRequested, m_worker,
            &PlayerWorker::setClipSpeed);
    connect(this, &Backend::setSpeedConnectedRequested, m_worker,
            &PlayerWorker::setConnectedSpeed);
    connect(this, &Backend::setColorRequested, m_worker,
            &PlayerWorker::setClipColor);
    connect(this, &Backend::trimRequested, m_worker, &PlayerWorker::trimClip);
    connect(this, &Backend::moveRequested, m_worker, &PlayerWorker::moveClip);
    connect(this, &Backend::toggleMarkerRequested, m_worker,
            &PlayerWorker::toggleMarker);
    connect(this, &Backend::removeMarkersInRequested, m_worker, &PlayerWorker::removeMarkersIn);
    connect(this, &Backend::setMarkerInfoRequested, m_worker,
            &PlayerWorker::setMarkerInfo);
    connect(this, &Backend::retimeRequested, m_worker, &PlayerWorker::applyRetime);
    connect(this, &Backend::audioEditRequested, m_worker, &PlayerWorker::applyAudioEdit);
    connect(this, &Backend::audioGestureRequested, m_worker, &PlayerWorker::applyAudioGesture);
    connect(this, &Backend::clipboardPasteRequested, m_worker, &PlayerWorker::pasteClipboardSnapshot);
    connect(m_worker, &PlayerWorker::audioClipboardFinished, this,
        [this](int request,bool changed,const QString &error,const QVariantList &added) {
            if(!error.isEmpty())setStatus(error);
            else if(changed)setStatus(tr("Pasted %n clip(s)",nullptr,added.size()));
            emit clipboardPasteFinished(request,changed,error,added);
        });
    connect(m_worker, &PlayerWorker::audioGestureFinished, this, [this](int request,bool changed,const QString &error,const QVariantMap &selected) {
        if (!error.isEmpty()) setStatus(error);
        emit audioGestureFinished(request,changed,error,selected);
    });
    connect(m_worker, &PlayerWorker::audioEditFinished, this, [this](int request,bool changed,const QString &error) {
        if(!error.isEmpty())setStatus(error);
        emit audioEditFinished(request,changed,error);
    });
    connect(this, &Backend::speedRampPrepareRequested, m_worker, &PlayerWorker::prepareSpeedRamp);
    connect(m_worker, &PlayerWorker::speedRampPrepared, this, &Backend::speedRampPrepared);
    connect(this, &Backend::speedRampRequested, m_worker, &PlayerWorker::applySpeedRamp);
    connect(m_worker, &PlayerWorker::retimeFinished, this, [this](int request, bool changed, const QString &error) {
        if (!error.isEmpty()) setStatus(error);
        else if (changed) setStatus(tr("Speed / Duration updated"));
        emit retimeFinished(request,changed,error);
    });
    connect(this, &Backend::markerEditRequested, m_worker, &PlayerWorker::applyMarkerEdit);
    connect(m_worker, &PlayerWorker::markerEditFinished, this,
            [this](int request, bool changed, const QString &error) {
                if (!error.isEmpty()) setStatus(error);
                else if (changed) setStatus(tr("Marker updated"));
                emit markerEditFinished(request, changed, error);
            });
    connect(this, &Backend::setMarkersRequested, m_worker,
            &PlayerWorker::setMarkers);
    connect(m_worker, &PlayerWorker::markersChanged, this,
            [this](const QVariantList &markers) {
                m_markers = markers;
                emit markersChanged();
            });
    connect(m_worker, &PlayerWorker::tracksChanged, this,
            [this](const QVariantList &tracks) {
                if (tracks != m_tracks) m_loudnessPass->invalidate();
                m_tracks = tracks;
                if (m_destTrack >= m_tracks.size()) { // its track went away
                    m_destTrack = 0;
                    emit destTrackChanged();
                }
                emit tracksChanged();
            });
    connect(this, &Backend::trackFlagRequested, m_worker,
            &PlayerWorker::setTrackFlag);
    connect(this, &Backend::setConnectedColorRequested, m_worker,
            &PlayerWorker::setConnectedColor);
    connect(this, &Backend::setCropRequested, m_worker, &PlayerWorker::setClipCrop);
    connect(this, &Backend::setConnectedCropRequested, m_worker,
            &PlayerWorker::setConnectedCrop);
    connect(this, &Backend::setConnectedAnchorRequested, m_worker, &PlayerWorker::setConnectedAnchor);
    connect(m_worker, &PlayerWorker::editOperationFinished, this, &Backend::setStatus);
    connect(m_worker, &PlayerWorker::editRefused, this, &Backend::sayRefused);
    connect(m_worker, &PlayerWorker::editRefused, this, &Backend::editRefused);
    connect(m_worker, &PlayerWorker::editNoted, this, &Backend::sayRefused);
    // A multicam clip placed on the timeline opens its viewer, so the number
    // keys cut it at once. Nothing else moves; closed afterwards, it stays
    // closed until the next one is placed.
    connect(m_worker, &PlayerWorker::multicamPlaced, this, [this] { setMulticamMode(true); });
    // The clip list an undo, a redo or a timeline switch left carries a new
    // "replay" number. Edits sent from here on name clips in that list, and
    // the worker is told so: it refuses the ones sent before (connected ahead
    // of onTimeline, so the answer is on its way before anything can act on
    // the new list). The 0 says this window answers at all.
    const auto drew = [worker = m_worker](int replay) {
        QMetaObject::invokeMethod(worker, [worker, replay] { worker->acknowledgeReplay(replay); }, Qt::QueuedConnection);
    };
    drew(0);
    connect(m_worker, &PlayerWorker::timelineChanged, this,
            [drew, drawn = 0](const QVariantList &, int, double, int, int, bool, bool, bool, const QVariantMap &,
                              const QVariantMap &context) mutable {
                const int replay = context.value(QStringLiteral("replay")).toInt();
                if (replay != drawn) drew(drawn = replay);
            });
    connect(this, &Backend::editPropertiesRequested, m_worker, &PlayerWorker::setEditProperties);
    connect(this, &Backend::motionPresetRequested, m_worker, &PlayerWorker::applyMotionPreset);
    connect(this, &Backend::setFrameBlendRequested, m_worker, &PlayerWorker::setFrameBlend);
    connect(this, &Backend::connectGeneratorRequested, this, [this](const QString &resource, int position, int length, bool adjust) {
        dispatchTimelinePlacement(resource, [this, resource, position, length, adjust] { m_worker->connectGenerator(resource, position, length, adjust); });
    });
    connect(this, &Backend::placeEntryRequested, m_worker, &PlayerWorker::pasteClip);
    connect(this, &Backend::setGeneratorColorRequested, m_worker, &PlayerWorker::setGeneratorColor);
    connect(this, &Backend::setConnectedRotationRequested, m_worker,
            &PlayerWorker::setConnectedRotation);
    connect(this, &Backend::rollOnRequested, m_worker, &PlayerWorker::rollEditOn);
    connect(this, &Backend::trimOnRequested, m_worker, &PlayerWorker::trimClipOn);
    connect(this, &Backend::removeConnectedDissolveRequested, m_worker,
            &PlayerWorker::removeConnectedDissolve);
    connect(this, &Backend::resizeConnectedDissolveRequested, m_worker,
            &PlayerWorker::resizeConnectedDissolve);
    connect(this, &Backend::restyleConnectedDissolveRequested, m_worker,
            &PlayerWorker::restyleConnectedDissolve);
    connect(this, &Backend::setConnectedFadesRequested, m_worker,
            &PlayerWorker::setConnectedFades);
    connect(this, &Backend::addTrackRequested, m_worker, &PlayerWorker::addTrack);
    connect(this, &Backend::removeTrackRequested, m_worker,
            &PlayerWorker::removeTrack);
    connect(this, &Backend::swapTracksRequested, m_worker,
            &PlayerWorker::swapTracks);
    connect(this, &Backend::fitToFillRequested, this, [this](int track, const QString &path, int position, int frames, int in, int out, bool audio) {
        dispatchTimelinePlacement(path, [this, track, path, position, frames, in, out, audio] { m_worker->fitToFill(track, path, position, frames, in, out, audio); },
            {{"op","fit"},{"track",track},{"position",position},{"frames",frames},{"audio",audio}});
    });
    connect(this, &Backend::placeOnTrackRequested, this, [this](int track, int mode, const QString &path, int position, int in, int out, bool audio, int takes) {
        dispatchTimelinePlacement(path, [this, track, mode, path, position, in, out, audio, takes] { m_worker->placeOnTrack(track, mode, path, position, in, out, audio, takes); },
            {{"op","place"},{"track",track},{"mode",mode},{"position",position},{"audio",audio},{"takes",takes}});
    });
    connect(this, &Backend::moveClipToRequested, m_worker,
            &PlayerWorker::moveClipTo);
    connect(this, &Backend::pasteRequested, this, [this](int track, int position, const QVariantMap &entry, bool insert) {
        dispatchTimelinePlacement(entry.value("path").toString(), [this, track, position, entry, insert] { m_worker->pasteClip(track, position, entry, insert); });
    });
    connect(this, &Backend::slideRequested, m_worker, &PlayerWorker::slideClip);
    connect(this, &Backend::joinRequested, m_worker, &PlayerWorker::joinClips);
    connect(this, &Backend::nudgeRequested, m_worker, &PlayerWorker::nudgeClip);
    connect(this, &Backend::removeClipsRequested, m_worker, &PlayerWorker::removeClips);
    connect(this, &Backend::nudgeClipsRequested, m_worker, &PlayerWorker::nudgeClips);
    connect(this, &Backend::moveClipsRequested, m_worker, &PlayerWorker::moveClips);
    connect(this, &Backend::setClipsDisabledRequested, m_worker, &PlayerWorker::setClipsDisabled);
    connect(this, &Backend::setClipsLinkedRequested, m_worker, &PlayerWorker::setClipsLinked);
    connect(this, &Backend::setLabelsRequested, m_worker, &PlayerWorker::setLabels);
    connect(this, &Backend::nestSequenceRequested, m_worker, &PlayerWorker::nestSequence);
    connect(this, &Backend::nestSequenceOnTrackRequested, m_worker, &PlayerWorker::nestSequenceOnTrack);
}

// Timelines of the project: making, importing, switching; clip looks.
void Backend::wireTimelines() {
    for (const auto changed : {&Backend::sequencesChanged, &Backend::libraryChanged,
                               &Backend::timelineEntriesChanged, &Backend::videoChanged})
        connect(this, changed, this, &Backend::timelineItemsChanged);
    connect(this, &Backend::renderAndReplaceRequested, m_worker, &PlayerWorker::renderAndReplace);
    connect(this, &Backend::restoreOriginalRequested, m_worker, &PlayerWorker::restoreOriginal);
    connect(m_worker, &PlayerWorker::renderProgress, this, [this](int track, int index, int percent) {
        if (percent >= 100 || percent < 0) {
            m_renderJob = QVariantMap();
            setStatus(percent < 0 ? tr("Render in Place failed — the clip changed, or it cannot be rendered alone")
                                  : tr("Rendered in place — Restore Original brings the clip back"));
        } else {
            m_renderJob = QVariantMap{{QStringLiteral("track"), track}, {QStringLiteral("index"), index},
                                      {QStringLiteral("percent"), percent}};
            if (percent == 0)
                setStatus(tr("Rendering the clip…"));
        }
        emit renderJobChanged();
    });
    connect(this, &Backend::pasteAttributesRequested, m_worker, &PlayerWorker::pasteAttributes);
    connect(this, &Backend::setBypassRequested, m_worker, &PlayerWorker::setBypass);
    connect(this, &Backend::setClipMarkersRequested, m_worker, &PlayerWorker::setClipMarkers);
    connect(this, &Backend::editClipMarkerRequested, m_worker, &PlayerWorker::editClipMarker);
    connect(this, &Backend::importCaptionsRequested, m_worker, &PlayerWorker::importCaptions);
    connect(this, &Backend::placeCaptionsRequested, m_worker,
            [worker = m_worker](const QVariantList &cues, const QVariantMap &style, const QVariantList &remove, const QString &label) {
        worker->placeCaptions(cues, style, remove, label);
    });
    connect(this, &Backend::splitManyRequested, m_worker, &PlayerWorker::splitMany);
    connect(this, &Backend::setMixAlignRequested, m_worker, &PlayerWorker::setMixAlign);
    connect(this, &Backend::sequenceCreateRequested, m_worker, &PlayerWorker::createSequence);
    connect(m_worker, &PlayerWorker::sequenceCreateFinished, this, [this](int request,bool created,const QString &error) {
        m_sequenceCreateContexts.remove(request);
        const auto target=timelinePlacementTarget();
        const bool ownsPending=request==m_pendingTimelineCreateRequest;
        const bool current=m_pendingTimelineGeneration==m_mediaGeneration && hasTimeline();
        if(ownsPending && !created)m_pendingTimelineCreateRequest=0;
        if (!error.isEmpty()) setStatus(error);
        else if (created) setStatus(tr("Timeline created"));
        // Capture the destination before external listeners can queue a switch,
        // reset or edit. Replay will validate it again on the worker thread.
        emit sequenceCreateFinished(request,created,error);
        if(!ownsPending)return;
        if(!created) {
            // Retain placements for retry; an external listener may already
            // have submitted the corrected request during the completion signal.
            if(!m_pendingTimelineCreateRequest && !m_pendingTimelinePlacements.isEmpty() && !m_pendingTimelinePrompted) {
                m_pendingTimelinePrompted=true;emit timelineCreationRequested();
            }
            return;
        }
        if(request!=m_pendingTimelineCreateRequest)return;
        auto pending=std::move(m_pendingTimelinePlacements);
        m_pendingTimelinePlacements.clear();
        cancelTimelinePlacement(); // clear queue metadata; moved requests still own their results
        if(!current || m_projectLoadPending) {
            const auto reason=tr("The project or timeline changed. Paste the clips again.");
            for(auto &placement:pending)if(placement.cancel)placement.cancel(reason);
            return;
        }
        QList<std::function<void(const QVariantMap &)>> actions;
        for(auto &placement:pending)actions.append(std::move(placement.execute));
        QMetaObject::invokeMethod(m_worker,[worker=m_worker,target,actions] {
            worker->replayTimelinePlacements(target,actions);
        },Qt::QueuedConnection);
    });
    connect(this, &Backend::projectPlaybackReset, this, &Backend::cancelTimelinePlacement);
    connect(this, &Backend::addSequenceRequested, m_worker, &PlayerWorker::addSequence);
    connect(this, &Backend::importSequenceRequested, m_worker, &PlayerWorker::importSequence);
    connect(m_worker, &PlayerWorker::sequenceImportFinished, this,
            [this](const QString &name, const QStringList &problems, bool ok, const QString &error) {
        const QString rateNote = std::exchange(m_timelineImportRateNote, QString());
        if (!ok) { setStatus(error); emit failed(error); return; }
        setDirty(true);
        const QString imported = problems.isEmpty() ? tr("Imported “%1” as a new sequence").arg(name)
                                                    : tr("Imported “%1” as a new sequence; %2 items need attention").arg(name).arg(problems.size());
        setStatus(rateNote.isEmpty() ? imported : imported + QStringLiteral(". ") + rateNote);
        emit timelineImported(name, problems);
    });
    connect(this, &Backend::switchSequenceRequested, m_worker, &PlayerWorker::switchSequence);
    connect(this, &Backend::renameSequenceRequested, m_worker, &PlayerWorker::renameSequence);
    connect(this, &Backend::moveSequenceToBinRequested, m_worker, &PlayerWorker::moveSequenceToBin);
    connect(this, &Backend::duplicateSequenceRequested, m_worker, &PlayerWorker::duplicateSequence);
    connect(this, &Backend::closeSequenceRequested, m_worker, &PlayerWorker::closeSequence);
    connect(this, &Backend::setSequencesRequested, m_worker, &PlayerWorker::setSequences);
    connect(m_worker, &PlayerWorker::sequencesChanged, this, &Backend::onSequences);
    connect(m_worker, &PlayerWorker::sequenceOpenFailed, this, [this](const QString &name) {
        setStatus(tr("Could not open sequence “%1” — its media is missing.").arg(name));
        emit failed(m_status);
    });
    connect(m_worker, &PlayerWorker::captionsImported, this, [this](int placed, int tracks) {
        if (placed == 0)
            setStatus(tr("No captions placed — is the Captions track locked?"));
        else
            setStatus(tr("Placed %n caption(s) on %1", nullptr, placed)
                          .arg(tracks > 1 ? tr("%n Captions track(s)", nullptr, tracks)
                                          : tr("the Captions track")));
    });
    connect(this, &Backend::captionStyleRequested, m_worker, &PlayerWorker::restyleCaptions);
    connect(this, &Backend::restyleCaptionRequested, m_worker, &PlayerWorker::restyleCaption);
    connect(this, &Backend::captionSpeakerRequested, m_worker, &PlayerWorker::setCaptionSpeaker);
    setupCaptionList();
    connect(this, &Backend::duplicateCaptionTrackRequested, m_worker, &PlayerWorker::duplicateCaptionTrack);
    connect(m_worker, &PlayerWorker::captionTrackDuplicated, this, [this](int, int captions) {
        setStatus(tr("Caption track duplicated: %n caption(s) copied", nullptr, captions));
    });
    connect(m_worker, &PlayerWorker::captionsRestyled, this, [this](int changed, int kept) {
        setStatus(kept ? tr("Caption style set: %n caption(s) restyled, %1 kept their own look", nullptr, changed).arg(kept)
                       : tr("Caption style set: %n caption(s) restyled", nullptr, changed));
    });
    // Until the worker says otherwise, a project is one sequence.
    m_sequences = {QVariantMap{{QStringLiteral("name"), QStringLiteral("Sequence 1")},
                               {QStringLiteral("position"), 0}, {QStringLiteral("configured"), false}}};
    connect(this, &Backend::setBlendRequested, m_worker, &PlayerWorker::setBlend);
    connect(this, &Backend::setScalingRequested, m_worker, &PlayerWorker::setScaling);
    connect(this, &Backend::setLensRequested, m_worker, &PlayerWorker::setLens);
    connect(this, &Backend::videoEffectRequested, m_worker, &PlayerWorker::editVideoEffect);
    connect(this, &Backend::findKeyBackdropRequested, m_worker, &PlayerWorker::findKeyBackdrop);
    connect(this, &Backend::stabilizeRequested, m_worker, &PlayerWorker::stabilize);
    connect(this, &Backend::removeStabRequested, m_worker, &PlayerWorker::removeStab);
    connect(m_worker, &PlayerWorker::stabProgress, this, &Backend::onStabProgress);
    connect(m_worker, &PlayerWorker::stabApplied, this, &Backend::onStabApplied);
}

// Comparison and two-up pictures, cancelling a frame, speed.
void Backend::wireViewer() {
    connect(this, &Backend::compareRequested, m_worker, &PlayerWorker::setCompare);
    connect(m_worker, &PlayerWorker::beforeFrameReady, this, &Backend::onBeforeFrame);
    connect(m_worker, &PlayerWorker::frameError, this, [this](const QString &error){setStatus(error);emit failed(error);});
    connect(this, &Backend::keptMediaSweepRequested, m_worker, &PlayerWorker::sweepKeptMedia);
    connect(m_worker, &PlayerWorker::keptMediaSwept, this, [this](const QVariantMap &report) {
        if (report.value(QStringLiteral("removed")).toBool()) emit unusedKeptMediaRemoved(report);
        else emit unusedKeptMediaFound(report);
    });
    connect(m_worker, &PlayerWorker::frameDiscarded, this, &Backend::flushPendingSeek);
    const auto cancelFrame=[this] { m_worker->cancelFrame(); };
    connect(this, &Backend::loadProjectRequested, this, cancelFrame);
    connect(this, &Backend::newProjectRequested, this, cancelFrame);
    connect(this, &Backend::switchSequenceRequested, this, cancelFrame);
    connect(this, &Backend::closePreviewRequested, this, cancelFrame);
    connect(this, &Backend::twoUpEdgeRequested, m_worker, &PlayerWorker::setTwoUpEdge);
    connect(m_worker, &PlayerWorker::twoUpReady, this, [this](const QImage &out, const QImage &in) {
        m_twoUpOutgoing = out;
        m_twoUpIncoming = in;
        emit twoUpFramesChanged();
    });
    connect(this, &Backend::pasteClipsRequested, m_worker, &PlayerWorker::pasteClips);
    connect(this, &Backend::setLabelRequested, m_worker, &PlayerWorker::setLabel);
    connect(this, &Backend::trackNameRequested, m_worker,
            &PlayerWorker::setTrackName);
    connect(this, &Backend::undoRequested, m_worker, &PlayerWorker::undo);
    connect(this, &Backend::redoRequested, m_worker, &PlayerWorker::redo);
    connect(this, &Backend::seekRequested, m_worker, &PlayerWorker::seek);
    connect(this, &Backend::speedRequested, this, [this](double speed) {
        const quint64 request = ++m_speedRequest;
        QMetaObject::invokeMethod(m_worker, [worker=m_worker, speed, request] {
            worker->requestSpeed(speed, request);
        }, Qt::QueuedConnection);
    });
}

// New, open and save.
void Backend::wireProjectFiles() {
    connect(this, &Backend::saveProjectRequested, this, [this] {
        m_savePending = true; emit projectBusyChanged();
        m_saveRevision = m_projectRevision;
    });
    connect(this, &Backend::saveProjectRequested, m_worker,
            [worker=m_worker](const QString &path, const QVariantMap &state) {
                worker->saveProject(path, ProjectSafety::encodeLibraryState(state));
            });
    connect(this, &Backend::loadProjectRequested, m_worker, &PlayerWorker::loadProject);
    connect(this, &Backend::newProjectRequested, m_worker, &PlayerWorker::newTimeline);
    connect(this, &Backend::projectCreateRequested, m_worker, &PlayerWorker::createProject);
    connect(m_worker, &PlayerWorker::projectCreateFinished, this, [this](int request,bool created,const QString &error) {
        if (request==m_configuredProjectRequest) {
            m_configuredProjectRequest=0;
            m_projectLoadPending=false; emit projectBusyChanged();
        }
        if (created) { setDirty(true); setStatus(tr("Created %1").arg(projectName())); }
        else setStatus(error);
        emit projectCreateFinished(request,created,error);
    });
    connect(m_worker, &PlayerWorker::timelineCleared, this, [this] {
        if (!m_configuredProjectRequest) { m_projectLoadPending = false; emit projectBusyChanged(); }
        discardRecovery();
        // Arrives after the empty-timeline announce (which dirtied us).
        ++m_mediaGeneration; m_mediaTasks->cancelAll();
        m_timelineImportDocument.clear(); m_timelineImportTask.clear();
        setMediaGang(false); m_pendingMediaPreview.clear(); m_mediaPreviews.clear(); emit mediaPreviewsChanged();
        m_mediaCatalog->restore({});
        m_library.clear();
        emit libraryChanged();
        m_markers.clear();
        emit markersChanged();
        m_pendingRanges.clear();
        m_appendAfterImport.clear();
        resetProjectPlayback();
        if (!m_projectPath.isEmpty()) {
            m_projectPath.clear();
            emit projectChanged();
        }
        setDirty(false);
        setStatus(tr("New project"));
    });
    connect(m_worker, &PlayerWorker::projectSaved, this,
            [this](bool ok, const QString &path, const QString &error) {
        m_savePending = false; emit projectBusyChanged();
        const QString name = QFileInfo(path).fileName();
        if (ok) {
            if (m_projectPath != path) {
                m_projectPath = path;
                emit projectChanged();
            }
            if (m_projectRevision == m_saveRevision) { setDirty(false); discardRecovery(); }
            addRecent(path);
            setStatus(tr("Saved %1").arg(name));
        } else {
            setStatus(tr("Could not save %1. %2").arg(name,error));
            emit failed(m_status);
        }
        refreshProjectSafety();
        emit projectSaveFinished(ok);
    });
    connect(m_worker, &PlayerWorker::projectLoaded, this, &Backend::onProjectLoaded);
}

// Clips arriving, filmstrips, waveforms and the frames to show.
void Backend::wirePictures() {
    connect(m_worker, &PlayerWorker::clipImported, this, &Backend::onImported);
    connect(m_worker, &PlayerWorker::timelineChanged, this, &Backend::onTimeline);
    connect(this, &Backend::decorationWindowRequested, m_worker, &PlayerWorker::setDecorationWindow);
    connect(m_worker, &PlayerWorker::historyChanged, this, [this](const QString &undo, const QString &redo) {
        if (m_undoText == undo && m_redoText == redo) return;
        m_undoText = undo; m_redoText = redo; emit historyChanged();
    });
    connect(m_worker, &PlayerWorker::filmstripReady, this, &Backend::onFilmstrip);
    connect(m_worker, &PlayerWorker::stripSourceReady, this,
            [this](const QString &key, const QString &path, int frame, int width,
                   const QVariantMap &inputTransform, const QVariantMap &workingManagement) {
        if (m_thumbs) m_thumbs->registerSource(key, path, frame, width, inputTransform, workingManagement);
    });
    connect(m_worker, &PlayerWorker::waveformReady, this, &Backend::onWaveform);
    connect(m_worker, &PlayerWorker::connectedWaveformReady, this,
            &Backend::onConnectedWaveform);
    connect(m_worker, &PlayerWorker::connectedFilmstripReady, this,
            &Backend::onConnectedFilmstrip);
    connect(m_worker, &PlayerWorker::sourceStripReady, this, &Backend::onSourceStrip);
    connect(m_worker, &PlayerWorker::libraryStripReady, this, &Backend::onLibraryStrip);
    connect(m_worker, &PlayerWorker::libraryWaveformReady, this, &Backend::onLibraryWave);
    connect(m_worker, &PlayerWorker::frameReady, this, &Backend::onFrame);
    connect(this, &Backend::skimRequested, m_worker, &PlayerWorker::skimTo);
    connect(m_worker, &PlayerWorker::skimFrameReady, this, &Backend::onSkimFrame);
    connect(m_worker, &PlayerWorker::skimFrameSettled, this, &Backend::onSkimSettled);
    connect(m_worker, &PlayerWorker::frameScheduled, this, &Backend::onFrameScheduled);
    connect(m_worker, &PlayerWorker::scheduleCleared, this, &Backend::onScheduleCleared);
    connect(m_worker, &PlayerWorker::framesSkipped, this, [this](int count) {
        if (playing()) m_droppedFrames += count;
    });
    m_scheduledTimer.setSingleShot(true);
    m_scheduledTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scheduledTimer, &QTimer::timeout, this, &Backend::presentScheduled);
    connect(m_worker, &PlayerWorker::previewOpened, this, &Backend::onPreviewOpened);
    connect(m_worker, &PlayerWorker::previewFrameReady, this, [this](const QImage &image, int position) { onPreviewFrame(image, position); });
    connect(m_worker, &PlayerWorker::playbackSpeedChanged, this, [this](double speed, quint64 request) {
        // An edit can stop the worker without a transport command. Reflect
        // that pause, but never let an older reply undo a newer Play/Shuttle.
        if (request != m_speedRequest || m_speed == speed) return;
        m_speed = speed;
        if (speed == 0.0) {
            m_scheduledFrames.clear(); m_scheduledTimer.stop();
            if (!m_viewingSource) { m_audioPosition.clear(); emit audioPositionChanged(); }
        }
        emit playingChanged();
    });
}

// The worker hands the library lines already decoded, with each clip's
// file checked there, so nothing here parses JSON or touches the disk.
void Backend::onProjectLoaded(bool ok, const QString &path, const QStringList &, const QVariantList &records) {
    m_projectLoadPending = false; emit projectBusyChanged();
    if (ok && path == m_replacementRecentPath) removeRecentProject(m_locateRecentPath);
    m_locateRecentPath.clear(); m_replacementRecentPath.clear();
    const QString name = QFileInfo(path).fileName();
    if (!ok) {
        m_recoveryLoadPath.clear();
        setStatus(tr("Could not open %1.").arg(name));
        emit failed(m_status);
        return;
    }
    resetProjectPlayback();
    // Rebuild the library by importing every remembered clip — that
    // regenerates thumbnails and metadata the project file doesn't hold.
    // These imports are part of the load, so they must not dirty it.
    m_restoringLibrary = true;
    ++m_mediaGeneration; m_mediaTasks->cancelAll();
    m_timelineImportDocument.clear(); m_timelineImportTask.clear();
    setMediaGang(false); m_pendingMediaPreview.clear(); m_mediaPreviews.clear(); emit mediaPreviewsChanged();
    m_mediaCatalog->restore({});
    m_library.clear();
    emit libraryChanged();
    m_restoringLibrary = true;
    m_pendingRanges.clear();
    m_pendingMeta.clear();
    m_pendingSubclips.clear();
    QSet<QString> seen;
    QHash<QString, QVariantMap> offline; // path → the offline entry made for it
    m_restoreOrder.clear();
    m_restorePending.clear();
    for (const QVariant &value : records) {
        const QVariantMap record = value.toMap();
        if (record.value(QStringLiteral("kind")) == QStringLiteral("clip"))
            m_restoreOrder.append(record.value(QStringLiteral("path")).toString());
    }
    // Entries placed here (offline media, subclips, multicams) are published
    // once after the loop: every libraryChanged subscriber walks the whole
    // library, so a per-entry emit made a large offline project quadratic.
    bool libraryGrew = false;
    for (const QVariant &value : records) {
        const QVariantMap record = value.toMap();
        const QString kind = record.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("media")) {
            m_mediaCatalog->restore(record.value(QStringLiteral("document")).toMap());
            continue;
        }
        if (kind != QStringLiteral("clip")) continue;
        const QString clipPath = record.value(QStringLiteral("path")).toString();
        const int rangeIn = record.value(QStringLiteral("rangeIn"), -1).toInt();
        const int rangeOut = record.value(QStringLiteral("rangeOut"), -1).toInt();
        QVariantMap logging = record.value(QStringLiteral("logging")).toMap();
        if (logging.value(QStringLiteral("libraryId")).toString().isEmpty())
            logging.insert(QStringLiteral("libraryId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (clipPath.startsWith(QStringLiteral("multicam:"))) {
            QVariantMap entry = logging;
            entry.insert(QStringLiteral("path"), clipPath);
            entry.insert(QStringLiteral("multicam"), true);
            entry.insert(QStringLiteral("hasVideo"), true);
            entry.insert(QStringLiteral("hasAudio"), true);
            if (rangeIn >= 0 || rangeOut >= 0) {
                entry.insert(QStringLiteral("rangeIn"), rangeIn);
                entry.insert(QStringLiteral("rangeOut"), rangeOut);
            }
            m_library.append(entry);
            libraryGrew = true;
            continue;
        }
        if (offline.contains(clipPath)) {
            // A subclip of offline media: an offline copy right away.
            QVariantMap sub = offline.value(clipPath);
            for (auto it = logging.constBegin(); it != logging.constEnd(); ++it)
                sub.insert(it.key(), it.value());
            if (!sub.value(QStringLiteral("duplicateRef")).toBool()) sub.insert(QStringLiteral("subclip"), true);
            sub.remove(QStringLiteral("rangeIn"));
            sub.remove(QStringLiteral("rangeOut"));
            if (rangeIn >= 0 || rangeOut >= 0) {
                sub.insert(QStringLiteral("rangeIn"), rangeIn);
                sub.insert(QStringLiteral("rangeOut"), rangeOut);
            }
            m_library.append(sub);
            libraryGrew = true;
            continue;
        }
        if (seen.contains(clipPath)) {
            // A second line for a path is a subclip: copied from its
            // source entry once that import lands.
            QVariantMap sub = logging;
            sub.insert(QStringLiteral("rangeIn"), rangeIn);
            sub.insert(QStringLiteral("rangeOut"), rangeOut);
            m_pendingSubclips.insert(clipPath, sub);
            continue;
        }
        seen.insert(clipPath);
        if (!record.value(QStringLiteral("exists")).toBool()) {
            // Offline media keeps its place (and its logging) so the
            // project still lists what it is missing, rather than
            // silently dropping the clip.
            QVariantMap entry = logging;
            entry.insert(QStringLiteral("name"),
                         logging.value(QStringLiteral("name"), QFileInfo(clipPath).fileName()));
            entry.insert(QStringLiteral("path"), clipPath);
            entry.insert(QStringLiteral("offline"), true);
            if (!entry.contains(QStringLiteral("duration"))) entry.insert(QStringLiteral("duration"), 0);
            if (!entry.contains(QStringLiteral("fps"))) entry.insert(QStringLiteral("fps"), 0);
            if (!entry.contains(QStringLiteral("width"))) entry.insert(QStringLiteral("width"), 0);
            if (!entry.contains(QStringLiteral("height"))) entry.insert(QStringLiteral("height"), 0);
            if (rangeIn >= 0 || rangeOut >= 0) {
                entry.insert(QStringLiteral("rangeIn"), rangeIn);
                entry.insert(QStringLiteral("rangeOut"), rangeOut);
            }
            m_library.append(entry);
            offline.insert(clipPath, entry);
            libraryGrew = true;
            continue;
        }
        if (rangeIn >= 0 || rangeOut >= 0)
            m_pendingRanges.insert(clipPath, qMakePair(rangeIn, rangeOut));
        if (!logging.isEmpty())
            m_pendingMeta.insert(clipPath, logging);
        m_restorePending.insert(clipPath);
        open(QUrl::fromLocalFile(clipPath));
    }
    if (libraryGrew) emit libraryChanged();
    // The project's timelines and markers are the worker's: it put them in
    // place with the clips, before this (PlayerWorker::loadProject).
    m_restoringLibrary = false;
    syncMediaSources();
    const bool recovering = m_recoveryLoadPath == path;
    discardRecovery();
    m_recoveryLoadPath.clear();
    const QString openedPath = recovering ? QString() : path;
    if (m_projectPath != openedPath) {
        m_projectPath = openedPath;
        emit projectChanged();
    }
    setDirty(recovering);
    if (!recovering) addRecent(path);
    setStatus(recovering ? tr("Recovered project copy. Use Save As to keep it.") : tr("Opened %1").arg(name));
    refreshProjectSafety();
}

void Backend::addRecent(const QString &path) {
    m_recent.removeAll(path);
    m_recent.prepend(path);
    while (m_recent.size() > 8)
        m_recent.removeLast();
    QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"))
        .setValue(QStringLiteral("recentProjects"), m_recent);
    emit recentChanged();
}

void Backend::openRecent(const QString &path) {
    if (!QFileInfo::exists(path)) { emit recentProjectMissing(path); return; }
    loadProject(QUrl::fromLocalFile(path));
}

QVariantMap Backend::newProjectDefaults() const {
    auto settings=Timeline::defaultProjectSettings();
    settings["title"]=tr("Untitled"); settings["colorspace"]=m_defaultColorspace;
    return settings;
}
QString Backend::projectCreationProblem(const QVariantMap &input, const QVariantMap &settings) const {
    const auto context=plainVariant(input).toMap();
    if (projectBusy() || m_exporting) return tr("Wait for the current project operation before creating a project.");
    bool found=false;
    for (const auto &v : m_sequences) found|=!context.value("anchorId").toString().isEmpty() && v.toMap().value("id")==context.value("anchorId");
    if (context.isEmpty() || context.value("generation").toInt()!=m_mediaGeneration || !found)
        return tr("The project changed. Close this dialog and reopen New Project.");
    return CreationNames::projectProblem(plainVariant(settings).toMap());
}
int Backend::createProject(const QVariantMap &input, const QVariantMap &inputSettings) {
    const int request=++m_projectCreateRequest;
    const auto context=plainVariant(input).toMap(), settings=plainVariant(inputSettings).toMap();
    const auto error=projectCreationProblem(context,settings);
    if (!error.isEmpty()) {
        QTimer::singleShot(0,this,[this,request,error]{setStatus(error);emit projectCreateFinished(request,false,error);});
    } else {
        pause(); m_configuredProjectRequest=request;
        m_projectLoadPending=true; emit projectBusyChanged();
        emit projectCreateRequested(request,context.value("anchorId").toString(),settings);
    }
    return request;
}

void Backend::newProject() {
    if (projectBusy()) { setStatus("Wait for the current project operation to finish."); return; }
    if (m_exporting)
        return;
    pause();
    m_projectLoadPending = true; emit projectBusyChanged();
    emit newProjectRequested();
}

Backend::~Backend() {
    m_worker->cancelFrame();
    // A tool's process still running is ended here, unheard: left to its own
    // destructor it would report its end to this object after the members
    // declared below it are gone.
    for (auto *tool : {&m_detectProc, &m_matchProc, &m_reframeProc}) {
        if (!*tool) continue;
        (*tool)->disconnect(this);
        (*tool)->kill();
        (*tool)->waitForFinished(2000);
        tool->reset();
    }
    m_reframeQueue.clear();
    // An AAF helper still running is told to stop and waited for: its result
    // is posted to this object.
    if (m_aafCancel) m_aafCancel->store(true);
    if (m_interchangePool) m_interchangePool->waitForDone();
    // QProcess destruction can emit finished while waiting for its child.
    // No transcription callback may enter a partly destroyed Backend.
    m_transcriber->abandon();
    delete m_scopeProcessor; m_scopeProcessor = nullptr;
    delete m_viewerProcessor; m_viewerProcessor = nullptr;
    delete m_projectArchive; m_projectArchive = nullptr;
    delete m_mediaTasks; m_mediaTasks = nullptr;
    m_thread.quit();
    m_thread.wait();
    for (const auto &path : m_discardedRecoveryPaths) { QFile::remove(path); QFile::remove(path+".discard"); }
}

QString Backend::displayNameFor(const QUrl &url) {
    if (url.isEmpty())
        return QStringLiteral("Untitled");
    if (url.isLocalFile()) {
        const QString name = QFileInfo(url.toLocalFile()).fileName();
        if (!name.isEmpty())
            return name;
    }
    const QString name = url.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled") : name;
}

QString Backend::timecodeFor(int frame, double fps) {
    if (fps <= 0.0 || frame < 0)
        return QStringLiteral("00:00:00:00");
    const int fi = qMax(1, qRound(fps));
    const int totalSeconds = frame / fi;
    // Timecode has no twenty-fourth hour: it starts again after a day.
    return QStringLiteral("%1:%2:%3:%4")
        .arg((totalSeconds / 3600) % 24, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frame % fi, 2, 10, QLatin1Char('0'));
}

// The JKL shuttle ladder: first press plays at 1× in that direction, each
// further press doubles, capped at 8×. Pressing against the current direction
// resets to 1× the other way.
double Backend::nextShuttleSpeed(double current, int direction) {
    if (direction == 0)
        return 0.0;
    const double sign = direction > 0 ? 1.0 : -1.0;
    if (current * sign >= 1.0)
        return sign * qMin(8.0, current * sign * 2.0);
    return sign;
}

void Backend::importDialog() {
    m_chooserMode = ChooserMode::Import;
    m_chooser->openFiles(QStringLiteral("Import Media"));
}

void Backend::exportDialog() {
    exportWithPreset(m_exportPreset);
}

void Backend::exportTo(const QUrl &url, const QString &preset) {
    if (!hasVideo() || m_exporting)
        return;
    pause();
    m_exportPreset = preset;
    QVariantMap spec = ExportSpec::fromLegacyPreset(preset);
    QString path = url.toLocalFile();
    const QString ext = ExportSpec::extension(spec);
    if (!path.endsWith(ext, Qt::CaseInsensitive))
        path += ext;
    // File ▸ Export is a one-job queue: the Deliver page shows it too.
    // The chooser confirms only its selected path, not an appended extension.
    if (path == url.toLocalFile() && QFileInfo::exists(path))
        spec["overwritePaths"] = QStringList{ExportSpec::outputKey(path)};
    const int id = enqueueRender(QFileInfo(path).completeBaseName(), path, spec,
                  preset == QStringLiteral("prores") ? QStringLiteral("ProRes 422 HQ")
                  : preset == QStringLiteral("audio") ? QStringLiteral("AAC audio")
                  : preset == QStringLiteral("h264-medium") ? QStringLiteral("H.264 medium")
                                                            : QStringLiteral("H.264 high"));
    if (id > 0) m_renderQueue->renderAll();
}

void Backend::exportWithPreset(const QString &preset) {
    if (!hasVideo() || m_exporting)
        return;
    pause();
    m_exportPreset = preset;
    m_chooserMode = ChooserMode::Export;
    if (preset == QStringLiteral("prores"))
        m_chooser->saveFile(QStringLiteral("Export Movie (ProRes 422)"),
                            QStringLiteral("Untitled.mov"),
                            QStringLiteral("QuickTime movie"),
                            {QStringLiteral("*.mov")});
    else if (preset == QStringLiteral("audio"))
        m_chooser->saveFile(QStringLiteral("Export Audio"),
                            QStringLiteral("Untitled.m4a"),
                            QStringLiteral("AAC audio"),
                            {QStringLiteral("*.m4a")});
    else
        m_chooser->saveFile(QStringLiteral("Export Movie"),
                            QStringLiteral("Untitled.mp4"),
                            QStringLiteral("MP4 video"), {QStringLiteral("*.mp4")});
}

QString Backend::projectName() const {
    return m_projectPath.isEmpty() ? (m_projectSettings.value("title").toString().isEmpty() ? QStringLiteral("Untitled") : m_projectSettings.value("title").toString())
                                   : QFileInfo(m_projectPath).completeBaseName();
}

void Backend::undo() {
    if (m_exporting || m_clipAttributesPending)
        return;
    // Say what is being taken back: the timeline changes, but a person who
    // pressed Ctrl+Z three times has no other way to know which three.
    const QString what = m_undoText;
    emit undoRequested();
    if (!what.isEmpty())
        setStatus(tr("Undid %1").arg(what));
}

void Backend::redo() {
    if (m_exporting || m_clipAttributesPending)
        return;
    const QString what = m_redoText;
    emit redoRequested();
    if (!what.isEmpty())
        setStatus(tr("Redid %1").arg(what));
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

// Why an edit did not happen. A finished task's notice gives way to it: left
// in the info area, "… — Completed" hid the reason until it was dismissed
// (every import leaves one). A warning or an error holds its place.
void Backend::sayRefused(const QString &why) {
    if (m_backgroundTasks && m_backgroundTasks->currentNotice().value(QStringLiteral("severity")).toString() == QLatin1String("success"))
        m_backgroundTasks->dismissNotice();
    setStatus(why);
}

void Backend::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backend::setDirty(bool dirty, bool workerUpdate) {
    if (dirty) {
        ++m_projectRevision;
        // Worker updates arrive in order: those before projectSaved are
        // already in its snapshot; later updates dirty the project again.
        // GUI library edits after the save request must remain unsaved.
        if (workerUpdate && m_savePending) ++m_saveRevision;
    }
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit projectChanged();
}

bool Backend::hasPendingWork() const {
    for (const auto &v : m_renderQueue->jobs()) {
        const auto job = v.toMap();
        if (job.value("status") == "queued" &&
            !job.value("spec").toMap().value("snapshotReady", true).toBool())
            return true;
    }
    // A load is not settled until the worker has named the opened project's
    // timelines (it always answers: see PlayerWorker::loadProject).
    return m_sequencesEchoPending || projectBusy() || busy() || m_mediaTasks->running() ||
           m_soundLibrary->scanning() || m_projectArchive->busy() ||
           (m_renderQueue->active() &&
            m_renderQueue->job(m_renderQueue->activeJob()).value("status") != "paused") ||
           !renderJob().isEmpty() || !stabJob().isEmpty() || !trackJob().isEmpty() ||
           (m_interchangePool && m_interchangePool->activeThreadCount() > 0) ||
           !m_aafTaskId.isEmpty() || m_colorMatteBusy || m_lutPreviewBusy ||
           m_loudnessPass->running() ||
           m_transcriber->running() ||
           !m_maskJob.isEmpty() || m_hardware->busy() ||
           // Tools that measure with a process or a pool job and edit when
           // it ends.
           detectingEdits() || m_matchProc || reframing() || m_photoshopSplitting;
}
