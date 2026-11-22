#include "backend_p.h"

// ── Transcription ─────────────────────────────────────────────────────────
void Backend::transcribe(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    if (m_transcriber->active()) {
        setStatus(tr("A transcription is already running"));
        return;
    }
    const QString why = m_transcriber->status();
    if (!why.isEmpty()) {
        setStatus(why);
        emit failed(why);
        return;
    }
    const QVariantMap entry = m_library.at(libraryIndex).toMap();
    const QString path = entry.value(QStringLiteral("path")).toString();
    if (entry.value(QStringLiteral("offline")).toBool() || path.isEmpty() || (path.contains(QLatin1Char(':')) && !QFileInfo::exists(path))) {
        setStatus(tr("That clip is offline — nothing to transcribe"));
        return;
    }
    if (!entry.value(QStringLiteral("hasAudio"), true).toBool()) {
        setStatus(tr("%1 has no audio to transcribe").arg(entry.value(QStringLiteral("name")).toString()));
        return;
    }
    m_transcriber->start(libraryIndex, path, entry.value(QStringLiteral("libraryId")).toString(),
                         entry.value(QStringLiteral("name")).toString());
}

// Where the clip a transcription was started for is now. The index can name
// another clip by the time whisper is done (a removal, another project): the
// result goes to the clip with this ID only.
int Backend::transcriptClip(const QVariantMap &job) const {
    const QString id = job.value(QStringLiteral("libraryId")).toString();
    const QString source = job.value(QStringLiteral("path")).toString();
    for (int i = 0; i < m_library.size(); ++i) {
        const QVariantMap e = m_library.at(i).toMap();
        if (e.value(QStringLiteral("path")).toString() == source
            && (id.isEmpty() || e.value(QStringLiteral("libraryId")).toString() == id))
            return i;
    }
    return -1;
}

void Backend::storeTranscript(int index, const QVariantList &segments, const QString &language) {
    setLibraryField(index, QStringLiteral("transcript"), segments.isEmpty() ? QVariant() : QVariant(segments));
    setLibraryField(index, QStringLiteral("transcriptLanguage"),
                    language.isEmpty() || segments.isEmpty() ? QVariant() : QVariant(language));
}

void Backend::transcriptionFinished(bool ok, int index, const QString &error) {
    const QString name = index >= 0 && index < m_library.size()
                             ? m_library.at(index).toMap().value(QStringLiteral("name")).toString() : QString();
    if (ok) {
        if (error.isEmpty())
            setStatus(tr("Transcript ready for %1").arg(name));
        else
            setStatus(tr("%1: %2").arg(name, error));
    } else {
        setStatus(error);
        emit failed(error);
    }
    continueCaptionQueue(ok);
}

// Generate Captions waits on its transcriptions: the next, or the captions.
void Backend::continueCaptionQueue(bool ok) {
    if (m_captionQueue.isEmpty()) return;
    m_captionTried.append(m_captionQueue.takeFirst());
    if (!ok) { m_captionQueue.clear(); return; }
    QTimer::singleShot(0, this, [this] {
        while (!m_captionQueue.isEmpty()) {
            for (int i = 0; i < m_library.size(); ++i)
                if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == m_captionQueue.first()) { transcribe(i); return; }
            m_captionTried.append(m_captionQueue.takeFirst()); // left the library meanwhile
        }
        generateCaptionsNow(true);
    });
}

void Backend::clearTranscript(int libraryIndex) {
    if (libraryIndex < 0 || libraryIndex >= m_library.size())
        return;
    if (!m_library.at(libraryIndex).toMap().contains(QStringLiteral("transcript")))
        return;
    setLibraryField(libraryIndex, QStringLiteral("transcript"), QVariant());
    setLibraryField(libraryIndex, QStringLiteral("transcriptLanguage"), QVariant());
    setStatus(tr("Transcript removed"));
}

void Backend::importCaptionsDialog() {
    if (!hasVideo())
        return;
    m_chooserMode = ChooserMode::ImportCaptions;
    m_chooser->openFile(QStringLiteral("Import Captions"), QStringLiteral("Subtitles"),
                        {QStringLiteral("*.srt"), QStringLiteral("*.vtt")});
}

void Backend::importCaptions(const QUrl &url) {
    if (!hasVideo() || m_exporting)
        return;
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not read %1").arg(QFileInfo(file).fileName()));
        return;
    }
    const QByteArray raw = file.readAll();
    QString text = QString::fromUtf8(raw);
    if (text.contains(QChar::ReplacementCharacter)) // not UTF-8: an old Latin-1 file
        text = QString::fromLatin1(raw);
    const QVariantList cues = Timeline::parseSubtitles(text, m_fps);
    if (cues.isEmpty()) {
        setStatus(tr("No captions found in %1").arg(QFileInfo(file).fileName()));
        return;
    }
    setStatus(tr("Importing %n caption(s) from %1…", nullptr, cues.size())
                  .arg(QFileInfo(file).fileName()));
    emit importCaptionsRequested(cues, captionStyle());
}

QVariantMap Backend::captionStyle() {
    return {{QStringLiteral("place"), QStringLiteral("lower")}, {QStringLiteral("size"), QStringLiteral("small")},
            {QStringLiteral("bg"), QStringLiteral("#99000000")}};
}

bool Backend::addCaption() {
    if (!hasVideo() || m_exporting) return false;
    const int frames = qMax(1, qRound(2 * (m_fps > 0 ? m_fps : 25.)));
    emit placeCaptionsRequested({QVariantMap{{QStringLiteral("start"), m_position}, {QStringLiteral("frames"), frames},
                                             {QStringLiteral("text"), tr("Caption")}}},
                                captionStyle(), {}, tr("Add caption"));
    setStatus(tr("Caption added at the playhead; double-click it to write its words"));
    return true;
}

bool Backend::splitCaption(int connectedIndex) {
    if (m_exporting || connectedIndex < 0 || connectedIndex >= m_connected.size()) return false;
    const QVariantMap entry = m_connected.at(connectedIndex).toMap();
    if (!entry.value(ClipKey::caption).toBool()) { setStatus(tr("Select a caption to split")); return false; }
    QStringList lines;
    for (const QString &line : entry.value(ClipKey::title).toString().split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty()) lines.append(line.trimmed());
    const int frames = entry.value(ClipKey::frames).toInt();
    if (lines.size() < 2 || frames < lines.size()) { setStatus(tr("Only a caption of two or more lines can be split")); return false; }
    // Each line gets time in proportion to its length: reading time.
    int letters = 0;
    for (const QString &line : std::as_const(lines)) letters += line.size();
    QVariantList cues;
    int at = entry.value(ClipKey::start).toInt(), left = frames;
    for (int i = 0; i < lines.size(); ++i) {
        const int rest = lines.size() - i - 1;
        const int share = i == lines.size() - 1 ? left
            : qBound(1, qRound(double(frames) * lines.at(i).size() / qMax(1, letters)), left - rest);
        cues.append(QVariantMap{{QStringLiteral("start"), at}, {QStringLiteral("frames"), share}, {QStringLiteral("text"), lines.at(i)}});
        at += share; left -= share;
    }
    emit placeCaptionsRequested(cues, Timeline::titleStyleOf(entry), {connectedIndex}, tr("Split caption"));
    setStatus(tr("Split into %n caption(s)", nullptr, lines.size()));
    return true;
}

bool Backend::joinCaptions(const QVariantList &indices) {
    if (m_exporting) return false;
    QList<QPair<int, int>> chosen; // start, flat index
    int track = -1;
    for (const QVariant &v : indices) {
        const int i = v.toInt();
        if (i < 0 || i >= m_connected.size()) return false;
        const QVariantMap e = m_connected.at(i).toMap();
        if (!e.value(ClipKey::caption).toBool() || (track >= 0 && e.value(ClipKey::track).toInt() != track)) {
            setStatus(tr("Select two or more captions on one caption track to join"));
            return false;
        }
        track = e.value(ClipKey::track).toInt();
        if (!std::any_of(chosen.begin(), chosen.end(), [i](const auto &c) { return c.second == i; }))
            chosen.append({e.value(ClipKey::start).toInt(), i});
    }
    if (chosen.size() < 2) { setStatus(tr("Select two or more captions on one caption track to join")); return false; }
    std::sort(chosen.begin(), chosen.end());
    QStringList words;
    QVariantList remove;
    int end = 0;
    for (const auto &c : std::as_const(chosen)) {
        const QVariantMap e = m_connected.at(c.second).toMap();
        words.append(e.value(ClipKey::title).toString().trimmed());
        end = qMax(end, c.first + e.value(ClipKey::frames).toInt());
        remove.append(c.second);
    }
    const QVariantMap first = m_connected.at(chosen.first().second).toMap();
    const int start = chosen.first().first;
    emit placeCaptionsRequested({QVariantMap{{QStringLiteral("start"), start}, {QStringLiteral("frames"), end - start},
                                             {QStringLiteral("text"), words.join(QLatin1Char(' '))}}},
                                Timeline::titleStyleOf(first), remove, tr("Join captions"));
    setStatus(tr("Joined %n caption(s)", nullptr, chosen.size()));
    return true;
}

bool Backend::generateCaptions() {
    m_captionQueue.clear();
    m_captionTried.clear();
    return generateCaptionsNow(false);
}

bool Backend::generateCaptionsNow(bool afterTranscribing) {
    if (!hasVideo() || m_exporting) return false;
    const double fps = m_fps > 0 ? m_fps : 25.;
    // Every clip whose sound is heard: storyline and connected, not titles.
    QVariantList clips;
    for (const QVariant &v : m_timeline) clips.append(v);
    for (const QVariant &v : m_connected) clips.append(v);
    QVariantList cues;
    QStringList missing;
    QSet<QString> links; // a picture and its linked sound are one source of speech
    int retimed = 0;
    for (const QVariant &v : std::as_const(clips)) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::gap).toBool() || !e.value(ClipKey::title).toString().isEmpty()
            || e.value(ClipKey::caption).toBool() || e.value(ClipKey::videoOnly).toBool())
            continue;
        const QString link = e.value(ClipKey::link).toString();
        if (!link.isEmpty() && links.contains(link)) continue;
        if (!link.isEmpty()) links.insert(link);
        const QString path = e.value(ClipKey::path).toString();
        QVariantMap source;
        for (const QVariant &l : std::as_const(m_library))
            if (l.toMap().value(QStringLiteral("path")).toString() == path) { source = l.toMap(); break; }
        if (source.isEmpty() || !source.value(QStringLiteral("hasAudio"), true).toBool())
            continue;
        const QVariantList transcript = source.value(QStringLiteral("transcript")).toList();
        if (transcript.isEmpty()) {
            if (!afterTranscribing && !m_captionTried.contains(path) && !missing.contains(path)) missing.append(path);
            continue;
        }
        // A retimed clip's sound no longer sits where the transcript says.
        if (!qFuzzyCompare(e.value(ClipKey::speed, 1.).toDouble(), 1.)
            || !e.value(ClipKey::retime).toMap().value(QStringLiteral("segments")).toList().isEmpty()) {
            ++retimed;
            continue;
        }
        const int in = e.value(ClipKey::in).toInt(), out = e.value(ClipKey::out).toInt();
        const int start = e.value(ClipKey::start).toInt();
        for (const QVariant &t : transcript) {
            const QVariantMap line = t.toMap();
            const QString text = line.value(QStringLiteral("text")).toString().trimmed();
            const int a = qMax(in, qRound(line.value(QStringLiteral("start")).toDouble() / 1000. * fps));
            const int b = qMin(out + 1, qRound(line.value(QStringLiteral("end")).toDouble() / 1000. * fps));
            if (text.isEmpty() || b - a < 1) continue;
            cues.append(QVariantMap{{QStringLiteral("start"), start + a - in}, {QStringLiteral("frames"), b - a},
                                    {QStringLiteral("text"), text}});
        }
    }
    if (!missing.isEmpty()) {
        if (!m_transcriber->available()) {
            setStatus(tr("Generate Captions cannot transcribe the clips: %1").arg(m_transcriber->status()));
            return false;
        }
        if (m_transcriber->active()) {
            setStatus(tr("A transcription is already running; try again when it is done"));
            return false;
        }
        m_captionQueue = missing;
        for (int i = 0; i < m_library.size(); ++i)
            if (m_library.at(i).toMap().value(QStringLiteral("path")).toString() == missing.first()) { transcribe(i); break; }
        setStatus(tr("Transcribing %n clip(s) first; the captions follow when that is done", nullptr, missing.size()));
        return true;
    }
    std::sort(cues.begin(), cues.end(), [](const QVariant &x, const QVariant &y) {
        return x.toMap().value(QStringLiteral("start")).toInt() < y.toMap().value(QStringLiteral("start")).toInt();
    });
    if (cues.isEmpty()) {
        setStatus(retimed ? tr("No captions: the clips with speech are retimed")
                          : tr("No speech was found in the timeline's clips"));
        return false;
    }
    emit placeCaptionsRequested(cues, captionStyle(), {}, tr("Generate captions"));
    setStatus(retimed ? tr("Generated %n caption(s); retimed clips were left out", nullptr, cues.size())
                      : tr("Generated %n caption(s)", nullptr, cues.size()));
    return true;
}

QVariantList Backend::captionCues() const {
    int captionTrack = -1;
    for (const QVariant &v : m_tracks) {
        const QVariantMap t = v.toMap();
        if (t.value(QStringLiteral("name")).toString() == QStringLiteral("Captions"))
            captionTrack = t.value(QStringLiteral("index")).toInt();
    }
    QVariantList cues;
    for (const QVariant &v : m_connected) {
        const QVariantMap e = v.toMap();
        const QString title = e.value(ClipKey::title).toString();
        if (title.isEmpty())
            continue;
        if (!e.value(ClipKey::caption).toBool()
            && e.value(ClipKey::track).toInt() != captionTrack)
            continue;
        QVariantMap cue;
        cue.insert(QStringLiteral("start"), e.value(ClipKey::start).toInt());
        cue.insert(QStringLiteral("frames"), e.value(ClipKey::frames).toInt());
        cue.insert(QStringLiteral("text"), title);
        cues.append(cue);
    }
    std::sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toInt()
               < b.toMap().value(QStringLiteral("start")).toInt();
    });
    return cues;
}

void Backend::exportCaptionsDialog() {
    if (captionCues().isEmpty()) {
        setStatus(tr("No captions to export — import a subtitle file or name a track “Captions”"));
        return;
    }
    m_chooserMode = ChooserMode::ExportCaptions;
    m_chooser->saveFile(QStringLiteral("Export Captions"),
                        projectName() + QStringLiteral(".srt"),
                        QStringLiteral("SubRip or WebVTT subtitles"), {QStringLiteral("*.srt"), QStringLiteral("*.vtt")});
}

bool Backend::exportCaptions(const QUrl &url) {
    const QVariantList cues = captionCues();
    if (cues.isEmpty())
        return false;
    QString path = url.toLocalFile();
    // WebVTT when the name says so; SubRip otherwise.
    const bool vtt = path.endsWith(QStringLiteral(".vtt"), Qt::CaseInsensitive);
    if (!vtt && !path.endsWith(QStringLiteral(".srt"), Qt::CaseInsensitive))
        path += QStringLiteral(".srt");
    // A complete replacement or none: a failed write keeps the old file.
    QSaveFile file(path);
    const QByteArray text = (vtt ? Timeline::formatVtt(cues, m_fps) : Timeline::formatSrt(cues, m_fps)).toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
        setStatus(tr("Could not write %1").arg(QFileInfo(path).fileName()));
        return false;
    }
    setStatus(tr("%n caption(s) written to %1", nullptr, cues.size())
                  .arg(QFileInfo(path).fileName()));
    return true;
}
