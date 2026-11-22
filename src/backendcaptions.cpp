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

QVariantMap Backend::defaultCaptionStyle() {
    return {{"font", QString()}, {"size", "small"}, {"pixelSize", 0}, {"color", "#ffffff"}, {"weight", QString()},
            {"italic", false}, {"outline", true}, {"bg", "#99000000"}, {"align", "centre"}, {"place", "bottom"},
            {"margin", 8}, {"width", 80}, {"lineSpacing", 1.0}, {"tracking", 0},
            {"maxChars", 42}, {"maxLines", 2}, {"minSeconds", 1.0}};
}

QVariantMap Backend::captionStyleSettings() const {
    QVariantMap style = defaultCaptionStyle();
    for (const QVariant &v : m_tracks) {
        const QVariantMap mix = v.toMap().value(QStringLiteral("mix")).toMap();
        if (mix.value(QStringLiteral("editKind")) != QStringLiteral("caption") || !mix.contains(QStringLiteral("captionStyle")))
            continue;
        const QVariantMap stored = mix.value(QStringLiteral("captionStyle")).toMap();
        for (auto it = stored.begin(); it != stored.end(); ++it)
            if (style.contains(it.key())) style[it.key()] = it.value();
        break;
    }
    return style;
}

// The look a new caption is made with, and the whole style for a caption
// track made to hold it. A preset size unless a pixel size is set.
QVariantMap Backend::captionStyle() const {
    const QVariantMap settings = captionStyleSettings();
    QVariantMap style = settings;
    for (const char *key : {"maxChars", "maxLines", "minSeconds"}) style.remove(QLatin1String(key));
    if (style.value(QStringLiteral("pixelSize")).toDouble() <= 0) style.remove(QStringLiteral("pixelSize"));
    style.insert(QStringLiteral("captionStyle"), settings);
    return style;
}

bool Backend::setCaptionStyle(const QVariantMap &input, bool everyCaption) {
    if (!hasTimeline() || m_exporting) {
        setStatus(tr("Caption Style needs a timeline, and no export running"));
        return false;
    }
    QVariantMap style = captionStyleSettings();
    const auto refuse = [this](const QString &why) { setStatus(tr("Caption style not set: %1").arg(why)); return false; };
    const auto number = [](const QVariant &v, double lo, double hi, bool *ok) {
        const double d = v.toDouble(ok);
        if (*ok) *ok = d >= lo && d <= hi;
        return d;
    };
    const QVariantMap value = plainVariant(input).toMap();
    for (auto it = value.begin(); it != value.end(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        bool ok = true;
        if (!style.contains(key)) return refuse(tr("%1 is not a caption style setting").arg(key));
        if (key == "font") style[key] = v.toString().trimmed();
        else if (key == "size") { if (!QStringList{"small", "medium", "large"}.contains(v.toString())) return refuse(tr("size is small, medium or large")); style[key] = v.toString(); }
        else if (key == "pixelSize") { const double d = number(v, 0, 2000, &ok); if (!ok) return refuse(tr("pixelSize is 0 to 2000")); style[key] = qRound(d); }
        else if (key == "color") { const QString c = v.toString().toLower(); if (!QRegularExpression("^#[0-9a-f]{6}$").match(c).hasMatch()) return refuse(tr("color is #rrggbb")); style[key] = c; }
        else if (key == "bg") {
            QString c = v.toString().toLower();
            if (QRegularExpression("^#[0-9a-f]{6}$").match(c).hasMatch()) c = "#ff" + c.mid(1);
            if (!c.isEmpty() && !QRegularExpression("^#[0-9a-f]{8}$").match(c).hasMatch()) return refuse(tr("bg is empty, #rrggbb or #aarrggbb"));
            style[key] = c;
        }
        else if (key == "weight") { if (!QStringList{"", "bold"}.contains(v.toString())) return refuse(tr("weight is empty or bold")); style[key] = v.toString(); }
        else if (key == "italic" || key == "outline") { if (v.metaType().id() != QMetaType::Bool) return refuse(tr("%1 is true or false").arg(key)); style[key] = v.toBool(); }
        else if (key == "align") { if (!QStringList{"left", "centre", "right"}.contains(v.toString())) return refuse(tr("align is left, centre or right")); style[key] = v.toString(); }
        else if (key == "place") { if (!QStringList{"top", "centre", "bottom"}.contains(v.toString())) return refuse(tr("place is top, centre or bottom")); style[key] = v.toString(); }
        else if (key == "margin") { const double d = number(v, 0, 45, &ok); if (!ok) return refuse(tr("margin is 0 to 45 (percent of the height)")); style[key] = d; }
        else if (key == "width") { const double d = number(v, 20, 100, &ok); if (!ok) return refuse(tr("width is 20 to 100 (percent of the width)")); style[key] = d; }
        else if (key == "lineSpacing") { const double d = number(v, .5, 4, &ok); if (!ok) return refuse(tr("lineSpacing is 0.5 to 4")); style[key] = d; }
        else if (key == "tracking") { const double d = number(v, -50, 200, &ok); if (!ok) return refuse(tr("tracking is -50 to 200")); style[key] = d; }
        else if (key == "maxChars") { const double d = number(v, 16, 80, &ok); if (!ok) return refuse(tr("maxChars is 16 to 80")); style[key] = qRound(d); }
        else if (key == "maxLines") { const double d = number(v, 1, 3, &ok); if (!ok) return refuse(tr("maxLines is 1 to 3")); style[key] = qRound(d); }
        else if (key == "minSeconds") { const double d = number(v, .3, 10, &ok); if (!ok) return refuse(tr("minSeconds is 0.3 to 10")); style[key] = d; }
    }
    // The centre of the frame has no margin; centre of the words, no width of its own.
    QVariantMap drawn = style;
    if (drawn.value("pixelSize").toDouble() <= 0) drawn.remove("pixelSize");
    emit captionStyleRequested(drawn, everyCaption);
    return true;
}

bool Backend::regenerateCaptions() {
    m_regeneratingCaptions = true;
    const bool started = generateCaptionsNow(false);
    if (!started || m_captionQueue.isEmpty()) m_regeneratingCaptions = false;
    return started;
}

QVariantList Backend::transcriptWords(const QVariantMap &segment) {
    const QVariantList own = segment.value(QStringLiteral("words")).toList();
    if (!own.isEmpty()) return own;
    // A transcript from before word timings: its words share its time out
    // by their length, each with room for the space after it.
    const QStringList words = segment.value(QStringLiteral("text")).toString().simplified().split(' ', Qt::SkipEmptyParts);
    const double start = segment.value(QStringLiteral("start")).toDouble(), end = segment.value(QStringLiteral("end")).toDouble();
    int letters = 0;
    for (const QString &w : words) letters += w.size() + 1;
    QVariantList out;
    double at = start;
    for (const QString &w : words) {
        const double next = at + (end - start) * (w.size() + 1) / qMax(1, letters);
        out.append(QVariantMap{{"text", w}, {"start", qRound(at)}, {"end", qRound(next)}});
        at = next;
    }
    return out;
}

// Words split into `parts` runs of about the same number of letters, in
// order (each run at least one word).
static QList<QList<int>> evenRuns(const QStringList &words, int parts) {
    QList<QList<int>> runs;
    int total = 0;
    for (const QString &w : words) total += w.size() + 1;
    int done = 0, at = 0;
    for (int part = 1; part <= parts; ++part) {
        QList<int> run;
        const double target = double(total) * part / parts;
        const int left = parts - part; // words the later runs need
        while (at < words.size() - left) {
            const int next = done + words.at(at).size() + 1;
            // Stop where the run's end is nearer the target than one word more.
            if (!run.isEmpty() && part < parts && qAbs(done - target) <= qAbs(next - target)) break;
            run.append(at++);
            done = next;
        }
        runs.append(run);
    }
    return runs;
}

// The lines a caption of `words` takes: as few as fit in maxChars, their
// lengths evened out. Empty when more than maxLines are needed.
static QStringList captionLines(const QStringList &words, int maxChars, int maxLines) {
    for (int lines = 1; lines <= qMin(maxLines, int(words.size())); ++lines) {
        QStringList out;
        bool fits = true;
        for (const QList<int> &run : evenRuns(words, lines)) {
            QStringList line;
            for (int i : run) line << words.at(i);
            const QString text = line.join(' ');
            // A single word longer than a line still goes on one.
            if (text.size() > maxChars && line.size() > 1) fits = false;
            out << text;
        }
        if (fits) return out;
    }
    return {};
}

QVariantList Backend::captionCuesFromWords(const QVariantList &input, const QVariantMap &lengths) {
    const int maxChars = qBound(16, lengths.value(QStringLiteral("maxChars"), 42).toInt(), 80);
    const int maxLines = qBound(1, lengths.value(QStringLiteral("maxLines"), 2).toInt(), 3);
    const double minMs = qBound(.3, lengths.value(QStringLiteral("minSeconds"), 1.).toDouble(), 10.) * 1000;
    constexpr double kPauseMs = 700; // a pause this long starts a new caption
    struct Word { QString text; double start, end; };
    // Phrases: a sentence's end, or a pause, ends one.
    QList<QList<Word>> phrases;
    for (const QVariant &v : input) {
        const QVariantMap m = v.toMap();
        const QString text = m.value(QStringLiteral("text")).toString().simplified();
        if (text.isEmpty()) continue;
        const double start = m.value(QStringLiteral("start")).toDouble();
        const Word word{text, start, qMax(start, m.value(QStringLiteral("end")).toDouble())};
        if (phrases.isEmpty()) { phrases.append({word}); continue; }
        const Word &last = phrases.last().last();
        const bool sentence = last.text.endsWith('.') || last.text.endsWith('?') || last.text.endsWith('!');
        if (sentence || word.start - last.end >= kPauseMs) phrases.append({word});
        else phrases.last().append(word);
    }
    // Each phrase over as few captions as hold it, shared out evenly, so no
    // caption is left with a word or two of the one before.
    QVariantList cues;
    for (const QList<Word> &phrase : std::as_const(phrases)) {
        QStringList texts;
        for (const Word &w : phrase) texts << w.text;
        for (int parts = 1; parts <= texts.size(); ++parts) {
            QList<QStringList> captions;
            const QList<QList<int>> runs = evenRuns(texts, parts);
            bool fits = true;
            for (const QList<int> &run : runs) {
                QStringList words;
                for (int i : run) words << texts.at(i);
                const QStringList lines = captionLines(words, maxChars, maxLines);
                if (lines.isEmpty()) { fits = false; break; }
                captions << lines;
            }
            if (!fits && parts < texts.size()) continue;
            for (int c = 0; c < runs.size(); ++c)
                cues.append(QVariantMap{{"start", phrase.at(runs[c].first()).start}, {"end", phrase.at(runs[c].last()).end},
                                        {"text", c < captions.size() ? captions[c].join('\n') : texts.mid(runs[c].first(), runs[c].size()).join(' ')}});
            break;
        }
    }
    // Long enough to read, where the next caption leaves room; never over it.
    for (int i = 0; i < cues.size(); ++i) {
        QVariantMap cue = cues.at(i).toMap();
        const double start = cue.value("start").toDouble();
        const double next = i + 1 < cues.size() ? cues.at(i + 1).toMap().value("start").toDouble() : 1e18;
        const double end = qMax(cue.value("end").toDouble(), qMin(start + minMs, next));
        cue["end"] = qMin(end, next);
        cues[i] = cue;
    }
    return cues;
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
    const QVariantMap lengths = captionStyleSettings();
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
        // The words the clip shows (by their middle), made into captions of
        // the lengths the caption style sets.
        QVariantList shown;
        for (const QVariant &t : transcript)
            for (const QVariant &w : transcriptWords(t.toMap())) {
                const QVariantMap word = w.toMap();
                const double middle = (word.value("start").toDouble() + word.value("end").toDouble()) / 2000. * fps;
                if (middle >= in && middle < out + 1) shown.append(word);
            }
        for (const QVariant &c : captionCuesFromWords(shown, lengths)) {
            const QVariantMap cue = c.toMap();
            const int a = qMax(in, qRound(cue.value(QStringLiteral("start")).toDouble() / 1000. * fps));
            const int b = qMin(out + 1, qRound(cue.value(QStringLiteral("end")).toDouble() / 1000. * fps));
            if (b - a < 1) continue;
            cues.append(QVariantMap{{QStringLiteral("start"), start + a - in}, {QStringLiteral("frames"), b - a},
                                    {QStringLiteral("text"), cue.value(QStringLiteral("text"))}});
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
        m_regeneratingCaptions = false;
        return false;
    }
    // Made again: every caption on the caption tracks goes, the new ones in
    // their place, in the one undo step.
    QVariantList remove;
    if (std::exchange(m_regeneratingCaptions, false))
        for (int i = 0; i < m_connected.size(); ++i)
            if (m_connected.at(i).toMap().value(ClipKey::caption).toBool()) remove.append(i);
    emit placeCaptionsRequested(cues, captionStyle(), remove, remove.isEmpty() ? tr("Generate captions") : tr("Regenerate captions"));
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
