#include "backend_p.h"
#include "captionformats.h"

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
    // Generate Captions asks for each channel as a speaker (Caption Style ▸ Rules).
    m_transcriber->start(libraryIndex, path, entry.value(QStringLiteral("libraryId")).toString(),
                         entry.value(QStringLiteral("name")).toString(), m_twoChannelTranscripts.remove(path));
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

bool Backend::setTranscriptLine(int libraryIndex, int line, const QString &text) {
    if (m_exporting) return false;
    const QVariantMap entry = libraryIndex >= 0 && libraryIndex < m_library.size() ? m_library.at(libraryIndex).toMap() : QVariantMap();
    const QVariantList transcript = entry.value(QStringLiteral("transcript")).toList();
    if (line < 0 || line >= transcript.size()) {
        setStatus(tr("That clip has no such transcript line"));
        return false;
    }
    const QString words = text.simplified();
    if (words.isEmpty()) {
        setStatus(tr("A transcript line needs words"));
        return false;
    }
    const QVariantMap before = transcript.at(line).toMap();
    if (words == before.value(QStringLiteral("text")).toString().simplified()) return true;
    // Its word timings belong to the words heard; typed ones share its time.
    QVariantMap after = before;
    after.insert(QStringLiteral("text"), words);
    after.remove(QStringLiteral("words"));
    emit transcriptEditRequested({{"libraryId", entry.value(QStringLiteral("libraryId"))}, {"file", entry.value(QStringLiteral("path"))},
                                  {"line", line}, {"before", before}, {"after", after}},
                                 tr("Transcript words"));
    return true;
}

// A transcript line's words, as typed (after) or as they were (before), on
// the library clip the edit was made on.
void Backend::applyTranscriptEdit(const QVariantMap &edit, bool after) {
    const QString id = edit.value(QStringLiteral("libraryId")).toString(), file = edit.value(QStringLiteral("file")).toString();
    const int line = edit.value(QStringLiteral("line")).toInt();
    for (int i = 0; i < m_library.size(); ++i) {
        const QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("path")).toString() != file || entry.value(QStringLiteral("libraryId")).toString() != id)
            continue;
        QVariantList transcript = entry.value(QStringLiteral("transcript")).toList();
        if (line < 0 || line >= transcript.size()) return;
        transcript[line] = edit.value(after ? QStringLiteral("after") : QStringLiteral("before"));
        setLibraryField(i, QStringLiteral("transcript"), transcript);
        return;
    }
}

void Backend::importCaptionsDialog() {
    if (!hasVideo())
        return;
    m_chooserMode = ChooserMode::ImportCaptions;
    m_chooser->openFile(QStringLiteral("Import Captions"), QStringLiteral("Captions"),
                        {QStringLiteral("*.srt"), QStringLiteral("*.vtt"), QStringLiteral("*.itt"), QStringLiteral("*.ttml"),
                         QStringLiteral("*.xml"), QStringLiteral("*.dfxp"), QStringLiteral("*.scc")});
}

void Backend::importCaptions(const QUrl &url) {
    if (!hasVideo() || m_exporting)
        return;
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not read %1").arg(QFileInfo(file).fileName()));
        return;
    }
    // SubRip, WebVTT, iTT (or any timed-text XML) or SCC, by what it holds;
    // its language from the file, or its name ("film.fr.srt").
    const CaptionFormats::ReadResult read = CaptionFormats::read(file.readAll(), m_fps, file.fileName(), startFrame());
    const QVariantList &cues = read.cues;
    if (cues.isEmpty()) {
        setStatus(tr("No captions found in %1").arg(QFileInfo(file).fileName()));
        return;
    }
    QVariantMap language;
    if (!read.language.isEmpty() && languageFor(read.language, QString(), &language)
        && language.value(QStringLiteral("code")) != QStringLiteral("und")) {
        const QString shown = language.value(QStringLiteral("name")).toString().isEmpty()
                                  ? language.value(QStringLiteral("code")).toString() : language.value(QStringLiteral("name")).toString();
        language.insert(QStringLiteral("trackName"), tr("Captions (%1)").arg(shown));
    } else {
        language.clear();
    }
    setStatus(tr("Importing %n caption(s) from %1…", nullptr, cues.size())
                  .arg(QFileInfo(file).fileName()));
    emit importCaptionsRequested(cues, captionStyle(), language);
}

QVariantMap Backend::defaultCaptionStyle() {
    return {{"font", QString()}, {"size", "small"}, {"pixelSize", 0}, {"color", "#ffffff"}, {"weight", QString()},
            {"italic", false}, {"outline", true}, {"bg", "#99000000"}, {"align", "centre"}, {"place", "bottom"},
            {"margin", 8}, {"width", 80}, {"lineSpacing", 1.0}, {"tracking", 0},
            {"shadow", false}, {"shadowColor", "#000000"}, {"shadowOffset", 3}, {"shadowSoftness", 4},
            {"outlineColor", "#000000"}, {"outlineWidth", 2}, {"boxPadding", -1}, {"boxRadius", 0},
            {"maxChars", 42}, {"maxLines", 2}, {"minSeconds", 1.0}};
}

// The box's opacity, in percent: its colour's alpha.
static int boxOpacityOf(const QString &bg) {
    const QColor c(bg);
    return bg.isEmpty() || !c.isValid() ? 0 : qRound(c.alpha() * 100. / 255.);
}

// A style's keys as connectTitleAt takes them: the lengths Generate Captions
// keeps to, and the "unset" values (preset size, automatic padding), left out.
static QVariantMap drawnCaptionStyle(QVariantMap style) {
    for (const char *key : {"maxChars", "maxLines", "minSeconds", "boxOpacity"}) style.remove(QLatin1String(key));
    if (style.value(QStringLiteral("pixelSize")).toDouble() <= 0) style.remove(QStringLiteral("pixelSize"));
    if (style.contains(QStringLiteral("boxPadding")) && style.value(QStringLiteral("boxPadding")).toDouble() < 0)
        style.remove(QStringLiteral("boxPadding"));
    return style;
}

int Backend::captionTrackAt(int track) const {
    if (track >= 0)
        return track < m_tracks.size() && m_tracks.at(track).toMap().value(QStringLiteral("mix")).toMap()
                                                 .value(QStringLiteral("editKind")) == QStringLiteral("caption") ? track : -1;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks.at(i).toMap().value(QStringLiteral("mix")).toMap().value(QStringLiteral("editKind")) == QStringLiteral("caption"))
            return i;
    return -1;
}

QVariantMap Backend::captionStyleSettings() const {
    return trackCaptionStyle(-1);
}

QVariantMap Backend::trackCaptionStyle(int track) const {
    QVariantMap style = defaultCaptionStyle();
    // The track's own style; for a track without one (or -1), the first a
    // caption track keeps.
    QVariantMap stored;
    if (track >= 0 && track < m_tracks.size())
        stored = m_tracks.at(track).toMap().value(QStringLiteral("mix")).toMap().value(QStringLiteral("captionStyle")).toMap();
    for (int i = 0; stored.isEmpty() && i < m_tracks.size(); ++i) {
        const QVariantMap mix = m_tracks.at(i).toMap().value(QStringLiteral("mix")).toMap();
        if (mix.value(QStringLiteral("editKind")) == QStringLiteral("caption"))
            stored = mix.value(QStringLiteral("captionStyle")).toMap();
    }
    for (auto it = stored.begin(); it != stored.end(); ++it)
        if (style.contains(it.key())) style[it.key()] = it.value();
    style.insert(QStringLiteral("boxOpacity"), boxOpacityOf(style.value(QStringLiteral("bg")).toString()));
    return style;
}

// The look a new caption is made with, and the whole style for a caption
// track made to hold it. A preset size unless a pixel size is set.
QVariantMap Backend::captionStyle() const {
    QVariantMap settings = captionStyleSettings();
    settings.remove(QStringLiteral("boxOpacity"));
    QVariantMap style = drawnCaptionStyle(settings);
    style.insert(QStringLiteral("captionStyle"), settings);
    return style;
}

bool Backend::applyCaptionStyleKeys(QVariantMap &style, const QVariantMap &input, bool lengths, QString *why) const {
    const auto refuse = [why](const QString &reason) { *why = reason; return false; };
    const auto number = [](const QVariant &v, double lo, double hi, bool *ok) {
        const double d = v.toDouble(ok);
        if (*ok) *ok = d >= lo && d <= hi;
        return d;
    };
    static const QRegularExpression rgb(QStringLiteral("^#[0-9a-f]{6}$")), argb(QStringLiteral("^#[0-9a-f]{8}$"));
    const QVariantMap value = plainVariant(input).toMap();
    for (auto it = value.begin(); it != value.end(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        bool ok = true;
        const bool lengthKey = key == "maxChars" || key == "maxLines" || key == "minSeconds";
        if ((!style.contains(key) && key != "boxOpacity") || (lengthKey && !lengths)) return refuse(tr("%1 is not a caption style setting").arg(key));
        if (key == "font") style[key] = v.toString().trimmed();
        else if (key == "size") { if (!QStringList{"small", "medium", "large"}.contains(v.toString())) return refuse(tr("size is small, medium or large")); style[key] = v.toString(); }
        else if (key == "pixelSize") { const double d = number(v, 0, 2000, &ok); if (!ok) return refuse(tr("pixelSize is 0 to 2000")); style[key] = qRound(d); }
        else if (key == "color" || key == "outlineColor") { const QString c = v.toString().toLower(); if (!rgb.match(c).hasMatch()) return refuse(tr("%1 is #rrggbb").arg(key)); style[key] = c; }
        else if (key == "shadowColor") {
            const QString c = v.toString().toLower();
            if (!rgb.match(c).hasMatch() && !argb.match(c).hasMatch()) return refuse(tr("shadowColor is #rrggbb or #aarrggbb"));
            style[key] = c;
        }
        else if (key == "bg") {
            QString c = v.toString().toLower();
            if (rgb.match(c).hasMatch()) c = "#ff" + c.mid(1);
            if (!c.isEmpty() && !argb.match(c).hasMatch()) return refuse(tr("bg is empty, #rrggbb or #aarrggbb"));
            style[key] = c;
        }
        else if (key == "boxOpacity") {
            // The box colour's alpha, in percent (a box of black when there was none).
            const double d = number(v, 0, 100, &ok);
            if (!ok) return refuse(tr("boxOpacity is 0 to 100"));
            QString c = style.value(QStringLiteral("bg")).toString().toLower();
            if (!argb.match(c).hasMatch()) c = QStringLiteral("#ff000000");
            style[QStringLiteral("bg")] = QStringLiteral("#%1").arg(qRound(d * 255 / 100), 2, 16, QLatin1Char('0')) + c.mid(3);
        }
        else if (key == "weight") { if (!QStringList{"", "bold"}.contains(v.toString())) return refuse(tr("weight is empty or bold")); style[key] = v.toString(); }
        else if (key == "italic" || key == "outline" || key == "shadow") { if (v.metaType().id() != QMetaType::Bool) return refuse(tr("%1 is true or false").arg(key)); style[key] = v.toBool(); }
        else if (key == "align") { if (!QStringList{"left", "centre", "right"}.contains(v.toString())) return refuse(tr("align is left, centre or right")); style[key] = v.toString(); }
        else if (key == "place") { if (!QStringList{"top", "centre", "bottom"}.contains(v.toString())) return refuse(tr("place is top, centre or bottom")); style[key] = v.toString(); }
        else if (key == "margin") { const double d = number(v, 0, 45, &ok); if (!ok) return refuse(tr("margin is 0 to 45 (percent of the height)")); style[key] = d; }
        else if (key == "width") { const double d = number(v, 20, 100, &ok); if (!ok) return refuse(tr("width is 20 to 100 (percent of the width)")); style[key] = d; }
        else if (key == "lineSpacing") { const double d = number(v, .5, 4, &ok); if (!ok) return refuse(tr("lineSpacing is 0.5 to 4")); style[key] = d; }
        else if (key == "tracking") { const double d = number(v, -50, 200, &ok); if (!ok) return refuse(tr("tracking is -50 to 200")); style[key] = d; }
        else if (key == "shadowOffset") { const double d = number(v, -50, 50, &ok); if (!ok) return refuse(tr("shadowOffset is -50 to 50 pixels")); style[key] = d; }
        else if (key == "shadowSoftness") { const double d = number(v, 0, 50, &ok); if (!ok) return refuse(tr("shadowSoftness is 0 to 50 pixels")); style[key] = d; }
        else if (key == "outlineWidth") { const double d = number(v, 0, 20, &ok); if (!ok) return refuse(tr("outlineWidth is 0 to 20 pixels")); style[key] = d; }
        else if (key == "boxPadding") { const double d = number(v, -1, 200, &ok); if (!ok) return refuse(tr("boxPadding is -1 (automatic) or 0 to 200 pixels")); style[key] = d < 0 ? -1. : d; }
        else if (key == "boxRadius") { const double d = number(v, 0, 200, &ok); if (!ok) return refuse(tr("boxRadius is 0 to 200 pixels")); style[key] = d; }
        else if (key == "maxChars") { const double d = number(v, 16, 80, &ok); if (!ok) return refuse(tr("maxChars is 16 to 80")); style[key] = qRound(d); }
        else if (key == "maxLines") { const double d = number(v, 1, 3, &ok); if (!ok) return refuse(tr("maxLines is 1 to 3")); style[key] = qRound(d); }
        else if (key == "minSeconds") { const double d = number(v, .3, 10, &ok); if (!ok) return refuse(tr("minSeconds is 0.3 to 10")); style[key] = d; }
    }
    return true;
}

bool Backend::setCaptionStyle(const QVariantMap &input, bool everyCaption) {
    return setTrackCaptionStyle(-1, input, everyCaption);
}

bool Backend::setTrackCaptionStyle(int track, const QVariantMap &input, bool everyCaption) {
    if (!hasTimeline() || m_exporting) {
        setStatus(tr("Caption Style needs a timeline, and no export running"));
        return false;
    }
    const auto refuse = [this](const QString &why) { setStatus(tr("Caption style not set: %1").arg(why)); return false; };
    if (track >= 0 && captionTrackAt(track) < 0)
        return refuse(tr("track %1 is not a caption track").arg(track));
    // Checked once against the defaults, so a bad key or value changes nothing.
    QVariantMap checked = defaultCaptionStyle();
    QString why;
    if (!applyCaptionStyleKeys(checked, input, true, &why))
        return refuse(why);
    // Each track keeps the rest of its own style.
    QVariantMap styles;
    QList<int> tracks;
    if (track >= 0) tracks.append(track);
    else for (int i = 0; i < m_tracks.size(); ++i) if (captionTrackAt(i) == i) tracks.append(i);
    const auto restyled = [&](int t) {
        QVariantMap style = trackCaptionStyle(t);
        applyCaptionStyleKeys(style, input, true, &why);
        style.remove(QStringLiteral("boxOpacity"));
        if (style.value(QStringLiteral("pixelSize")).toDouble() <= 0) style.remove(QStringLiteral("pixelSize"));
        if (style.value(QStringLiteral("boxPadding")).toDouble() < 0) style.remove(QStringLiteral("boxPadding"));
        return style;
    };
    for (int t : std::as_const(tracks)) styles.insert(QString::number(t), restyled(t));
    if (tracks.isEmpty()) styles.insert(QStringLiteral("-1"), restyled(-1));
    emit captionStyleRequested(styles, everyCaption);
    return true;
}

// ── One caption ───────────────────────────────────────────────────────────
QVariantMap Backend::captionLook(int connectedIndex) const {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size()) return {};
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (!e.value(ClipKey::caption).toBool()) return {};
    QVariantMap look = defaultCaptionStyle();
    for (const char *key : {"maxChars", "maxLines", "minSeconds"}) look.remove(QLatin1String(key));
    const QVariantMap own = Timeline::titleStyleOf(e);
    for (auto it = own.begin(); it != own.end(); ++it)
        if (look.contains(it.key()) && it.value().isValid() && !it.value().isNull()) look[it.key()] = it.value();
    if (look.value(QStringLiteral("bg")).isNull()) look[QStringLiteral("bg")] = QString();
    const int track = e.value(ClipKey::track).toInt();
    look.insert(QStringLiteral("boxOpacity"), boxOpacityOf(look.value(QStringLiteral("bg")).toString()));
    look.insert(QStringLiteral("x"), e.value(ClipKey::rectX, 0.).toDouble());
    look.insert(QStringLiteral("y"), e.value(ClipKey::rectY, 0.).toDouble());
    look.insert(QStringLiteral("speaker"), e.value(ClipKey::speaker).toString());
    look.insert(QStringLiteral("track"), track);
    look.insert(QStringLiteral("ownStyle"), captionOwnStyle(connectedIndex));
    return look;
}

bool Backend::captionOwnStyle(int connectedIndex) const {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size()) return false;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (!e.value(ClipKey::caption).toBool()) return false;
    const int track = e.value(ClipKey::track).toInt();
    const QVariantMap stored = track >= 0 && track < m_tracks.size()
        ? m_tracks.at(track).toMap().value(QStringLiteral("mix")).toMap().value(QStringLiteral("captionStyle")).toMap() : QVariantMap();
    return Timeline::captionHasOwnLook(e, stored.isEmpty() && captionTrackAt(-1) >= 0
                                             ? m_tracks.at(captionTrackAt(-1)).toMap().value(QStringLiteral("mix")).toMap()
                                                   .value(QStringLiteral("captionStyle")).toMap()
                                             : stored);
}

bool Backend::setCaptionLook(int connectedIndex, const QVariantMap &input) {
    const auto refuse = [this](const QString &why) { setStatus(tr("Caption not changed: %1").arg(why)); return false; };
    if (m_exporting) return refuse(tr("an export is running"));
    QVariantMap look = captionLook(connectedIndex);
    if (look.isEmpty()) return refuse(tr("select a caption"));
    QVariantMap keys = plainVariant(input).toMap();
    // Its own place: offsets from where the style sets it, in parts of the
    // frame's width and height (-1 to 1).
    QVariantList place;
    if (keys.contains(QStringLiteral("x")) || keys.contains(QStringLiteral("y"))) {
        bool okX = true, okY = true;
        const double x = keys.contains(QStringLiteral("x")) ? keys.take(QStringLiteral("x")).toDouble(&okX) : look.value(QStringLiteral("x")).toDouble();
        const double y = keys.contains(QStringLiteral("y")) ? keys.take(QStringLiteral("y")).toDouble(&okY) : look.value(QStringLiteral("y")).toDouble();
        if (!okX || !okY || qAbs(x) > 1 || qAbs(y) > 1) return refuse(tr("x and y are -1 to 1 (parts of the frame)"));
        place = {x, y};
    }
    QVariantMap style = look;
    for (const char *key : {"x", "y", "speaker", "track", "ownStyle"}) style.remove(QLatin1String(key));
    QString why;
    if (!applyCaptionStyleKeys(style, keys, false, &why)) return refuse(why);
    style = drawnCaptionStyle(style);
    style.insert(QStringLiteral("caption"), true);
    style.insert(QStringLiteral("ownStyle"), true);
    emit restyleCaptionRequested(connectedIndex, style, place, tr("Caption look"));
    return true;
}

bool Backend::useTrackCaptionStyle(int connectedIndex, bool on) {
    if (m_exporting || connectedIndex < 0 || connectedIndex >= m_connected.size()) return false;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (!e.value(ClipKey::caption).toBool()) { setStatus(tr("Select a caption")); return false; }
    if (on) {
        // Back to the track's look and place: its own goes.
        QVariantMap style = drawnCaptionStyle(trackCaptionStyle(e.value(ClipKey::track).toInt()));
        style.insert(QStringLiteral("caption"), true);
        style.insert(QStringLiteral("ownStyle"), false);
        emit restyleCaptionRequested(connectedIndex, style, {0., 0., 1., 1.}, tr("Use the track's caption style"));
    } else {
        // Kept as it is, but no longer changed with the track's style.
        QVariantMap style = Timeline::titleStyleOf(e);
        style.insert(QStringLiteral("ownStyle"), true);
        emit restyleCaptionRequested(connectedIndex, style, {}, tr("Keep the caption's own look"));
    }
    return true;
}

bool Backend::setCaptionSpeaker(const QVariantList &connectedIndices, const QString &speaker) {
    if (m_exporting) return false;
    const QString name = speaker.simplified();
    if (name.size() > 80) { setStatus(tr("A speaker's name is at most 80 letters")); return false; }
    QVariantList indices;
    for (const QVariant &v : connectedIndices) {
        bool ok = false;
        const int i = v.toInt(&ok);
        if (!ok || i < 0 || i >= m_connected.size() || !m_connected.at(i).toMap().value(ClipKey::caption).toBool()) {
            setStatus(tr("Select captions to name their speaker"));
            return false;
        }
        indices.append(i);
    }
    if (indices.isEmpty()) return false;
    emit captionSpeakerRequested(indices, name, name.isEmpty() ? QVariantMap() : captionSettingsWithSpeaker(name));
    return true;
}

// ── Caption track languages ───────────────────────────────────────────────
QVariantList Backend::captionLanguages() const {
    const QList<QPair<const char *, QString>> known{
        {"en", tr("English")}, {"en-GB", tr("English (UK)")}, {"en-US", tr("English (US)")}, {"fr", tr("French")},
        {"de", tr("German")}, {"es", tr("Spanish")}, {"it", tr("Italian")}, {"pt", tr("Portuguese")},
        {"pt-BR", tr("Portuguese (Brazil)")}, {"nl", tr("Dutch")}, {"sv", tr("Swedish")}, {"da", tr("Danish")},
        {"nb", tr("Norwegian")}, {"fi", tr("Finnish")}, {"pl", tr("Polish")}, {"cs", tr("Czech")},
        {"ga", tr("Irish")}, {"cy", tr("Welsh")}, {"el", tr("Greek")}, {"tr", tr("Turkish")}, {"ru", tr("Russian")},
        {"uk", tr("Ukrainian")}, {"ar", tr("Arabic")}, {"he", tr("Hebrew")}, {"hi", tr("Hindi")},
        {"zh-Hans", tr("Chinese (Simplified)")}, {"zh-Hant", tr("Chinese (Traditional)")}, {"ja", tr("Japanese")},
        {"ko", tr("Korean")}};
    QVariantList out;
    for (const auto &l : known) out.append(QVariantMap{{"code", QString::fromLatin1(l.first)}, {"name", l.second}});
    return out;
}

// A language code as BCP 47 writes it ("pt-br" → "pt-BR"), or "" when it is
// not one.
static QString languageCode(const QString &text) {
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$"));
    const QString code = text.trimmed().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (!shape.match(code).hasMatch()) return {};
    QStringList parts = code.split(QLatin1Char('-'));
    parts[0] = parts[0].toLower();
    for (int i = 1; i < parts.size(); ++i) {
        QString &p = parts[i];
        if (p.size() == 2 || (p.size() == 3 && p.at(0).isDigit())) p = p.toUpper();
        else if (p.size() == 4) p = p.left(1).toUpper() + p.mid(1).toLower();
        else p = p.toLower();
    }
    return parts.join(QLatin1Char('-'));
}

QVariantMap Backend::captionTrackLanguage(int track) const {
    if (captionTrackAt(track) < 0 || track < 0) return {};
    const QVariantMap stored = m_tracks.at(track).toMap().value(QStringLiteral("mix")).toMap().value(QStringLiteral("language")).toMap();
    const QString code = stored.value(QStringLiteral("code")).toString();
    const QString name = stored.value(QStringLiteral("name")).toString();
    const bool set = !code.isEmpty() && code != QStringLiteral("und");
    return {{"code", set ? code : QStringLiteral("und")}, {"name", set ? name : QString()},
            {"shown", set ? (name.isEmpty() ? code : name) : tr("Not set")}};
}

bool Backend::languageFor(const QString &code, const QString &name, QVariantMap *language) {
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QStringLiteral("und"), Qt::CaseInsensitive) == 0) {
        *language = {{"code", "und"}, {"name", QString()}};
        return true;
    }
    const QString tidy = languageCode(trimmed);
    if (tidy.isEmpty()) {
        setStatus(tr("“%1” is not a language code (such as en, fr or pt-BR)").arg(trimmed));
        return false;
    }
    QString shown = name.simplified().left(60);
    if (shown.isEmpty())
        for (const QVariant &v : captionLanguages())
            if (v.toMap().value(QStringLiteral("code")).toString().compare(tidy, Qt::CaseInsensitive) == 0)
                shown = v.toMap().value(QStringLiteral("name")).toString();
    *language = {{"code", tidy}, {"name", shown}};
    return true;
}

bool Backend::setCaptionTrackLanguage(int track, const QString &code, const QString &name) {
    if (m_exporting || track < 0 || captionTrackAt(track) != track) {
        setStatus(tr("Choose a caption track to set its language"));
        return false;
    }
    QVariantMap language;
    if (!languageFor(code, name, &language)) return false;
    setTrackMix(track, QStringLiteral("language"), language.value(QStringLiteral("code")) == QStringLiteral("und") ? QVariant() : QVariant(language));
    setStatus(language.value(QStringLiteral("code")) == QStringLiteral("und")
                  ? tr("Caption track language not set")
                  : tr("Caption track language: %1").arg(language.value(QStringLiteral("name")).toString().isEmpty()
                                                             ? language.value(QStringLiteral("code")).toString()
                                                             : language.value(QStringLiteral("name")).toString()));
    return true;
}

bool Backend::duplicateCaptionTrack(int track, const QString &code, const QString &name) {
    if (m_exporting || track < 0 || captionTrackAt(track) != track) {
        setStatus(tr("Choose a caption track to duplicate"));
        return false;
    }
    QVariantMap language;
    if (!languageFor(code, name, &language)) return false;
    const QString shown = language.value(QStringLiteral("name")).toString().isEmpty()
                              ? language.value(QStringLiteral("code")).toString() : language.value(QStringLiteral("name")).toString();
    const QString title = language.value(QStringLiteral("code")) == QStringLiteral("und")
                              ? tr("Captions copy") : tr("Captions (%1)").arg(shown);
    emit duplicateCaptionTrackRequested(track, language, title);
    setDirty(true);
    return true;
}

bool Backend::regenerateCaptions() {
    // Afresh, as Generate Captions: a clip tried before is tried again.
    m_captionQueue.clear();
    m_captionTried.clear();
    m_twoChannelTranscripts.clear();
    m_regeneratingCaptions = true;
    const bool started = generateCaptionsNow(false);
    if (!started || m_captionQueue.isEmpty()) m_regeneratingCaptions = false;
    return started;
}

QVariantList Backend::transcriptWords(const QVariantMap &segment) {
    QVariantList own = segment.value(QStringLiteral("words")).toList();
    // A segment's speaker (its channel) is each of its words'.
    const QVariant channel = segment.value(QStringLiteral("channel"));
    if (channel.isValid())
        for (QVariant &w : own) { QVariantMap word = w.toMap(); word.insert(QStringLiteral("channel"), channel); w = word; }
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
        QVariantMap word{{"text", w}, {"start", qRound(at)}, {"end", qRound(next)}};
        if (channel.isValid()) word.insert(QStringLiteral("channel"), channel);
        out.append(word);
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
    struct Word { QString text; double start, end; int channel; };
    // Phrases: a sentence's end, or a pause, ends one; so does another
    // speaker (each channel of a stereo clip one, when asked for).
    QList<QList<Word>> phrases;
    for (const QVariant &v : input) {
        const QVariantMap m = v.toMap();
        const QString text = m.value(QStringLiteral("text")).toString().simplified();
        if (text.isEmpty()) continue;
        const double start = m.value(QStringLiteral("start")).toDouble();
        const Word word{text, start, qMax(start, m.value(QStringLiteral("end")).toDouble()), m.value(QStringLiteral("channel"), -1).toInt()};
        if (phrases.isEmpty()) { phrases.append({word}); continue; }
        const Word &last = phrases.last().last();
        const bool sentence = last.text.endsWith('.') || last.text.endsWith('?') || last.text.endsWith('!');
        if (sentence || word.start - last.end >= kPauseMs || word.channel != last.channel) phrases.append({word});
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
            for (int c = 0; c < runs.size(); ++c) {
                QVariantMap cue{{"start", phrase.at(runs[c].first()).start}, {"end", phrase.at(runs[c].last()).end},
                                {"text", c < captions.size() ? captions[c].join('\n') : texts.mid(runs[c].first(), runs[c].size()).join(' ')}};
                if (phrase.first().channel >= 0) cue.insert(QStringLiteral("channel"), phrase.first().channel);
                cues.append(cue);
            }
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
    m_twoChannelTranscripts.clear();
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
    // Each channel of a stereo clip a speaker: transcripts made so, the
    // speakers named as the rules name them.
    const QVariantMap rules = captionRules();
    const bool twoSpeakers = rules.value(QStringLiteral("twoSpeakers")).toBool();
    const QStringList speakerNames{rules.value(QStringLiteral("leftSpeaker")).toString(), rules.value(QStringLiteral("rightSpeaker")).toString()};
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
        if (transcript.isEmpty() || (twoSpeakers && !Transcriber::hasChannels(transcript) && !m_captionTried.contains(path))) {
            if (!afterTranscribing && !m_captionTried.contains(path) && !missing.contains(path)) missing.append(path);
            if (transcript.isEmpty() || !afterTranscribing) continue;
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
            QVariantMap placed{{QStringLiteral("start"), start + a - in}, {QStringLiteral("frames"), b - a},
                               {QStringLiteral("text"), cue.value(QStringLiteral("text"))}};
            const int channel = cue.value(QStringLiteral("channel"), -1).toInt();
            if (twoSpeakers && (channel == 0 || channel == 1)) placed.insert(QStringLiteral("speaker"), speakerNames.at(channel));
            cues.append(placed);
        }
    }
    if (!missing.isEmpty()) {
        if (!m_transcriber->available()) {
            setStatus(tr("Make Captions cannot transcribe the clips: %1").arg(m_transcriber->status()));
            return false;
        }
        if (m_transcriber->active()) {
            setStatus(tr("A transcription is already running; try again when it is done"));
            return false;
        }
        m_captionQueue = missing;
        if (twoSpeakers) for (const QString &path : std::as_const(missing)) m_twoChannelTranscripts.insert(path);
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
    chooseCaptionExportFile(QStringLiteral("srt"), -1);
}

QVariantList Backend::captionFormats() const {
    return {QVariantMap{{"format", "srt"}, {"name", tr("SubRip (.srt)")}, {"suffix", ".srt"}},
            QVariantMap{{"format", "vtt"}, {"name", tr("WebVTT (.vtt)")}, {"suffix", ".vtt"}},
            QVariantMap{{"format", "itt"}, {"name", tr("iTT, timed text for video stores (.itt)")}, {"suffix", ".itt"}},
            QVariantMap{{"format", "scc"}, {"name", tr("SCC, broadcast captions (.scc)")}, {"suffix", ".scc"}}};
}

QVariantList Backend::captionTrackCues() const {
    // The captions on each track: caption-tagged titles, and titles on a
    // track named "Captions" from before caption tracks were marked.
    QHash<int, QVariantList> byTrack;
    QSet<int> named;
    for (const QVariant &v : m_tracks) {
        const QVariantMap t = v.toMap();
        if (t.value(QStringLiteral("name")).toString() == QStringLiteral("Captions")) named.insert(t.value(QStringLiteral("index")).toInt());
    }
    for (const QVariant &v : m_connected) {
        const QVariantMap e = v.toMap();
        const QString title = e.value(ClipKey::title).toString();
        const int track = e.value(ClipKey::track).toInt();
        if (title.isEmpty() || (!e.value(ClipKey::caption).toBool() && !named.contains(track))) continue;
        byTrack[track].append(QVariantMap{{"start", e.value(ClipKey::start).toInt()}, {"frames", e.value(ClipKey::frames).toInt()},
                                          {"text", title}});
    }
    QVariantList out;
    for (const QVariant &v : m_tracks) {
        const int track = v.toMap().value(QStringLiteral("index")).toInt();
        if (!byTrack.contains(track)) continue;
        QVariantList cues = byTrack.value(track);
        std::stable_sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("start")).toInt() < b.toMap().value(QStringLiteral("start")).toInt();
        });
        const QVariantMap language = v.toMap().value(QStringLiteral("mix")).toMap().value(QStringLiteral("language")).toMap();
        const QString code = language.value(QStringLiteral("code")).toString();
        const QVariantMap style = trackCaptionStyle(track);
        const QString place = style.value(QStringLiteral("place")).toString();
        const QString align = style.value(QStringLiteral("align")).toString();
        out.append(QVariantMap{{"track", track}, {"code", code == QStringLiteral("und") ? QString() : code},
                               {"name", code == QStringLiteral("und") ? QString() : language.value(QStringLiteral("name")).toString()},
                               {"place", place == QStringLiteral("top") || place == QStringLiteral("centre") ? place : QStringLiteral("bottom")},
                               {"align", align == QStringLiteral("left") || align == QStringLiteral("right") ? align : QStringLiteral("centre")},
                               {"cues", cues}});
    }
    return out;
}

QVariantList Backend::captionExportTracks() const {
    QVariantList out;
    for (const QVariant &v : captionTrackCues()) {
        const QVariantMap t = v.toMap();
        const int track = t.value(QStringLiteral("track")).toInt();
        out.append(QVariantMap{{"track", track}, {"name", m_tracks.value(track).toMap().value(QStringLiteral("name"))},
                               {"language", captionTrackAt(track) == track ? captionTrackLanguage(track)
                                                                            : QVariantMap{{"code", "und"}, {"name", QString()}, {"shown", tr("Not set")}}},
                               {"count", t.value(QStringLiteral("cues")).toList().size()}});
    }
    return out;
}

void Backend::chooseCaptionExportFile(const QString &format, int track) {
    if (captionCues().isEmpty()) {
        setStatus(tr("No captions to export — import a caption file or name a track “Captions”"));
        return;
    }
    m_captionExportFormat = CaptionFormats::formats().contains(format) ? format : QStringLiteral("srt");
    m_captionExportTrack = track;
    m_chooserMode = ChooserMode::ExportCaptions;
    const QString suffix = CaptionFormats::extension(m_captionExportFormat);
    QString name;
    for (const QVariant &v : captionFormats())
        if (v.toMap().value(QStringLiteral("format")) == m_captionExportFormat) name = v.toMap().value(QStringLiteral("name")).toString();
    m_chooser->saveFile(QStringLiteral("Export Captions"), projectName() + suffix, name, {QLatin1Char('*') + suffix});
}

bool Backend::exportCaptions(const QUrl &url) {
    const QString format = CaptionFormats::formatOfPath(url.toLocalFile());
    return !exportCaptionFiles(url, format.isEmpty() ? QStringLiteral("srt") : format, -1).isEmpty();
}

QStringList Backend::exportCaptionFiles(const QUrl &url, const QString &formatIn, int track) {
    const QString format = formatIn.trimmed().toLower() == QStringLiteral("webvtt") ? QStringLiteral("vtt") : formatIn.trimmed().toLower();
    if (!CaptionFormats::formats().contains(format)) {
        setStatus(tr("Unknown caption format “%1” (srt, vtt, itt or scc)").arg(formatIn));
        return {};
    }
    QVariantList tracks = captionTrackCues();
    if (track >= 0) {
        QVariantList chosen;
        for (const QVariant &v : tracks)
            if (v.toMap().value(QStringLiteral("track")).toInt() == track) chosen.append(v);
        if (chosen.isEmpty()) {
            setStatus(tr("Track %1 holds no captions").arg(track));
            return {};
        }
        tracks = chosen;
    }
    if (tracks.isEmpty()) {
        setStatus(tr("No captions to export — import a caption file or name a track “Captions”"));
        return {};
    }
    QString path = url.toLocalFile();
    if (path.isEmpty()) {
        setStatus(tr("No file name for the captions"));
        return {};
    }
    // The file's name with the format's suffix; a file each beside it for
    // every caption track.
    const QString suffix = CaptionFormats::extension(format);
    const QString given = CaptionFormats::formatOfPath(path);
    const QString stem = given.isEmpty() ? path : path.left(path.size() - QFileInfo(path).suffix().size() - 1);
    QStringList paths;
    if (track >= 0) paths.append(stem + suffix);
    else {
        QStringList languages;
        for (const QVariant &v : std::as_const(tracks)) languages.append(v.toMap().value(QStringLiteral("code")).toString());
        paths = CaptionFormats::trackPaths(stem, languages, format);
    }
    // Every file made first: a caption SCC cannot hold leaves every file as it was.
    QList<QByteArray> texts;
    QStringList warnings;
    int count = 0;
    for (const QVariant &v : std::as_const(tracks)) {
        const QVariantMap t = v.toMap();
        CaptionFormats::WriteOptions options;
        options.language = t.value(QStringLiteral("code")).toString();
        options.place = t.value(QStringLiteral("place")).toString();
        options.align = t.value(QStringLiteral("align")).toString();
        options.timecodeStart = startFrame();
        QString why, late;
        const QVariantList cues = t.value(QStringLiteral("cues")).toList();
        texts.append(CaptionFormats::write(format, cues, m_fps, options, &why, &late));
        if (!why.isEmpty()) {
            setStatus(why);
            emit failed(why);
            return {};
        }
        if (!late.isEmpty()) warnings.append(late);
        count += int(cues.size());
    }
    for (int i = 0; i < paths.size(); ++i) {
        // A complete replacement or none: a failed write keeps the old file.
        QSaveFile file(paths.at(i));
        if (!file.open(QIODevice::WriteOnly) || file.write(texts.at(i)) != texts.at(i).size() || !file.commit()) {
            setStatus(tr("Could not write %1").arg(QFileInfo(paths.at(i)).fileName()));
            return {};
        }
    }
    QStringList names;
    for (const QString &p : std::as_const(paths)) names.append(QFileInfo(p).fileName());
    setStatus((tr("%n caption(s) written to %1", nullptr, count).arg(names.join(QStringLiteral(", ")))
               + (warnings.isEmpty() ? QString() : QStringLiteral(" — ") + warnings.join(QLatin1Char(' ')))));
    return paths;
}
