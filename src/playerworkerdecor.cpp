#include "playerworker_p.h"
#include "stagetimer.h"
#include "decoder.h"
#include "audiosamplesource.h"
#include "sound/mixer.h"
#include <QBuffer>

namespace {
// A tile is this many pixels high, whatever its picture's size.
constexpr int kTileHeight = 90;
// OMAEDIT_TILES_ON_WORKER=1: tiles drawn on the worker's own thread through
// the live timeline, one each pass of its event loop, as they were before
// they had a thread: the reference the new way is compared with.
bool tilesOnWorker() { return qEnvironmentVariableIntValue("OMAEDIT_TILES_ON_WORKER") == 1; }
// What the tile thread's timeline draws a picture at before it is made tile
// height: the full frame (0), as the worker's own timeline drew it, so that a
// tile is the same picture whichever way it was made. OMAEDIT_TILE_DRAW=n
// draws it n tile heights high instead (the engine's own reduction of the
// frame, then the same smooth scaling), for measuring: at 1, 2 and 4 the tile
// is not the full frame's (tilesMadeOffTheWorkersThreadAreTheWorkersOwn says
// by how much), which is why the full frame is drawn.
int tileDrawHeights() {
    bool given = false;
    const int asked = qEnvironmentVariableIntValue("OMAEDIT_TILE_DRAW", &given);
    return given ? qBound(0, asked, 16) : 0;
}
// What makes the pictures and the peaks, as far as it is chosen: tiles made
// on one graphics processor, by one decoder or by one version of the editor
// are not offered as another's.
QString keptBy() {
    static const QString chosen = QCoreApplication::applicationVersion() + QLatin1Char('/')
                                  + QSettings().value(QStringLiteral("hardware/engineGpu")).toString() + QLatin1Char('/')
                                  + qEnvironmentVariable("OMAEDIT_ENGINE") + QLatin1Char('/');
    return chosen + SuiteVideo::defaultDecodeOptions().backend;
}
// A timeline of its own for a storyline's tiles, made from tileShot(): the
// tile thread's, or this thread's when it draws them itself. False when it
// cannot be built.
bool restoreTileTimeline(Timeline &timeline, const QVariantMap &shot) {
    const QVariantMap resolved = shot.value("profile").toMap();
    QVariantMap project = shot.value("project").toMap();
    project["custom"] = true;
    for (const auto &key : QStringList{"width", "height", "fpsNum", "fpsDen"}) project[key] = resolved.value(key);
    timeline.setAllowOffline(true);
    timeline.setProjectSettings(project);
    timeline.setMediaSources(shot.value("mediaSources").toMap(), shot.value("proxyMode", "original").toString());
    if (!timeline.restore(shot.value("entries").toList(), Timeline::trackFlagsFromVariant(shot.value("tracks").toList()))
        || timeline.frameCount() != shot.value("frames").toInt())
        return false;
    auto *p = timeline.profile();
    p->set_sample_aspect(resolved.value("sarNum", 1).toInt(), resolved.value("sarDen", 1).toInt());
    p->set_display_aspect(resolved.value("darNum", 16).toInt(), resolved.value("darDen", 9).toInt());
    p->set_progressive(resolved.value("progressive", 1).toInt());
    p->set_colorspace(resolved.value("colorspace", 709).toInt());
    timeline.setMasterMixMap(shot.value("master").toMap());
    // A tile is a frame somewhere else every time: few decoders kept open,
    // little kept of what they read.
    timeline.setDecoderLimits(4, qint64(256) << 20, qBound(2, QThread::idealThreadCount() / 4, 8));
    return true;
}
QString sha(const QVariant &what) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(what).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex());
}
}

// The tile thread's own (see makeTiles): no other thread touches these.
struct PlayerWorker::TileBench {
    std::shared_ptr<const QVariantMap> shot;  // what `timeline` shows, or is to
    std::unique_ptr<Timeline> timeline;       // a copy of the worker's, with an engine and decoders of its own
    bool restored = false;
    QByteArray settingsKey;                   // of `shot`, made when a kept tile is first looked for
    std::function<QByteArray(int)> frameKey;  // and what each of its frames shows
    std::unique_ptr<Timeline> clip;           // one connected clip alone, with its timing
    QVariantMap clipOf;
    bool clipRestored = false;
};
// A sound's peaks as they are being made (see makePeaks). The peak thread's
// while a batch runs; the worker's thread only hands it from job to job.
struct PlayerWorker::WaveBuild {
    QString mark;                                   // which sound exactly `data` is of
    std::shared_ptr<WavePeaks::Data> data;
    std::unique_ptr<AudioSampleSource::Reader> reader;  // a file's sound
    std::unique_ptr<sound::Mixer> mixer;                // a clip's own, as it plays
    qint64 total = 0;
};
struct PlayerWorker::WaveAnswer {
    QString taskId, id;
    std::shared_ptr<WavePeaks::Data> data;
    qint64 made = 0, total = 0;
    bool done = false, failed = false;
    int read = 0, kept = 0;
};
struct PlayerWorker::TileAnswer {
    QString taskId;
    std::shared_ptr<const QVariantMap> shot;  // the timeline its tiles are of (a storyline's only)
    int first = 0;
    QStringList keys;
    QList<QImage> tiles;
    QList<QByteArray> packed;                 // each as its file holds it
    int drawn = 0, kept = 0, rebuilds = 0;
    bool failed = false;
    bool unbuilt = false;  // the thread's timeline could not be built: the worker's own draws the job
    QString error;
};

void PlayerWorker::setDecorationWindow(double first, double last, int pixels) {
    if (!std::isfinite(first) || !std::isfinite(last)) return;
    first=qMax(0.,first); last=qMax(first,last); pixels=qBound(0,pixels,16384);
    if (m_decorWindowSet && first==m_decorFirst && last==m_decorLast && pixels==m_decorPixels) return;
    m_decorWindowSet=true;m_decorFirst=first;m_decorLast=last;m_decorPixels=pixels;
    queueFilmstrips();
}

void PlayerWorker::queueFilmstrips() {
    // Requeue timeline strips only — pending source/library strips stay valid.
    for (int i = m_stripJobs.size() - 1; i >= 0; --i)
        if (m_stripJobs.at(i).timeline)
            finishStripWork(i, QStringLiteral("cancelled"));
    const QVariantList entries = m_timeline.entries();
    const double fps = m_timeline.fps();
    const auto timingIdentity = [&](const QVariantMap &entry) {
        QVariantMap timing;
        for(const char *key:{"path","compoundJson","speed","retime","freeze","sourceMeta","sourceOrigin","mixIn","frameBlend"})
            if(entry.contains(key))timing[key]=entry.value(key);
        timing["media"]=m_timeline.mediaSources().value(entry.value(ClipKey::path).toString());
        const QString path = entry.value(ClipKey::path).toString();
        if (ProjectFile::isProject(path)) {
            const QFileInfo file(path);
            timing["nestedModified"] = file.lastModified().toMSecsSinceEpoch();
            timing["nestedBytes"] = file.size();
        }
        timing["fps"]=fps;
        return QStringLiteral("timeline-timing-v1:")+QString::fromUtf8(QJsonDocument::fromVariant(timing).toJson(QJsonDocument::Compact));
    };
    // A thumbnail occupies about 100 logical pixels. Only plan cells inside
    // the buffered viewport, anchored to the clip so scrolling reuses frames.
    // A block meets its neighbour at the cut a transition sits across, so its
    // strip and its wave cover the block's frames, not the entry's
    // (MixShape): `block.lead` frames before the entry's start and
    // `block.tail` fewer at its end.
    const QList<MixShape::Block> blocks = MixShape::blocks(entries);
    // Each clip's identity (its hash, as text), made once for a clip in view.
    QHash<int, QString> identities;
    const auto identityOf = [&](int at) {
        auto it = identities.find(at);
        if (it == identities.end()) it = identities.insert(at, QString::number(qHash(timingIdentity(entries.at(at).toMap()))));
        return it.value();
    };
    // `story`: the clip's place in `entries` when it is the storyline's (its
    // tiles are frames of the storyline), -1 for a clip drawn off its file.
    const auto planStrip = [&](StripJob &job, const QVariantMap &entry, const MixShape::Block &block,
                               int origin, const QString &identity, int story) {
        const int frames = entry.value(ClipKey::frames).toInt() + block.lead - block.tail;
        const int start = entry.value(ClipKey::start).toInt() - block.lead;
        const int in = entry.value(ClipKey::in).toInt() - block.lead; // keys name the frame shown
        origin -= block.lead;
        const int fallbackTiles = fps > 0.0 ? qBound(1, int(frames / (fps * 2.0)), 16) : 1;
        const double step = m_decorWindowSet && m_decorPixels > 0
            ? qMax(1., 100. * qMax(1., m_decorLast - m_decorFirst) / m_decorPixels)
            : double(frames) / fallbackTiles;
        const double visibleFirst = m_decorWindowSet ? qMax(0., m_decorFirst - start) : 0.;
        const double visibleLast = m_decorWindowSet ? qMin(double(frames), m_decorLast - start) : frames;
        const qint64 firstCell = qMax(qint64(0), qint64(std::floor(visibleFirst / step)));
        const qint64 endCell = qMax(firstCell + 1, qint64(std::ceil(visibleLast / step)));
        const int count = int(qMin(qint64(128), endCell - firstCell));
        job.stripLayout = {{"first", firstCell * step}, {"step", step}, {"shown", frames},
                           {"frames", entry.value(ClipKey::frames)}, {"in", entry.value(ClipKey::in)}, {"path", entry.value(ClipKey::path)}};
        // On the frames of a transition a storyline tile is of two clips:
        // the one before (`into` this clip, up to its start) or the one
        // after (`out` of it, from where that one's transition begins). Such
        // a tile is named by both, by the other's frame and by how far
        // through the transition it lies; named by its own clip alone, it was
        // shown for that clip's frame with the transition gone or changed,
        // the neighbour's picture still in it.
        const auto storyClip = [&](int at) {
            const QVariantMap other = at >= 0 && at < entries.size() ? entries.at(at).toMap() : QVariantMap();
            return other.value(ClipKey::lane).toInt() < 1 && !other.value(ClipKey::gap).toBool() ? other : QVariantMap();
        };
        const int entryStart = entry.value(ClipKey::start).toInt(), mixIn = qMax(0, entry.value(ClipKey::mixIn).toInt());
        const QVariantMap before = story >= 0 && mixIn > 0 ? storyClip(story - 1) : QVariantMap();
        const QVariantMap after = story >= 0 ? storyClip(story + 1) : QVariantMap();
        const int afterMix = qMax(0, after.value(ClipKey::mixIn).toInt()), afterStart = after.value(ClipKey::start).toInt();
        const auto shared = [&](int at) {  // the timeline frame `at`
            const bool into = !before.isEmpty() && at < entryStart;
            const bool out = !into && afterMix > 0 && at >= afterStart - afterMix;
            if (!into && !out) return identity;
            const QVariantMap &other = into ? before : after;
            const int through = into ? at - (entryStart - mixIn) : at - (afterStart - afterMix);
            return QString::number(qHash(QStringLiteral("%1|%2|%3|%4/%5|%6").arg(identity, identityOf(into ? story - 1 : story + 1))
                .arg(other.value(ClipKey::in).toInt() + qMax(0, other.value(ClipKey::mixIn).toInt()) + at - other.value(ClipKey::start).toInt())
                .arg(through).arg(into ? mixIn : afterMix).arg((into ? before : entry).value(ClipKey::mixStyle).toString())));
        };
        for (int j = 0; j < count; ++j) {
            const double cellStart = (firstCell + j) * step;
            const double cellEnd = qMin(double(frames), cellStart + step);
            const int offset = qBound(0, int(qMin(double(frames - 1), (cellStart + cellEnd) / 2.)), frames - 1);
            job.positions.append(origin + offset);
            job.keys.append(QStringLiteral("strip-%1-%2").arg(story >= 0 ? shared(start + offset) : identity).arg(in + offset));
        }
    };
    int connectedIndex = -1; // running index into the connected list
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap entry = entries.at(i).toMap();
        const int lane = entry.value(ClipKey::lane).toInt();
        if (lane >= 1) ++connectedIndex;
        const MixShape::Block block = blocks.at(i);
        const int clipStart=entry.value(ClipKey::start).toInt()-block.lead, clipFrames=entry.value(ClipKey::frames).toInt()+block.lead-block.tail;
        // No count of clips here: the window and the four-pixel width below
        // decide (as Backend::pruneTimelineDecor does, which must agree), so
        // the storyline cannot use up the turn of every clip after it.
        if (entry.value(ClipKey::gap).toBool()) continue;
        if (m_decorWindowSet && (m_decorPixels==0 || m_decorLast<=m_decorFirst
            || clipStart>=m_decorLast || double(clipStart)+clipFrames<=m_decorFirst
            || double(clipFrames)*m_decorPixels/qMax(1.,m_decorLast-m_decorFirst)<4.)) continue;
        // Kilobytes of text for a clip with source settings: made, and
        // hashed, once for a clip in view, not for every tile and peak.
        const QString identity=identityOf(i);
        // Connected clips get a waveform band (their audio mixes in — and
        // detached audio LIVES there) and, when they carry a picture, a
        // filmstrip decoded off their own file at SOURCE frames (the
        // timeline composite would show whatever is stacked on top). Titles
        // carry neither; a compound has no file to open.
        if (lane >= 1) {
            if (!entry.value(ClipKey::title).toString().isEmpty())
                continue;
            const int frames = clipFrames;       // the block's, as drawn
            const int entryStart = entry.value(ClipKey::start).toInt(); // what an answer is matched by
            const int in = entry.value(ClipKey::in).toInt() - block.lead;
            const QString path = entry.value(ClipKey::path).toString();
            if (frames < 1)
                continue;
            if (!entry.value(ClipKey::audioOnly).toBool()
                && entry.value(ClipKey::compoundJson).toString().isEmpty()
                && !path.isEmpty() && !entry.value(ClipKey::pictureless).toBool()) { // (a recording has no pictures)
                StripJob job;
                job.timeline=true;
                job.index = connectedIndex;
                job.start = entryStart;
                job.lane = lane;
                job.libraryPath = path;
                const bool timed=entry.contains(ClipKey::retime) || entry.contains(ClipKey::freeze) || entry.contains(ClipKey::sourceOrigin)
                    || !qFuzzyCompare(entry.value(ClipKey::speed,1.).toDouble(),1.)
                    || !qFuzzyCompare(Timeline::interpretationFactor(m_timeline.mediaSources().value(path).toMap()),1.);
                if(timed) {
                    // Decode an isolated clip with the same timing as playback.
                    // Opening the raw file at output-frame positions would show
                    // the wrong material for ramps, reversals and interpreted fps.
                    for(const char *key:{"path","speed","retime","freeze","sourceMeta","sourceOrigin","length","frameBlend"})
                        if(entry.contains(key))job.timedClip[key]=entry.value(key);
                    const int first=in+entry.value(ClipKey::mixIn).toInt();
                    job.timedClip["in"]=first;job.timedClip["out"]=first+frames-1;
                    job.timedClip["frames"]=frames;job.timedClip["start"]=0;
                    job.timedClip["track"]=0;job.timedClip["lane"]=0;job.timedClip["index"]=0;
                    job.timedClip["videoOnly"]=true;
                }
                planStrip(job, entry, block, timed?block.lead:entry.value(ClipKey::in).toInt()+entry.value(ClipKey::mixIn).toInt(), identity, -1);
                m_stripJobs.append(job);
            }
            // A picture whose sound sits on its own track (or that has none)
            // plays no sound, and shows none.
            if (entry.value(ClipKey::videoOnly).toBool())
                continue;
            StripJob wave;
            wave.timeline=true;
            wave.index = connectedIndex;
            wave.start = entryStart;
            wave.wave = true;
            wave.lane = lane;
            // The sound the clip plays: a file's peaks serve every clip of
            // it, whatever part each plays (WavePeaks::Source).
            wave.source = WavePeaks::Source::of(entry, fps);
            if (wave.source.kind != WavePeaks::Source::None) m_stripJobs.append(wave);
            continue;
        }
        const int frames = clipFrames;       // the block's, as drawn
        const int entryStart = entry.value(ClipKey::start).toInt(); // what an answer is matched by
        if (frames < 1)
            continue;
        // A recording a project holds on the picture row has no pictures: no
        // strip is asked of it.
        if (!entry.value(ClipKey::pictureless).toBool()) {
            StripJob job;
            job.timeline=true;
            job.index = i;
            job.start = entryStart;
            planStrip(job, entry, block, entryStart, identity, i);
            m_stripJobs.append(job);
        }

        // A waveform job per clip too. A picture whose sound sits on an
        // audio track plays none, and shows none.
        if (entry.value(ClipKey::videoOnly).toBool())
            continue;
        StripJob wave;
        wave.timeline=true;
        wave.index = i;
        wave.start = entryStart;
        wave.wave = true;
        wave.source = WavePeaks::Source::of(entry, fps);
        if (wave.source.kind != WavePeaks::Source::None) m_stripJobs.append(wave);
    }
    // The sounds in view stay held (the store's spare allowance is for the
    // others): an edit, a scroll or a zoom reads none of them again.
    QHash<QString, std::shared_ptr<WavePeaks::Data>> held;
    for (const StripJob &job : std::as_const(m_stripJobs))
        if (job.timeline && job.wave && !held.contains(job.source.id))
            if (const auto data = WavePeaks::Store::instance()->find(job.source.id)) held.insert(job.source.id, data);
    m_waveHeld = held;
    // A sound half made and out of view keeps what it has, a few of them.
    for (auto it = m_waveBuilds.begin(); m_waveBuilds.size() > 16 && it != m_waveBuilds.end();)
        it = held.contains(it.key()) ? std::next(it) : m_waveBuilds.erase(it);
    reportStripWork(true);
    scheduleStripWork();
}

void PlayerWorker::queueSourceStrip() {
    const int frames = m_preview.frameCount();
    const double fps = m_preview.fps();
    if (frames < 1)
        return;
    // A sound file has no pictures to strip; its waveform stands in for them.
    const QVariantMap facts = m_preview.metadata();
    if (facts.value(QStringLiteral("videoCodec")).toString().isEmpty() && !facts.value(QStringLiteral("audioCodec")).toString().isEmpty())
        return;
    StripJob job;
    job.sourcePath = m_previewPath;
    // Source strips keep a capped overview. Cache keys use SOURCE frames,
    // allowing the timeline to reuse any matching samples.
    const int tiles = fps > 0.0 ? qBound(1, int(frames / (fps * 2.0)), 16) : 1;
    for (int j = 0; j < tiles; ++j) {
        const int frame = qMin(frames - 1, int((j + 0.5) * frames / tiles));
        job.positions.append(frame);
        job.keys.append(QStringLiteral("strip-%1-%2")
                            .arg(qHash(m_previewPath))
                            .arg(frame));
    }
    // Replace any stale source job (a new preview supersedes the old one).
    for (int i = m_stripJobs.size() - 1; i >= 0; --i)
        if (!m_stripJobs.at(i).sourcePath.isEmpty())
            finishStripWork(i, QStringLiteral("cancelled"));
    m_stripJobs.prepend(job); // the strip the user is looking at goes first
    reportStripWork(true);
    scheduleStripWork();
}

void PlayerWorker::boundLibraryDecorJobs() {
    int count=0;
    for(const auto &job:std::as_const(m_stripJobs))if(!job.timeline && !job.libraryPath.isEmpty())++count;
    for(int i=0;count>128 && i<m_stripJobs.size();){
        if(!m_stripJobs[i].timeline && !m_stripJobs[i].libraryPath.isEmpty()){finishStripWork(i,QStringLiteral("cancelled"));--count;}
        else ++i;
    }
}

// Another timeline is opened: the jobs made for the one before end (its
// strips and waves, the source viewer's strip, a bin clip's strip, which is
// drawn with that timeline's media settings). A bin clip's waveform is of its
// file alone, whichever timeline is open, and nothing asks for it a second
// time: its job stays, or the clip's card would never show its sound.
void PlayerWorker::endTimelineDecorJobs() {
    for (int i = int(m_stripJobs.size()) - 1; i >= 0; --i) {
        const StripJob &job = m_stripJobs.at(i);
        if (!(job.wave && !job.timeline && !job.libraryPath.isEmpty())) finishStripWork(i, QStringLiteral("cancelled"));
    }
}

void PlayerWorker::queueLibraryStrip(const QString &path, int frames, double fps) {
    if (frames < 1)
        return;
    StripJob job;
    job.libraryPath = path;
    // A touch sparser than the big strips — it fills a browser row.
    const int tiles = fps > 0.0 ? qBound(1, int(frames / (fps * 2.0)), 12) : 1;
    for (int j = 0; j < tiles; ++j) {
        const int frame = qMin(frames - 1, int((j + 0.5) * frames / tiles));
        job.positions.append(frame);
        job.keys.append(QStringLiteral("strip-%1-%2")
                            .arg(qHash(path))
                            .arg(frame));
    }
    m_stripJobs.append(job); // lowest priority — behind whatever is queued
    boundLibraryDecorJobs();
    reportStripWork(true);
    scheduleStripWork();
}

void PlayerWorker::queueLibraryWave(const QString &path, int frames) {
    for(int i=m_stripJobs.size()-1;i>=0;--i)
        if(!m_stripJobs[i].timeline && m_stripJobs[i].wave && m_stripJobs[i].libraryPath==path)finishStripWork(i,QStringLiteral("cancelled"));
    if (frames < 1)
        return;
    StripJob job;
    job.libraryPath = path;
    job.wave = true;
    // The file's own peaks, whole: a timeline clip of it reads the same ones.
    job.source = WavePeaks::Source::ofFile(path, m_timeline.fps() > 0 ? m_timeline.fps() : 25.);
    m_stripJobs.append(job);
    boundLibraryDecorJobs();
    reportStripWork(true);
    scheduleStripWork();
}

void PlayerWorker::reportStripWork(bool registerNew) {
    for (int i=0; i<(registerNew ? m_stripJobs.size() : qMin(1, int(m_stripJobs.size()))); ++i) {
        auto &job=m_stripJobs[i];
        if (job.taskId.isEmpty()) job.taskId=QStringLiteral("cache:")+QUuid::createUuid().toString(QUuid::WithoutBraces);
        const int percent=job.wave ? (job.waveTotal > 0 ? int(qMin<qint64>(100, job.waveMade*100/job.waveTotal)) : 0)
                                   : job.keys.isEmpty() ? 0 : int(job.tiles.size()*100/job.keys.size());
        const QString state=i==0 ? (exportActive() || m_speed!=0 ? QStringLiteral("waiting") : QStringLiteral("running")) : QStringLiteral("queued");
        if (job.reportedPercent==percent && job.reportedState==state) continue;
        if (job.reportedState==state && m_cacheFeedbackClock.elapsed()<m_cacheFeedbackNext) continue;
        if (i==0) m_cacheFeedbackNext=m_cacheFeedbackClock.elapsed()+200;
        job.reportedPercent=percent; job.reportedState=state;
        const QString path=job.libraryPath.isEmpty() ? job.sourcePath : job.libraryPath;
        const QString name=path.isEmpty() ? tr("Timeline") : QFileInfo(path).fileName();
        emit cacheTaskChanged({{"id",job.taskId}, {"name",name}, {"type",job.wave ? tr("Waveform") : tr("Thumbnail")},
            {"state",state}, {"percent",percent}, {"cache",true}, {"announce",false}, {"canCancel",true}, {"message",path}});
    }
}
void PlayerWorker::finishStripWork(int index, const QString &state, const QString &error) {
    const auto job=m_stripJobs.takeAt(index);
    if (job.stop) job.stop->store(true);  // a batch of its peaks still being made ends there
    if (job.taskId.isEmpty()) return;
    const QString path=job.libraryPath.isEmpty() ? job.sourcePath : job.libraryPath;
    emit cacheTaskChanged({{"id",job.taskId}, {"name",path.isEmpty() ? tr("Timeline") : QFileInfo(path).fileName()},
        {"type",job.wave ? tr("Waveform") : tr("Thumbnail")}, {"state",state}, {"percent",state=="completed" ? 100 : job.reportedPercent},
        {"cache",true}, {"announce",state=="failed"}, {"canCancel",false}, {"message",error.isEmpty() ? path : error}});
}
void PlayerWorker::cancelCacheTask(const QString &id) {
    for (int i=m_stripJobs.size()-1; i>=0; --i) if (m_stripJobs.at(i).taskId==id) finishStripWork(i, QStringLiteral("cancelled"));
    scheduleStripWork();
}
void PlayerWorker::scheduleStripWork() {
    reportStripWork();
    if (m_stripScheduled || m_stripJobs.isEmpty())
        return;
    m_stripScheduled = true;
    // One tile per event-loop pass, so queued edits, seeks and the playback
    // pump always jump the queue.
    QTimer::singleShot(0, this, &PlayerWorker::processStripTile);
}

void PlayerWorker::processStripTile() {
    m_stripScheduled = false;
    if (m_stripJobs.isEmpty())
        return;
    if (exportActive() || m_speed != 0.0) {
        reportStripWork();
        return; // decoding is contended right now; resumed when idle again
    }

    // The command-line editor shows no filmstrips: their pictures are not
    // decoded there (waveforms still are; inspect reports them).
    static const bool agent = qApp && qApp->property("omaedit.agent").toBool();
    if (agent && !m_stripJobs.first().wave) {
        m_stripJobs.removeFirst();
        scheduleStripWork();
        return;
    }
    // The first job, or, while a batch of its is out, the first job of the
    // other kind: peaks and tiles each have a thread, so a clip's tiles need
    // not wait for the waveform of the clip before it, nor its peaks for a
    // tile. (Not for what this thread draws itself: the source viewer's strip.)
    int head = 0;
    if (m_stripJobs.first().making && !agent) {
        const bool wave = m_stripJobs.first().wave;
        head = -1;
        for (int i = 1; i < m_stripJobs.size() && head < 0; ++i)
            if (m_stripJobs.at(i).wave != wave) head = i;
        if (head < 0 || m_stripJobs.at(head).making || !m_stripJobs.at(head).sourcePath.isEmpty()
            || (!m_stripJobs.at(head).wave && (tilesOnWorker() || m_stripJobs.at(head).onWorker)))
            return;
    }
    StripJob &job = m_stripJobs[head];
    if (job.wave) {
        // Peaks are made off this thread (makePeaks): a batch is asked for
        // here and its answer (peaksMade) asks for the next.
        if (job.making)
            return;
        // A sound found a moment ago to be the file as it stands costs nothing.
        if (m_cacheFeedbackClock.elapsed() - m_waveChecked.value(job.source.id, -10000) < 1000) {
            if (const auto data = WavePeaks::Store::instance()->find(job.source.id); data && data->complete()) {
                announceWave(job, data);
                finishStripWork(head);
                scheduleStripWork();
                return;
            }
        }
        makePeaks(job);
        scheduleStripWork();  // (the first job of the other kind, beside it)
        return;
    }
    const bool sourceJob = !job.sourcePath.isEmpty();
    const bool libraryJob = !job.libraryPath.isEmpty();
    if (sourceJob && (!m_sourceMode || m_previewPath != job.sourcePath)) {
        finishStripWork(head, QStringLiteral("cancelled")); // the preview it belonged to is gone
        scheduleStripWork();
        return;
    }
    const bool timedJob=!job.timedClip.isEmpty();
    if (!sourceJob && !job.onWorker && !tilesOnWorker()) {
        // Tiles are made off this thread (makeTiles): a batch is asked for
        // here and its answer (tilesMade) asks for the next. Only the source
        // viewer's strip is still drawn here, through the viewer's own player.
        if (job.making)
            return;
        // Tiles already known cost nothing.
        while (job.tiles.size() < job.keys.size()) {
            const auto *known = m_stripCache.object(job.keys.at(job.tiles.size()));
            if (!known) break;
            const QImage tile = *known;
            landTile(job, tile, {});
        }
        if (job.tiles.size() == job.keys.size()) {
            announceStrip(job);
            finishStripWork(head);
            scheduleStripWork();
            return;
        }
        makeTiles(job);
        scheduleStripWork();  // (the first job of the other kind, beside it)
        return;
    }
    if(timedJob && !job.timedTimeline) {
        job.timedTimeline=std::make_shared<Timeline>();
        auto settings=m_timeline.projectSettings();const auto *profile=m_timeline.profile();
        settings["custom"]=true;settings["width"]=profile->width();settings["height"]=profile->height();
        settings["fpsNum"]=profile->frame_rate_num();settings["fpsDen"]=profile->frame_rate_den();
        job.timedTimeline->setProjectSettings(settings);
        job.timedTimeline->setMediaSources(m_timeline.mediaSources());
        if(!job.timedTimeline->restore({job.timedClip})) {
            finishStripWork(head,QStringLiteral("failed"),tr("Could not prepare the clip's thumbnail timing."));
            scheduleStripWork();return;
        }
    }
    if (libraryJob && !timedJob && !job.player) {
        job.player = std::make_shared<MediaPlayer>();
        if (!job.player->open(job.libraryPath)) {
            finishStripWork(head, QStringLiteral("failed"), tr("Could not open thumbnail source."));
            scheduleStripWork();
            return;
        }
        job.player->setInputTransform(m_timeline.mediaSources().value(job.libraryPath).toMap());
    }
    const int i = job.tiles.size();
    const QString &key = job.keys.at(i);
    const auto *cachedTile=m_stripCache.object(key);
    QImage tile = cachedTile ? *cachedTile : QImage();
    // A storyline's tiles are of the storyline alone here too (tileShot): a
    // copy of it, made again after an edit. (The timeline itself, with the
    // tracks over it, only when no copy can be made.)
    Timeline *story = &m_timeline;
    if (tile.isNull() && !sourceJob && !timedJob && !libraryJob) {
        const auto shot = tileShot();
        if (m_tileOwnShot != shot) {
            m_tileOwnShot = shot;
            m_tileOwn = std::make_unique<Timeline>();
            if (!restoreTileTimeline(*m_tileOwn, *shot)) m_tileOwn.reset();
        }
        if (m_tileOwn) story = m_tileOwn.get();
    }
    if (tile.isNull()) {
        RenderScope render(frameRequest());
        // A tile of the timeline is a frame somewhere else on it: drawn
        // through the playhead's decoders it left every track's decoder
        // there, and the next play began with each of them seeking back.
        m_timeline.setAside(true);
        const auto atPlayhead = qScopeGuard([this] { m_timeline.setAside(false); });
        tile = sourceJob    ? m_preview.frameAt(job.positions.at(i))
               : timedJob ? WorkingFrame::shown(job.timedTimeline->decodeAt(job.positions.at(i),false).image)
               : libraryJob ? job.player->frameAt(job.positions.at(i))
                            : WorkingFrame::shown(story->decodeAt(job.positions.at(i), false).image);
        // A seek or stop takes priority over background decorations. Leave
        // this tile pending; the next idle pass retries if the job survives.
        if(RenderScope::cancelled()){scheduleStripWork();return;}
        if (!tile.isNull()) {
            tile = tile.scaledToHeight(90, Qt::SmoothTransformation);
            m_stripCache.insert(key, new QImage(tile), qMax(1, int((tile.sizeInBytes()+1023)/1024)));
        }
    }
    if (tile.isNull()) {
        finishStripWork(head, QStringLiteral("failed"), tr("Could not decode a thumbnail. The source may be offline or unsupported."));
        scheduleStripWork();
        return;
    }
    ThumbnailDiskCache::write(key,tile);
    if (sourceJob || (libraryJob && !timedJob)) {
        const auto original = sourceJob ? job.sourcePath : job.libraryPath;
        const auto path = sourceJob ? m_timeline.playbackPath(original) : original;
        emit stripSourceReady(key, path, job.positions.at(i), tile.width(),
                              m_timeline.mediaSources().value(original).toMap(),
                              sourceJob ? m_timeline.linearSourceManagement(original) : QVariantMap());
    }
    job.tiles.append(tile);
    if (job.tiles.size() == job.keys.size()) {
        announceStrip(job);
        finishStripWork(head);
    }
    scheduleStripWork();
}

// A finished strip, told to whoever shows it.
void PlayerWorker::announceStrip(const StripJob &job) {
    const bool libraryJob = !job.libraryPath.isEmpty();
    if (!job.sourcePath.isEmpty()) {
        emit sourceStripReady(job.sourcePath, job.keys, job.tiles);
        // Decoding tiles moved the preview producer; park it back on the
        // frame the user is actually looking at.
        emit previewFrameReady(m_preview.frameAt(m_previewPos), m_previewPos);
    } else if (libraryJob && job.lane >= 1) {
        emit connectedFilmstripReady(job.index, job.start, job.keys, job.tiles, job.stripLayout);
    } else if (libraryJob) {
        emit libraryStripReady(job.libraryPath, job.keys, job.tiles);
    } else {
        emit filmstripReady(job.index, job.start, job.keys, job.tiles, job.stripLayout);
    }
}

// The next tile of a job made off this thread, taken into the job. A tile
// just made comes with its file's bytes (`packed`, made on the tile thread)
// and is written to this run's disk cache there too, behind the batches:
// no picture is packed and no file written on this thread.
void PlayerWorker::landTile(StripJob &job, const QImage &tile, const QByteArray &packed) {
    const int i = int(job.tiles.size());
    const QString key = job.keys.at(i);
    if (!packed.isEmpty()) m_tilePool.start([key, tile, packed] { StageTimer::Scope timed("decor.tile.write"); ThumbnailDiskCache::write(key, tile, packed); });
    else ThumbnailDiskCache::write(key, tile);  // (there already, as a rule: then nothing is written)
    if (!job.libraryPath.isEmpty() && job.timedClip.isEmpty())
        emit stripSourceReady(key, job.libraryPath, job.positions.at(i), tile.width(),
                              m_timeline.mediaSources().value(job.libraryPath).toMap(), QVariantMap());
    job.tiles.append(tile);
}

// The timeline as tiles are made of it: the same object for as long as the
// worker's timeline shows the same pictures, so that the tile thread rebuilds
// its copy only after an edit, and a tile drawn from a timeline that has been
// edited since is told from one of the timeline as it stands (tilesMade).
// Everything in it is shared with the worker's own lists: no copy is made.
//
// It is the storyline alone. A clip's filmstrip is that clip's pictures: drawn
// with the tracks over it, a clip under another wore the other's pictures in
// its own strip (and kept them, under its own tiles' names, when the other
// had gone). The clips over it have strips of their own, off their files.
// And a clip turned off is drawn as if on: its strip is still its pictures
// (it drew none, and those empty tiles stayed when it was turned on again).
std::shared_ptr<const QVariantMap> PlayerWorker::tileShot() {
    // (Picked out once for a clip list: the timeline hands the same list
    // until it is edited, and so does this, which is what makes telling
    // whether the shot still stands cost nothing.)
    if (const QVariantList all = m_timeline.entries(); m_tileStoryOf != all) {
        m_tileStoryOf = all;
        m_tileStory.clear();
        for (const QVariant &entry : all) {
            QVariantMap clip = entry.toMap();
            if (clip.value(ClipKey::lane).toInt() >= 1) continue;
            if (!clip.value(ClipKey::disabled).toBool()) { m_tileStory.append(entry); continue; }
            clip[ClipKey::disabled] = false;
            m_tileStory.append(clip);
        }
    }
    const QVariantList entries = m_tileStory;
    const QVariantList tracks = m_timeline.trackFlagsVariant();
    const auto *p = m_timeline.profile();
    QVariantMap profile;
    if (p) profile = {{"width", p->width()}, {"height", p->height()}, {"fpsNum", p->frame_rate_num()}, {"fpsDen", p->frame_rate_den()},
                      {"sarNum", p->sample_aspect_num()}, {"sarDen", p->sample_aspect_den()}, {"darNum", p->display_aspect_num()},
                      {"darDen", p->display_aspect_den()}, {"progressive", p->progressive()}, {"colorspace", p->colorspace()}};
    if (m_tileShot) {
        const QVariantMap &was = *m_tileShot;
        if (was.value("entries").toList() == entries && was.value("tracks").toList() == tracks && was.value("profile").toMap() == profile
            && was.value("project").toMap() == m_timeline.projectSettings() && was.value("mediaSources").toMap() == m_timeline.mediaSources()
            && was.value("proxyMode").toString() == m_timeline.proxyMode())
            return m_tileShot;
    }
    m_tileShot = std::make_shared<const QVariantMap>(QVariantMap{
        {"entries", entries}, {"tracks", tracks}, {"master", m_timeline.masterMix()}, {"project", m_timeline.projectSettings()},
        {"mediaSources", m_timeline.mediaSources()}, {"proxyMode", m_timeline.proxyMode()}, {"fps", m_timeline.fps()},
        {"frames", m_timeline.primaryFrameCount()}, {"profile", profile}});
    return m_tileShot;
}

// The next tiles of a filmstrip job, made on the tile thread.
//
// Threads: this thread plans the batch and keeps the job, the caches and the
// counters. The tile thread (one, so batches never overlap) owns TileBench:
// a copy of the timeline restored from tileShot() as the background render
// restores its renderers', with an engine of its own (the editor's choice of
// graphics processor, or the CPU twin: render::createEngine, as the worker's
// timeline is given) and decoders of its own, and a second small timeline for
// one connected clip with its timing. A library clip's player goes with its
// job, as a waveform's does. Everything else is handed over by value, and the
// answer comes back as a queued call (tilesMade).
//
// A batch stops when its job ends (`stop`: an edit or a scroll requeues the
// timeline's jobs) or the editor starts to play or export (`held`); the
// picture then being drawn is given up between two of its layers. None is
// asked for while the editor plays or exports.
//
// Kept between runs (ThumbnailDiskCache::keepImage): a tile is looked for
// there before it is drawn, under a key that says exactly what it shows.
void PlayerWorker::makeTiles(StripJob &job) {
    const bool library = !job.libraryPath.isEmpty();
    const bool timed = !job.timedClip.isEmpty();
    const int first = int(job.tiles.size());
    QList<int> positions;
    QStringList keys;
    // Up to the next tile already known (the cache has that one).
    for (int i = first; i < job.keys.size() && positions.size() < 8; ++i) {
        if (i > first && m_stripCache.contains(job.keys.at(i))) break;
        positions.append(job.positions.at(i));
        keys.append(job.keys.at(i));
    }
    if (!job.stop) job.stop = std::make_shared<std::atomic_bool>(false);
    if (job.taskId.isEmpty()) job.taskId = QStringLiteral("cache:") + QUuid::createUuid().toString(QUuid::WithoutBraces);  // what its answer is matched by
    if (!job.bench) job.bench = std::make_shared<JobBench>();
    if (library && !timed && !job.player) job.player = std::make_shared<MediaPlayer>();
    if (!m_tileBench) m_tileBench = std::make_shared<TileBench>();
    job.making = true;
    ++m_tileBatches;
    m_tilesHeld->store(false);  // (none is asked for while it would be true)
    // A storyline's tiles are frames of the whole timeline; a connected
    // clip's with its own timing are that clip's alone, drawn in a timeline
    // of the same size and rate; any other clip's come off its file.
    const std::shared_ptr<const QVariantMap> shot = !library ? tileShot() : nullptr;
    QVariantMap clipProject, sources;
    if (timed) {
        clipProject = m_timeline.projectSettings();
        const auto *profile = m_timeline.profile();
        clipProject["custom"] = true; clipProject["width"] = profile->width(); clipProject["height"] = profile->height();
        clipProject["fpsNum"] = profile->frame_rate_num(); clipProject["fpsDen"] = profile->frame_rate_den();
        sources = m_timeline.mediaSources();
    }
    const QVariantMap transform = library && !timed ? m_timeline.mediaSources().value(job.libraryPath).toMap() : QVariantMap();
    m_tilePool.start([this, taskId = job.taskId, stop = job.stop, held = m_tilesHeld, bench = m_tileBench, mine = job.bench, player = job.player,
                      path = job.libraryPath, timedClip = job.timedClip, library, timed, first, positions, keys, shot, clipProject, sources,
                      transform, keeping = ThumbnailDiskCache::keeping(), heights = tileDrawHeights(), drawnBy = keptBy(),
                      pause = qEnvironmentVariableIntValue("OMAEDIT_TILE_PAUSE_MS"), drawing = m_viewerDrawing, asked = m_viewerAsked] {
        TileAnswer answer;
        answer.taskId = taskId;
        answer.shot = shot;
        answer.first = first;
        const auto gone = [&] { return stop->load() || held->load(); };
        const QString cannot = tr("Could not decode a thumbnail. The source may be offline or unsupported.");
        // Smaller than the frame only: the picture as the engine reduces it.
        const auto drawSize = [&](const Timeline &timeline) {
            const int height = heights * kTileHeight;
            if (heights < 1 || timeline.height() <= height) return QSize();
            return QSize(qMax(2, int(std::lround(double(timeline.width()) * height / timeline.height())) & ~1), height);
        };
        const auto asTile = [](const QImage &picture) {
            const QImage shown = WorkingFrame::shown(picture);
            return shown.isNull() || shown.height() == kTileHeight ? shown : shown.scaledToHeight(kTileHeight, Qt::SmoothTransformation);
        };
        // What says exactly what each of these tiles shows.
        QStringList exact;
        if (keeping && !gone()) {
            StageTimer::Scope timedKeys("decor.tile.keys");
            const QString size = QStringLiteral("|h%1|d%2").arg(kTileHeight).arg(heights);
            if (!library) {
                if (bench->shot != shot) { bench->shot = shot; bench->restored = false; bench->settingsKey.clear(); bench->frameKey = {}; }
                if (bench->settingsKey.isEmpty()) {
                    QVariantMap all = *shot;
                    all["nested"] = RenderSnapshot::nestedDocuments(all.value("entries").toList());
                    bench->settingsKey = PreviewRenderCache::settingsKey(all, drawnBy);
                }
                if (!bench->frameKey) bench->frameKey = PreviewRenderCache::frameKeys(shot->value("entries").toList());
                for (const int position : positions)
                    exact.append(QStringLiteral("tile-v1|story|") + QString::fromLatin1(bench->settingsKey.toHex()) + QLatin1Char('|')
                                 + QString::fromLatin1(QCryptographicHash::hash(bench->frameKey(position), QCryptographicHash::Sha1).toHex()) + size);
            } else if (timed) {
                const QVariantMap nested = RenderSnapshot::nestedDocuments({timedClip});
                const QString clip = sha(QVariantList{timedClip, clipProject, sources.value(path), nested, drawnBy,
                                                      RenderSnapshot::filesNow(QVariantList{timedClip, sources.value(path), nested})});
                for (const int position : positions) exact.append(QStringLiteral("tile-v1|clip|%1|%2").arg(clip).arg(position) + size);
            } else {
                const QString file = sha(QVariantList{path, transform, drawnBy, RenderSnapshot::filesNow(QVariantList{path, transform})});
                for (const int position : positions) exact.append(QStringLiteral("tile-v1|file|%1|%2|h%3").arg(file).arg(position).arg(kTileHeight));
            }
        }
        QElapsedTimer spent;
        spent.start();
        for (int i = 0; i < positions.size() && !gone() && (i == 0 || spent.elapsed() < 40); ++i) {
            QImage tile;
            QByteArray packed;
            if (i < exact.size()) tile = ThumbnailDiskCache::keptImage(exact.at(i), &packed);
            if (!tile.isNull()) {
                // (As the tiles drawn here are: 8 bits, colour beside alpha.)
                tile = tile.convertToFormat(QImage::Format_RGBA8888);
                ++answer.kept;
                answer.tiles.append(tile);
                answer.packed.append(packed);
                continue;
            }
            // OMAEDIT_TILE_PAUSE_MS: every tile drawn takes this much longer
            // (for tests: a batch still out when a seek or an edit arrives).
            for (QElapsedTimer paused; pause > 0 && !gone() && (!paused.isValid() || paused.elapsed() < pause);) {
                if (!paused.isValid()) paused.start();
                QThread::msleep(2);
            }
            // The viewer's own picture first: a tile waits while one is
            // being drawn, and is given up when one is asked for or begun
            // (it stays owed). Not for ever: a picture two seconds in the
            // drawing is not one to wait out.
            QElapsedTimer waited;
            waited.start();
            while (drawing->load() > 0 && !gone() && waited.elapsed() < 2000) QThread::usleep(500);
            const bool patient = drawing->load() == 0;
            const quint64 asks = asked->load();
            RenderScope render(std::make_shared<RenderRequest>([stop, held, drawing, asked, asks, patient] {
                return stop->load() || held->load() || (patient && (drawing->load() > 0 || asked->load() != asks));
            }));
            if (!library) {
                if (bench->shot != shot) { bench->shot = shot; bench->restored = false; bench->settingsKey.clear(); bench->frameKey = {}; }
                if (!bench->restored) {
                    StageTimer::Scope timedGraph("decor.tile.graph");  // OMAEDIT_STAGE_TRACE: what this thread takes off the worker's
                    ++answer.rebuilds;
                    if (!bench->timeline) bench->timeline = std::make_unique<Timeline>();
                    if (!restoreTileTimeline(*bench->timeline, *shot)) {
                        bench->timeline.reset();
                        answer.unbuilt = true;
                        break;
                    }
                    bench->restored = true;
                }
                StageTimer::Scope timedDraw("decor.tile.draw");
                bench->timeline->setDrawSize(drawSize(*bench->timeline));
                tile = asTile(bench->timeline->decodeAt(positions.at(i), false).image);
            } else if (timed) {
                if (!bench->clipRestored || bench->clipOf != QVariantMap{{"clip", timedClip}, {"project", clipProject}, {"sources", sources}}) {
                    StageTimer::Scope timedGraph("decor.tile.graph");
                    if (!bench->clip) bench->clip = std::make_unique<Timeline>();
                    bench->clipOf = {{"clip", timedClip}, {"project", clipProject}, {"sources", sources}};
                    bench->clip->setProjectSettings(clipProject);
                    bench->clip->setMediaSources(sources);
                    bench->clipRestored = bench->clip->restore({timedClip});
                    if (!bench->clipRestored) {
                        bench->clip.reset();
                        answer.unbuilt = true;
                        break;
                    }
                    bench->clip->setDecoderLimits(2, qint64(128) << 20, qBound(2, QThread::idealThreadCount() / 4, 8));
                }
                StageTimer::Scope timedDraw("decor.tile.draw");
                bench->clip->setDrawSize(drawSize(*bench->clip));
                tile = asTile(bench->clip->decodeAt(positions.at(i), false).image);
            } else {
                if (!mine->opened && !mine->openFailed) {
                    mine->opened = player->open(path);
                    mine->openFailed = !mine->opened;
                    if (mine->opened) player->setInputTransform(transform);
                }
                if (mine->openFailed) {
                    answer.failed = true;
                    answer.error = tr("Could not open thumbnail source.");
                    break;
                }
                StageTimer::Scope timedDraw("decor.tile.draw");
                tile = player->frameAt(positions.at(i));
                if (!tile.isNull()) tile = tile.scaledToHeight(kTileHeight, Qt::SmoothTransformation);
            }
            // Given up for a seek, an edit or play: the tile stays owed. (One
            // that was finished all the same is kept.)
            if (gone() || (tile.isNull() && RenderScope::cancelled())) break;
            if (tile.isNull()) {
                answer.failed = true;
                answer.error = cannot;
                break;
            }
            ++answer.drawn;
            {
                // Packed once, for the file kept between runs and this run's own.
                StageTimer::Scope timedKeep("decor.tile.keep");
                packed = ThumbnailDiskCache::packed(tile);
                if (i < exact.size()) ThumbnailDiskCache::keepImage(exact.at(i), tile, packed);
            }
            answer.tiles.append(tile);
            answer.packed.append(packed);
        }
        answer.keys = keys.mid(0, answer.tiles.size());
        QMetaObject::invokeMethod(this, [this, answer] { tilesMade(answer); }, Qt::QueuedConnection);
    });
}

void PlayerWorker::tilesMade(const TileAnswer &answer) {
    m_tilesDrawn += answer.drawn;
    m_tilesKept += answer.kept;
    m_tileRebuilds += answer.rebuilds;
    m_tileIdle.start();
    // A storyline tile is a frame of the timeline it was drawn from: one of a
    // timeline edited since is not a tile of this one (its key leaves the
    // look of the clips out), and is dropped. Its job, when still here, asks
    // again of the timeline as it stands.
    const bool current = !answer.shot || answer.shot == tileShot();
    int at = -1;
    for (int i = 0; i < m_stripJobs.size(); ++i)
        if (m_stripJobs.at(i).taskId == answer.taskId && m_stripJobs.at(i).making && !m_stripJobs.at(i).wave) { at = i; break; }
    if (at >= 0) m_stripJobs[at].making = false;
    if (current) {
        for (int i = 0; i < answer.tiles.size(); ++i) {
            const QImage &tile = answer.tiles.at(i);
            m_stripCache.insert(answer.keys.at(i), new QImage(tile), qMax(1, int((tile.sizeInBytes() + 1023) / 1024)));
            // (A job that has gone leaves its tiles for the one that follows it.)
            const QByteArray packed = answer.packed.value(i);
            if (at >= 0 && m_stripJobs.at(at).tiles.size() == answer.first + i) landTile(m_stripJobs[at], tile, packed);
            else { const QString key = answer.keys.at(i); m_tilePool.start([key, tile, packed] { ThumbnailDiskCache::write(key, tile, packed); }); }
        }
    }
    if (at >= 0) {
        // (As it was drawn before there was a tile thread: nothing is lost
        // but the thread, for this strip.)
        if (answer.unbuilt && current) m_stripJobs[at].onWorker = true;
        if (answer.failed && current) {
            finishStripWork(at, QStringLiteral("failed"), answer.error);
        } else if (m_stripJobs.at(at).tiles.size() == m_stripJobs.at(at).keys.size()) {
            announceStrip(m_stripJobs.at(at));
            finishStripWork(at);
        }
    }
    scheduleStripWork();
}

// The tile thread's timeline, engine and decoders go on that thread (behind
// whatever it is doing); the next tile asked for makes them again.
void PlayerWorker::endTileThread() {
    if (!m_tileBench) return;
    m_tilePool.start([bench = std::move(m_tileBench)]() mutable { bench.reset(); });
    m_tileBench.reset();
}

// A waveform job's sound is ready: told to whoever shows the clip. The
// peaks themselves are in the store (WavePeaks::Store), under the id.
void PlayerWorker::announceWave(const StripJob &job, const std::shared_ptr<WavePeaks::Data> &data) {
    const QVariantMap wave{{"id", job.source.id}, {"rate", data->rate()}, {"channels", data->channels()}, {"samples", data->samples()}};
    if (job.timeline && !m_waveHeld.contains(job.source.id)) m_waveHeld.insert(job.source.id, data);
    if (!job.timeline) {
        // A bin clip's are held too, the latest of them, until its picture
        // has taken them up (64 MiB of peaks: about twelve hours of mono).
        for (int i = int(m_waveLibrary.size()) - 1; i >= 0; --i)
            if (m_waveLibrary.at(i).first == job.source.id) m_waveLibrary.removeAt(i);
        m_waveLibrary.prepend({job.source.id, data});
        qint64 bytes = 0;
        for (int i = 0; i < m_waveLibrary.size(); ++i) {
            bytes += m_waveLibrary.at(i).second->bytes();
            if (i > 0 && bytes > (qint64(64) << 20)) { m_waveLibrary.resize(i); break; }
        }
    }
    if (!job.libraryPath.isEmpty() && !job.timeline) emit libraryWaveformReady(job.libraryPath, wave);
    else if (job.lane >= 1) emit connectedWaveformReady(job.index, job.start, wave);
    else emit waveformReady(job.index, job.start, wave);
}

// The next stretch of a sound's peaks, made on the peak thread: a file is
// read through by a reader of its own, as the mixer reads it (a proxy when
// the timeline plays one); a clip whose sound is not a file's as it is (a
// speed map, a compound, an EQ…) is played alone through a mixer of its own.
//
// Threads: this thread plans the batch and keeps the job and the counters.
// The peak thread (one, so batches never overlap) owns the WaveBuild while a
// batch runs; it is handed what it needs by value and answers with a queued
// call. `stop` (atomic) ends a batch whose job has gone; what it made stays
// in the build, and the next job of that sound carries on from there. No
// batch is asked for while the editor plays or exports, and a batch is short.
//
// Kept between runs (ThumbnailDiskCache::keep): a sound's finished peaks, a
// file each, under a mark that says exactly which sound they are of; looked
// for before any sound is read.
void PlayerWorker::makePeaks(StripJob &job) {
    if (!job.stop) job.stop = std::make_shared<std::atomic_bool>(false);
    if (job.taskId.isEmpty()) job.taskId = QStringLiteral("cache:") + QUuid::createUuid().toString(QUuid::WithoutBraces);  // what its answer is matched by
    auto &build = m_waveBuilds[job.source.id];
    if (!build) build = std::make_shared<WaveBuild>();
    const bool file = job.source.kind == WavePeaks::Source::File;
    if (file && job.playerPath.isEmpty()) job.playerPath = m_timeline.playbackPath(job.source.path);
    job.making = true;
    const auto *profile = m_timeline.profile();
    const int fpsNum = profile ? profile->frame_rate_num() : 25, fpsDen = profile ? profile->frame_rate_den() : 1;
    m_peakPool.start([this, taskId = job.taskId, stop = job.stop, build, source = job.source, readPath = job.playerPath, file, fpsNum, fpsDen,
                      sources = file ? QVariantMap() : m_timeline.mediaSources(), keeping = ThumbnailDiskCache::keeping(),
                      version = QCoreApplication::applicationVersion(), pause = qEnvironmentVariableIntValue("OMAEDIT_PEAK_PAUSE_MS")] {
        // OMAEDIT_PEAK_PAUSE_MS: every batch begins this much later (for
        // tests: a batch still out when an edit ends its job).
        if (pause > 0) QThread::msleep(pause);
        WaveAnswer answer;
        answer.taskId = taskId;
        answer.id = source.id;
        auto *store = WavePeaks::Store::instance();
        StageTimer::Scope timed("decor.wave.peaks");
        // Which sound exactly: the file as it is on disk, or everything the
        // clip's sound is made from.
        QString mark;
        if (file) {
            mark = QStringLiteral("peaks-v2|file|") + sha(QVariantList{readPath, source.stream, version, RenderSnapshot::filesNow(readPath)});
        } else {
            const QVariantMap nested = RenderSnapshot::nestedDocuments({source.clip});
            mark = QStringLiteral("peaks-v2|clip|") + sha(QVariantList{source.clip, fpsNum, fpsDen, sources, nested, version,
                                                                      RenderSnapshot::filesNow(QVariantList{source.clip, sources, nested})});
        }
        const WavePeaks::Recipe recipe{source.kind, readPath, source.stream, source.clip, sources, fpsNum, fpsDen};
        const auto answerWith = [&](const std::shared_ptr<WavePeaks::Data> &data) {
            answer.data = data;
            answer.done = true;
            answer.made = answer.total = data->samples();
        };
        const auto send = [&] { QMetaObject::invokeMethod(this, [this, answer] { peaksMade(answer); }, Qt::QueuedConnection); };
        // Made already, in this run or an earlier one.
        if (store->mark(source.id) == mark)
            if (const auto known = store->find(source.id); known && known->complete()) {
                answerWith(known);
                return send();
            }
        if (build->mark != mark) *build = WaveBuild{mark, nullptr, nullptr, nullptr, 0};
        if (!build->data && keeping)
            if (const auto kept = WavePeaks::Data::unpacked(ThumbnailDiskCache::kept(mark, "peaks"))) {
                store->put(source.id, mark, kept, recipe);
                answer.kept = 1;
                answerWith(kept);
                return send();
            }
        if (!build->data && !stop->load()) {
            int rate = 48000, channels = 2;
            if (file) {
                QString error;
                AudioSampleSource::Options options;
                options.streamIndex = source.stream;
                build->reader = AudioSampleSource::Reader::open(readPath, options, &error);
                if (build->reader) {
                    rate = build->reader->sampleRate();
                    channels = qBound(1, build->reader->channelCount(), 2);  // more than two are folded to stereo, as they play
                    build->total = build->reader->sampleCount(rate);
                } else if (!error.contains(QLatin1String("no decodable sound stream"))) {
                    answer.failed = true;  // (a picture with no sound plays as silence: a flat wave)
                }
            } else {
                build->mixer = std::make_unique<sound::Mixer>();
                build->mixer->configure({{source.clip}, {}, {}, sources, fpsNum, fpsDen});
                if (!build->mixer->unsupported().isEmpty()) answer.failed = true;
                build->total = sound::Mixer::frameStart(source.clip.value(ClipKey::frames).toLongLong(), double(fpsNum) / qMax(1, fpsDen), 48000);
            }
            if (answer.failed) {
                *build = WaveBuild{};
                return send();
            }
            build->data = std::make_shared<WavePeaks::Data>(rate, channels, build->total);
            // In the store from the first: the picture fills in as it is made.
            store->put(source.id, mark, build->data, recipe);
        }
        // A batch is short, so that play starting meanwhile waits for little.
        QElapsedTimer spent;
        spent.start();
        const auto &data = build->data;
        const int rate = data ? data->rate() : 48000, channels = data ? data->channels() : 2;
        bool first = true;
        while (data && !stop->load() && data->made() < build->total && (first || spent.elapsed() < 40)) {
            first = false;
            const qint64 from = data->made();
            const int count = int(qMin<qint64>(32768, build->total - from));
            QString error;
            if (build->reader) {
                const QVector<float> pcm = build->reader->read(from, count, rate, channels, &error);
                if (pcm.size() != qsizetype(count) * channels) { answer.failed = true; break; }
                data->append(pcm.constData(), count);
            } else {
                sound::Samples pcm;
                if (!build->mixer->render(from, count, rate, channels, pcm, &error) || qsizetype(pcm.size()) != qsizetype(count) * channels) { answer.failed = true; break; }
                data->append(pcm.data(), count);
            }
        }
        if (answer.failed) {
            *build = WaveBuild{};
            return send();
        }
        if (!data) return send();  // (its job went before anything was opened: nothing made, nothing to say)
        answer.data = data;
        answer.made = data->made();
        answer.total = build->total;
        if (data->made() >= build->total) {
            data->finish();
            if (keeping) ThumbnailDiskCache::keep(mark, "peaks", data->packed());
            answer.done = true;
            answer.read = 1;
            build->reader.reset();
            build->mixer.reset();
        }
        store->touch(source.id);
        send();
    });
}

void PlayerWorker::peaksMade(const WaveAnswer &answer) {
    m_peaksRead += answer.read;
    m_peaksKept += answer.kept;
    if (answer.done || answer.failed) m_waveBuilds.remove(answer.id);
    if (answer.done) m_waveChecked.insert(answer.id, m_cacheFeedbackClock.elapsed());
    for (int at = 0; at < m_stripJobs.size(); ++at) {
        StripJob &job = m_stripJobs[at];
        if (job.taskId != answer.taskId || !job.making || !job.wave) continue;
        job.making = false;
        job.waveMade = answer.made;
        job.waveTotal = answer.total;
        if (answer.failed) {
            finishStripWork(at, QStringLiteral("failed"), tr("Could not open waveform source."));
        } else if (answer.done) {
            announceWave(job, answer.data);
            finishStripWork(at);
        }
        break;
    }
    scheduleStripWork();
}
