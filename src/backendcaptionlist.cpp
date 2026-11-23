#include "backend_p.h"
#include "captionchecks.h"
#include "playerworker.h"

// ── The caption list (Edit page) ──────────────────────────────────────────
// Its rows, the caption checks and their rules, speakers and their colours,
// words edited in place or replaced, and the timing tools. Every change is
// one undo step, made on the worker (playerworkercaptionlist.cpp).

void Backend::setupCaptionList() {
    connect(m_worker, &PlayerWorker::captionSettingsChanged, this, [this](const QVariantMap &settings) {
        if (settings == m_captionSettings) return;
        m_captionSettings = settings;
        emit captionSettingsChanged();
    });
    connect(this, &Backend::captionSettingsRequested, m_worker, &PlayerWorker::setCaptionSettings);
    connect(this, &Backend::captionTextsRequested, m_worker, &PlayerWorker::setCaptionTexts);
    connect(this, &Backend::transcriptEditRequested, m_worker, &PlayerWorker::editTranscript);
    connect(m_worker, &PlayerWorker::transcriptEditReplayed, this, &Backend::applyTranscriptEdit);
    connect(this, &Backend::retimeCaptionsRequested, m_worker, &PlayerWorker::retimeCaptions);
    connect(this, &Backend::renameCaptionSpeakerRequested, m_worker, &PlayerWorker::renameCaptionSpeaker);
    connect(m_worker, &PlayerWorker::captionsRetimed, this, [this](int moved, int kept) {
        if (kept > 0)
            setStatus(tr("%n caption(s) moved; %1 stayed where they were: another clip is in the way", nullptr, moved).arg(kept));
        else if (moved > 0)
            setStatus(tr("%n caption(s) moved", nullptr, moved));
    });
}

QVariantList Backend::captionRows() const {
    QVariantList rows;
    for (int i = 0; i < m_connected.size(); ++i) {
        const QVariantMap e = m_connected.at(i).toMap();
        if (!e.value(ClipKey::caption).toBool() || e.value(ClipKey::title).toString().isEmpty()) continue;
        rows.append(QVariantMap{{"index", i}, {"track", e.value(ClipKey::track).toInt()}, {"start", e.value(ClipKey::start).toInt()},
                                {"frames", e.value(ClipKey::frames).toInt()}, {"text", e.value(ClipKey::title).toString()}});
    }
    return rows;
}

QVariantList Backend::captionList(const QVariantMap &input) const {
    const QVariantMap filter = plainVariant(input).toMap();
    const QVariantMap rules = m_captionSettings.value(QStringLiteral("rules")).toMap();
    const QVariantList rows = captionRows();
    const QHash<int, QVariantList> problems = CaptionChecks::problems(rows, rules, m_fps);
    QHash<QString, QString> colours;
    for (const QVariant &v : captionSpeakers())
        colours.insert(v.toMap().value(QStringLiteral("name")).toString(), v.toMap().value(QStringLiteral("colour")).toString());
    QHash<int, QVariantMap> languages;
    const int onlyTrack = filter.value(QStringLiteral("track"), -1).toInt();
    const QString onlyLanguage = filter.value(QStringLiteral("language")).toString();
    const bool bySpeaker = filter.contains(QStringLiteral("speaker"));
    const QString onlySpeaker = filter.value(QStringLiteral("speaker")).toString();
    const QString search = filter.value(QStringLiteral("search")).toString().simplified();
    const bool onlyProblems = filter.value(QStringLiteral("problems")).toBool();
    const double fps = m_fps > 0 ? m_fps : 25.;
    QVariantList out;
    for (const QVariant &v : rows) {
        QVariantMap row = v.toMap();
        const int index = row.value(QStringLiteral("index")).toInt(), track = row.value(QStringLiteral("track")).toInt();
        const QString speaker = m_connected.at(index).toMap().value(ClipKey::speaker).toString();
        if (!languages.contains(track)) {
            const QVariantMap l = captionTrackLanguage(track);
            languages.insert(track, {{"code", l.value(QStringLiteral("code"))}, {"shown", l.value(QStringLiteral("shown"))}});
        }
        const QVariantMap language = languages.value(track);
        if (onlyTrack >= 0 && track != onlyTrack) continue;
        if (!onlyLanguage.isEmpty() && language.value(QStringLiteral("code")).toString().compare(onlyLanguage, Qt::CaseInsensitive) != 0) continue;
        if (bySpeaker && speaker != onlySpeaker) continue;
        if (!search.isEmpty() && !row.value(QStringLiteral("text")).toString().simplified().contains(search, Qt::CaseInsensitive)) continue;
        if (onlyProblems && !problems.contains(index)) continue;
        const int start = row.value(QStringLiteral("start")).toInt(), frames = row.value(QStringLiteral("frames")).toInt();
        row.insert(QStringLiteral("end"), start + frames);
        row.insert(QStringLiteral("seconds"), frames / fps);
        row.insert(QStringLiteral("language"), language);
        row.insert(QStringLiteral("speaker"), speaker);
        row.insert(QStringLiteral("colour"), speaker.isEmpty() ? QString() : colours.value(speaker, CaptionChecks::colourFor(speaker)));
        row.insert(QStringLiteral("problems"), problems.value(index));
        out.append(row);
    }
    std::stable_sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const int sx = x.value(QStringLiteral("start")).toInt(), sy = y.value(QStringLiteral("start")).toInt();
        return sx < sy || (sx == sy && x.value(QStringLiteral("track")).toInt() < y.value(QStringLiteral("track")).toInt());
    });
    return out;
}

QStringList Backend::captionCheckWarnings() const {
    const QVariantList rows = captionRows();
    if (rows.isEmpty()) return {};
    const QVariantMap rules = m_captionSettings.value(QStringLiteral("rules")).toMap();
    return CaptionChecks::summary(CaptionChecks::problems(rows, rules, m_fps), rules);
}

// ── Rules ─────────────────────────────────────────────────────────────────
QVariantMap Backend::captionRules() const {
    return CaptionChecks::rules(m_captionSettings.value(QStringLiteral("rules")).toMap());
}

bool Backend::setCaptionRules(const QVariantMap &input) {
    if (!hasTimeline() || m_exporting) {
        setStatus(tr("Caption rules need a timeline, and no export running"));
        return false;
    }
    QVariantMap rules = captionRules();
    QString why;
    if (!CaptionChecks::applyRules(rules, plainVariant(input).toMap(), &why)) {
        setStatus(tr("Caption rules not set: %1").arg(why));
        return false;
    }
    // Only what differs from the defaults is kept.
    const QVariantMap defaults = CaptionChecks::defaultRules();
    QVariantMap stored;
    for (auto it = rules.begin(); it != rules.end(); ++it)
        if (it.value() != defaults.value(it.key())) stored.insert(it.key(), it.value());
    QVariantMap settings = m_captionSettings;
    if (stored.isEmpty()) settings.remove(QStringLiteral("rules"));
    else settings.insert(QStringLiteral("rules"), stored);
    if (settings != m_captionSettings) {
        emit captionSettingsRequested(settings, tr("Caption rules"));
        setStatus(tr("Caption rules set"));
    }
    return true;
}

// ── Speakers ──────────────────────────────────────────────────────────────
QVariantList Backend::captionSpeakers() const {
    QHash<QString, int> counts;
    QStringList used;
    for (const QVariant &v : m_connected) {
        const QVariantMap e = v.toMap();
        const QString speaker = e.value(ClipKey::speaker).toString();
        if (!e.value(ClipKey::caption).toBool() || speaker.isEmpty()) continue;
        if (!counts.contains(speaker)) used.append(speaker);
        ++counts[speaker];
    }
    QVariantList out;
    QSet<QString> listed;
    for (const QVariant &v : m_captionSettings.value(QStringLiteral("speakers")).toList()) {
        const QVariantMap s = v.toMap();
        const QString name = s.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || listed.contains(name)) continue;
        listed.insert(name);
        const QString colour = s.value(QStringLiteral("colour")).toString();
        out.append(QVariantMap{{"name", name}, {"colour", colour.isEmpty() ? CaptionChecks::colourFor(name) : colour},
                               {"captions", counts.value(name)}});
    }
    // The others in the palette's colours no speaker has yet, by name.
    QStringList taken;
    for (const QVariant &v : std::as_const(out)) taken.append(v.toMap().value(QStringLiteral("colour")).toString());
    std::sort(used.begin(), used.end(), [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    for (const QString &name : std::as_const(used)) {
        if (listed.contains(name)) continue;
        QString colour = CaptionChecks::colourFor(name);
        for (const QString &c : CaptionChecks::palette())
            if (!taken.contains(c)) { colour = c; break; }
        taken.append(colour);
        out.append(QVariantMap{{"name", name}, {"colour", colour}, {"captions", counts.value(name)}});
    }
    return out;
}

// The caption settings with `name` in the speakers list (a colour of its
// own: `colour`, or the first of the palette no listed speaker has).
static QVariantMap withSpeaker(QVariantMap settings, const QString &name, const QString &colour, QStringList taken = {}) {
    QVariantList speakers = settings.value(QStringLiteral("speakers")).toList();
    int at = -1;
    for (int i = 0; i < speakers.size(); ++i) {
        const QVariantMap s = speakers.at(i).toMap();
        taken.append(s.value(QStringLiteral("colour")).toString());
        if (s.value(QStringLiteral("name")).toString() == name) at = i;
    }
    if (at >= 0 && colour.isEmpty()) return settings;
    QString chosen = colour;
    if (chosen.isEmpty()) {
        chosen = CaptionChecks::colourFor(name);
        for (const QString &c : CaptionChecks::palette())
            if (!taken.contains(c)) { chosen = c; break; }
    }
    const QVariantMap entry{{"name", name}, {"colour", chosen}};
    if (at >= 0) speakers[at] = entry;
    else speakers.append(entry);
    settings.insert(QStringLiteral("speakers"), speakers);
    return settings;
}

QVariantMap Backend::captionSettingsWithSpeaker(const QString &name) const {
    // A name already shown keeps the colour it is shown in.
    for (const QVariant &v : captionSpeakers())
        if (v.toMap().value(QStringLiteral("name")).toString() == name)
            return withSpeaker(m_captionSettings, name, v.toMap().value(QStringLiteral("colour")).toString());
    QStringList shown;
    for (const QVariant &v : captionSpeakers()) shown.append(v.toMap().value(QStringLiteral("colour")).toString());
    return withSpeaker(m_captionSettings, name, QString(), shown);
}

QStringList Backend::captionSpeakerColours() const {
    return CaptionChecks::palette();
}

bool Backend::setCaptionSpeakerColour(const QString &speaker, const QString &colour) {
    if (!hasTimeline() || m_exporting) return false;
    const QString name = speaker.simplified();
    if (name.isEmpty() || name.size() > 80) { setStatus(tr("A speaker's name is 1 to 80 letters")); return false; }
    static const QRegularExpression rgb(QStringLiteral("^#[0-9a-f]{6}$"));
    const QString c = colour.trimmed().toLower();
    if (!c.isEmpty() && !rgb.match(c).hasMatch()) { setStatus(tr("A speaker's colour is #rrggbb")); return false; }
    const QVariantMap settings = c.isEmpty() ? captionSettingsWithSpeaker(name) : withSpeaker(m_captionSettings, name, c);
    if (settings != m_captionSettings) emit captionSettingsRequested(settings, tr("Speaker colour"));
    return true;
}

bool Backend::renameCaptionSpeaker(const QString &speaker, const QString &newName) {
    if (!hasTimeline() || m_exporting) return false;
    const QString to = newName.simplified();
    if (to.size() > 80) { setStatus(tr("A speaker's name is at most 80 letters")); return false; }
    bool known = false;
    for (const QVariant &v : captionSpeakers()) known = known || v.toMap().value(QStringLiteral("name")).toString() == speaker;
    if (!known || speaker.isEmpty()) { setStatus(tr("There is no speaker “%1”").arg(speaker)); return false; }
    if (to == speaker) return true;
    // In the list: renamed in its place (into one already there: the two are one).
    QVariantMap settings = m_captionSettings;
    QVariantList speakers, before = settings.value(QStringLiteral("speakers")).toList();
    bool present = false;
    for (const QVariant &v : before) present = present || v.toMap().value(QStringLiteral("name")).toString() == to;
    for (const QVariant &v : before) {
        QVariantMap s = v.toMap();
        if (s.value(QStringLiteral("name")).toString() != speaker) { speakers.append(s); continue; }
        if (to.isEmpty() || present) continue;
        s.insert(QStringLiteral("name"), to);
        speakers.append(s);
    }
    if (speakers.isEmpty()) settings.remove(QStringLiteral("speakers"));
    else settings.insert(QStringLiteral("speakers"), speakers);
    emit renameCaptionSpeakerRequested(speaker, to, settings);
    setStatus(to.isEmpty() ? tr("Speaker “%1” removed").arg(speaker) : tr("Speaker “%1” is now “%2”").arg(speaker, to));
    return true;
}

// ── Words ─────────────────────────────────────────────────────────────────
static QString tidyCaptionText(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines;
    for (const QString &line : text.split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty()) lines.append(line.trimmed());
    return lines.join(QLatin1Char('\n'));
}

bool Backend::chosenCaptions(const QVariantList &connectedIndices, QList<int> *out) const {
    out->clear();
    const QVariantList indices = plainVariant(connectedIndices).toList();
    if (indices.isEmpty()) {
        for (const QVariant &v : captionRows()) out->append(v.toMap().value(QStringLiteral("index")).toInt());
        return true;
    }
    for (const QVariant &v : indices) {
        bool ok = false;
        const int i = v.toInt(&ok);
        if (!ok || i < 0 || i >= m_connected.size() || !m_connected.at(i).toMap().value(ClipKey::caption).toBool()) return false;
        if (!out->contains(i)) out->append(i);
    }
    return true;
}

bool Backend::setCaptionText(int connectedIndex, const QString &text) {
    if (m_exporting) return false;
    if (connectedIndex < 0 || connectedIndex >= m_connected.size() || !m_connected.at(connectedIndex).toMap().value(ClipKey::caption).toBool()) {
        setStatus(tr("Choose a caption to change its words"));
        return false;
    }
    const QString words = tidyCaptionText(text);
    if (words.isEmpty()) { setStatus(tr("A caption needs words: delete it instead")); return false; }
    if (words == m_connected.at(connectedIndex).toMap().value(ClipKey::title).toString()) return true;
    emit captionTextsRequested({QVariantMap{{"index", connectedIndex}, {"text", words}}}, tr("Caption words"));
    return true;
}

int Backend::replaceCaptionText(const QString &find, const QString &replacement, const QVariantList &connectedIndices, bool matchCase) {
    if (m_exporting) return -1;
    if (find.isEmpty()) { setStatus(tr("Type the words to find")); return -1; }
    QList<int> chosen;
    if (!chosenCaptions(connectedIndices, &chosen)) { setStatus(tr("Replace works on captions only")); return -1; }
    QVariantList changes;
    for (int i : std::as_const(chosen)) {
        const QString was = m_connected.at(i).toMap().value(ClipKey::title).toString();
        QString now = was;
        now.replace(find, replacement, matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive);
        now = tidyCaptionText(now);
        if (now != was && !now.isEmpty()) changes.append(QVariantMap{{"index", i}, {"text", now}});
    }
    if (changes.isEmpty()) { setStatus(tr("No caption has “%1”").arg(find)); return 0; }
    emit captionTextsRequested(changes, tr("Replace in captions"));
    setStatus(tr("Replaced in %n caption(s)", nullptr, int(changes.size())));
    return int(changes.size());
}

// ── Timing ────────────────────────────────────────────────────────────────
namespace {
struct Span { int index, start, end; bool chosen; };
// Every clip on each track that holds a chosen caption, in time order.
QHash<int, QList<Span>> captionTracksOf(const QVariantList &connected, const QList<int> &chosen) {
    QHash<int, QList<Span>> tracks;
    for (int i : chosen) tracks.insert(connected.at(i).toMap().value(ClipKey::track).toInt(), {});
    for (int i = 0; i < connected.size(); ++i) {
        const QVariantMap e = connected.at(i).toMap();
        const int track = e.value(ClipKey::track).toInt();
        if (!tracks.contains(track)) continue;
        const int start = e.value(ClipKey::start).toInt();
        tracks[track].append({i, start, start + e.value(ClipKey::frames).toInt(), chosen.contains(i)});
    }
    for (QList<Span> &list : tracks)
        std::sort(list.begin(), list.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    return tracks;
}
QVariantMap timing(const Span &s) {
    return {{"index", s.index}, {"start", s.start}, {"frames", s.end - s.start}};
}
} // namespace

int Backend::retimeCaptionsTo(const QVariantList &changes, const QString &label, const QString &none) {
    if (changes.isEmpty()) { setStatus(none); return 0; }
    emit retimeCaptionsRequested(changes, label);
    return int(changes.size());
}

int Backend::extendCaptionsToNext(const QVariantList &connectedIndices) {
    QList<int> chosen;
    if (m_exporting || !chosenCaptions(connectedIndices, &chosen)) { setStatus(tr("Choose captions to extend")); return -1; }
    const int gap = captionRules().value(QStringLiteral("gapFrames")).toInt();
    QVariantList changes;
    const auto tracks = captionTracksOf(m_connected, chosen);
    for (const QList<Span> &list : tracks)
        for (int i = 0; i + 1 < list.size(); ++i) {
            Span s = list[i];
            const int end = list[i + 1].start - gap;
            if (!s.chosen || end <= s.end) continue;
            s.end = end;
            changes.append(timing(s));
        }
    return retimeCaptionsTo(changes, tr("Extend captions to the next"), tr("No caption could be made longer: each already reaches the next"));
}

int Backend::closeCaptionGaps(const QVariantList &connectedIndices, int frames) {
    QList<int> chosen;
    if (m_exporting || !chosenCaptions(connectedIndices, &chosen)) { setStatus(tr("Choose captions to close their gaps")); return -1; }
    if (frames < 1 || frames > 1000) { setStatus(tr("Close gaps under 1 to 1000 frames")); return -1; }
    QVariantList changes;
    const auto tracks = captionTracksOf(m_connected, chosen);
    for (const QList<Span> &list : tracks)
        for (int i = 0; i + 1 < list.size(); ++i) {
            Span s = list[i];
            const int gap = list[i + 1].start - s.end;
            if (!s.chosen || gap <= 0 || gap >= frames) continue;
            s.end = list[i + 1].start;
            changes.append(timing(s));
        }
    return retimeCaptionsTo(changes, tr("Close gaps between captions"), tr("No gap under %n frame(s) between captions", nullptr, frames));
}

int Backend::shiftCaptions(const QVariantList &connectedIndices, int frames) {
    QList<int> chosen;
    if (m_exporting || !chosenCaptions(connectedIndices, &chosen)) { setStatus(tr("Choose captions to shift")); return -1; }
    if (frames == 0) { setStatus(tr("Shift captions by some frames, earlier or later")); return -1; }
    QVariantList changes;
    for (int i : std::as_const(chosen)) {
        const QVariantMap e = m_connected.at(i).toMap();
        const int start = e.value(ClipKey::start).toInt() + frames;
        if (start < 0) { setStatus(tr("Captions cannot start before the timeline: shift them by less")); return -1; }
        changes.append(QVariantMap{{"index", i}, {"start", start}, {"frames", e.value(ClipKey::frames).toInt()}});
    }
    return retimeCaptionsTo(changes, tr("Shift captions"), tr("No captions to shift"));
}

int Backend::snapCaptionsToCuts(const QVariantList &connectedIndices, int frames) {
    QList<int> chosen;
    if (m_exporting || !chosenCaptions(connectedIndices, &chosen)) { setStatus(tr("Choose captions to snap")); return -1; }
    if (frames < 1 || frames > 1000) { setStatus(tr("Snap within 1 to 1000 frames")); return -1; }
    // The cuts: where a picture starts or ends, on the main track and any
    // other picture track (not titles, captions or sound).
    QList<int> cuts;
    const auto addCuts = [&cuts](const QVariantMap &e) {
        if (e.value(ClipKey::gap).toBool() || e.value(ClipKey::audioOnly).toBool() || e.value(ClipKey::caption).toBool()
            || !e.value(ClipKey::title).toString().isEmpty()) return;
        const int start = e.value(ClipKey::start).toInt();
        cuts << start << start + e.value(ClipKey::frames).toInt();
    };
    for (const QVariant &v : m_timeline) addCuts(v.toMap());
    for (const QVariant &v : m_connected) addCuts(v.toMap());
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    const auto nearest = [&](int at) {
        int best = at, distance = frames + 1;
        for (int cut : std::as_const(cuts))
            if (qAbs(cut - at) < distance && cut != at) { best = cut; distance = qAbs(cut - at); }
        return distance <= frames ? best : at;
    };
    QVariantList changes;
    auto tracks = captionTracksOf(m_connected, chosen);
    for (QList<Span> &list : tracks) {
        // Each chosen caption's edges onto the cuts near them...
        QList<Span> wanted = list;
        for (Span &s : wanted)
            if (s.chosen) {
                const int start = nearest(s.start), end = nearest(s.end);
                if (end > start) { s.start = start; s.end = end; }
            }
        // ...never over the caption before or after it: two edges on one cut meet there.
        for (int i = 0; i + 1 < wanted.size(); ++i)
            if (wanted[i].end > wanted[i + 1].start) {
                if (wanted[i].chosen && wanted[i + 1].start > wanted[i].start) wanted[i].end = wanted[i + 1].start;
                else if (wanted[i + 1].chosen && wanted[i].end < wanted[i + 1].end) wanted[i + 1].start = wanted[i].end;
                else { wanted[i] = list[i]; wanted[i + 1] = list[i + 1]; }
            }
        for (int i = 0; i < wanted.size(); ++i)
            if (wanted[i].chosen && (wanted[i].start != list[i].start || wanted[i].end != list[i].end)) changes.append(timing(wanted[i]));
    }
    return retimeCaptionsTo(changes, tr("Snap captions to cuts"), tr("No caption edge is within %n frame(s) of a cut", nullptr, frames));
}
