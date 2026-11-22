#include "backend_p.h"

static QString clipsWord(int n) {
    return QCoreApplication::translate("Backend", "%n clip(s)", nullptr, n);
}

static void storePresets(const QVariantList &presets) {
    QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"))
        .setValue(QStringLiteral("attributePresets"),
                  QString::fromUtf8(QJsonDocument::fromVariant(presets).toJson(QJsonDocument::Compact)));
}

static QString bypassName(const QString &key) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("color"), QCoreApplication::translate("Backend", "Colour")},
        {QStringLiteral("crop"), QCoreApplication::translate("Backend", "Crop")},
        {QStringLiteral("flip"), QCoreApplication::translate("Backend", "Flip")},
        {QStringLiteral("opacity"), QCoreApplication::translate("Backend", "Opacity")},
        {QStringLiteral("rect"), QCoreApplication::translate("Backend", "Transform")},
        {QStringLiteral("level"), QCoreApplication::translate("Backend", "Level")},
        {QStringLiteral("eq"), QCoreApplication::translate("Backend", "Equaliser")},
        {QStringLiteral("audioMap"), QCoreApplication::translate("Backend", "Channels")},
        {QStringLiteral("normalize"), QCoreApplication::translate("Backend", "Normalise")},
        {QStringLiteral("blend"), QCoreApplication::translate("Backend", "Blend mode")},
        {QStringLiteral("lens"), QCoreApplication::translate("Backend", "Lens correction")},
        {QStringLiteral("stab"), QCoreApplication::translate("Backend", "Stabilisation")}};
    return names.value(key, key);
}

QVariantList Backend::markers() const {
    return m_markers;
}

void Backend::setTrackFlag(int track, const QString &flag, bool on) {
    if (track < 0 || track >= m_tracks.size())
        return;
    emit trackFlagRequested(track, flag, on);
}

void Backend::setDestTrack(int track) {
    if (track < 0 || track >= qMax(1, m_tracks.size()) || track == m_destTrack)
        return;
    m_destTrack = track;
    emit destTrackChanged();
}

void Backend::addTrack(bool audio) {
    if (m_exporting)
        return;
    emit addTrackRequested(audio);
}

void Backend::removeTrack(int track) {
    if (m_exporting || track < 1 || track >= m_tracks.size())
        return;
    emit removeTrackRequested(track);
}

int Backend::trackNeighbour(int track, bool up) const {
    if (track < 1 || track >= m_tracks.size())
        return -1;
    const QString kind =
        m_tracks.at(track).toMap().value(QStringLiteral("kind")).toString();
    const bool audio = kind == QStringLiteral("audio");
    const int step = audio == up ? -1 : 1; // video climbs, audio descends
    for (int i = track + step; i >= 1 && i < m_tracks.size(); i += step)
        if (m_tracks.at(i).toMap().value(QStringLiteral("kind")).toString() == kind)
            return i;
    return -1;
}

void Backend::moveTrack(int track, bool up) {
    const int partner = trackNeighbour(track, up);
    if (m_exporting || partner < 0)
        return;
    if (m_destTrack == track || m_destTrack == partner) {
        m_destTrack = m_destTrack == track ? partner : track;
        emit destTrackChanged();
    }
    emit swapTracksRequested(track, partner);
    setStatus(tr("Moved %1 %2")
                  .arg(m_tracks.at(track).toMap().value(QStringLiteral("name")).toString(),
                       up ? QStringLiteral("up") : QStringLiteral("down")));
}

void Backend::pasteClip(int track, int frame, const QVariantMap &entry,
                        bool insert) {
    if (m_exporting || entry.isEmpty() || track < 0 || track > m_tracks.size()
        || frame < 0)
        return;
    const QString name = entry.value(ClipKey::name).toString();
    emit pasteRequested(track, frame, clipsApart({entry}).value(0).toMap(), insert);
    setStatus((insert ? tr("Duplicated %1 on %2") : tr("Pasted %1 on %2"))
                  .arg(name, track < m_tracks.size()
                           ? m_tracks.value(track).toMap()
                                 .value(QStringLiteral("name")).toString()
                           : tr("a new track")));
}
void Backend::nudgeClip(int track, int index, int delta) {
    if (m_exporting || track < 0 || track >= m_tracks.size() || index < 0
        || delta == 0)
        return;
    emit nudgeRequested(track, index, delta);
    setStatus(delta < 0 ? tr("Nudged %n frame(s) left", nullptr, qAbs(delta))
                        : tr("Nudged %n frame(s) right", nullptr, qAbs(delta)));
}

void Backend::removeClips(const QVariantList &addrs, bool ripple) {
    if (m_exporting || addrs.isEmpty())
        return;
    setStatus(ripple ? tr("Deleted %n clip(s)", nullptr, addrs.size())
                     : tr("Lifted %n clip(s)", nullptr, addrs.size()));
    emit removeClipsRequested(addrs, ripple);
}
void Backend::nudgeClips(const QVariantList &addrs, int delta) {
    if (m_exporting || addrs.isEmpty() || delta == 0)
        return;
    setStatus((delta < 0 ? tr("Nudged %1 left by %n frame(s)", nullptr, qAbs(delta))
                         : tr("Nudged %1 right by %n frame(s)", nullptr, qAbs(delta)))
                  .arg(clipsWord(addrs.size())));
    emit nudgeClipsRequested(addrs, delta);
}
void Backend::moveClips(const QVariantList &moves) {
    if (m_exporting || moves.isEmpty())
        return;
    setStatus(tr("Moved %n clip(s)", nullptr, moves.size()));
    emit moveClipsRequested(moves);
}
void Backend::setClipsDisabled(const QVariantList &addrs, bool disabled) {
    if (m_exporting || addrs.isEmpty())
        return;
    setStatus(disabled ? tr("Disabled %n clip(s)", nullptr, addrs.size())
                       : tr("Enabled %n clip(s)", nullptr, addrs.size()));
    emit setClipsDisabledRequested(addrs, disabled);
}
void Backend::linkClips(const QVariantList &addrs) {
    if (m_exporting || addrs.size() < 2)
        return;
    emit setClipsLinkedRequested(plainVariant(addrs).toList(), true);
}
void Backend::unlinkClips(const QVariantList &addrs) {
    if (m_exporting || addrs.isEmpty())
        return;
    emit setClipsLinkedRequested(plainVariant(addrs).toList(), false);
}
void Backend::setLabels(const QVariantList &addrs, const QString &label) {
    if (m_exporting || addrs.isEmpty())
        return;
    setStatus(label.isEmpty() ? tr("Label cleared on %n clip(s)", nullptr, addrs.size())
                              : tr("Labelled %n clip(s) %1", nullptr, addrs.size()).arg(label));
    emit setLabelsRequested(addrs, label);
}
void Backend::pasteAttributes(const QVariantList &addrs, const QVariantMap &source,
                              const QStringList &groups) {
    if (m_exporting || addrs.isEmpty() || source.isEmpty() || groups.isEmpty())
        return;
    QStringList known;
    for (const QString &g : groups)
        if (Timeline::attributeGroups().contains(g))
            known.append(g);
    if (known.isEmpty())
        return;
    setStatus(tr("Pasted %n attribute(s) onto %1", nullptr, known.size())
                  .arg(clipsWord(addrs.size())));
    emit pasteAttributesRequested(plainVariant(addrs).toList(), plainVariant(source).toMap(), known);
}

void Backend::savePreset(const QString &name, const QVariantMap &source, const QStringList &groups) {
    const QString title = name.trimmed();
    if (title.isEmpty() || source.isEmpty() || groups.isEmpty())
        return;
    // Only the attribute fields ride along — never the clip's path, span
    // or markers.
    static const QStringList fields = {
        QStringLiteral("colorB"), QStringLiteral("colorC"), QStringLiteral("colorS"),
        QStringLiteral("cropL"), QStringLiteral("cropR"), QStringLiteral("cropT"), QStringLiteral("cropB"),
        QStringLiteral("rectX"), QStringLiteral("rectY"), QStringLiteral("rectW"), QStringLiteral("rectH"),
        QStringLiteral("rotation"), QStringLiteral("flipH"), QStringLiteral("flipV"), QStringLiteral("scaling"),
        QStringLiteral("opacity"), QStringLiteral("opacityKf"), QStringLiteral("volume"),
        QStringLiteral("volumeKf"), QStringLiteral("pan"), QStringLiteral("eq"),
        QStringLiteral("audioMap"), QStringLiteral("normalize"), QStringLiteral("blend"),
        QStringLiteral("lensK1"), QStringLiteral("lensK2"), QStringLiteral("lensCx"), QStringLiteral("lensCy"),
        QStringLiteral("fadeIn"), QStringLiteral("fadeOut"), QStringLiteral("speed"),
        QStringLiteral("cropFeather"), QStringLiteral("cropInvert"), QStringLiteral("anchorX"),
        QStringLiteral("anchorY"), QStringLiteral("frameBlend"), QStringLiteral("videoEffects"),
        QStringLiteral("editKeys"), QStringLiteral("effectOrder")};
    const auto cleanSource=plainVariant(source).toMap();QVariantMap attributes;
    for (const QString &f : fields)
        if (cleanSource.contains(f))
            attributes.insert(f, cleanSource.value(f));
    if(groups.contains("speed") && (cleanSource.contains("retime") || cleanSource.contains("speedPattern"))) {
        const auto result=SpeedRampEdit::preset(cleanSource);
        if(!result.value("valid").toBool()){setStatus(result.value("error").toString());emit failed(m_status);return;}
        attributes["speedPattern"]=result.value("pattern");
    }
    QVariantMap preset{{QStringLiteral("name"), title},
                       {QStringLiteral("groups"), groups},
                       {QStringLiteral("attributes"), attributes}};
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.at(i).toMap().value(QStringLiteral("name")).toString() == title)
            m_presets.removeAt(i--);
    m_presets.append(preset);
    storePresets(m_presets);
    emit presetsChanged();
    setStatus(tr("Preset “%1” saved").arg(title));
}

void Backend::deletePreset(const QString &name) {
    const int before = m_presets.size();
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.at(i).toMap().value(QStringLiteral("name")).toString() == name)
            m_presets.removeAt(i--);
    if (m_presets.size() == before)
        return;
    storePresets(m_presets);
    emit presetsChanged();
    m_effectShelf->dropKey(QStringLiteral("preset:") + name);
    setStatus(tr("Preset “%1” deleted").arg(name));
}

QVariantMap Backend::preset(const QString &name) const {
    for (const QVariant &v : m_presets)
        if (v.toMap().value(QStringLiteral("name")).toString() == name)
            return v.toMap();
    return QVariantMap();
}

void Backend::setBypass(int track, int index, const QString &key, bool on) {
    if (m_exporting || track < 0 || index < 0 || !Timeline::bypassableKeys().contains(key))
        return;
    setStatus(on ? tr("%1 bypassed").arg(bypassName(key))
                 : tr("%1 back on").arg(bypassName(key)));
    emit setBypassRequested(track, index, key, on);
}

QVariantMap Backend::entryOn(int track, int index) const {
    if (track == 0)
        return index >= 0 && index < m_timeline.size() ? m_timeline.at(index).toMap() : QVariantMap();
    for (const QVariant &v : m_connected) {
        const QVariantMap map = v.toMap();
        if (map.value(ClipKey::track).toInt() == track
            && map.value(ClipKey::index).toInt() == index)
            return map;
    }
    return QVariantMap();
}

void Backend::addClipMarkerOn(int track, int index, int sourceFrame, const QString &type,
                              const QString &text, const QString &notes) {
    setClipMarkerOn(track, index, sourceFrame, type, text, notes);
}

void Backend::setClipMarkerOn(int track, int index, int sourceFrame, const QString &type,
                              const QString &text, const QString &notes) {
    const QVariantMap entry = entryOn(track, index);
    if (m_exporting || entry.isEmpty() || sourceFrame < 0 || entry.value(ClipKey::gap).toBool())
        return;
    QVariantList markers = entry.value(ClipKey::markers).toList();
    QVariantMap marker;
    marker.insert(QStringLiteral("frame"), sourceFrame);
    marker.insert(QStringLiteral("type"), type.isEmpty() ? QStringLiteral("standard") : type);
    marker.insert(QStringLiteral("text"), text);
    marker.insert(QStringLiteral("notes"), notes);
    bool replaced = false;
    for (int i = 0; i < markers.size(); ++i) {
        if (markers.at(i).toMap().value(QStringLiteral("frame")).toInt() == sourceFrame) {
            markers[i] = marker;
            replaced = true;
        }
    }
    if (!replaced)
        markers.append(marker);
    setStatus(replaced ? tr("Clip marker changed")
                       : tr("Clip marker on %1")
                             .arg(entry.value(ClipKey::name).toString()));
    // The change, not this mirror's whole list: a second marker set before
    // the worker's echo arrives must not drop the first.
    emit editClipMarkerRequested(track, index, marker, false);
}

void Backend::removeClipMarkerOn(int track, int index, int sourceFrame) {
    const QVariantMap entry = entryOn(track, index);
    if (m_exporting || entry.isEmpty())
        return;
    QVariantList markers = entry.value(ClipKey::markers).toList();
    const int before = markers.size();
    for (int i = markers.size() - 1; i >= 0; --i)
        if (markers.at(i).toMap().value(QStringLiteral("frame")).toInt() == sourceFrame)
            markers.removeAt(i);
    if (markers.size() == before)
        return;
    setStatus(tr("Clip marker removed"));
    emit editClipMarkerRequested(track, index, {{QStringLiteral("frame"), sourceFrame}}, true);
}

bool Backend::detectEdits(int track, int index, bool blade) {
    const QVariantMap entry = entryOn(track, index);
    const auto refuse = [this](const QString &why) { setStatus(why); return false; };
    if (m_exporting || entry.isEmpty()) return false;
    if (m_detectProc) return refuse(tr("Already looking for edits; wait for it or cancel it"));
    if (entry.value(ClipKey::gap).toBool() || !entry.value(ClipKey::title).toString().isEmpty()
        || entry.value(ClipKey::audioOnly).toBool())
        return refuse(tr("Detect Edits needs a clip with pictures"));
    if (!qFuzzyCompare(entry.value(ClipKey::speed, 1.).toDouble(), 1.)
        || !entry.value(ClipKey::retime).toMap().value(QStringLiteral("segments")).toList().isEmpty())
        return refuse(tr("Detect Edits works on clips at normal speed"));
    const QString path = entry.value(ClipKey::path).toString();
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (path.isEmpty() || !QFileInfo::exists(path)) return refuse(tr("The clip's media is not here"));
    if (ffmpeg.isEmpty()) return refuse(tr("Detect Edits needs ffmpeg installed"));
    const double fps = m_fps > 0 ? m_fps : 25.;
    const int in = entry.value(ClipKey::in).toInt(), out = entry.value(ClipKey::out).toInt();
    const int start = entry.value(ClipKey::start).toInt();
    const QString name = entry.value(ClipKey::name).toString();
    // Small pictures are enough to see a cut, and much quicker to read.
    m_detectProc = std::make_unique<QProcess>();
    m_detectProc->setProgram(ffmpeg);
    m_detectProc->setArguments({QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                                QStringLiteral("-ss"), QString::number(in / fps, 'f', 6),
                                QStringLiteral("-t"), QString::number((out - in + 1) / fps, 'f', 6),
                                QStringLiteral("-i"), path, QStringLiteral("-an"), QStringLiteral("-sn"),
                                QStringLiteral("-vf"), QStringLiteral("scale=160:-2,select='gt(scene,0.35)',showinfo"),
                                QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
    connect(m_detectProc.get(), &QProcess::finished, this, [this, track, index, blade, in, out, start, fps, name, entry](int code, QProcess::ExitStatus status) {
        const QString log = QString::fromUtf8(m_detectProc->readAllStandardError());
        m_detectProc.release()->deleteLater();
        emit detectingEditsChanged();
        if (status != QProcess::NormalExit || code != 0) {
            setStatus(tr("Could not look for edits in %1").arg(name));
            emit editsDetected(-1);
            return;
        }
        // The clip may have moved meanwhile: act only on the same clip.
        const QVariantMap now = entryOn(track, index);
        if (now.value(ClipKey::path) != entry.value(ClipKey::path) || now.value(ClipKey::in).toInt() != in
            || now.value(ClipKey::start).toInt() != start) {
            setStatus(tr("%1 changed while its edits were found; try again").arg(name));
            emit editsDetected(-1);
            return;
        }
        static const QRegularExpression time(QStringLiteral("pts_time:\\s*([0-9.]+)"));
        QList<int> frames; // source frames where a new shot starts
        for (auto it = time.globalMatch(log); it.hasNext();) {
            const int frame = in + qRound(it.next().captured(1).toDouble() * fps);
            if (frame > in && frame <= out && !frames.contains(frame)) frames.append(frame);
        }
        std::sort(frames.begin(), frames.end());
        if (frames.isEmpty()) {
            setStatus(tr("No edits found in %1").arg(name));
            emit editsDetected(0);
            return;
        }
        if (blade) {
            QVariantList positions;
            for (int f : std::as_const(frames)) positions.append(start + f - in);
            emit splitManyRequested(track, positions, tr("Detect Edits"));
            setStatus(tr("Cut %1 at %n edit(s)", nullptr, frames.size()).arg(name));
        } else {
            QVariantList markers = now.value(ClipKey::markers).toList();
            for (int f : std::as_const(frames))
                markers.append(QVariantMap{{QStringLiteral("frame"), f}, {QStringLiteral("type"), QStringLiteral("standard")},
                                           {QStringLiteral("text"), tr("Edit")}, {QStringLiteral("notes"), QString()}});
            emit setClipMarkersRequested(track, index, markers);
            setStatus(tr("Marked %n edit(s) in %1", nullptr, frames.size()).arg(name));
        }
        emit editsDetected(frames.size());
    });
    watchToolProcess(m_detectProc.get(), [this, name] {
        m_detectProc.release()->deleteLater();
        emit detectingEditsChanged();
        setStatus(tr("Could not look for edits in %1").arg(name));
        emit editsDetected(-1);
    });
    m_detectProc->start();
    if (!m_detectProc) return false; // it could not be started, and said so
    emit detectingEditsChanged();
    setStatus(tr("Looking for edits in %1…").arg(name));
    return true;
}

void Backend::watchToolProcess(QProcess *process, const std::function<void()> &failedToStart) {
    // A program that cannot be started never reports that it finished.
    connect(process, &QProcess::errorOccurred, this, [failedToStart](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) failedToStart();
    });
    // ffmpeg writes as it works. Five minutes without a byte is media that
    // stopped answering (a dropped mount): ended here, it finishes as a
    // failure like any other instead of holding its tool for ever.
    auto *quiet = new QTimer(process);
    quiet->setSingleShot(true);
    quiet->setInterval(5 * 60 * 1000);
    connect(quiet, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::readyReadStandardError, quiet, qOverload<>(&QTimer::start));
    connect(process, &QProcess::readyReadStandardOutput, quiet, qOverload<>(&QTimer::start));
    quiet->start();
}

void Backend::cancelDetectEdits() {
    if (!m_detectProc) return;
    m_detectProc->disconnect(this);
    m_detectProc->kill();
    m_detectProc.release()->deleteLater();
    emit detectingEditsChanged();
    setStatus(tr("Stopped looking for edits"));
}

void Backend::pasteClips(const QVariantList &entries, int frame) {
    if (m_exporting || entries.isEmpty() || frame < 0)
        return;
    setStatus(tr("Pasted %n clip(s)", nullptr, entries.size()));
    emit pasteClipsRequested(clipsApart(entries), frame);
}

QVariantList Backend::clipsApart(const QVariantList &entries) const {
    return Timeline::clipsApart(plainVariant(entries).toList(), m_timeline + m_connected);
}

QVariantMap Backend::clipboardClock() const {
    return {{"fpsNum",m_audioEditContext.value("fpsNum")},{"fpsDen",m_audioEditContext.value("fpsDen")}};
}

QVariantMap Backend::frameTime(int frame) const {
    const qint64 num=m_audioEditContext.value("fpsNum").toLongLong(),den=m_audioEditContext.value("fpsDen").toLongLong();
    if(frame<0 || num<1 || den<1)return {};
    return AudioTimeline::writeTime({frame*den,num});
}

int Backend::pasteClipboardSnapshot(const QVariantMap &input,const QVariantMap &inputPosition,int destinationTrack,bool insert) {
    auto clipboard=plainVariant(input).toMap();auto position=plainVariant(inputPosition).toMap();
    if(position.isEmpty() && m_duration==0)position=AudioTimeline::writeTime({0,1});
    // A copy still carrying its clip's transitions (one read from the clip
    // list as it stands) is pasted as the clip drawn.
    clipboard["entries"]=clipsApart(clipboard.value("entries").toList());
    const auto copies=clipboard.value("entries").toList();
    const int request=++m_audioEditRequest;
    const auto cancel=[this,request](const QString &error) {
        // Complete after QML has stored the returned request, even if Cancel
        // arrives synchronously from the creation-prompt signal.
        QTimer::singleShot(0,this,[this,request,error] {
            if(!error.isEmpty())setStatus(error);
            emit clipboardPasteFinished(request,false,error,{});
        });
    };
    AudioTimeline::Time at;
    QString error;
    const bool deferred=m_timelineCreationPrompt && !hasTimeline();
    if(projectBusy() || m_exporting || (!deferred && m_audioEditContext.value("sequenceId").toString().isEmpty())
        || (deferred && creationContext().value("anchorId").toString().isEmpty()))
        error=tr("The project or timeline changed. Paste the clips again.");
    if(error.isEmpty() && (!AudioTimeline::readTime(position,&at) || at.ticks<0 || copies.isEmpty() || destinationTrack<-1
        || clipboard.value("fpsNum").toInt()<1 || clipboard.value("fpsDen").toInt()<1))
        error=QCoreApplication::translate("Timeline","The copied clips or destination timing are invalid.");
    for(const auto &value:copies)if(error.isEmpty() && value.metaType().id()!=QMetaType::QVariantMap)
        error=QCoreApplication::translate("Timeline","The copied clips or destination timing are invalid.");
    if(!error.isEmpty()) {cancel(error);return request;}
    const QVariantMap placement{{"position",position},{"destinationTrack",destinationTrack},{"insert",insert},
        {"duplicate",clipboard.value("duplicate").toBool()}};
    const auto target=timelinePlacementTarget();
    viewTimeline();
    if(deferred) {
        queueTimelinePlacement(copies.first().toMap().value("path").toString(),{
            [worker=m_worker,request,clipboard,placement](const QVariantMap &current) {
                worker->pasteClipboardSnapshot(request,current,clipboard,placement);
            },cancel});
    } else emit clipboardPasteRequested(request,target,clipboard,placement);
    return request;
}

int Backend::duplicateClips(const QVariantList &input) {
    const auto copies=clipsApart(input);auto clipboard=clipboardClock(); // the clips drawn: the copy goes in at the cut
    clipboard["entries"]=copies;clipboard["duplicate"]=true;
    const int num=clipboard.value("fpsNum").toInt(),den=clipboard.value("fpsDen").toInt();
    AudioTimeline::Time end{0,1};bool valid=!copies.isEmpty() && num>0 && den>0;
    for(const auto &value:copies) {
        const auto entry=value.toMap();AudioTimeline::Time start,length;
        if(entry.contains(ClipKey::audioTiming))valid=valid && AudioTimeline::readSpan(entry,&start,&length);
        else {
            const auto s=entry.value(ClipKey::start).toLongLong(),n=entry.value(ClipKey::frames).toLongLong();
            valid=valid && s>=0 && s<=100000000 && n>0 && n<=100000000
                && entry.value(ClipKey::start).toDouble()==double(s) && entry.value(ClipKey::frames).toDouble()==double(n);
            if(valid){start={s*den,num};length={n*den,num};}
        }
        const auto tail=valid?AudioTimeline::add(start,length):std::nullopt;
        if(!tail){valid=false;break;}
        if(__int128(tail->ticks)*end.rate>__int128(end.ticks)*tail->rate)end=*tail;
    }
    if(!valid)clipboard["entries"]=QVariantList{};
    return pasteClipboardSnapshot(clipboard,valid?AudioTimeline::writeTime(end):QVariantMap{},-1,true);
}

void Backend::replaceClip(int libraryIndex, int track, int index, bool fromEnd, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (m_exporting || !hasVideo() || libraryIndex < 0
        || libraryIndex >= m_library.size() || track < 0
        || track >= m_tracks.size() || index < 0)
        return;
    // The clip to cover: V1 by timeline index, any other track by its
    // index on that track.
    QVariantMap target;
    if (track == 0) {
        if (index >= m_timeline.size())
            return;
        target = m_timeline.at(index).toMap();
    } else {
        for (const QVariant &v : m_connected) {
            const QVariantMap map = v.toMap();
            if (map.value(ClipKey::track).toInt() == track
                && map.value(ClipKey::index).toInt() == index)
                target = map;
        }
        if (target.isEmpty())
            return;
    }
    if (target.value(ClipKey::gap).toBool()
        || !target.value(ClipKey::title).toString().isEmpty())
        return;
    const QVariantMap source = placementSource(libraryIndex);
    if (source.isEmpty()) return;
    const auto range = rangeOf(source);
    // The clip as it is drawn: in a transition its entry runs to the
    // transition's last frame and starts after it, where the clip the editor
    // sees is cut at the cut (MixShape).
    int start = target.value(ClipKey::start).toInt(), frames = target.value(ClipKey::frames).toInt();
    {
        const QVariantList &list = track == 0 ? m_timeline : m_connected;
        const QList<MixShape::Block> blocks = MixShape::blocks(list);
        for (int i = 0; i < list.size(); ++i) {
            const QVariantMap e = list.at(i).toMap();
            if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index) {
                start -= blocks.at(i).lead;
                frames += blocks.at(i).lead - blocks.at(i).tail;
            }
        }
    }
    const int duration = source.value(QStringLiteral("duration")).toInt();
    // From the range's start, or counted back from its end.
    const int in = fromEnd ? (range.second < 0 ? duration - 1 : range.second) - frames + 1 : qMax(0, range.first);
    if (frames < 1 || in < 0 || in + frames > duration) {
        setStatus(tr("%1 is too short to replace %2")
                      .arg(source.value(QStringLiteral("name")).toString(),
                           target.value(ClipKey::name).toString()));
        return;
    }
    auto placementTag=source.value("placementTag").toMap();
    placementTag["_sourceFrames"]=frames;
    placementTag["_replaces"]=index; // the transitions on this clip's cuts stay on the new one
    emit tagNextPlacementRequested(placementTag);
    const int takes = takesFor(libraryIndex, asked);
    emit placeOnTrackRequested(track, 2,
                               source.value(QStringLiteral("path")).toString(),
                               start,
                               in, in + frames - 1, false, takes);
    setStatus(tr("Replaced %1 with %2")
                  .arg(target.value(ClipKey::name).toString(),
                       takenName(source.value(QStringLiteral("name")).toString(), takes)));
}
void Backend::fitToFill(int libraryIndex, int track, int position, int frames, int replaces) {
    if (libraryOffline(libraryIndex))
        return;
    if (m_exporting || !hasVideo() || libraryIndex < 0 || libraryIndex >= m_library.size()
        || track < 0 || track > m_tracks.size() || position < 0 || frames < 1)
        return;
    const QVariantMap source = placementSource(libraryIndex);
    if (source.isEmpty()) return;
    const auto range = rangeOf(source);
    const int duration = source.value(QStringLiteral("duration")).toInt();
    const int in = qMax(0, range.first);
    const int out = range.second < 0 ? qMax(0, duration - 1) : qMin(range.second, qMax(0, duration - 1));
    if (out < in)
        return;
    const double span=source.value("placementTag").toMap().value("sourceSpan",double(out-in+1)).toDouble();
    const double speed = span / double(frames);
    if (speed < 0.05 || speed > 20.0) {
        const auto problem=tr("Fit to Fill would need %1× — out of the 0.05–20× range")
                      .arg(QString::number(speed, 'f', 2));
        setStatus(problem);emit failed(problem);
        return;
    }
    auto placementTag=source.value("placementTag").toMap();
    if (replaces >= 0)
        placementTag["_replaces"]=replaces; // fitted over that clip: the transitions on its cuts stay
    emit tagNextPlacementRequested(placementTag);
    emit fitToFillRequested(track, source.value(QStringLiteral("path")).toString(), position,
                            frames, in, out,
                            source.value(QStringLiteral("audioOnly")).toBool());
    setStatus(tr("Fit %1 to %2 at %3×")
                  .arg(source.value(QStringLiteral("name")).toString())
                  .arg(Interchange::durationText(frames, fps()))
                  .arg(QString::number(speed, 'f', 2)));
}

void Backend::previewClipAt(int libraryIndex, int frame, int rangeIn, int rangeOut) {
    if (libraryOffline(libraryIndex) || libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    m_pendingSourceSeek = qMax(0, frame);
    m_pendingSourceRangeIn = rangeIn;
    m_pendingSourceRangeOut = rangeOut;
    if (m_viewingSource && m_previewLibraryIndex == libraryIndex) {
        // Already up: park and mark now.
        const int seekTo = m_pendingSourceSeek;
        m_pendingSourceSeek = -1;
        if (m_pendingSourceRangeIn >= 0 || m_pendingSourceRangeOut >= 0)
            setRange(m_pendingSourceRangeIn, m_pendingSourceRangeOut);
        m_pendingSourceRangeIn = m_pendingSourceRangeOut = -1;
        seek(seekTo);
        return;
    }
    previewClip(libraryIndex);
}

void Backend::setClipLabel(int timelineIndex, const QString &label) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    if (m_timeline.at(timelineIndex).toMap()
            .value(ClipKey::label).toString() == label)
        return;
    emit setLabelRequested(0, timelineIndex, label);
}
void Backend::setConnectedLabel(int connectedIndex, const QString &label) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (e.value(ClipKey::label).toString() == label)
        return;
    emit setLabelRequested(e.value(ClipKey::track).toInt(),
                           e.value(ClipKey::index).toInt(), label);
}
void Backend::renameTrack(int track, const QString &name) {
    if (track < 0 || track >= m_tracks.size())
        return;
    emit trackNameRequested(track, name);
}
void Backend::moveClipTo(int track, int index, int newTrack, int newStart) {
    if (m_exporting || track < 0 || newTrack < 0 || newTrack > m_tracks.size())
        return;
    emit moveClipToRequested(track, index, newTrack, qMax(0, newStart));
}

void Backend::setMarkerInfo(int frame, const QString &text, const QString &type,
                            const QString &notes) {
    emit setMarkerInfoRequested(frame, text.trimmed(), type, notes);
}

QVariantMap Backend::markerEditTarget(int track, int index, int frame) const {
    const auto entry = track >= 0 ? entryOn(track, index) : QVariantMap();
    const auto markers = track >= 0 ? entry.value(ClipKey::markers).toList() : m_markers;
    QVariantMap marker;
    for (const auto &v : markers) if (v.toMap().value("frame").toInt() == frame) { marker = v.toMap(); break; }
    if (marker.isEmpty() || (track >= 0 && (entry.isEmpty() || entry.value(ClipKey::gap).toBool()))) return {};
    const auto sequence = m_sequences.value(m_activeSequence).toMap();
    return {{"track",track},{"index",index},{"entry",entry},{"marker",marker},
            {"entryState",MarkerEdit::clipState(entry)},
            {"generation",m_mediaGeneration},
            {"sequenceId",sequence.value("id")},{"sequenceName",sequence.value("name")},
            {"maximum",qMax(frame, track >= 0 ? entry.value(ClipKey::length).toInt() - 1 : m_duration - 1)},
            {"fps",m_fps > 0 ? m_fps : 25.0}};
}

QString Backend::markerEditProblem(const QVariantMap &input) const {
    const auto target = plainVariant(input).toMap();
    if (target.isEmpty()) return tr("This marker is no longer available.");
    if (projectBusy() || m_exporting) return tr("Wait for the current project operation before editing markers.");
    if (target.value("generation").toInt() != m_mediaGeneration ||
        target.value("sequenceId").toString() != m_sequences.value(m_activeSequence).toMap().value("id").toString())
        return tr("The project or sequence changed. Close this editor and reopen the marker.");
    const int track = target.value("track",-1).toInt();
    if (track >= 0 && (track >= m_tracks.size() || m_tracks[track].toMap().value("locked").toBool()))
        return tr("The marker's track is locked or no longer available.");
    const auto entry = track >= 0 ? entryOn(track, target.value("index").toInt()) : QVariantMap();
    if (track >= 0 && MarkerEdit::clipState(entry) != target.value("entryState").toString())
        return tr("The clip changed. Close this editor and reopen the marker.");
    const auto markers = track >= 0 ? entry.value(ClipKey::markers).toList() : m_markers;
    if (!markers.contains(target.value("marker"))) return tr("This marker changed or was removed. Reopen it to continue.");
    return {};
}

QStringList Backend::markerColours() const { return MarkerEdit::colours(); }
int Backend::applyMarkerEdit(const QVariantMap &input, int frame, const QString &type,
                             const QString &text, const QString &notes, bool remove,
                             const QString &colour) {
    const int request = ++m_markerEditRequest;
    const auto target = plainVariant(input).toMap();
    const auto error = markerEditProblem(target);
    if (!error.isEmpty()) {
        QTimer::singleShot(0, this, [this,request,error] { setStatus(error); emit markerEditFinished(request,false,error); });
        return request;
    }
    emit markerEditRequested(request,target,frame,type,text.trimmed(),notes,remove,colour);
    return request;
}

void Backend::removeMarkersIn(int first, int last) {
    if (first >= 0 && last >= first)
        emit removeMarkersInRequested(first, last);
}

void Backend::toggleMarker() {
    if (!hasVideo())
        return;
    // The worker owns the list (its undo stacks cover it); the mirror here
    // is only read for status/UI, updated when markersChanged echoes back.
    bool exists = false;
    for (const QVariant &m : m_markers)
        exists = exists
                 || m.toMap().value(QStringLiteral("frame")).toInt() == m_position;
    setStatus(exists ? tr("Marker removed")
                     : tr("Marker at %1").arg(timecode(m_position)));
    emit toggleMarkerRequested(m_position);
}

void Backend::appendClipAt(int libraryIndex, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    const int takes = takesFor(libraryIndex, asked);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    if (m_destTrack > 0 && hasVideo())
        emit placeOnTrackRequested(m_destTrack, 0,
                                   entry.value(QStringLiteral("path")).toString(),
                                   0, range.first, range.second, false, takes);
    else
        emit appendRequested(entry.value(QStringLiteral("path")).toString(),
                             range.first, range.second, takes);
    setStatus(tr("Appended %1").arg(takenName(entry.value(QStringLiteral("name")).toString(), takes)));
}

void Backend::dropClipAt(int libraryIndex, int track, int frame, bool audio, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size() || track < 0
        || track > m_tracks.size() || frame < 0)
        return;
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    const QString path = entry.value(QStringLiteral("path")).toString();
    const int takes = takesFor(libraryIndex, asked);
    const QString name = takenName(entry.value(QStringLiteral("name")).toString(), takes);
    if (!hasVideo()) { // nothing to overwrite yet — this starts the edit
        emit insertRequested(path, 0, range.first, range.second, takes);
        setStatus(tr("Added %1").arg(name));
        return;
    }
    emit placeOnTrackRequested(track, 2, path, frame, range.first, range.second,
                               audio, takes);
    // A row takes what it can hold: an audio row the sound, whatever was
    // asked for; a video row hands the sound alone to an audio track.
    const bool audioRow = track < m_tracks.size()
        ? m_tracks.value(track).toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("audio") : audio;
    // (A recording is its sound: there is no picture for a video row.)
    const bool recording = entry.value(QStringLiteral("hasAudio")).toBool()
                           && !entry.value(QStringLiteral("hasVideo"), true).toBool();
    if ((takes == EditTakes::Sound || recording) && !audioRow) {
        setStatus(tr("Placed %1 on an audio track").arg(name));
        return;
    }
    setStatus(tr("Placed %1 on %2")
                  .arg(audioRow ? entry.value(QStringLiteral("name")).toString() : name,
                       track < m_tracks.size()
                                 ? m_tracks.value(track).toMap()
                                       .value(QStringLiteral("name")).toString()
                                 : (audio ? tr("a new audio track") : tr("a new video track"))));
}
void Backend::slideClip(int track, int index, int delta) {
    if (m_exporting || track < 0 || track >= m_tracks.size() || index < 1
        || delta == 0)
        return;
    emit slideRequested(track, index, delta);
    setStatus(delta < 0 ? tr("Slid %n frame(s) left", nullptr, qAbs(delta))
                        : tr("Slid %n frame(s) right", nullptr, qAbs(delta)));
}
void Backend::joinClip(int track, int index) {
    if (m_exporting || track < 0 || track >= m_tracks.size() || index < 0)
        return;
    emit joinRequested(track, index);
    setStatus(tr("Joined clips"));
}
void Backend::applyTransitionAt(int track, int index, const QString &style) {
    if (m_exporting || track < 0 || index < 0)
        return;
    const int length = qMax(2, qRound(m_fps)); // ~1 s, like Ctrl+T
    // A new one sits across the cut in its style, as one edit; one already
    // there is restyled where it is.
    if (track == 0) {
        if (index >= m_timeline.size())
            return;
        const bool has = m_timeline.at(index).toMap()
                             .value(ClipKey::mixOut).toInt() > 0;
        if (!has)
            emit addTransitionRequested(0, index, length, style);
        else
            emit restyleDissolveRequested(index, style);
    } else {
        int flat = -1;
        for (int i = 0; i < m_connected.size(); ++i) {
            const QVariantMap e = m_connected.at(i).toMap();
            if (e.value(ClipKey::track).toInt() == track
                && e.value(ClipKey::index).toInt() == index)
                flat = i;
        }
        if (flat < 0)
            return;
        const bool has = m_connected.at(flat).toMap()
                             .value(ClipKey::mixOut).toInt() > 0;
        if (!has)
            emit addTransitionRequested(track, index, length, style);
        else
            emit restyleConnectedDissolveRequested(flat, style);
    }
    // By the name the Transitions list and the Inspector give it (a refusal
    // from the edit itself, such as no clip to go into, then says why).
    QString shown = style;
    for (const QVariant &v : transitionStyles())
        if (v.toMap().value(QStringLiteral("key")).toString() == render::transitionBase(style))
            shown = v.toMap().value(QStringLiteral("name")).toString();
    setStatus(tr("Transition: %1").arg(shown));
}
void Backend::addTitleAt(const QString &text, int frame, const QVariantMap &style) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !hasVideo() || frame < 0)
        return;
    emit connectTitleRequested(trimmed, frame, qMax(2, qRound(m_fps * 4)), style);
    setStatus(tr("Title “%1” added").arg(trimmed));
}

bool Backend::addGenerator(const QString &kind, const QString &color) {
    return addGeneratorAt(kind, m_position, color);
}

bool Backend::addGeneratorAt(const QString &kind, int frame, const QString &color) {
    const QString resource = Timeline::generatorResource(kind, color);
    if (resource.isEmpty()) {
        setStatus(tr("There is no generator called “%1”.").arg(kind));
        return false;
    }
    if (m_exporting) {
        setStatus(tr("Wait for the current export to finish."));
        return false;
    }
    if (frame < 0)
        return false;
    // An empty timeline is fine: the worker starts the edit with the
    // generator on V1. An adjustment clip grades what lies under it, and
    // there is nothing yet.
    if (kind == QStringLiteral("adjust") && !hasVideo()) {
        setStatus(tr("An effect layer grades the picture under it; add a clip first."));
        return false;
    }
    const bool adjust = kind == QStringLiteral("adjust");
    // With nothing placed yet the rate comes from the timeline's settings.
    double rate = m_fps;
    const int fpsNum = m_projectSettings.value(QStringLiteral("fpsNum")).toInt();
    const int fpsDen = m_projectSettings.value(QStringLiteral("fpsDen")).toInt();
    if (rate <= 0 && fpsNum > 0 && fpsDen > 0)
        rate = double(fpsNum) / fpsDen;
    emit connectGeneratorRequested(resource, frame, qMax(2, qRound(rate * 4)), adjust);
    setStatus(adjust ? tr("Effect layer added — its colour, lens, flip and crop grade everything under it")
              : kind == QStringLiteral("bars") ? tr("Colour bars added")
              : kind == QStringLiteral("noise") ? tr("Noise added")
              : kind == QStringLiteral("count") ? tr("Counting leader added")
                                                : tr("Solid colour added"));
    return true;
}

void Backend::setGeneratorColor(int track, int index, const QString &color) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Solid colour %1").arg(color));
    emit setGeneratorColorRequested(track, index, color);
}

void Backend::insertClipAt(int libraryIndex, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    const int takes = takesFor(libraryIndex, asked);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    if (m_destTrack > 0 && hasVideo())
        emit placeOnTrackRequested(m_destTrack, 1,
                                   entry.value(QStringLiteral("path")).toString(),
                                   m_position, range.first, range.second, false, takes);
    else
        emit insertRequested(entry.value(QStringLiteral("path")).toString(),
                             hasVideo() ? m_position : 0, range.first, range.second, takes);
    setStatus(tr("Inserted %1").arg(takenName(entry.value(QStringLiteral("name")).toString(), takes)));
}

void Backend::overwriteClipAt(int libraryIndex, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    const int takes = takesFor(libraryIndex, asked);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    if (m_destTrack > 0 && hasVideo())
        emit placeOnTrackRequested(m_destTrack, 2,
                                   entry.value(QStringLiteral("path")).toString(),
                                   m_position, range.first, range.second, false, takes);
    else
        emit overwriteRequested(entry.value(QStringLiteral("path")).toString(),
                                hasVideo() ? m_position : 0, range.first, range.second, takes);
    setStatus(tr("Overwrote with %1")
                  .arg(takenName(entry.value(QStringLiteral("name")).toString(), takes)));
}

void Backend::overwriteClipBacktimed(int libraryIndex, const QString &asked) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    if (!hasVideo()) { overwriteClipAt(libraryIndex, asked); return; } // nothing to end against yet
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    int in = qMax(0, range.first);
    const int out = range.second < 0 ? entry.value(QStringLiteral("duration")).toInt() - 1 : range.second;
    int start = m_position - (out - in + 1);
    if (start < 0) { in -= start; start = 0; } // no room for its head
    const int takes = takesFor(libraryIndex, asked);
    const QString name = takenName(entry.value(QStringLiteral("name")).toString(), takes);
    if (out < in) {
        setStatus(tr("Move the playhead to where %1 should end").arg(name));
        return;
    }
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    if (m_destTrack > 0)
        emit placeOnTrackRequested(m_destTrack, 2, entry.value(QStringLiteral("path")).toString(), start, in, out, false, takes);
    else
        emit overwriteRequested(entry.value(QStringLiteral("path")).toString(), start, in, out, takes);
    setStatus(tr("Overwrote with %1, ending at the playhead").arg(name));
}

void Backend::removeClip(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    emit removeRequested(timelineIndex);
}

void Backend::liftClip(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    emit liftRequested(timelineIndex);
}

void Backend::connectClipAt(int libraryIndex) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size()) return;
    if (!hasVideo()) { appendClipAt(libraryIndex); return; } // first clip establishes the storyline
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    emit connectRequested(entry.value(QStringLiteral("path")).toString(),
                          m_position, range.first, range.second, false);
    setStatus(tr("Connected %1")
                  .arg(entry.value(QStringLiteral("name")).toString()));
}

void Backend::connectAudioClipAt(int libraryIndex) {
    if (libraryOffline(libraryIndex))
        return;
    if (libraryIndex < 0 || libraryIndex >= m_library.size() || !hasVideo())
        return;
    const QVariantMap entry = placementSource(libraryIndex);
    if (entry.isEmpty()) return;
    const auto range = rangeOf(entry);
    emit tagNextPlacementRequested(entry.value("placementTag").toMap());
    emit connectRequested(entry.value(QStringLiteral("path")).toString(),
                          m_position, range.first, range.second, true);
    setStatus(tr("Connected %1 (audio only)")
                  .arg(entry.value(QStringLiteral("name")).toString()));
}

void Backend::updateTitle(int connectedIndex, const QString &text,
                          const QVariantMap &style) {
    const QString trimmed = text.trimmed();
    if (connectedIndex < 0 || connectedIndex >= m_connected.size()
        || trimmed.isEmpty())
        return;
    emit updateTitleRequested(connectedIndex, trimmed, style);
    setStatus(tr("Title changed to “%1”").arg(trimmed));
}

void Backend::trimConnectedClip(int connectedIndex, int in, int out, int newStart) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size() || newStart < 0)
        return;
    emit trimConnectedRequested(connectedIndex, in, out, newStart, m_rippleTrim);
}

void Backend::addTitle(const QString &text, const QVariantMap &style) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !hasVideo())
        return; // a title needs a storyline under it
    emit connectTitleRequested(trimmed, m_position,
                               qMax(2, qRound(m_fps * 4)), // ~4 s
                               style);
    setStatus(tr("Title “%1” added").arg(trimmed));
}

void Backend::setConnectedRect(int connectedIndex, double x, double y,
                               double w, double h) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const bool full = w <= 0.0 || h <= 0.0 || (x == 0.0 && y == 0.0
                                               && w == 1.0 && h == 1.0);
    setStatus(full ? tr("Connected clip back to full frame")
                   : tr("Picture in picture"));
    emit setConnectedRectRequested(connectedIndex, x, y, w, h);
}

void Backend::removeConnectedClip(int connectedIndex) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    emit removeConnectedRequested(connectedIndex);
}

void Backend::moveConnectedClip(int connectedIndex, int newStart) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size() || newStart < 0)
        return;
    emit moveConnectedRequested(connectedIndex, newStart);
}

void Backend::deleteRange(int start, int end) {
    if (!hasVideo() || start < 0 || end < start)
        return;
    setStatus(tr("Range deleted (%1)").arg(Interchange::durationText(end - start + 1, fps())));
    emit removeRangeRequested(start, end);
}

void Backend::rollEditOn(int track, int index, int delta) {
    if (track == 0) {
        rollEdit(index, delta);
        return;
    }
    if (m_exporting || m_viewingSource || track < 0 || track >= m_tracks.size() || index < 0 || delta == 0)
        return;
    emit rollOnRequested(track, index, delta);
}

void Backend::trimClipOn(int track, int index, int in, int out) {
    if (track < 0 || track >= qMax(1, m_tracks.size()) || index < 0)
        return;
    emit trimOnRequested(track, index, in, out);
}

void Backend::trimToPlayhead(int track, int index, bool end) {
    if (!hasVideo() || m_viewingSource || m_exporting || track < 0 || track >= m_tracks.size() || index < 0)
        return;
    emit trimToFrameRequested(track, index, m_position, end, m_rippleTrim);
}

void Backend::rollEdit(int timelineIndex, int delta) {
    if (m_exporting || m_viewingSource || timelineIndex < 0 || timelineIndex >= m_timeline.size() - 1
        || delta == 0)
        return;
    emit rollRequested(timelineIndex, delta);
}

void Backend::makeCompound(int start, int end) {
    if (!hasVideo() || start < 0 || end < start)
        return;
    emit makeCompoundRequested(start, end);
}

void Backend::makeCompoundSelection(const QVariantList &addresses) {
    const QVariantList addrs = plainVariant(addresses).toList();
    QSet<int> indices;
    QSet<QString> links;
    for (const QVariant &v : addrs) {
        const QVariantMap addr = v.toMap();
        if (addr.value(QStringLiteral("track"), -1).toInt() != 0)
            continue;
        const int index = addr.value(QStringLiteral("index"), -1).toInt();
        if (index < 0 || index >= m_timeline.size())
            return;
        indices.insert(index);
        const QString link = m_timeline.at(index).toMap().value(ClipKey::link).toString();
        if (!link.isEmpty()) links.insert(link);
    }
    if (indices.isEmpty()) {
        setStatus(tr("Select adjacent clips on V1 to make a nested clip."));
        return;
    }
    // Linked sound is selected with its picture, but stays on its audio track.
    for (const QVariant &v : addrs) {
        const QVariantMap addr = v.toMap();
        const int track = addr.value(QStringLiteral("track"), -1).toInt();
        if (track == 0) continue;
        bool mate = false;
        for (const QVariant &c : m_connected) {
            const QVariantMap entry = c.toMap();
            if (entry.value(ClipKey::track).toInt() == track
                && entry.value(ClipKey::index) == addr.value(QStringLiteral("index"))
                && entry.value(ClipKey::audioOnly).toBool()
                && links.contains(entry.value(ClipKey::link).toString())) {
                mate = true;
                break;
            }
        }
        if (!mate) {
            setStatus(tr("Select clips on V1 only; nested clips do not include other tracks."));
            return;
        }
    }
    const int first = *std::min_element(indices.cbegin(), indices.cend());
    const int last = *std::max_element(indices.cbegin(), indices.cend());
    if (indices.size() != last - first + 1) {
        setStatus(tr("Select adjacent V1 clips, or mark an In/Out range to include the clips between them."));
        return;
    }
    const QVariantMap tail = m_timeline.at(last).toMap();
    makeCompound(m_timeline.at(first).toMap().value(ClipKey::start).toInt(),
                 tail.value(ClipKey::start).toInt() + tail.value(ClipKey::frames).toInt() - 1);
}

void Backend::breakApartClip(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size()
        || !m_timeline.at(timelineIndex).toMap()
               .value(ClipKey::compound).toBool())
        return;
    emit breakApartRequested(timelineIndex);
}

void Backend::insertGapAtPlayhead() {
    if (!hasVideo())
        return;
    setStatus(tr("Gap inserted"));
    emit insertGapRequested(m_position, qMax(2, qRound(m_fps * 3))); // ~3 s
}

void Backend::closeGap(int track, int frame) {
    if (!hasVideo() || track < 0 || frame < 0)
        return;
    setStatus(tr("Gap deleted"));
    emit closeGapRequested(track, frame);
}

void Backend::deleteGaps() {
    if (!hasVideo())
        return;
    emit deleteGapsRequested();
}

void Backend::toggleClipDisabled(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const bool disabled = m_timeline.at(timelineIndex).toMap()
                              .value(ClipKey::disabled).toBool();
    setStatus(disabled ? tr("Clip enabled")
                       : tr("Clip disabled"));
    emit setDisabledRequested(timelineIndex, !disabled);
}

void Backend::bladeAtPlayhead() {
    if (!hasVideo())
        return;
    emit splitRequested(m_position);
}

void Backend::bladeAllAtPlayhead() {
    if (!hasVideo() || m_viewingSource || m_exporting) return;
    emit splitAllRequested(m_position);
}

void Backend::bladeAt(int track, int frame) {
    if (!hasVideo() || frame < 0 || track < 0)
        return;
    if (track == 0)
        emit splitRequested(frame);
    else
        emit splitOnRequested(track, frame);
}

void Backend::toggleDissolve(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    if (entry.value(ClipKey::mixOut).toInt() > 0) {
        emit removeDissolveRequested(timelineIndex);
        setStatus(tr("Dissolve removed"));
        return;
    }
    emit addTransitionRequested(0, timelineIndex, qMax(2, qRound(m_fps)), QString()); // ~1 s, across the cut
    if (timelineIndex + 1 >= m_timeline.size())
        return; // a dissolve needs a next clip: the edit says so
    setStatus(tr("Cross dissolve into %1")
                  .arg(m_timeline.at(timelineIndex + 1).toMap()
                           .value(QStringLiteral("name")).toString()));
}

void Backend::trimClip(int timelineIndex, int in, int out) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    emit trimRequested(timelineIndex, in, out, m_rippleTrim);
}

namespace {
// The frames of its source a clip lends the transitions it is in: those
// ahead of its in point (the transition into it) and past its out point (the
// one out of it). Its entry's in and out count them; the clip drawn does not.
int lentFrames(const QVariantList &list, int at) {
    const QVariantMap entry = list.at(at).toMap();
    int lent = MixShape::parts(entry).extA;
    if (entry.value(ClipKey::mixIn).toInt() > 0)
        for (const QVariant &v : list) {
            const QVariantMap before = v.toMap();
            if (before.value(ClipKey::track) == entry.value(ClipKey::track)
                && before.value(ClipKey::index).toInt() == entry.value(ClipKey::index).toInt() - 1)
                lent += MixShape::parts(before).extB;
        }
    return lent;
}
}

int Backend::durationNow(bool connected, int index) const {
    const QVariantList &list = connected ? m_connected : m_timeline;
    if (index < 0 || index >= list.size())
        return 0;
    const QVariantMap entry = list.at(index).toMap();
    return qMax(0, entry.value(ClipKey::out).toInt() - entry.value(ClipKey::in).toInt() + 1 - lentFrames(list, index));
}

int Backend::durationLimit(bool connected, int index) const {
    const QVariantList &list = connected ? m_connected : m_timeline;
    if (index < 0 || index >= list.size())
        return 0;
    const QVariantMap entry = list.at(index).toMap();
    // From the clip's own in point to the end of what it can show (less the
    // frames a transition on either cut goes on needing).
    return qMax(0, entry.value(ClipKey::length).toInt() - entry.value(ClipKey::in).toInt() - lentFrames(list, index));
}

bool Backend::changeDuration(bool connected, int index, int frames) {
    const QVariantList &list = connected ? m_connected : m_timeline;
    if (index < 0 || index >= list.size() || frames < 1)
        return false;
    const QVariantMap entry = list.at(index).toMap();
    const QString name = entry.value(ClipKey::name).toString();
    const int in = entry.value(ClipKey::in).toInt();
    // (As the clip is drawn: a transition's frames past its cuts are not its length.)
    const int current = durationNow(connected, index);
    const int limit = durationLimit(connected, index);
    if (frames > limit) {
        setStatus(limit > current
                      ? tr("%1 can be at most %2 long; its media ends there").arg(name, timecodeFor(limit, m_fps))
                      : tr("%1 can't be longer; its media ends at the clip's end").arg(name));
        if (limit <= current)
            return false;
        frames = limit;
    }
    if (frames == current)
        return false;
    const int out = entry.value(ClipKey::out).toInt() + frames - current;
    if (connected)
        emit trimConnectedRequested(index, in, out, entry.value(ClipKey::start).toInt(), false);
    else
        emit trimRequested(index, in, out, true); // the storyline closes up behind it
    setStatus(tr("%1 is now %2 long").arg(name, timecodeFor(frames, m_fps)));
    return true;
}

void Backend::slipClip(int timelineIndex, int delta) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size() || delta == 0)
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    const int in = entry.value(ClipKey::in).toInt() + delta;
    const int out = entry.value(ClipKey::out).toInt() + delta;
    if (in < 0 || out >= entry.value(ClipKey::length).toInt())
        return;
    // A ripple resize to the same length ripples nothing; the normal mode
    // would open a gap to hold the old start.
    emit trimRequested(timelineIndex, in, out, true);
    setStatus(tr("Slipped %1 by %2%3")
                  .arg(entry.value(ClipKey::name).toString())
                  .arg(delta < 0 ? QStringLiteral("−") : QStringLiteral("+"))
                  .arg(Interchange::durationText(qAbs(delta), fps())));
}

namespace {
const QStringList &takesNames() {
    static const QStringList names{QStringLiteral("both"), QStringLiteral("picture"), QStringLiteral("sound")};
    return names;
}
}

QString Backend::editTakes() const { return takesNames().value(m_editTakes); }

void Backend::setEditTakes(const QString &takes) {
    const int index = takesNames().indexOf(takes);
    if (index < 0 || index == m_editTakes)
        return;
    m_editTakes = index;
    if (!qApp || !qApp->property("omaedit.agent").toBool())
        QSettings().setValue(QStringLiteral("edit/takes"), takes);
    emit editTakesChanged();
    setStatus(index == EditTakes::Picture ? tr("Edits use the picture only")
              : index == EditTakes::Sound ? tr("Edits use the sound only")
                                          : tr("Edits use picture and sound"));
}

// A clip with no sound cannot give its sound alone, nor a recording its
// picture: the edit takes the clip as it is, never nothing. A multicam clip's
// sound follows its own rule (which angle is heard), so it is taken whole.
int Backend::takesFor(int libraryIndex, const QString &asked) const {
    const QVariantMap entry = m_library.value(libraryIndex).toMap();
    // One edit may ask for itself; a word that is none of the three is no choice.
    const int choice = takesNames().contains(asked) ? int(takesNames().indexOf(asked)) : m_editTakes;
    if (choice == EditTakes::Both || entry.value(QStringLiteral("multicam")).toBool())
        return EditTakes::Both;
    const bool sound = entry.value(QStringLiteral("hasAudio"), true).toBool();
    const bool picture = !entry.value(QStringLiteral("audioOnly")).toBool()
                         && entry.value(QStringLiteral("hasVideo"), true).toBool();
    if (choice == EditTakes::Sound)
        return sound && picture ? EditTakes::Sound : EditTakes::Both;
    return picture && sound ? EditTakes::Picture : EditTakes::Both;
}

QString Backend::editTakesFor(int libraryIndex) const { return takesNames().value(takesFor(libraryIndex)); }

QString Backend::takenName(const QString &name, int takes) {
    return takes == EditTakes::Picture ? tr("%1 (picture only)").arg(name)
           : takes == EditTakes::Sound ? tr("%1 (sound only)").arg(name) : name;
}

void Backend::setRippleTrim(bool on) {
    if (on == m_rippleTrim)
        return;
    m_rippleTrim = on;
    emit rippleTrimChanged();
    setStatus(on ? tr("Trim mode: ripple")
                 : tr("Trim mode: normal"));
}

void Backend::moveClip(int from, int to) {
    if (from < 0 || to < 0 || from >= m_timeline.size() || to >= m_timeline.size())
        return;
    emit moveRequested(from, to);
}
