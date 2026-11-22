#include "backend_p.h"

QString Backend::saveStill() {
    // (Whole: a picture drawn for the viewer alone stands for this one.)
    const QImage frame = wholeProgramFrame();
    if (frame.isNull()) {
        setStatus(tr("No frame to save."));
        return QString();
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    dir += QStringLiteral("/OmaEdit Stills");
    QDir().mkpath(dir);
    QString name = projectName().isEmpty() ? QStringLiteral("Still") : projectName();
    name.replace(QRegularExpression(QStringLiteral("[\\/:*?\"<>|]")), QStringLiteral("_"));
    QString tc = timecode(m_position);
    tc.replace(QLatin1Char(':'), QLatin1Char('.'));
    const QString suffix=WorkingFrame::isTagged(frame)?QStringLiteral(".exr"):QStringLiteral(".png");
    QString path = dir + QLatin1Char('/') + name + QLatin1Char('_') + tc + suffix;
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir + QLatin1Char('/') + name + QLatin1Char('_') + tc + QStringLiteral("_%1").arg(n) + suffix;
    if (!(WorkingFrame::isTagged(frame) ? WorkingFrame::fromTagged(frame).save(path) : frame.save(path))) {
        setStatus(tr("Could not write %1.").arg(path));
        return QString();
    }
    setStatus(tr("Saved still %1").arg(QFileInfo(path).fileName()));
    return path;
}

// ── Color page: grades, scopes, stills, LUTs ─────────────────────────────

void Backend::setupColor() {
    m_stills = new StillsGallery(this);
    m_scopeOptions = QVariantMap{{QStringLiteral("intensity"), 1.0}, {QStringLiteral("channels"), QStringLiteral("rgb")},
                                 {QStringLiteral("clamp"), false}, {QStringLiteral("graticule"), true}};
    // queueGrade only posts an event: post from the sending thread so these
    // edits retain their order relative to ordinary queued worker commands.
    connect(this, &Backend::setGradeRequested, this, [this](int track, int index, const QVariantMap &grade) {
        m_worker->queueGrade(track, index, grade, m_editGesture);
    });
    connect(this, &Backend::setGradeVersionsRequested, m_worker, &PlayerWorker::setGradeVersions);
    connect(this, &Backend::applyColourSourceRequested, m_worker, &PlayerWorker::applyColourSource);
    connect(this, &Backend::editGradeVersionRequested, m_worker, &PlayerWorker::editGradeVersion);
    connect(m_worker, &PlayerWorker::gradeVersionFinished, this, &Backend::setStatus);
    connect(this, &Backend::pasteColorRequested, m_worker, &PlayerWorker::pasteColor);
    connect(this, &Backend::autoBalanceRequested, m_worker, &PlayerWorker::autoBalance);
    connect(this, &Backend::matchColorRequested, m_worker, &PlayerWorker::matchColor);
    connect(m_worker, &PlayerWorker::matchColorFinished, this, [this](int, int, int node, const QString &message) {
        setStatus(message);
        emit matchFinished(node >= 0, message);
    });
    connect(m_worker, &PlayerWorker::autoBalanceFinished, this,
            [this](int track, int index, int node, const QString &message) {
        setStatus(message);
        emit autoBalanceFinished(track, index, node);
    });
    connect(m_worker, &PlayerWorker::colorPasteFinished, this,
            [this](int track, int index, int node, const QString &message) {
        setStatus(message);
        if (node >= 0) emit colorPasted(track, index, node);
    });
    connect(this, &Backend::tracksChanged, this, &Backend::refreshGradeClip);
    connect(this, &Backend::trackWindowRequested, m_worker, &PlayerWorker::trackWindow);
    connect(this, &Backend::trackWindowRangeRequested, m_worker, &PlayerWorker::trackWindowRange);
    connect(this, &Backend::stopWindowTrackingRequested, m_worker, &PlayerWorker::stopWindowTracking);
    connect(m_worker, &PlayerWorker::trackProgress, this, [this](int track, int index, int node, int percent) {
        m_trackJob = percent >= 0 && percent < 100
                         ? QVariantMap{{QStringLiteral("track"), track}, {QStringLiteral("index"), index}, {QStringLiteral("node"), node}, {QStringLiteral("percent"), percent}}
                         : QVariantMap();
        emit trackJobChanged();
        if (percent == 100)
            setStatus(tr("Window tracked"));
        else if (percent == -1)
            setStatus(tr("Could not track the selected window."));
    });
    connect(this, &Backend::positionChanged, this, &Backend::refreshGradeClip);
    connect(this, &Backend::timelineEntriesChanged, this, [this] { refreshColorClips(); refreshGradeClip(); });
    connect(this, &Backend::decorChanged, this, &Backend::refreshColorClips);
    connect(this, &Backend::frameChanged, this, [this] {
        if(m_frame.isNull())cancelScopes(true);
        else if(m_scopesActive)renderScopes();
    });
    connect(this, &Backend::projectPlaybackReset, this, [this] { cancelScopes(true);renderScopes(); });
    connect(this, &Backend::projectBusyChanged, this, [this] {
        if(m_projectLoadPending)cancelScopes(true);
        else renderScopes();
    });
    connect(this, &Backend::projectSettingsChanged, this, [this] { sendPlaybackDisplay();cancelScopes();renderScopes(); });
    connect(this, &Backend::playingChanged, this, [this] { if(!playing())renderScopes(); });
    connect(this, &Backend::seekRequested, this, [this] { if(!m_viewingSource)cancelScopes(); });
    connect(this, &Backend::switchSequenceRequested, this, [this] { cancelScopes(true); });
    connect(this, &Backend::projectChanged, this, [this] {
        m_stills->setDirectory(StoragePreferences::galleryRoot(m_projectPath));
    });
    connect(m_stills, &StillsGallery::imagesReady, this, &Backend::refreshStillThumbs);
    connect(m_stills, &StillsGallery::failed, this, [this](const QString &message) {
        setStatus(message);
        emit failed(message);
    });
    refreshStillThumbs();
    setupLuts();
    setupColorTools();
}

QVariantMap Backend::videoClipAt(int frame) const {
    QVariantMap best;
    int bestTrack = -1;
    auto consider = [&](const QVariantMap &e) {
        if (e.value(ClipKey::gap).toBool() || e.value(ClipKey::audioOnly).toBool())
            return;
        if (!e.value(ClipKey::title).toString().isEmpty())
            return;
        const int start = e.value(ClipKey::start).toInt();
        const int frames = e.value(ClipKey::frames).toInt();
        if (frame < start || frame >= start + frames)
            return;
        const int track = e.value(ClipKey::track).toInt();
        if (track > bestTrack) { bestTrack = track; best = e; }
    };
    for (const QVariant &v : m_timeline) consider(v.toMap());
    for (const QVariant &v : m_connected) consider(v.toMap());
    return best;
}

void Backend::refreshGradeClip() {
    QVariantMap e = videoClipAt(m_position);
    QVariantMap out;
    if (!e.isEmpty()) {
        out.insert(QStringLiteral("track"), e.value(ClipKey::track));
        out.insert(QStringLiteral("index"), e.value(ClipKey::index));
        out.insert(QStringLiteral("name"), e.value(ClipKey::name));
        out.insert(QStringLiteral("start"), e.value(ClipKey::start));
        out.insert(QStringLiteral("frames"), e.value(ClipKey::frames));
        out.insert(QStringLiteral("path"), e.value(ClipKey::path));
        // An ungraded clip presents the default grade (one node at rest) so
        // the page always has something to edit; a stable node id keeps the
        // map equal across refreshes.
        QVariantMap grade = e.value(ClipKey::grade).toMap();
        if (grade.value(QStringLiteral("nodes")).toList().isEmpty()) {
            grade = Timeline::defaultGrade();
            QVariantList nodes = grade.value(QStringLiteral("nodes")).toList();
            QVariantMap n = nodes.first().toMap();
            n.insert(QStringLiteral("id"), QStringLiteral("n0"));
            nodes[0] = n;
            grade.insert(QStringLiteral("nodes"), nodes);
        }
        out.insert(QStringLiteral("grade"), grade);
        out.insert(QStringLiteral("gradeVersions"), e.value(ClipKey::gradeVersions).toList());
        out.insert(QStringLiteral("gradeVersion"), e.value(ClipKey::gradeVersion).toInt());
        out.insert(QStringLiteral("locked"), m_tracks.value(e.value(ClipKey::track).toInt()).toMap()
                                                 .value(QStringLiteral("locked")).toBool());
        out.insert(QStringLiteral("bypass"), e.value(ClipKey::bypass).toStringList().contains(QStringLiteral("grade")));
        out.insert(QStringLiteral("label"), e.value(ClipKey::label));
    }
    if (out == m_gradeClip)
        return;
    m_gradeClip = out;
    emit gradeClipChanged();
}

void Backend::refreshColorClips() {
    QVariantList out;
    auto add = [&](const QVariantMap &e, const QVariantMap &decor) {
        if (e.value(ClipKey::gap).toBool() || e.value(ClipKey::audioOnly).toBool()
            || !e.value(ClipKey::title).toString().isEmpty())
            return;
        QVariantMap c;
        c.insert(QStringLiteral("track"), e.value(ClipKey::track));
        c.insert(QStringLiteral("index"), e.value(ClipKey::index));
        c.insert(QStringLiteral("name"), e.value(ClipKey::name));
        c.insert(QStringLiteral("start"), e.value(ClipKey::start));
        c.insert(QStringLiteral("frames"), e.value(ClipKey::frames));
        c.insert(QStringLiteral("label"), e.value(ClipKey::label));
        c.insert(QStringLiteral("markers"), e.value(ClipKey::markers).toList().size());
        c.insert(QStringLiteral("graded"), !Timeline::gradeIsIdentity(e.value(ClipKey::grade).toMap()));
        c.insert(QStringLiteral("versions"), e.value(ClipKey::gradeVersions).toList().size());
        const QVariantList strip = decor.value(QStringLiteral("strip")).toList();
        c.insert(QStringLiteral("thumb"), strip.isEmpty() ? QString() : strip.first().toString());
        out.append(c);
    };
    for (int i = 0; i < m_timeline.size(); ++i)
        add(m_timeline.at(i).toMap(), m_timelineDecor.value(i).toMap());
    for (int i = 0; i < m_connected.size(); ++i)
        add(m_connected.at(i).toMap(), m_connectedDecor.value(i).toMap());
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const int sa = a.toMap().value(QStringLiteral("start")).toInt(), sb = b.toMap().value(QStringLiteral("start")).toInt();
        return sa != sb ? sa < sb : a.toMap().value(QStringLiteral("track")).toInt() < b.toMap().value(QStringLiteral("track")).toInt();
    });
    if (out == m_colorClips)
        return;
    m_colorClips = out;
    emit colorClipsChanged();
}

// Coalescing belongs in the worker's ordered command queue: one GUI-side
// pending slot loses apply-to-all edits and can outlive a save or clip move.
void Backend::setGrade(int track, int index, const QVariantMap &grade) {
    if (m_exporting || track < 0 || index < 0)
        return;
    emit setGradeRequested(track, index, plainVariant(grade).toMap());
}

void Backend::applyColourSource(int track, int index, const QString &scope, const QVariantMap &fields) {
    if (m_exporting || track < 0 || index < 0)
        return;
    emit applyColourSourceRequested(track, index, scope, plainVariant(fields).toMap());
}

void Backend::setGradeVersions(int track, int index, const QVariantList &versions, int active) {
    if (m_exporting || track < 0 || index < 0)
        return;
    emit setGradeVersionsRequested(track, index, plainVariant(versions).toList(), active);
}

void Backend::editGradeVersion(int track, int index, const QString &action,
                               int version, const QString &name) {
    if (m_exporting) {
        setStatus(tr("Cannot change grade versions while exporting."));
        return;
    }
    emit editGradeVersionRequested(track, index, action, version, name);
}

bool Backend::copyColor(const QString &kind, const QVariantMap &grade, int node) {
    const QVariantMap copy = plainVariant(grade).toMap();
    const QVariantList nodes = copy.value(QStringLiteral("nodes")).toList();
    if (nodes.isEmpty() || (kind != QStringLiteral("node") && kind != QStringLiteral("grade"))) {
        setStatus(tr("Select a correction to copy."));
        return false;
    }
    if (kind == QStringLiteral("node")) {
        if (node < 0 || node >= nodes.size() || nodes.at(node).toMap().isEmpty()) {
            setStatus(tr("Select a correction layer to copy."));
            return false;
        }
        m_copiedNode = nodes.at(node).toMap();
        m_copiedNode.remove(QStringLiteral("frames"));
    } else {
        m_copiedGrade = copy;
    }
    emit colorClipboardChanged();
    setStatus(kind == QStringLiteral("node") ? tr("Correction layer copied") : tr("Complete grade copied"));
    return true;
}

void Backend::pasteColor(int track, int index, const QString &kind, int afterNode, const QString &parentPath) {
    if (m_exporting) {
        setStatus(tr("Cannot paste a correction while exporting."));
        return;
    }
    const QVariantMap payload = kind == QStringLiteral("node") ? m_copiedNode
        : kind == QStringLiteral("grade") ? m_copiedGrade : QVariantMap();
    if (payload.isEmpty()) {
        setStatus(kind == QStringLiteral("node") ? tr("Copy a correction layer first.")
                                                  : tr("Copy a grade first."));
        return;
    }
    emit pasteColorRequested(track, index, kind, payload, afterNode, parentPath);
}

void Backend::trackWindow(int track, int index, int node, double x, double y, double w, double h) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Tracking window…"));
    emit trackWindowRequested(track, index, node, x, y, w, h);
}

bool Backend::reframeToFit(const QVariantList &clips) {
    if (m_exporting || clips.isEmpty()) return false;
    if (reframing()) { setStatus(tr("Already reframing; wait for it or stop it")); return false; }
    m_reframeQueue = plainVariant(clips).toList();
    emit reframingChanged();
    reframeNext();
    return true;
}

void Backend::cancelReframeToFit() {
    m_reframeQueue.clear();
    if (m_reframeProc) {
        m_reframeProc->disconnect(this);
        m_reframeProc->kill();
        m_reframeProc.release()->deleteLater();
    }
    emit reframingChanged();
    setStatus(tr("Reframe to Fit stopped"));
}

void Backend::reframeNext() {
    if (m_reframeQueue.isEmpty()) { emit reframingChanged(); return; }
    const QVariantMap target = m_reframeQueue.takeFirst().toMap();
    const int track = target.value(QStringLiteral("track")).toInt(), index = target.value(QStringLiteral("index")).toInt();
    const QVariantMap entry = entryOn(track, index);
    const QString name = entry.value(ClipKey::name).toString();
    const auto skip = [this, track, index](const QString &why) {
        setStatus(why);
        emit reframeToFitFinished(track, index, false, why);
        QTimer::singleShot(0, this, &Backend::reframeNext);
    };
    if (entry.isEmpty() || entry.value(ClipKey::gap).toBool() || entry.value(ClipKey::audioOnly).toBool()
        || !entry.value(ClipKey::title).toString().isEmpty())
        return skip(tr("Reframe to Fit needs a clip with pictures"));
    if (!qFuzzyCompare(entry.value(ClipKey::speed, 1.).toDouble(), 1.)
        || !entry.value(ClipKey::retime).toMap().value(QStringLiteral("segments")).toList().isEmpty())
        return skip(tr("%1 is retimed; Reframe to Fit works on clips at normal speed").arg(name));
    const QString path = entry.value(ClipKey::path).toString();
    QVariantMap source;
    for (const QVariant &v : std::as_const(m_library))
        if (v.toMap().value(QStringLiteral("path")).toString() == path) { source = v.toMap(); break; }
    // The pixel shape is kept as a ratio ("1:1", "4:3") or a number.
    double pixelAspect = 1;
    const QString shape = source.value(QStringLiteral("pixelAspect")).toString();
    if (shape.contains(QLatin1Char(':'))) {
        const double num = shape.section(QLatin1Char(':'), 0, 0).toDouble(), den = shape.section(QLatin1Char(':'), 1, 1).toDouble();
        if (num > 0 && den > 0) pixelAspect = num / den;
    } else if (shape.toDouble() > 0) {
        pixelAspect = shape.toDouble();
    }
    const double sourceAspect = source.value(QStringLiteral("height")).toDouble() > 0
        ? source.value(QStringLiteral("width")).toDouble() * pixelAspect / source.value(QStringLiteral("height")).toDouble() : 0.;
    const double frameAspect = videoDisplayAspect();
    if (!QFileInfo::exists(path)) return skip(tr("%1's media is not here").arg(name));
    if (!(sourceAspect > 0)) return skip(tr("%1's picture size is not known yet; try again in a moment").arg(name));
    if (qAbs(sourceAspect / frameAspect - 1) < 0.02) return skip(tr("%1 already has the timeline's shape").arg(name));
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return skip(tr("Reframe to Fit needs ffmpeg installed"));
    const double fps = m_fps > 0 ? m_fps : 25.;
    const int in = entry.value(ClipKey::in).toInt(), out = entry.value(ClipKey::out).toInt();
    // Two small grey pictures a second, in the picture's shape, are plenty to
    // follow a subject.
    const int sampleWidth = 160;
    const int sampleHeight = qBound(16, int(std::lround(sampleWidth / sourceAspect / 2)) * 2, 640);
    const qsizetype sampleBytes = qsizetype(sampleWidth) * sampleHeight;
    m_reframeProc = std::make_unique<QProcess>();
    m_reframeProc->setProgram(ffmpeg);
    m_reframeProc->setArguments({QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
                                 QStringLiteral("-ss"), QString::number(in / fps, 'f', 6),
                                 QStringLiteral("-t"), QString::number((out - in + 1) / fps, 'f', 6),
                                 QStringLiteral("-i"), path, QStringLiteral("-an"), QStringLiteral("-sn"),
                                 QStringLiteral("-vf"), QStringLiteral("fps=2,scale=%1:%2,format=gray").arg(sampleWidth).arg(sampleHeight),
                                 QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-")});
    emit reframingChanged();
    setStatus(tr("Reframe to Fit: following %1…").arg(name));
    // Each picture is reduced to its point of interest as it arrives: a long
    // clip's pictures are never all held at once.
    const auto found = std::make_shared<QList<QPointF>>();
    const auto unread = std::make_shared<QByteArray>();
    const auto reduce = [this, found, unread, sampleWidth, sampleHeight, sampleBytes] {
        if (!m_reframeProc) return;
        unread->append(m_reframeProc->readAllStandardOutput());
        qsizetype at = 0;
        for (; at + sampleBytes <= unread->size(); at += sampleBytes) {
            const QImage picture(reinterpret_cast<const uchar *>(unread->constData() + at), sampleWidth, sampleHeight, sampleWidth, QImage::Format_Grayscale8);
            found->append(ReframeToFit::interest(picture));
        }
        unread->remove(0, at);
    };
    connect(m_reframeProc.get(), &QProcess::readyReadStandardOutput, this, reduce);
    connect(m_reframeProc.get(), &QProcess::finished, this,
            [this, track, index, entry, name, sourceAspect, frameAspect, fps, skip, reduce, found](int code, QProcess::ExitStatus status) {
        reduce();
        m_reframeProc.release()->deleteLater();
        if (status != QProcess::NormalExit || code != 0 || found->isEmpty())
            return skip(tr("Could not read %1's pictures").arg(name));
        const QVariantMap now = entryOn(track, index);
        if (now.value(ClipKey::path) != entry.value(ClipKey::path) || now.value(ClipKey::in) != entry.value(ClipKey::in))
            return skip(tr("%1 changed while it was followed; try again").arg(name));
        QList<QPointF> path = ReframeToFit::smooth(*found, 5);
        // Only the cropped direction follows; the other stays centred.
        const bool across = sourceAspect > frameAspect;
        for (QPointF &p : path) (across ? p.ry() : p.rx()) = .5;
        const auto start = ReframeToFit::place(sourceAspect, frameAspect, path.first());
        double low = across ? start.x : start.y, high = low;
        QList<ReframeToFit::Placement> placements;
        for (const QPointF &p : std::as_const(path)) {
            placements.append(ReframeToFit::place(sourceAspect, frameAspect, p));
            const double v = across ? placements.last().x : placements.last().y;
            low = qMin(low, v); high = qMax(high, v);
        }
        const QString key = across ? QStringLiteral("rectX") : QStringLiteral("rectY");
        QVariantMap editKeys = now.value(ClipKey::editKeys).toMap();
        for (const char *k : {"rectX", "rectY", "rectW", "rectH"}) editKeys.remove(QLatin1String(k));
        QVariantMap patch{{QStringLiteral("rectW"), start.scale}, {QStringLiteral("rectH"), start.scale},
                          {QStringLiteral("rectX"), start.x}, {QStringLiteral("rectY"), start.y}};
        // A subject that hardly moves gets a still frame: no keys at all.
        if (high - low > 0.02) {
            QVariantList keys;
            const int frames = now.value(ClipKey::frames).toInt();
            for (int i = 0; i < placements.size(); i += 2) { // one a second
                const int frame = qMin(frames - 1, qRound(i * fps / 2.));
                keys.append(QVariantMap{{QStringLiteral("frame"), frame}, {QStringLiteral("value"), across ? placements.at(i).x : placements.at(i).y},
                                        {QStringLiteral("interpolation"), QStringLiteral("smooth")}});
            }
            editKeys.insert(key, keys);
        } else {
            patch.insert(key, (low + high) / 2);
        }
        patch.insert(QStringLiteral("editKeys"), editKeys);
        setEditProperties(track, index, patch);
        const QString message = high - low > 0.02 ? tr("%1 now fills the frame and follows its subject").arg(name)
                                                  : tr("%1 now fills the frame, framed on its subject").arg(name);
        setStatus(message);
        emit reframeToFitFinished(track, index, true, message);
        QTimer::singleShot(0, this, &Backend::reframeNext);
    });
    watchToolProcess(m_reframeProc.get(), [this, skip, name] {
        m_reframeProc.release()->deleteLater();
        skip(tr("Could not read %1's pictures").arg(name));
    });
    m_reframeProc->start();
}

bool Backend::setMatchReference(int track, int index, int clipFrame) {
    const QVariantMap entry = entryOn(track, index);
    if (entry.isEmpty() || entry.value(ClipKey::gap).toBool() || !entry.value(ClipKey::title).toString().isEmpty())
        return false;
    const int frame = qBound(0, clipFrame, qMax(0, entry.value(ClipKey::frames).toInt() - 1));
    m_matchReference = {{QStringLiteral("track"), track}, {QStringLiteral("index"), index}, {QStringLiteral("frame"), frame},
                        {QStringLiteral("name"), entry.value(ClipKey::name)}, {QStringLiteral("path"), entry.value(ClipKey::path)}};
    emit matchReferenceChanged();
    setStatus(tr("Match reference: %1 at %2").arg(entry.value(ClipKey::name).toString(), timecodeFor(frame, m_fps)));
    return true;
}

bool Backend::matchColor(int track, int index, int clipFrame) {
    const auto refuse = [this](const QString &why) { setStatus(why); emit matchFinished(false, why); return false; };
    if (m_exporting) return refuse(tr("Cannot match colour while exporting."));
    if (m_matchReference.isEmpty()) return refuse(tr("Set a match reference first: Modify ▸ Match ▸ Set Match Reference"));
    const QVariantMap reference = entryOn(m_matchReference.value(QStringLiteral("track")).toInt(), m_matchReference.value(QStringLiteral("index")).toInt());
    if (reference.value(ClipKey::path) != m_matchReference.value(QStringLiteral("path")))
        return refuse(tr("The match reference has moved or gone; set it again."));
    emit matchColorRequested(track, index, clipFrame, m_matchReference.value(QStringLiteral("track")).toInt(),
                             m_matchReference.value(QStringLiteral("index")).toInt(), m_matchReference.value(QStringLiteral("frame")).toInt());
    return true;
}

QVariantMap Backend::soundEntryFor(const QVariantMap &entry) const {
    if (!entry.value(ClipKey::videoOnly).toBool()) return entry;
    const QString link = entry.value(ClipKey::link).toString();
    if (link.isEmpty()) return {};
    for (const QVariant &v : m_connected) {
        const QVariantMap mate = v.toMap();
        if (mate.value(ClipKey::link).toString() == link && mate.value(ClipKey::audioOnly).toBool()) return mate;
    }
    return {};
}

bool Backend::matchAudio(int track, int index) {
    const auto refuse = [this](const QString &why) { setStatus(why); emit matchFinished(false, why); return false; };
    if (m_exporting) return refuse(tr("Cannot match audio while exporting."));
    if (m_matchProc) return refuse(tr("Already matching audio; wait for it to finish."));
    if (m_matchReference.isEmpty()) return refuse(tr("Set a match reference first: Modify ▸ Match ▸ Set Match Reference"));
    const QVariantMap target = soundEntryFor(entryOn(track, index));
    const QVariantMap reference = soundEntryFor(entryOn(m_matchReference.value(QStringLiteral("track")).toInt(),
                                                        m_matchReference.value(QStringLiteral("index")).toInt()));
    if (target.isEmpty() || reference.isEmpty()) return refuse(tr("Match Audio needs two clips with sound."));
    if (target.value(ClipKey::track) == reference.value(ClipKey::track)
        && target.value(ClipKey::index) == reference.value(ClipKey::index))
        return refuse(tr("Choose a different clip from the reference."));
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return refuse(tr("Match Audio needs ffmpeg installed"));
    const double fps = m_fps > 0 ? m_fps : 25.;
    // Both measured in one ffmpeg run: each shown part through ebur128.
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin")};
    for (const QVariantMap &e : {target, reference}) {
        const QString path = e.value(ClipKey::path).toString();
        if (!QFileInfo::exists(path)) return refuse(tr("%1's media is not here").arg(e.value(ClipKey::name).toString()));
        const int in = e.value(ClipKey::in).toInt(), out = e.value(ClipKey::out).toInt();
        args << QStringLiteral("-ss") << QString::number(in / fps, 'f', 6) << QStringLiteral("-t")
             << QString::number(qMin(out - in + 1, qRound(600 * fps)) / fps, 'f', 6) << QStringLiteral("-i") << path;
    }
    args << QStringLiteral("-filter_complex")
         << QStringLiteral("[0:a:0]ebur128=framelog=quiet[a];[1:a:0]ebur128=framelog=quiet[b]")
         << QStringLiteral("-map") << QStringLiteral("[a]") << QStringLiteral("-f") << QStringLiteral("null") << QStringLiteral("-")
         << QStringLiteral("-map") << QStringLiteral("[b]") << QStringLiteral("-f") << QStringLiteral("null") << QStringLiteral("-");
    m_matchProc = std::make_unique<QProcess>();
    m_matchProc->setProgram(ffmpeg);
    m_matchProc->setArguments(args);
    setStatus(tr("Measuring %1 and %2…").arg(target.value(ClipKey::name).toString(), reference.value(ClipKey::name).toString()));
    connect(m_matchProc.get(), &QProcess::finished, this, [this, target, reference, refuse](int code, QProcess::ExitStatus status) {
        const QString log = QString::fromUtf8(m_matchProc->readAllStandardError());
        m_matchProc.release()->deleteLater();
        // Each ebur128 instance prints a summary, "Integrated loudness:" then
        // "I: -23.0 LUFS":
        // instance 0 is the clip, 1 the reference.
        static const QRegularExpression integrated(QStringLiteral("Parsed_ebur128_([01])[^\\]]*\\] Summary:\\s+Integrated loudness:\\s+I:\\s+(-?[0-9.]+|-inf)\\s+LUFS"));
        double values[2] = {qQNaN(), qQNaN()};
        for (auto it = integrated.globalMatch(log); it.hasNext();) {
            const auto match = it.next();
            values[match.captured(1).toInt()] = match.captured(2) == QStringLiteral("-inf") ? -qInf() : match.captured(2).toDouble();
        }
        if (status != QProcess::NormalExit || code != 0 || std::isnan(values[0]) || std::isnan(values[1])) {
            refuse(tr("Could not measure the two clips' sound."));
            return;
        }
        const double mine = values[0], theirs = values[1];
        if (!std::isfinite(mine) || !std::isfinite(theirs) || mine < -70 || theirs < -70) {
            refuse(tr("One of the clips is silent; nothing to match."));
            return;
        }
        // The same media through each clip's own volume.
        const double delta = qBound(-60., (theirs + reference.value(ClipKey::volume).toDouble())
                                                - (mine + target.value(ClipKey::volume).toDouble()), 24.);
        const QVariantMap now = entryOn(target.value(ClipKey::track).toInt(), target.value(ClipKey::index).toInt());
        if (now.value(ClipKey::path) != target.value(ClipKey::path)) {
            refuse(tr("The clip changed while it was measured; try again."));
            return;
        }
        if (qAbs(delta) < 0.1) {
            setStatus(tr("%1 already matches the reference's loudness").arg(target.value(ClipKey::name).toString()));
            emit matchFinished(true, QString());
            return;
        }
        const bool connected = target.value(ClipKey::track).toInt() > 0;
        int flat = target.value(ClipKey::index).toInt();
        if (connected) {
            flat = -1;
            for (int i = 0; i < m_connected.size() && flat < 0; ++i) {
                const QVariantMap e = m_connected.at(i).toMap();
                if (e.value(ClipKey::track) == target.value(ClipKey::track) && e.value(ClipKey::index) == target.value(ClipKey::index)) flat = i;
            }
            if (flat < 0) { refuse(tr("The clip changed while it was measured; try again.")); return; }
        }
        const QVariantList keys = now.value(ClipKey::volumeKf).toList();
        if (!keys.isEmpty()) {
            // A keyframed clip keeps its curve, moved as a whole.
            QVariantList shifted;
            for (const QVariant &v : keys) {
                QVariantMap key = v.toMap();
                key.insert(QStringLiteral("db"), qBound(-60., key.value(QStringLiteral("db")).toDouble() + delta, 12.));
                shifted.append(key);
            }
            connected ? setConnectedVolumeKf(flat, shifted) : setClipVolumeKf(flat, shifted);
        } else {
            connected ? adjustConnectedVolume(flat, delta) : adjustVolume(flat, delta);
        }
        const QString message = tr("%1 %2 by %3 dB to match %4")
            .arg(target.value(ClipKey::name).toString(), delta > 0 ? tr("raised") : tr("lowered"),
                 QString::number(qAbs(delta), 'f', 1), reference.value(ClipKey::name).toString());
        setStatus(message);
        emit matchFinished(true, message);
    });
    watchToolProcess(m_matchProc.get(), [this, refuse] {
        m_matchProc.release()->deleteLater();
        refuse(tr("Could not measure the two clips' sound."));
    });
    m_matchProc->start();
    return static_cast<bool>(m_matchProc); // false when it could not be started, which was said
}

void Backend::autoBalance(int track, int index, int clipFrame) {
    if (m_exporting) {
        setStatus(tr("Cannot auto balance while exporting."));
        return;
    }
    emit autoBalanceRequested(track, index, clipFrame);
}

QVariantMap Backend::defaultGrade() const { return Timeline::defaultGrade(); }

QString Backend::colorAt(double fx, double fy) const {
    // The working frame's colour, also in the moment a seek's picture is
    // still the one drawn for the viewer.
    const QImage frame = wholeProgramFrame();
    if (frame.isNull())
        return QString();
    const int x = qBound(0, int(fx * frame.width()), frame.width() - 1);
    const int y = qBound(0, int(fy * frame.height()), frame.height() - 1);
    return frame.pixelColor(x, y).name();
}
QVariantMap Backend::defaultGradeNode(const QString &label) const { return Timeline::defaultGradeNode(label); }

void Backend::setScopesActive(bool on) {
    if (on == m_scopesActive)
        return;
    m_scopesActive = on;
    sendPlaybackDisplay();
    emit scopesActiveChanged();
    if (on)
        renderScopes();
    else
        cancelScopes();
}

void Backend::setScopeOptions(const QVariantMap &options) {
    const QVariantMap plain = plainVariant(options).toMap();
    if (plain == m_scopeOptions)
        return;
    m_scopeOptions = plain;
    sendPlaybackDisplay();
    emit scopeOptionsChanged();
    if (m_scopesActive)
        renderScopes();
}

void Backend::setThumbnailProvider(ThumbnailProvider *provider) {
    if(provider==m_thumbs)return;
    cancelScopes();m_thumbs=provider;
    if(m_stills)refreshStillThumbs();
    renderScopes();
}

void Backend::cancelScopes(bool clear) {
    if(m_scopeProcessor)m_scopeProcessor->cancel();
    if(clear) {
        if(m_thumbs)m_thumbs->clearScopes();
        if(m_scopeTick){m_scopeTick=0;emit scopeTickChanged();}
    }
}

// The GUI only snapshots immutable input and publishes complete scope batches.
void Backend::renderScopes() {
    if (!m_scopesActive || !m_thumbs || m_frame.isNull() || m_projectLoadPending)
        return;
    // A picture the engine made for the viewer alone (8 bits, at the viewer's
    // size) is not measured. Its scopes were counted on the engine as it was
    // drawn, with the options of that moment; without counts, or with counts
    // of other options, the scopes wait (the working frame it stands for, or
    // the next picture, is on its way).
    std::shared_ptr<const std::vector<quint32>> counts;
    if (!m_frame.text(QStringLiteral("omaedit.shown")).isEmpty()) {
        const auto counted = ScopeProcessing::counted(m_frame);
        if (!counted.cells || counted.options != measuredScopeOptions())
            return;
        counts = counted.cells;
    }
    if(!m_scopeProcessor) {
        m_scopeProcessor=new ScopeProcessor(this);
        connect(m_scopeProcessor,&ScopeProcessor::ready,this,[this](const ScopeProcessing::Result &result) {
            if(!m_scopesActive || !m_thumbs || m_projectLoadPending)return;
            if(!result.error.isEmpty()){setStatus(tr("Could not update scopes: %1").arg(result.error));return;}
            if(result.images.size()!=4)return;
            // Keep zero for an empty project. Avoid signed overflow in very long sessions.
            m_scopeRevision=m_scopeRevision==std::numeric_limits<int>::max()?1:m_scopeRevision+1;
            m_thumbs->insertScopes(m_scopeRevision,result.images);
            m_scopeTick=m_scopeRevision;emit scopeTickChanged();
        });
    }
    m_scopeProcessor->submit({m_frame,m_scopeOptions,
        LinearRenderer::outputSpace("rec"+m_projectSettings.value("colorspace",709).toString()),0,counts},
        playing() && !m_seekInFlight && m_pendingSeek<0);
}

void Backend::refreshStillThumbs() {
    if (!m_thumbs)
        return;
    for (const QVariant &v : m_stills->items()) {
        const QVariantMap m = v.toMap();
        const QString key = QStringLiteral("still:") + m.value(QStringLiteral("id")).toString();
        const auto display=m.value("thumbnail").toString();
        const auto path=display.isEmpty()?m.value("path").toString():m_stills->directory()+'/'+display;
        m_thumbs->registerSource(key,path);

    }
}

QString Backend::grabGradeStill(const QString &name, const QString &album) {
    if (m_frame.isNull()) {
        setStatus(tr("No frame to grab."));
        return QString();
    }
    const QVariantMap clip = m_gradeClip;
    const QString id = m_stills->grab(wholeProgramFrame(), clip.value(QStringLiteral("grade")).toMap(),
                                      name.isEmpty() ? clip.value(QStringLiteral("name")).toString() : name, album,
                                      clip.value(QStringLiteral("name")).toString(), m_position);
    if (id.isEmpty()) {
        setStatus(tr("Could not save the still."));
        return QString();
    }
    setStatus(tr("Still grabbed"));
    return id;
}

void Backend::applyStill(const QString &id, const QVariantList &addrs) {
    const QVariantMap still = m_stills->item(id);
    if (still.isEmpty())
        return;
    const QVariantMap grade = still.value(QStringLiteral("grade")).toMap();
    int n = 0;
    for (const QVariant &v : addrs) {
        const QVariantMap a = plainVariant(v).toMap();
        if (!a.contains(QStringLiteral("track")))
            continue;
        setGrade(a.value(QStringLiteral("track")).toInt(), a.value(QStringLiteral("index")).toInt(), grade);
        ++n;
    }
    setStatus(n == 1 ? tr("Grade applied") : tr("Grade applied to %1 clips").arg(n));
}

void Backend::chooseLutFolder() {
    m_chooserMode = ChooserMode::LutFolder;
    m_chooser->openDirectory(QStringLiteral("LUT Folder"));
}
