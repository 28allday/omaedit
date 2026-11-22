#include "backend_p.h"
#include "audiotimeline.h"
#include "retimemap.h"

// ── Beat maps: the beats of a recording's music, on its library clips ─────

namespace {
double firstPositive(const QVariantMap &meta, std::initializer_list<const char *> keys) {
    for (const char *key : keys)
        if (const double v = meta.value(QLatin1String(key)).toDouble(); v > 0) return v;
    return 0;
}
}

void Backend::setupBeats() {
    m_beatMapper = new BeatMapper(this);
    connect(this, &Backend::playingChanged, m_beatMapper, [this] { m_beatMapper->setYielding(playing()); });
    connect(this, &Backend::libraryChanged, this, &Backend::readBeatMaps);
    // The beat lines are made when asked for and kept until the clips or the
    // maps change; an edit with no mapped music about costs nothing.
    const auto forget = [this] {
        if (m_beatLinesValid && m_beatLines.isEmpty() && m_beatTimes.isEmpty()) return;
        m_beatLinesValid = false;
        emit beatLinesChanged();
    };
    connect(this, &Backend::timelineEntriesChanged, this, forget);
    connect(this, &Backend::beatMapsChanged, this, forget);
    connect(m_beatMapper, &BeatMapper::finished, this,
            [this](const QString &path, const QVariantMap &result, const QString &error, bool cancelled) {
        const QString name = QFileInfo(path).fileName();
        QString message;
        const bool found = !result.value(QStringLiteral("beats")).toList().isEmpty();
        if (cancelled) {
            message = tr("Beat mapping of %1 cancelled").arg(name);
        } else if (!error.isEmpty()) {
            message = tr("The beats of %1 could not be mapped: %2").arg(name, error);
        } else if (!found) {
            // Speech, effects, a drone: said plainly, and nothing is stored.
            message = tr("No steady beat was found in %1").arg(name);
        } else {
            storeBeatMap(path, result);
            message = tr("%1: %2 BPM, %n beat(s) mapped", nullptr, int(result.value(QStringLiteral("beats")).toList().size()))
                          .arg(name, QString::number(result.value(QStringLiteral("bpm")).toDouble(), 'f', 1));
        }
        setStatus(message);
        emit beatMapFinished(path, found && !cancelled && error.isEmpty(), message);
    });
}

// Every library clip of the file carries the map (a subclip is the same
// recording), so whichever is looked at has it.
void Backend::storeBeatMap(const QString &path, const QVariant &map) {
    for (int i = 0; i < m_library.size(); ++i)
        if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == path)
            setLibraryField(i, QStringLiteral("beatMap"), map);
    readBeatMaps();
}

// The library's maps by file. Told of every library change, and only a change
// to the maps themselves moves the revision the timeline's ticks follow.
void Backend::readBeatMaps() {
    // What the maps are, said briefly, so a library change that leaves them
    // alone (a thumbnail, a rating) costs no more than this.
    QString signature;
    QList<QPair<QString, QVariantMap>> found;
    QSet<QString> seen;
    for (const QVariant &v : std::as_const(m_library)) {
        const QVariantMap entry = v.toMap();
        const auto held = entry.constFind(QStringLiteral("beatMap"));
        if (held == entry.constEnd()) continue;
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QVariantMap map = held->toMap();
        const QVariantList list = map.value(QStringLiteral("beats")).toList();
        if (list.isEmpty() || path.isEmpty() || seen.contains(path)) continue;
        seen.insert(path);
        signature += path + QLatin1Char('|') + QString::number(list.size()) + QLatin1Char('|') + list.first().toString()
                     + QLatin1Char('|') + list.last().toString() + QLatin1Char('|') + map.value(QStringLiteral("bpm")).toString()
                     + QLatin1Char('|') + map.value(QStringLiteral("modified")).toString()
                     + QLatin1Char('|') + map.value(QStringLiteral("bar")).toMap().value(QStringLiteral("first"), -1).toString() + QLatin1Char('\n');
        found.append({path, map});
    }
    if (signature == m_beatSignature) return;
    m_beatSignature = signature;
    m_beatTimes.clear();
    for (const auto &[path, map] : std::as_const(found)) {
        BeatTimes times;
        const QVariantList list = map.value(QStringLiteral("beats")).toList();
        times.beats.reserve(list.size());
        for (const QVariant &beat : list) {
            const double seconds = beat.toDouble();
            // Ascending, as they were found; anything else in a file is left out.
            if (std::isfinite(seconds) && seconds >= 0 && (times.beats.isEmpty() || seconds > times.beats.last()))
                times.beats.append(seconds);
        }
        if (times.beats.isEmpty()) continue;
        times.duration = map.value(QStringLiteral("duration")).toDouble();
        times.bpm = map.value(QStringLiteral("bpm")).toDouble();
        // The bar, where the map has one: four beats, from the one named.
        if (const QVariantMap bar = map.value(QStringLiteral("bar")).toMap(); bar.contains(QStringLiteral("first"))) {
            const int per = bar.value(QStringLiteral("beats"), 4).toInt(), first = bar.value(QStringLiteral("first")).toInt();
            if (per >= 2 && per <= 16 && first >= 0 && first < per) { times.perBar = per; times.firstBar = first; }
        }
        m_beatTimes.insert(path, times);
    }
    ++m_beatMapRevision;
    emit beatMapsChanged();
}

bool Backend::mapBeats(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return false;
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    const QString path = entry.value(QStringLiteral("path")).toString();
    const QString name = entry.value(QStringLiteral("name")).toString();
    if (entry.value(QStringLiteral("multicam")).toBool() || ProjectFile::isProject(path)) {
        setStatus(tr("Map the beats of the recording itself: %1 is made of several").arg(name));
        return false;
    }
    if (entry.value(QStringLiteral("offline")).toBool() || path.isEmpty() || !QFileInfo(path).isFile()) {
        setStatus(tr("That clip is offline — there is no sound to map"));
        return false;
    }
    if (!entry.value(QStringLiteral("hasAudio"), true).toBool()) {
        setStatus(tr("%1 has no sound to map the beats of").arg(name));
        return false;
    }
    if (m_beatMapper->mapping(path)) {
        setStatus(tr("The beats of %1 are being mapped already").arg(name));
        return false;
    }
    if (!m_beatMapper->map(path))
        return false;
    setStatus(tr("Mapping the beats of %1…").arg(name));
    return true;
}

bool Backend::mapBeatsOn(int track, int index) {
    const QString path = entryOn(track, index).value(ClipKey::path).toString();
    for (int i = 0; i < m_library.size() && !path.isEmpty(); ++i)
        if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == path)
            return mapBeats(i);
    setStatus(tr("This clip has no recording in the library to map the beats of"));
    return false;
}

void Backend::clearBeatMap(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    const QString path = entry.value(QStringLiteral("path")).toString();
    m_beatMapper->cancel(path);
    if (!m_beatTimes.contains(path))
        return;
    storeBeatMap(path, QVariant());
    setStatus(tr("Beat map of %1 cleared").arg(entry.value(QStringLiteral("name")).toString()));
}

void Backend::clearBeatMapOn(int track, int index) {
    const QString path = entryOn(track, index).value(ClipKey::path).toString();
    for (int i = 0; i < m_library.size() && !path.isEmpty(); ++i)
        if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
            clearBeatMap(i);
            return;
        }
}

void Backend::cancelBeatMap(const QString &path) { m_beatMapper->cancel(path); }

QVariantMap Backend::beatMapOf(const QString &path) const {
    const auto it = m_beatTimes.constFind(path);
    const bool has = it != m_beatTimes.constEnd();
    return {{QStringLiteral("bpm"), has ? it->bpm : 0.}, {QStringLiteral("count"), has ? int(it->beats.size()) : 0},
            {QStringLiteral("mapping"), m_beatMapper->mapping(path)}};
}

// Which of its file's beats a timeline clip plays, as whole frames from the
// clip's start. The clip's time is followed as the mixer follows it: its in
// point, its speed (a reversed clip plays the file backwards), a speed map,
// an interpreted frame rate, a marked source origin, or a sound cut's exact
// times. Each frame's stretch of the file is looked up among the beats.
QVector<Backend::BeatAt> Backend::beatLinesOfEntry(const QVariantMap &e) const {
    QVector<BeatAt> frames;
    const auto it = m_beatTimes.constFind(e.value(ClipKey::path).toString());
    const int length = e.value(ClipKey::frames).toInt();
    const double fps = m_fps;
    if (it == m_beatTimes.constEnd() || length < 1 || fps <= 0 || e.value(ClipKey::gap).toBool()
        || !e.value(ClipKey::title).toString().isEmpty() || e.contains(ClipKey::compoundJson))
        return frames;
    const QVector<double> &beats = it->beats;
    const double duration = it->duration;
    const QVariantMap meta = e.value(ClipKey::sourceMeta).toMap();
    const double forced = meta.value(QStringLiteral("forceFps")).toDouble(), native = firstPositive(meta, {"sourceFps", "fps"});
    const double factor = forced > 0 && native > 0 ? forced / native : 1;
    const double speed = e.value(ClipKey::speed, 1.).toDouble() == 0 ? 1. : e.value(ClipKey::speed, 1.).toDouble();
    const double effective = std::abs(speed) * factor;
    const QVariantMap retime = e.value(ClipKey::retime).toMap();
    const bool marked = e.contains(ClipKey::sourceOrigin) && retime.isEmpty();
    const double origin = e.value(ClipKey::sourceOrigin).toDouble();
    const double first = e.value(ClipKey::in).toDouble() + e.value(ClipKey::mixIn).toDouble();
    // The file's time, in seconds, at `frame` frames into the clip.
    std::function<double(double)> at;
    RetimeMap map;
    if (e.contains(ClipKey::audioTiming)) {
        AudioTimeline::Time in;
        if (!AudioTimeline::readTime(e.value(ClipKey::audioTiming).toMap().value(QStringLiteral("in")), &in) || in.rate <= 0)
            return frames;
        const double start = double(in.ticks) / double(in.rate);
        at = [=](double frame) { const double t = start + frame / fps; return speed < 0 ? duration - t : t; };
    } else if (!retime.isEmpty() || (marked && !qFuzzyCompare(effective, 1.))) {
        const int nativeFrames = int(firstPositive(meta, {"sourceDuration", "duration"}));
        if (!map.set(retime.isEmpty() ? RetimeMap::conform(nativeFrames, native, fps, speed, factor, origin) : retime))
            return frames;
        at = [&map, first, factor, fps](double frame) { return map.sourceAt(first + frame) * factor / fps; };
    } else {
        at = [=](double frame) {
            const double t = (first + frame) / fps * effective + (marked ? origin : 0.);
            return speed < 0 ? duration - t : t;
        };
    }
    // What a beat of the file is: a beat, or the first of a bar, of every
    // fourth bar, of every sixteenth… A map without the bar counts in the
    // same fours from its first beat, so its lines thin as the others do.
    const int perBar = it->perBar, firstBar = std::max(0, it->firstBar);
    const bool barred = it->firstBar >= 0;
    const auto what = [&](qsizetype beat) {
        BeatAt out;
        const qsizetype along = beat - firstBar;
        if (along % perBar != 0) return out;
        out.bar = barred;
        out.level = 1;
        for (qsizetype bar = along / perBar; out.level < 8 && bar % 4 == 0; bar /= 4) ++out.level;
        return out;
    };
    double from = at(0);
    for (int frame = 0; frame < length; ++frame) {
        const double to = at(frame + 1);
        const double low = std::min(from, to), high = std::max(from, to);
        if (high > low) {
            for (auto beat = std::lower_bound(beats.cbegin(), beats.cend(), low); beat != beats.cend() && *beat < high; ++beat) {
                // The frame the beat starts in or, past its middle, the next.
                const int landed = frame + ((*beat - from) / (to - from) >= 0.5 ? 1 : 0);
                if (landed >= length) continue;
                BeatAt line = what(beat - beats.cbegin());
                line.frame = landed;
                frames.append(line);
            }
        }
        from = to;
    }
    // One to a frame: where two beats land on the same one, the greater.
    std::sort(frames.begin(), frames.end(), [](const BeatAt &a, const BeatAt &b) { return a.frame != b.frame ? a.frame < b.frame : a.level > b.level; });
    frames.erase(std::unique(frames.begin(), frames.end(), [](const BeatAt &a, const BeatAt &b) { return a.frame == b.frame; }), frames.end());
    return frames;
}

QVector<int> Backend::beatsOfEntry(const QVariantMap &e) const {
    QVector<int> frames;
    for (const BeatAt &beat : beatLinesOfEntry(e)) frames.append(beat.frame);
    return frames;
}

QVariantList Backend::beatLines() const {
    if (m_beatLinesValid)
        return m_beatLines;
    m_beatLines.clear();
    m_beatLinesValid = true;
    if (m_beatTimes.isEmpty())
        return m_beatLines;
    for (const QVariantList *list : {&m_timeline, &m_connected})
        for (qsizetype at = 0; at < list->size(); ++at) {
            const QVariantMap entry = list->at(at).toMap();
            const QVector<BeatAt> beats = beatLinesOfEntry(entry);
            if (beats.isEmpty()) continue;
            const int start = entry.value(ClipKey::start).toInt();
            const int track = list == &m_timeline ? 0 : entry.value(ClipKey::track).toInt();
            const int index = list == &m_timeline ? int(at) : entry.value(ClipKey::index).toInt();
            for (const BeatAt &beat : beats)
                m_beatLines.append(QVariantMap{{QStringLiteral("frame"), start + beat.frame}, {QStringLiteral("level"), beat.level},
                                               {QStringLiteral("bar"), beat.bar}, {QStringLiteral("track"), track}, {QStringLiteral("index"), index}});
        }
    return m_beatLines;
}

QVariantList Backend::clipBeats(int track, int index) const {
    QVariantList out;
    if (m_beatTimes.isEmpty())
        return out;
    for (const int frame : beatsOfEntry(entryOn(track, index)))
        out.append(frame);
    return out;
}

QVariantList Backend::beatFrames() const {
    QVariantList out;
    if (m_beatTimes.isEmpty())
        return out;
    QSet<int> seen;
    for (const QVariantList *list : {&m_timeline, &m_connected})
        for (const QVariant &v : *list) {
            const QVariantMap entry = v.toMap();
            const int start = entry.value(ClipKey::start).toInt();
            for (const int frame : beatsOfEntry(entry))
                if (!seen.contains(start + frame)) { seen.insert(start + frame); out.append(start + frame); }
        }
    return out;
}
