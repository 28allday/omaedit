#include "playerworker_p.h"

void PlayerWorker::removeStab(int track, int index) {
    if (staleEdit())
        return;
    if (exportActive())
        return;
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setStabAt(track, index, QString(), 0, 0))
        return;
    recordEdit(before, true, tr("Remove stabilisation"));
    announceTimeline();
    refreshFrame();
}

void PlayerWorker::stabilize(int track, int index, int shakiness, int smoothing) {
    if (exportActive())
        return;
    QVariantMap clip;
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap m = v.toMap();
        if (m.value(ClipKey::track).toInt() == track
            && m.value(ClipKey::index).toInt() == index)
            clip = m;
    }
    const QString path = clip.value(ClipKey::path).toString();
    if(clip.contains(ClipKey::retime)) {
        emit frameError(tr("Render this clip in place before analysing stabilisation with speed segments."));
        emit stabProgress(track,index,-1);return;
    }
    if (path.isEmpty() || path.startsWith(QStringLiteral("color:"))
        || clip.value(ClipKey::gap).toBool()
        || !clip.value(ClipKey::title).toString().isEmpty()
        || clip.value(ClipKey::audioOnly).toBool()
        // A retimed clip's in/out are timewarp frames: the analysis would
        // read the wrong stretch of the source. (Its cache key would also
        // collide with the 100 % clip's.)
        || !qFuzzyCompare(clip.value(ClipKey::speed, 1.0).toDouble()
                          * Timeline::interpretationFactor(m_timeline.mediaSources().value(path).toMap()), 1.0)) {
        emit stabProgress(track, index, -1);
        return;
    }
    shakiness = qBound(1, shakiness, 10);
    smoothing = qBound(0, smoothing, 100);
    const int in = clip.value(ClipKey::in).toInt();
    const int out = clip.value(ClipKey::out).toInt();
    // The clip will point at the analysis: kept, not cached.
    const QDir dir(StoragePreferences::keptMediaRoot()
                   + QStringLiteral("/stabilise"));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    // The analysis sees the clip's picture as the timeline's frame shows it
    // (fitted, filled or at its own size, read natively): a picture of
    // another shape is not stretched over the frame. Its first frame says
    // what that picture is.
    auto source = std::make_unique<render::MediaSource>();
    QSize seen;
    {
        QString why;
        render::Picture first;
        if (m_timeline.fps() <= 0 || !m_timeline.profile() || !source->open(path, &why)
            || !source->read(in / m_timeline.fps(), first, &why) || first.width <= 0 || first.height <= 0) {
            emit stabProgress(track, index, -1);
            return;
        }
        const auto *profile = m_timeline.profile();
        const double frameAspect = profile->sample_aspect_den() > 0 ? double(profile->sample_aspect_num()) / profile->sample_aspect_den() : 1.0;
        seen = render::analysisFrame(clip.value(ClipKey::scaling).toString(),
                                     m_timeline.projectSettings().value(QStringLiteral("scaling")).toString(), first.width, first.height,
                                     first.pixelAspect / (frameAspect > 0 ? frameAspect : 1.0), QSize(profile->width(), profile->height()));
    }
    // (The frame is part of the key: an analysis made of another picture
    // size, or of the picture stretched as it once was, is not taken for this.
    // So are the timeline's clock and the file itself: another rate, or
    // another file at the same path, is another analysis.)
    const QFileInfo file(path);
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((path + QLatin1Char('|') + QString::number(in) + QLatin1Char('|')
                                  + QString::number(out) + QLatin1Char('|') + QString::number(shakiness)
                                  + QStringLiteral("|%1x%2").arg(seen.width()).arg(seen.height())
                                  + QLatin1Char('|') + QString::number(m_timeline.fps(), 'g', 17)
                                  + QLatin1Char('|') + QString::number(file.size())
                                  + QLatin1Char('|') + QString::number(file.lastModified().toMSecsSinceEpoch()))
                                     .toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(20));
    const QString results = render::analysisFileName(dir.absoluteFilePath(key), seen);
    if (QFileInfo::exists(results)) { // analysed before: apply at once
        const QVariantList before = m_timeline.entries();
        if (m_timeline.setStabAt(track, index, results, shakiness, smoothing)) {
            recordEdit(before, true, tr("Stabilise"));
            announceTimeline();
            refreshFrame();
            announceStabilised(track, index, results, smoothing);
        } else {
            emit stabProgress(track, index, -1);
        }
        return;
    }
    auto job = std::make_unique<StabJob>();
    job->track = track;
    job->index = index;
    job->shakiness = shakiness;
    job->smoothing = smoothing;
    job->path = path;
    job->results = results;
    job->in = in;
    job->out = out;
    job->pos = in;
    job->fps = m_timeline.fps();
    job->source = std::move(source);
    job->analysis = std::make_unique<render::MotionAnalysis>();
    // Written as it goes: a quit mid-analysis must not leave a half file
    // that the next run takes for a finished one.
    QString why;
    if (job->fps <= 0 || !job->analysis->open(results + QStringLiteral(".part"), shakiness, seen, &why)) {
        emit stabProgress(track, index, -1);
        return;
    }
    m_stabJob = std::move(job);
    emit stabProgress(track, index, 0);
    scheduleStabWork();
}

// Tracking operates on an isolated, ungraded cut; clip-relative frames
// preserve trims and retiming, and each timer turn leaves the UI responsive.
void PlayerWorker::trackWindow(int track, int index, int node, double, double, double, double) {
    trackWindowRange(track, index, node, 0, 0, 1, false);
}
void PlayerWorker::trackWindowRange(int track, int index, int node, int windowIndex, int frame, int direction,
                                    bool singleFrame, const QString &nodePath) {
    if (exportActive())
        return;
    QVariantMap clip;
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index)
            clip = e;
    }
    const QVariantMap grade = m_timeline.gradeAt(track, index);
    const QVariantList nodes = grade.value("nodes").toList();
    if (clip.isEmpty() || clip.value(ClipKey::offline).toBool() || clip.value(ClipKey::audioOnly).toBool() ||
        m_timeline.trackLocked(track) || node < 0 || node >= nodes.size() || frame < 0 ||
        frame >= clip.value(ClipKey::frames).toInt()) {
        emit gradeVersionFinished(QStringLiteral("Select an online, unlocked video clip and a correction window."));
        emit trackProgress(track, index, node, -1);
        return;
    }
    const QString targetPath = nodePath.isEmpty() ? nodes[node].toMap().value("id").toString() : nodePath;
    const QVariantMap correction = GradeEngine::nodeAt(grade, targetPath);
    const QVariantList extras = correction.value("windows").toList();
    if (windowIndex < 0 || windowIndex > extras.size()) {
        emit trackProgress(track, index, node, -1);
        return;
    }
    const QVariantMap window = windowIndex == 0 ? correction.value("window").toMap() : extras[windowIndex - 1].toMap();
    if (!window.value("enabled").toBool()) {
        emit gradeVersionFinished(QStringLiteral("Enable the selected shape mask first."));
        return;
    }
    dropTrackJob();
    setSpeed(0);
    auto job = std::make_unique<TrackJob>();
    job->track = track;
    job->index = index;
    job->node = node;
    job->window = windowIndex;
    job->nodeId = targetPath;
    job->sequenceId = m_sequences[m_activeSequence].id;
    job->clip = clip;
    job->originalWindow = window;
    job->start = frame;
    job->step = direction < 0 ? -1 : 1;
    job->end = singleFrame ? std::clamp(frame + job->step, 0, clip.value(ClipKey::frames).toInt() - 1)
                           : (job->step > 0 ? clip.value(ClipKey::frames).toInt() - 1 : 0);
    if (job->start == job->end) {
        emit gradeVersionFinished(QStringLiteral("There are no more frames in that direction."));
        return;
    }
    job->timeline = std::make_unique<Timeline>();
    QVariantMap settings = m_timeline.projectSettings();
    settings["custom"] = true;
    const double scale = std::min(1., 320. / std::max(1, m_timeline.width()));
    settings["width"] = std::max(16, int(m_timeline.width() * scale) / 2 * 2);
    settings["height"] = std::max(16, int(m_timeline.height() * scale) / 2 * 2);
    job->timeline->setProjectSettings(settings);
    for (const char *key :
         {"grade", "gradeVersions", "gradeVersion", "mixIn", "mixOut", "mixAlign", "mixExtA", "mixExtB"})
        clip.remove(key);
    if(m_timeline.linearRendering()) {
        auto inputGrade=Timeline::defaultGrade();inputGrade["nodes"]=QVariantList{};
        inputGrade["management"]=m_timeline.linearSourceManagement(clip.value(ClipKey::path).toString());clip[ClipKey::grade]=inputGrade;
    }
    clip[ClipKey::track] = 0;
    clip[ClipKey::lane] = 0;
    clip[ClipKey::index] = 0;
    clip[ClipKey::start] = 0;
    clip[ClipKey::gapBefore] = 0;
    if (!job->timeline->restore({clip})) {
        emit trackProgress(track, index, node, -1);
        return;
    }
    if(m_timeline.linearRendering())job->timeline->profile()->set_sample_aspect(m_timeline.profile()->sample_aspect_num(),m_timeline.profile()->sample_aspect_den());
    // Trackers follow the encoded picture, as the eye sees it.
    const QImage image = job->timeline->decodeAt(frame, false).picture();
    QVariantMap startWindow = window;
    const QVariantList oldTrack = window.value("track").toList();
    for (const char *key : {"x", "y", "w", "h"}) {
        QVariantMap a, b;
        for (const QVariant &v : oldTrack) {
            const auto point = v.toMap();
            if (!point.contains(key))
                continue;
            const int f = point.value("frame").toInt();
            if (f <= frame && (a.isEmpty() || f > a.value("frame").toInt()))
                a = point;
            if (f >= frame && (b.isEmpty() || f < b.value("frame").toInt()))
                b = point;
        }
        if (a.isEmpty())
            a = b;
        if (b.isEmpty())
            b = a;
        if (!a.isEmpty()) {
            const double t =
                a.value("frame") == b.value("frame")
                    ? 0.
                    : double(frame - a.value("frame").toInt()) / (b.value("frame").toInt() - a.value("frame").toInt());
            startWindow[key] = a.value(key).toDouble() + (b.value(key).toDouble() - a.value(key).toDouble()) * t;
        }
    }
    const double x = startWindow.value("x", .5).toDouble(), y = startWindow.value("y", .5).toDouble(),
                 w = startWindow.value("w", .4).toDouble(), h = startWindow.value("h", .4).toDouble();
    if (!job->tracker.start(image, QRectF(x - w / 2, y - h / 2, w, h))) {
        emit trackProgress(track, index, node, -1);
        emit gradeVersionFinished(job->tracker.error());
        return;
    }
    job->positions.append(QVariantMap{{"frame", frame}, {"x", x}, {"y", y}, {"confidence", 1.}});
    job->pos = frame + job->step;
    m_trackJob = std::move(job);
    emit trackProgress(track, index, node, 0);
    scheduleTrackWork();
}
void PlayerWorker::scheduleTrackWork() {
    if (m_trackScheduled || !m_trackJob)
        return;
    m_trackScheduled = true;
    QTimer::singleShot(0, this, &PlayerWorker::processTrackFrame);
}
void PlayerWorker::dropTrackJob() {
    if (!m_trackJob)
        return;
    const int t = m_trackJob->track, i = m_trackJob->index, n = m_trackJob->node;
    m_trackJob.reset();
    emit trackProgress(t, i, n, -2);
}
void PlayerWorker::stopWindowTracking(bool keep) {
    finishWindowTracking(keep, keep ? QStringLiteral("Tracking stopped; completed positions kept")
                                    : QStringLiteral("Tracking cancelled"));
}
void PlayerWorker::processTrackFrame() {
    m_trackScheduled = false;
    if (!m_trackJob)
        return;
    if (exportActive()) {
        QTimer::singleShot(200, this, &PlayerWorker::scheduleTrackWork);
        return;
    }
    TrackJob &job = *m_trackJob;
    const QImage image = job.timeline->decodeAt(job.pos, false).picture();
    QPointF centre;
    double confidence = 0;
    if (!job.tracker.next(image, &centre, &confidence)) {
        finishWindowTracking(true, job.tracker.error());
        return;
    }
    job.positions.append(
        QVariantMap{{"frame", job.pos}, {"x", centre.x()}, {"y", centre.y()}, {"confidence", confidence}});
    if (job.pos == job.end) {
        finishWindowTracking(true);
        return;
    }
    job.pos += job.step;
    emit trackProgress(
        job.track, job.index, job.node,
        std::clamp(int(100. * std::abs(job.pos - job.start) / (std::abs(job.end - job.start) + 1)), 0, 99));
    scheduleTrackWork();
}
void PlayerWorker::finishWindowTracking(bool keep, const QString &message) {
    if (!m_trackJob)
        return;
    auto job = std::move(m_trackJob);
    if (!keep || job->positions.size() < 2) {
        emit trackProgress(job->track, job->index, job->node, -2);
        if (!message.isEmpty())
            emit gradeVersionFinished(message);
        return;
    }
    QVariantMap clip;
    for (const QVariant &v : m_timeline.entries()) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() == job->track && e.value(ClipKey::index).toInt() == job->index)
            clip = e;
    }
    bool valid = m_sequences[m_activeSequence].id == job->sequenceId;
    for (const char *key : {"path", "in", "out", "speed", "retime", "freeze", "sourceMeta", "frameBlend"})
        valid &= clip.value(key) == job->clip.value(key);
    QVariantMap grade = m_timeline.gradeAt(job->track, job->index);
    QVariantList nodes = grade.value("nodes").toList();
    valid &= job->node >= 0 && job->node < nodes.size();
    QVariantMap n = GradeEngine::nodeAt(grade, job->nodeId);
    valid &= !n.isEmpty();
    QVariantList extras = n.value("windows").toList();
    QVariantMap window = job->window == 0 ? n.value("window").toMap() : extras.value(job->window - 1).toMap();
    valid &= window == job->originalWindow;
    if (!valid) {
        emit trackProgress(job->track, job->index, job->node, -1);
        emit gradeVersionFinished(QStringLiteral("The clip or window changed while tracking. Its edits were kept."));
        return;
    }
    QMap<int, QVariantMap> positions;
    for (const QVariant &v : window.value("track").toList())
        positions[v.toMap().value("frame").toInt()] = v.toMap();
    for (const QVariant &v : job->positions)
        positions[v.toMap().value("frame").toInt()] = v.toMap();
    QVariantList output;
    for (const QVariantMap &p : positions)
        output << p;
    window["track"] = output;
    if (job->window == 0)
        n["window"] = window;
    else {
        extras[job->window - 1] = window;
        n["windows"] = extras;
    }
    GradeEngine::replaceNode(&grade, job->nodeId, n);
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setGradeAt(job->track, job->index, grade)) {
        emit trackProgress(job->track, job->index, job->node, -1);
        return;
    }
    recordEdit(before, true, tr("Track colour window"));
    announceTimeline();
    refreshFrame();
    emit trackProgress(job->track, job->index, job->node, 100);
    if (!message.isEmpty())
        emit gradeVersionFinished(message);
}
void PlayerWorker::scheduleStabWork() {
    if (m_stabScheduled || !m_stabJob)
        return;
    m_stabScheduled = true;
    QTimer::singleShot(0, this, &PlayerWorker::processStabFrame);
}

void PlayerWorker::processStabFrame() {
    m_stabScheduled = false;
    if (!m_stabJob)
        return;
    StabJob &job = *m_stabJob;
    // A few frames per pass: the analysis reads every frame in order and
    // writes the transform file as it goes.
    for (int n = 0; n < 4 && job.pos <= job.out; ++n) {
        // The source's picture shown at this timeline frame.
        render::Picture picture;
        QString why;
        if (!job.source->read(job.pos / job.fps, picture, &why) || !job.analysis->add(picture.frame, &why)) {
            const int track = job.track, index = job.index;
            const QString part = job.results + QStringLiteral(".part");
            m_stabJob.reset();
            QFile::remove(part);
            emit stabProgress(track, index, -1);
            return;
        }
        ++job.pos;
    }
    const int total = qMax(1, job.out - job.in + 1);
    if (job.pos <= job.out) {
        emit stabProgress(job.track, job.index, qBound(0, (job.pos - job.in) * 99 / total, 99));
        scheduleStabWork();
        return;
    }
    const QString part = job.results + QStringLiteral(".part");
    QString file;
    if (job.analysis->finish(nullptr) && QFileInfo::exists(part)) {
        QFile::remove(job.results);
        if (QFile::rename(part, job.results))
            file = job.results;
    }
    int track = job.track, index = job.index;
    const int shakiness = job.shakiness, smoothing = job.smoothing;
    // Edits kept flowing while the frames were read: the clip may have
    // moved (or gone). Find it again by what it is, not where it was.
    {
        const QString path = job.path;
        const int in = job.in, out = job.out;
        auto matches = [&](const QVariantMap &m) {
            return m.value(ClipKey::path).toString() == path
                   && m.value(ClipKey::in).toInt() == in
                   && m.value(ClipKey::out).toInt() == out
                   && !m.contains(ClipKey::retime)
                   && qFuzzyCompare(m.value(ClipKey::speed, 1.0).toDouble(), 1.0);
        };
        bool found = false;
        QVariantMap sameSpot;
        for (const QVariant &v : m_timeline.entries()) {
            const QVariantMap m = v.toMap();
            if (m.value(ClipKey::track).toInt() == track
                && m.value(ClipKey::index).toInt() == index)
                sameSpot = m;
        }
        if (!sameSpot.isEmpty() && matches(sameSpot)) {
            found = true;
        } else {
            for (const QVariant &v : m_timeline.entries()) {
                const QVariantMap m = v.toMap();
                if (matches(m)) {
                    track = m.value(ClipKey::track).toInt();
                    index = m.value(ClipKey::index).toInt();
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            file.clear(); // the analysis stays cached for the next request
    }
    const int askedTrack = job.track, askedIndex = job.index;
    m_stabJob.reset();
    if (file.isEmpty()) {
        emit stabProgress(askedTrack, askedIndex, -1);
        return;
    }
    const QVariantList before = m_timeline.entries();
    if (!m_timeline.setStabAt(track, index, file, shakiness, smoothing)) {
        emit stabProgress(askedTrack, askedIndex, -1);
        return;
    }
    recordEdit(before, true, tr("Stabilise clip"));
    announceTimeline();
    refreshFrame();
    announceStabilised(track, index, file, smoothing);
}

void PlayerWorker::announceStabilised(int track, int index, const QString &results, int smoothing) {
    emit stabProgress(track, index, 100);
    // (Read for the timeline's frame, as its renderer reads it: the one kept
    // reading serves both.)
    const auto *profile = m_timeline.profile();
    const QSize frame = profile ? QSize(profile->width(), profile->height()) : QSize();
    if (const auto corrections = render::stabilisation(results, smoothing, frame, nullptr); corrections && !corrections->empty())
        emit stabApplied(track, index, corrections->zoom, corrections->steadied);
}
