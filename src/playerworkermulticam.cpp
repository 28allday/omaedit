#include "playerworker_p.h"

namespace {

// Per-frame mono peak envelope, capped so a long take never stalls the
// worker for minutes (audio-only decodes, no get_image).
QList<float> envelopeOf(MediaPlayer &player, double rate, double factor) {
    QList<float> native,env;
    const double effective=player.fps()*factor;
    const int frames=qMin(player.frameCount(),qMax(1,int(std::ceil(effective*180))));
    native.reserve(frames);
    for (int i = 0; i < frames; ++i) {
        const QList<float> lr = player.audioPeaksAt(i);
        native.append(qMax(lr.value(0, 0.0f), lr.value(1, 0.0f)));
    }
    const int count=qMin(int(rate*180),int(std::ceil(frames*rate/effective)));
    env.reserve(count);
    for(int i=0;i<count;++i) {
        const double at=i*effective/rate;const int first=qBound(0,int(std::floor(at)),frames-1);
        const float mix=float(at-std::floor(at));
        env.append(native[first]*(1.f-mix)+native[qMin(first+1,frames-1)]*mix);
    }
    float mean = 0.0f;
    for (float v : env)
        mean += v;
    mean = env.isEmpty() ? 0.0f : mean / env.size();
    for (float &v : env)
        v -= mean;
    return env;
}

// Lag L maximising the cross-correlation of a and b, where b[t] lines up
// with a[t + L]: L > 0 = b starts L frames after a. Normalised by the
// WHOLE signals' energy, not the overlap's — two flat stretches would
// otherwise correlate perfectly — and only lags with a decent overlap
// count, so a tail's chance match never wins.
int bestLag(const QList<float> &a, const QList<float> &b, int maxLag, double *score) {
    int best = 0;
    double bestScore = -2.0;
    double saa = 0.0, sbb = 0.0;
    for (float v : a)
        saa += double(v) * v;
    for (float v : b)
        sbb += double(v) * v;
    if (saa <= 0.0 || sbb <= 0.0) {
        if (score)
            *score = bestScore;
        return 0;
    }
    const int minOverlap = qMax(8, qMin(a.size(), b.size()) / 4);
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double sab = 0.0;
        int n = 0;
        for (int t = qMax(0, lag); t < a.size() && t - lag < b.size(); ++t) {
            sab += double(a.at(t)) * b.at(t - lag);
            ++n;
        }
        if (n < minOverlap)
            continue;
        const double s = sab / std::sqrt(saa * sbb);
        if (s > bestScore) {
            bestScore = s;
            best = lag;
        }
    }
    if (score)
        *score = bestScore;
    return best;
}
}

// ── Multicam ─────────────────────────────────────────────────────────────
void PlayerWorker::tagNextPlacement(const QVariantMap &tag) {
    m_pendingTag = tag;
}

void PlayerWorker::applyPendingTagAt(int track, int index) {
    if (m_pendingTag.isEmpty())
        return;
    const auto markers=m_pendingTag.value("markers").toList();
    if(index>=0 && !markers.isEmpty())m_timeline.setClipMarkersAt(track,index,markers);
    const QString json = m_pendingTag.value(QStringLiteral("mcam")).toString();
    if (!json.isEmpty() && index >= 0
        && m_timeline.setMulticamAt(track, index, json, m_pendingTag.value(QStringLiteral("angle")).toInt()))
        emit multicamPlaced();
    m_pendingTag.clear();
}

void PlayerWorker::applyPendingTag(int track, int position) {
    if (m_pendingTag.isEmpty())
        return;
    applyPendingTagAt(track, m_timeline.clipIndexAt(track, position));
    m_pendingTag.clear();
}

void PlayerWorker::switchAngle(int track, int index, int angle) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const auto clip=m_timeline.entryMapOn(track,index);
    const auto camera=QJsonDocument::fromJson(clip.value(ClipKey::mcam).toString().toUtf8()).object();
    if(m_timeline.angleSwitchAudioLocked(track,index)) {
        emit editRefused(tr("Unlock the linked audio track before changing angles."));return;
    }
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.switchAngleOn(track, index, angle)) {
        if(camera.value("version").toInt()==2 && clip.value(ClipKey::mcamAngle).toInt()!=angle)
            emit editRefused(m_timeline.trackLocked(track)
                ?tr("%1 is locked; the angle did not change.").arg(m_timeline.trackName(track))
                :tr("That angle has no footage for this part of the clip; the angle did not change."));
        return;
    }
    recordEdit(before, true, tr("Switch angle"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::cutToAngle(int track, int index, int frame, int angle,
                              bool keepAudio) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    const QVariantList tracksBefore = m_timeline.trackFlagsVariant();
    // The cut blades the linked audio too (fresh link for the tails);
    // with audio-follow-video off the engine's settle pass leaves the
    // tail's sound on the angle it was.
    Q_UNUSED(keepAudio); // the engine holds the setting (setAudioFollowsVideo)
    const QVariantMap clip = m_timeline.entryMapOn(track, index);
    if(m_timeline.angleSwitchAudioLocked(track,index)) {
        emit editRefused(tr("Unlock the linked audio track before changing angles."));return;
    }
    const int start = clip.value(ClipKey::start).toInt();
    bool ok = false;
    if (frame < 0) {
        // No cut: the whole segment the clip is part of changes angle.
        ok = m_timeline.switchAngleOn(track, index, angle);
    } else if(QJsonDocument::fromJson(clip.value(ClipKey::mcam).toString().toUtf8()).object().value("version").toInt()==2
              || Timeline::angleWithoutFootage(clip) || frame == start) {
        ok=m_timeline.cutToAngleOn(track,index,frame,angle);
    } else if (frame > start && frame < start + clip.value(ClipKey::frames).toInt()) {
        ok = m_timeline.splitOn(track, frame);
        const int right = m_timeline.clipIndexContaining(track, frame);
        ok = ok && right >= 0 && m_timeline.switchAngleOn(track, right, angle);
    }
    if (!ok) {
        if (!putBack(before, tracksBefore))
            return;
        // The Backend announced the cut; say why nothing changed instead.
        const bool inTransition = frame > start && (clip.value(ClipKey::mixIn).toInt() > 0 || clip.value(ClipKey::mixOut).toInt() > 0);
        emit editRefused(m_timeline.trackLocked(track)
                             ? tr("%1 is locked; the angle did not change.").arg(m_timeline.trackName(track))
                             : inTransition
                             ? tr("A transition holds an edge of the clip; it was not cut to another angle.")
                             : tr("That angle has no footage for this part of the clip; the angle did not change."));
        return;
    }
    recordEdit(before, true, tr("Cut to angle"));
    announceTimeline();
    refreshFrame();
    // Black where the angle has no footage: said, with the angle's name.
    const auto without = [angle](const QVariantList &clips) {
        int count = 0;
        for (const QVariant &v : clips) count += Timeline::angleWithoutFootage(v.toMap()) && v.toMap().value(ClipKey::mcamAngle).toInt() == angle;
        return count;
    };
    if (without(m_timeline.entries()) > without(before)) {
        const QList<Timeline::Angle> angles = Timeline::anglesOf(clip.value(ClipKey::mcam).toString());
        emit editNoted(tr("%1 has no footage for part of this segment; it shows black there.").arg(angles.value(angle).name));
    }
}

void PlayerWorker::setMulticamSound(int track, int index, int source) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantMap clip = m_timeline.entryMapOn(track, index);
    const QVariantMap camera = QJsonDocument::fromJson(clip.value(ClipKey::mcam).toString().toUtf8()).toVariant().toMap();
    if (camera.isEmpty() || clip.value(ClipKey::audioOnly).toBool())
        return;
    const QString name = source < 0 ? QString() : Timeline::soundSourcesIn(camera).value(source).toMap().value(QStringLiteral("name")).toString();
    const QVariantList before = m_timeline.entries();
    const QVariantList tracksBefore = m_timeline.trackFlagsVariant();
    Timeline::SoundChange done;
    if (!m_timeline.setSoundSourceOn(track, index, source, &done)) {
        if (!putBack(before, tracksBefore))
            return;
        emit editRefused(done.refused);
        return;
    }
    if (done.changed == 0) {
        emit editRefused(source < 0 ? tr("The sound follows the picture already") : tr("The sound is from %1 already").arg(name));
        return;
    }
    recordEdit(before, true, tr("Sound from"));
    announceTimeline();
    refreshFrame();
    const QString clipName = camera.value(QStringLiteral("name")).toString();
    QString said = source < 0 ? tr("“%1”: the sound follows the picture now, on %n piece(s).", nullptr, done.changed).arg(clipName)
                              : tr("“%1”: sound from %2 now, on %n piece(s).", nullptr, done.changed).arg(clipName, name);
    if (done.removed > 0)
        said += QLatin1Char(' ') + tr("%n piece(s) left without sound: theirs was removed or unlinked.", nullptr, done.removed);
    if (done.retimed > 0)
        said += QLatin1Char(' ') + tr("%n retimed or held piece(s) have no sound now: the source does not cover all of them.", nullptr, done.retimed);
    emit editNoted(said);
}

void PlayerWorker::collapseMulticam(const QString &sequenceId) {
    if (exportActive())
        return;
    int index = -1;
    for (int i = 0; i < m_sequences.size(); ++i)
        if (m_sequences.at(i).id == sequenceId) index = i;
    if (index < 0) {
        emit editRefused(tr("That timeline is no longer in the project; nothing was collapsed."));
        return;
    }
    const QString name = m_sequences.at(index).name;
    const auto said = [&](const Timeline::Collapse &done) {
        QString text = tr("“%1”: multicam collapsed to ordinary clips, %n piece(s).", nullptr, done.pieces).arg(name);
        if (done.unlinked > 0)
            text += QLatin1Char(' ') + tr("%n sound clip(s) left unlinked: the sound is not its picture's own.", nullptr, done.unlinked);
        return text;
    };
    const auto none = [&] { return tr("“%1” has no multicam clips; nothing was collapsed.").arg(name); };
    if (index == m_activeSequence) {
        const QVariantList before = m_timeline.entries();
        const QVariantList tracksBefore = m_timeline.trackFlagsVariant();
        Timeline::Collapse done;
        if (!m_timeline.collapseMulticam(&done)) {
            if (!putBack(before, tracksBefore))
                return;
            emit editRefused(done.pieces == 0 ? none()
                             : done.lockedTrack >= 0 ? tr("%1 is locked; nothing was collapsed.").arg(m_timeline.trackName(done.lockedTrack))
                             : tr("The timeline could not be rebuilt; nothing was collapsed."));
            return;
        }
        // (Nothing to settle: no picture and no sound was moved or changed.)
        recordEdit(before, false, tr("Collapse Multicam"));
        announceTimeline();
        refreshFrame();
        emit editNoted(said(done));
        return;
    }
    // A timeline that is not open: its clips as they are kept.
    QVariantList clips = m_sequences.at(index).snapshot;
    QVariantMap state;
    const bool hasState = !clips.isEmpty() && clips.last().toMap().contains(QStringLiteral("omavid:markers"));
    if (hasState)
        state = clips.takeLast().toMap();
    const std::vector<Timeline::TrackFlags> flags = Timeline::trackFlagsFromVariant(state.value(QStringLiteral("omavid:tracks")).toList());
    QSet<int> locked;
    for (size_t track = 0; track < flags.size(); ++track)
        if (flags[track].locked) locked.insert(int(track));
    Timeline::Collapse done = Timeline::multicamCollapsed(clips, locked);
    if (done.pieces == 0) {
        emit editRefused(none());
        return;
    }
    if (done.lockedTrack >= 0) {
        // Named as its header names it: its own name, or V1… and A1… in order.
        const Timeline::TrackFlags &track = flags[size_t(done.lockedTrack)];
        QString trackName = track.name;
        if (trackName.isEmpty()) {
            int ordinal = 0;
            for (int i = 0; i <= done.lockedTrack; ++i) ordinal += flags[size_t(i)].kind == track.kind;
            trackName = (track.kind == Timeline::Track::Audio ? QStringLiteral("A%1") : QStringLiteral("V%1")).arg(ordinal);
        }
        emit editRefused(tr("%1 of “%2” is locked; nothing was collapsed.").arg(trackName, name));
        return;
    }
    if (hasState)
        done.entries.append(state);
    const QVariantMap beforeState = sourceAttributeState({});
    QVariantMap afterState = beforeState;
    QVariantList sequences = afterState.value(QStringLiteral("sequences")).toList();
    QVariantMap changed = sequences.value(index).toMap();
    changed[QStringLiteral("snapshot")] = done.entries;
    sequences[index] = changed;
    afterState[QStringLiteral("sequences")] = sequences;
    QVariantList before = snapshotNow();
    QVariantMap sentinel = before.last().toMap();
    sentinel[QStringLiteral("omavid:sourceState")] = beforeState;
    sentinel[QStringLiteral("omavid:undoLabel")] = tr("Collapse Multicam");
    before.last() = sentinel;
    // As Change Clip Attributes does: the history goes in first, and comes
    // out again if the timelines cannot take the change.
    const auto redo = m_redoStack;
    m_undoStack.append(before);
    m_redoStack.clear();
    const QString problem = restoreSourceAttributeState(afterState, true, false);
    if (!problem.isEmpty()) {
        m_undoStack.removeLast();
        m_redoStack = redo;
        emit editRefused(problem);
        return;
    }
    m_lastEditGesture = 0;
    ++m_editRevision;
    while (m_undoStack.size() > 100)
        m_undoStack.removeFirst();
    emit editNoted(said(done));
}

void PlayerWorker::syncByAudio(const QStringList &paths,double syncFps,const QVariantMap &sources) {
    QList<int> offsets;
    if (paths.size() < 2) {
        emit clipsSynced(paths, offsets, QStringLiteral("Pick at least two clips to sync."));
        return;
    }
    QList<QList<float>> envelopes;
    double fps=syncFps;
    for (const QString &path : paths) {
        MediaPlayer player;
        if (!player.open(path)) {
            emit clipsSynced(paths, offsets,
                             QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        if(fps<=0)fps=player.fps()>0?player.fps():25.;
        const double factor=Timeline::interpretationFactor(sources.value(path).toMap());
        if(!std::isfinite(fps) || fps<=0 || fps>240. || !std::isfinite(factor) || factor<=0
            || !std::isfinite(player.fps()*factor) || player.fps()*factor<=0 || player.fps()*factor>240.
            || player.frameCount()<1) {
            emit clipsSynced(paths,offsets,QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName()));return;
        }
        envelopes.append(envelopeOf(player,fps,factor)); // first 3 interpreted minutes at one clock
    }
    // Everything against the first clip; a lag is how far each starts after
    // it. Shift so the earliest starter sits at 0.
    QList<int> lags{0};
    const int maxLag = int(fps * 120);
    for (int i = 1; i < envelopes.size(); ++i) {
        double score = 0.0;
        const int lag = bestLag(envelopes.at(0), envelopes.at(i), maxLag, &score);
        if (score < 0.2) {
            emit clipsSynced(paths, offsets,
                             QStringLiteral("No waveform match between %1 and %2.")
                                 .arg(QFileInfo(paths.at(0)).fileName(),
                                      QFileInfo(paths.at(i)).fileName()));
            return;
        }
        lags.append(lag);
    }
    const int earliest = *std::min_element(lags.cbegin(), lags.cend());
    for (int lag : lags)
        offsets.append(lag - earliest);
    emit clipsSynced(paths, offsets, QString());
}

void PlayerWorker::decodeAngles(const QStringList &paths, const QList<int> &frames,
                                int seq) {
    // Off this thread: a grid of pictures made here held every frame of
    // playback up behind it (four 1080p angles played 2 pictures a second).
    // The decoders belong to the grid's one thread; the Backend asks for one
    // grid at a time. Each angle is drawn as the source viewer draws a clip:
    // by the engine, alone on a timeline of its own shape with the project's
    // colour settings and its own input colour space, at the size of a cell
    // and for the viewer's display, so an angle looks in the grid as it will
    // on the programme. Without a display (no viewer): its working frame,
    // put out as Rec.709.
    QList<QVariantMap> managements;
    for (const QString &path : paths) managements.append(m_timeline.linearSourceManagement(path));
    m_anglePool.start([this, bench = m_angleBench, paths, frames, seq, managements, project = m_timeline.projectSettings(),
                       sources = m_timeline.mediaSources(), mode = m_timeline.proxyMode(), display = m_playbackDisplay] {
        // Keep decoders only for the angles asked about — a new multicam clip
        // under the playhead swaps the set.
        for (auto it = bench->players.begin(); it != bench->players.end();) {
            if (!paths.contains(it.key()))
                it = bench->players.erase(it);
            else
                ++it;
        }
        QList<QImage> out;
        for (int i = 0; i < paths.size(); ++i) {
            const int frame = frames.value(i, -1);
            if (frame < 0) {
                out.append(QImage());
                continue;
            }
            const QString &path = paths.at(i);
            std::shared_ptr<MediaPlayer> &player = bench->players[path];
            if (!player) {
                player = std::make_shared<MediaPlayer>();
                if (!player->open(path)) {
                    player.reset();
                    bench->players.remove(path);
                    out.append(QImage());
                    continue;
                }
            }
            player->setInputTransform(sources.value(path).toMap());
            player->setWorkingManagement(managements.value(i));
            player->drawOnEngine(path, project, sources, mode);
            const int width = qMin(480, player->width());
            if (display.isValid() && width > 0)
                player->setViewerDraw(QSize(width, qMax(1, qRound(double(width) * player->height() / player->width()))), display);
            else
                player->setViewerDraw({}, {});
            const MediaPlayer::Decoded decoded = player->decodeAt(qMin(frame, player->frameCount() - 1), false);
            QImage image = decoded.working.isNull() ? decoded.image : decoded.picture();
            if (!image.isNull() && image.width() > 480)
                image = image.scaledToWidth(480, Qt::SmoothTransformation);
            out.append(image);
        }
        emit anglesReady(out, seq);
    });
}
