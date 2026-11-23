#include "timeline_p.h"

// "hh:mm:ss,mmm" / "hh:mm:ss.mmm" / "mm:ss.mmm" → frames at fps (-1 = bad).
static int subtitleFrames(const QString &stamp, double fps) {
    QString t = stamp.trimmed();
    t.replace(QLatin1Char(','), QLatin1Char('.'));
    const QStringList parts = t.split(QLatin1Char(':'));
    if (parts.size() < 2 || parts.size() > 3)
        return -1;
    bool ok = true;
    double seconds = 0;
    for (const QString &part : parts) {
        bool good = false;
        const double v = part.toDouble(&good);
        ok = ok && good;
        seconds = seconds * 60 + v;
    }
    if (!ok || seconds < 0)
        return -1;
    return qRound(seconds * fps);
}

static QString srtStamp(int frame, double fps) {
    const qint64 ms = qRound64(frame * 1000.0 / fps);
    return QStringLiteral("%1:%2:%3,%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QVariantMap Timeline::titleStyleOf(const QVariantMap &entry) {
    QVariantMap style;
    style.insert(QStringLiteral("place"), entry.value(ClipKey::titlePlace));
    style.insert(QStringLiteral("size"), entry.value(ClipKey::titleSize));
    style.insert(QStringLiteral("font"), entry.value(ClipKey::titleFont));
    style.insert(QStringLiteral("color"), entry.value(ClipKey::titleColor));
    style.insert(QStringLiteral("align"), entry.value(ClipKey::titleAlign));
    style.insert(QStringLiteral("weight"), entry.value(ClipKey::titleWeight));
    style.insert(QStringLiteral("italic"), entry.value(ClipKey::titleItalic));
    style.insert(QStringLiteral("bg"), entry.value(ClipKey::titleBg));
    style.insert(QStringLiteral("outline"), entry.value(ClipKey::titleOutline, true));
    style.insert(QStringLiteral("caption"), entry.value(ClipKey::caption));
    for(const auto &pair:QList<QPair<QString,QString>>{{"titleTracking","tracking"},{"titleLineSpacing","lineSpacing"},{"titlePixelSize","pixelSize"},{"titleMargin","margin"},{"titleWidth","width"}})
        if(entry.contains(pair.first))style[pair.second]=entry.value(pair.first);
    // The fuller look rides with the rest, flat, as connectTitleAt takes it.
    const QVariantMap look = entry.value(ClipKey::titleLook).toMap();
    for (auto it = look.begin(); it != look.end(); ++it) style.insert(it.key(), it.value());
    if (entry.contains(ClipKey::speaker)) style.insert(QStringLiteral("speaker"), entry.value(ClipKey::speaker));
    if (entry.value(ClipKey::ownStyle).toBool()) style.insert(QStringLiteral("ownStyle"), true);
    return style;
}

QVariantMap Timeline::captionLookOf(const QVariantMap &style) {
    const auto text = [&](const char *key, const QString &fallback) {
        const QString v = style.value(QLatin1String(key)).toString();
        return v.isEmpty() ? fallback : v;
    };
    const auto number = [&](const char *key, double fallback) {
        const QVariant v = style.value(QLatin1String(key));
        return v.isValid() && !v.isNull() ? qRound(v.toDouble() * 100) / 100. : fallback;
    };
    const QString place = text("place", QStringLiteral("centre"));
    const bool edge = place == QStringLiteral("top") || place == QStringLiteral("bottom");
    const bool shadow = style.value("shadow").toBool();
    return {{"place", place}, {"size", text("size", QStringLiteral("medium"))}, {"font", text("font", {})},
            {"color", text("color", QStringLiteral("#ffffff")).toLower()}, {"align", text("align", QStringLiteral("centre"))},
            {"weight", text("weight", {})}, {"italic", style.value("italic").toBool()}, {"bg", text("bg", {}).toLower()},
            {"outline", !style.contains("outline") || style.value("outline").isNull() || style.value("outline").toBool()},
            {"pixelSize", number("pixelSize", 0)}, {"tracking", number("tracking", 0)}, {"lineSpacing", number("lineSpacing", 1)},
            {"margin", edge ? number("margin", 8) : 0.}, {"width", edge ? number("width", 90) : 0.},
            {"shadow", shadow}, {"shadowColor", shadow ? text("shadowColor", QStringLiteral("#000000")).toLower() : QString()},
            {"shadowOffset", shadow ? number("shadowOffset", 3) : 0.}, {"shadowSoftness", shadow ? number("shadowSoftness", 4) : 0.},
            {"outlineColor", text("outlineColor", QStringLiteral("#000000")).toLower()}, {"outlineWidth", number("outlineWidth", 2)},
            {"boxPadding", number("boxPadding", -1)}, {"boxRadius", number("boxRadius", 0)}};
}

bool Timeline::captionHasOwnLook(const QVariantMap &e, const QVariantMap &trackStyle) {
    const bool moved = !qFuzzyIsNull(e.value(ClipKey::rectX, 0.).toDouble()) || !qFuzzyIsNull(e.value(ClipKey::rectY, 0.).toDouble())
                       || !qFuzzyCompare(e.value(ClipKey::rectW, 1.).toDouble(), 1.) || !qFuzzyCompare(e.value(ClipKey::rectH, 1.).toDouble(), 1.);
    return e.value(ClipKey::ownStyle).toBool() || moved
           || captionLookOf(titleStyleOf(e)) != captionLookOf(trackStyle.isEmpty() ? legacyCaptionStyle() : trackStyle);
}

QVariantMap Timeline::legacyCaptionStyle() {
    return {{"place", "lower"}, {"size", "small"}, {"bg", "#99000000"}};
}

bool Timeline::connectTitleAt(const QString &text, int position, int length,
                                 const QVariantMap &style, int lane) {
    if (position < 0 || length < 1 || text.isEmpty())
        return false;
    // A title can be the timeline's first content. An empty primary playlist
    // is enough: the compositor supplies black beneath connected pictures.
    // This also initializes title-only snapshots during reopen and history.
    if (!primary() && !lockProfileTo(QStringLiteral("color:black")))
        return false;
    if (lane > 0 && lane < trackCount() && !m_restoring && !trackLocked(lane)
        && breakLaneDissolves(lane, position, position + length) < 0)
        return false;

    // Unknown style keys fall back to the defaults, so old projects (and
    // hand-edited XML) stay loadable.
    const QString place = style.value(QStringLiteral("place")).toString();
    const QString sizeKey = style.value(QStringLiteral("size")).toString();
    const QString font = style.value(QStringLiteral("font")).toString();
    QString color = style.value(QStringLiteral("color")).toString();
    if (!color.startsWith(QLatin1Char('#')) || color.size() != 7)
        color.clear(); // only #rrggbb; anything else means default white
    const bool lower = place == QStringLiteral("lower");
    // Captions sit by a frame edge: "top" or "bottom", a margin from it and a
    // width to wrap in (typography below). The older "lower" and "centre"
    // keep their look.
    const QString placed = place == QStringLiteral("top") || place == QStringLiteral("bottom") ? place
                           : lower ? QStringLiteral("lower") : QStringLiteral("centre");
    const bool edge = placed == QStringLiteral("top") || placed == QStringLiteral("bottom");
    const int divisor = sizeKey == QStringLiteral("small") ? 12
                        : sizeKey == QStringLiteral("large") ? 5 : 8;
    // Styling beyond the basics (all optional; absent = the old look):
    // align left|centre|right, weight "bold", italic, a background box
    // colour (#aarrggbb — "" = none), outline on/off (default on).
    QString align = style.value(QStringLiteral("align")).toString();
    if (align != QStringLiteral("left") && align != QStringLiteral("right"))
        align = QStringLiteral("centre");
    const bool bold = style.value(QStringLiteral("weight")).toString() == QStringLiteral("bold");
    const bool italic = style.value(QStringLiteral("italic")).toBool();
    QString bg = style.value(QStringLiteral("bg")).toString();
    if (!bg.startsWith(QLatin1Char('#')) || (bg.size() != 9 && bg.size() != 7))
        bg.clear();
    else if (bg.size() == 7)
        bg = QStringLiteral("#ff") + bg.mid(1);
    const bool outline = !style.contains(QStringLiteral("outline"))
                         || style.value(QStringLiteral("outline")).toBool();

    edit::Producer producer(*m_profile, "color:#00000000"); // transparent
    if (!producer.is_valid())
        return false;
    // A title has a still's room, so its end can be dragged or typed longer;
    // it shows `length` frames of it.
    producer.set("length", qMax(length, MediaPlayer::kStillFrames));
    producer.set_in_and_out(0, length - 1);
    // The style rides the producer next to the text — filters don't survive
    // the path-based restore, producer properties (and project XML) do.
    producer.set("omavid:title", text.toUtf8().constData());
    producer.set("omavid:titlePlace", placed.toUtf8().constData());
    producer.set("omavid:titleSize",
                 divisor == 12 ? "small" : divisor == 5 ? "large" : "medium");
    if (!font.isEmpty())
        producer.set("omavid:titleFont", font.toUtf8().constData());
    if (!color.isEmpty())
        producer.set("omavid:titleColor", color.toUtf8().constData());
    if (align != QStringLiteral("centre"))
        producer.set("omavid:titleAlign", align.toUtf8().constData());
    if (bold)
        producer.set("omavid:titleWeight", "bold");
    if (italic)
        producer.set("omavid:titleItalic", 1);
    if (!bg.isEmpty())
        producer.set("omavid:titleBg", bg.toUtf8().constData());
    if (!outline)
        producer.set("omavid:titleOutline", 0);
    if (style.value(QStringLiteral("caption")).toBool() || (lane>0 && lane<trackCount() && m_tracks[size_t(lane)].mix.value("editKind")=="caption"))
        producer.set("omavid:caption", 1);
    // A caption's speaker, and its own look kept from its track's style.
    const QString speaker = style.value(QStringLiteral("speaker")).toString().simplified().left(80);
    if (!speaker.isEmpty())
        producer.set("omavid:speaker", speaker.toUtf8().constData());
    if (style.value(QStringLiteral("ownStyle")).toBool())
        producer.set("omavid:ownStyle", 1);

    QVariantMap typography;
    if(style.contains("tracking"))typography["tracking"]=qBound(-50.,style.value("tracking").toDouble(),200.);
    if(style.contains("lineSpacing"))typography["lineSpacing"]=qBound(.5,style.value("lineSpacing").toDouble(),4.);
    if(style.value("pixelSize").toDouble()>0)typography["pixelSize"]=qBound(1.,style.value("pixelSize").toDouble(),2000.);
    // Margin from the edge and the width text wraps in, in percent of the
    // frame's height and width. An edge place always has them, so it is
    // drawn by the engine, which wraps and places by them.
    if(style.contains("margin")||edge)typography["margin"]=qBound(0.,style.value("margin",8.).toDouble(),45.);
    if(style.contains("width")||edge)typography["width"]=qBound(20.,style.value("width",90.).toDouble(),100.);
    // The fuller look (EditEngine::titleLookKeys), each only when given: a
    // title without them is drawn as titles always were.
    const auto colour = [](const QVariant &v) {
        const QString c = v.toString().toLower();
        static const QRegularExpression hex(QStringLiteral("^#([0-9a-f]{6}|[0-9a-f]{8})$"));
        return hex.match(c).hasMatch() ? c : QString();
    };
    const auto given = [&style](const char *key) {
        const QVariant v = style.value(QLatin1String(key));
        return v.isValid() && !v.isNull();
    };
    // A value that draws as the default (no shadow, a black edge of 2, square
    // corners, automatic padding: below 0) is left out, so a title in the old
    // look is kept as it always was.
    if (given("shadow") && style.value("shadow").toBool()) {
        typography["shadow"] = true;
        if (given("shadowColor") && !colour(style.value("shadowColor")).isEmpty()) typography["shadowColor"] = colour(style.value("shadowColor"));
        if (given("shadowOffset")) typography["shadowOffset"] = qBound(-50., style.value("shadowOffset").toDouble(), 50.);
        if (given("shadowSoftness")) typography["shadowSoftness"] = qBound(0., style.value("shadowSoftness").toDouble(), 50.);
    }
    if (given("outlineColor") && !colour(style.value("outlineColor")).isEmpty() && colour(style.value("outlineColor")) != QStringLiteral("#000000")
        && colour(style.value("outlineColor")) != QStringLiteral("#ff000000"))
        typography["outlineColor"] = colour(style.value("outlineColor"));
    if (given("outlineWidth") && !qFuzzyCompare(style.value("outlineWidth").toDouble(), 2.))
        typography["outlineWidth"] = qBound(0., style.value("outlineWidth").toDouble(), 20.);
    if (given("boxPadding") && style.value("boxPadding").toDouble() >= 0)
        typography["boxPadding"] = qMin(style.value("boxPadding").toDouble(), 200.);
    if (given("boxRadius") && style.value("boxRadius").toDouble() > 0)
        typography["boxRadius"] = qMin(style.value("boxRadius").toDouble(), 200.);
    if(!typography.isEmpty()){
        producer.set("omavid:titleTypography",QJsonDocument::fromVariant(typography).toJson(QJsonDocument::Compact).constData());
        QVariantMap styled=style;for(const QString &key:EditEngine::titleLookKeys())styled.remove(key);for(auto it=typography.begin();it!=typography.end();++it)styled[it.key()]=it.value();styled["place"]=placed;
        edit::Filter titleFilter(*m_profile,"omaedit.title");
        titleFilter.set("config",QJsonDocument::fromVariant(QVariantMap{{"group","title"},{"style",styled},{"text",text},{"height",m_profile->height()}}).toJson(QJsonDocument::Compact).constData());
        producer.attach(titleFilter);
        return placeConnected(producer,position,0,length-1,lane>0?lane-1:-1);
    }
    edit::Filter textFilter(*m_profile, "dynamictext");
    if (!textFilter.is_valid())
        return false;
    textFilter.set("argument", text.toUtf8().constData());
    // Lower third: the text box hugs the bottom band.
    textFilter.set("geometry", lower ? "0%/65%:100%x30%" : "0%/0%:100%x100%");
    textFilter.set("halign", align.toUtf8().constData());
    textFilter.set("valign", "middle");
    textFilter.set("size", m_profile->height() / divisor);
    if (!font.isEmpty())
        textFilter.set("family", font.toUtf8().constData());
    textFilter.set("weight", bold ? 700 : 400);
    textFilter.set("style", italic ? "italic" : "normal");
    textFilter.set("fgcolour",
                   color.isEmpty()
                       ? "#ffffffff"
                       : QStringLiteral("#ff%1").arg(color.mid(1))
                             .toUtf8().constData());
    textFilter.set("olcolour", "#ff000000");
    textFilter.set("outline", outline ? 2 : 0);
    textFilter.set("bgcolour", bg.isEmpty() ? "#00000000" : bg.toUtf8().constData());
    if (!bg.isEmpty())
        textFilter.set("pad", m_profile->height() / (divisor * 4));
    producer.attach(textFilter);

    return placeConnected(producer, position, 0, length - 1,
                          lane > 0 ? lane - 1 : -1);
}

bool Timeline::placeConnected(edit::Producer &producer, int position, int in,
                                 int out, int laneHint) {
    std::unique_ptr<edit::Producer> cut(producer.cut(in >= 0 ? in : 0,
                                                    out >= 0 ? out : -1));
    if (!cut || !cut->is_valid())
        return false;

    // Overlapping connects auto-stack into the lowest free lane OF THEIR
    // KIND (audio connects live on audio lanes, shown below the storyline);
    // restore() pins the lane instead to rebuild layouts.
    const bool audioOnly = producer.get_int("omavid:audioOnly") != 0;
    const int laneIdx = laneHint >= 0
                            ? laneHint
                            : pickLane(position, cut->get_playtime(), audioOnly);
    if (laneIdx + 1 < trackCount() && m_tracks[size_t(laneIdx) + 1].locked)
        return false; // a locked track takes nothing
    if (laneIdx+1<trackCount() && !m_tracks[size_t(laneIdx)+1].audioEntries.isEmpty()) return false;
    edit::Playlist *target = ensureLane(laneIdx);
    if (!target)
        return false;
    m_tracks[size_t(laneIdx) + 1].kind = audioOnly ? Track::Audio : Track::Video;
    keepCarvedClips(laneIdx + 1, position, position + cut->get_playtime());
    const bool ok = laneOverwrite(target, position, *cut);
    tidyConnected();
    return ok;
}

edit::Playlist *Timeline::ensureLane(int laneIdx) {
    if (!m_profile || laneIdx < 0)
        return nullptr;
    while (laneTotal() <= laneIdx) {
        Track track;
        track.playlist = std::make_unique<edit::Playlist>(*m_profile);
        m_tracks.push_back(std::move(track));
    }
    return laneAt(laneIdx);
}

bool Timeline::laneSpanFree(int laneIdx, int start, int span) const {
    if (laneIdx >= laneTotal())
        return true;
    edit::Playlist *lanePl = laneAt(laneIdx);
    // Starts are counted along the way: asking the playlist for each clip's
    // start walks it from the top every time.
    int clipStart = 0;
    for (int i = 0; i < lanePl->count(); ++i) {
        const int clipLen = lanePl->clip_length(i);
        if (!lanePl->is_blank(i) && clipStart < start + span && clipStart + clipLen > start)
            return false;
        clipStart += clipLen;
    }
    return true;
}

// A lane's kind is the kind of the clips it holds (they never mix — this
// picker keeps it that way); an empty lane takes either kind.
bool Timeline::laneIsKind(int laneIdx, bool audioOnly) const {
    if (laneIdx >= laneTotal())
        return true;
    if (!m_tracks[size_t(laneIdx)+1].audioEntries.isEmpty()) return false; // frame insertion cannot overwrite its logical cuts
    edit::Playlist *lanePl = laneAt(laneIdx);
    for (int i = 0; i < lanePl->count(); ++i) {
        if (lanePl->is_blank(i) || lanePl->is_mix(i))
            continue;
        edit::ClipInfo *info = lanePl->clip_info(i);
        if (!info)
            continue;
        const bool laneAudio =
            info->producer && info->producer->get_int("omavid:audioOnly") != 0;
        delete info;
        return laneAudio == audioOnly;
    }
    // Nothing but blanks: the header's kind decides (a user-added A2 stays
    // an audio track).
    return m_tracks[size_t(laneIdx) + 1].kind == (audioOnly ? Track::Audio
                                                             : Track::Video);
}

int Timeline::pickLane(int start, int span, bool audioOnly) const {
    for (int i = 0; i < laneTotal(); ++i)
        if (!m_tracks[size_t(i) + 1].locked && m_tracks[size_t(i)+1].mix.value("editKind")!="caption" && laneIsKind(i, audioOnly)
            && laneSpanFree(i, start, span))
            return i;
    return laneTotal();
}

int Timeline::soundTrackFor(int start, int span, bool free) const {
    for (int i = 0; i < laneTotal(); ++i)
        if (!m_tracks[size_t(i) + 1].locked && m_tracks[size_t(i) + 1].mix.value("editKind") != "caption"
            && laneIsKind(i, true) && (!free || laneSpanFree(i, start, span)))
            return i + 1;
    return trackCount();
}

bool Timeline::removeConnected(int index) {
    int laneIdx = -1, entry = -1;
    if (!connectedLocate(index, &laneIdx, &entry))
        return false;
    // Its dissolves go with it (see removeOn).
    const int from = laneAt(laneIdx)->clip_start(entry);
    const int broken = breakLaneDissolves(laneIdx + 1, from, from + laneAt(laneIdx)->clip_length(entry));
    if (broken < 0 || (broken > 0 && !connectedLocate(index, &laneIdx, &entry)))
        return false;
    delete laneAt(laneIdx)->replace_with_blank(entry);
    tidyConnected();
    return true;
}

bool Timeline::moveConnected(int index, int newStart) {
    int laneIdx = -1, entry = -1;
    if (!connectedLocate(index, &laneIdx, &entry) || newStart < 0)
        return false;
    int span = laneAt(laneIdx)->clip_length(entry);
    // The clip's own dissolves go with it, and any it lands across.
    const int from = laneAt(laneIdx)->clip_start(entry);
    int broken = breakLaneDissolves(laneIdx + 1, from, from + span);
    if (broken > 0) {
        if (!connectedLocate(index, &laneIdx, &entry))
            return false;
        // A transition made of handles gave the clip its own first frames
        // back: it goes from the cut, as far as it was asked to.
        newStart = qMax(0, newStart + laneAt(laneIdx)->clip_start(entry) - from);
        span = laneAt(laneIdx)->clip_length(entry);
    }
    if (broken >= 0) {
        const int landed = breakLaneDissolves(laneIdx + 1, newStart, newStart + span);
        broken = landed < 0 ? -1 : broken + landed;
    }
    if (broken < 0 || (broken > 0 && !connectedLocate(index, &laneIdx, &entry)))
        return false;
    edit::Playlist *lanePl = laneAt(laneIdx);
    std::unique_ptr<edit::Producer> cut(lanePl->replace_with_blank(entry));
    if (!cut || !cut->is_valid())
        return false;
    // Sideways moves stay in their lane — a horizontal drag never changes track.
    lanePl->consolidate_blanks(0); // the hole it left merges with its neighbours
    keepCarvedClips(laneIdx + 1, newStart, newStart + span);
    const bool ok = laneOverwrite(lanePl, newStart, *cut);
    tidyConnected();
    return ok;
}

bool Timeline::trimConnected(int index, int in, int out, int newStart) {
    int laneIdx = -1, entry = -1;
    if (!connectedLocate(index, &laneIdx, &entry))
        return false;
    if (int track = -1, clip = -1; connectedAddress(index, &track, &clip))
        if (const std::optional<bool> held = trimLaneClipInTransition(track, clip, in, out, newStart, false))
            return *held;
    return trimLaneEntry(laneIdx, entry, in, out, newStart);
}

bool Timeline::trimLaneEntry(int laneIdx, int entry, int in, int out,
                                int newStart) {
    if (newStart < 0)
        return false;
    edit::Playlist *lanePl = laneAt(laneIdx);
    edit::ClipInfo *info = lanePl->clip_info(entry);
    if (!info)
        return false;
    const int length = info->length;
    delete info;
    if (in < 0 || out < in || out >= length)
        return false;
    // The trimmed clip goes back with insert_at, which shoves a clip it lands
    // on (or a blank too short for it) along the lane instead of covering it:
    // a span reaching another clip, or an edge beside a dissolve, is refused.
    if ((entry > 0 && lanePl->is_mix(entry - 1)) || (entry + 1 < lanePl->count() && lanePl->is_mix(entry + 1)))
        return false;
    for (int i = 0, start = 0; i < lanePl->count(); ++i) {
        const int end = start + lanePl->clip_length(i); // counted along, not asked for each clip
        if (i != entry && !lanePl->is_blank(i) && newStart < end && newStart + (out - in + 1) > start)
            return false;
        start = end;
    }
    const auto animatedEntry=entryMapOn(laneIdx+1,clipIndexContaining(laneIdx+1,lanePl->clip_start(entry)));
    std::unique_ptr<edit::Producer> cut(lanePl->replace_with_blank(entry));
    if (!cut || !cut->is_valid())
        return false;
    // insert_at consumes only the ONE blank under the position: merge the
    // hole with any blank beside it so a grown clip lands in the room a
    // ripple trim just made instead of shoving its neighbour.
    lanePl->consolidate_blanks(0);
    // Anchored keyframe filters go stale across set_in_and_out — capture
    // and re-apply shifted, like the storyline trim does.
    const int oldIn = cut->get_in();
    const QVariantList volumeKf = volumeKfOf(cut.get());
    const QVariantList opacityKf = opacityKfOf(cut.get());
    // Fades are anchored to the edges they sit on: remember them so they
    // follow the edges, as a trim on V1 keeps them.
    int fadeIn = 0, fadeOut = 0;
    fadeLengths(cut.get(), oldIn, cut->get_out(), &fadeIn, &fadeOut);
    cut->set_in_and_out(in, out);
    if (fadeIn > 0 || fadeOut > 0)
        applyFades(cut.get(), in, out, qMin(fadeIn, out - in + 1), qMin(fadeOut, out - in + 1));
    auto shiftKeys = [&](const QVariantList &keys, const char *valueKey) {
        QVariantList shifted;
        for (const QVariant &v : keys) {
            QVariantMap key = v.toMap();
            key.insert(QStringLiteral("frame"),
                       key.value(QStringLiteral("frame")).toInt()
                           + (oldIn - in));
            Q_UNUSED(valueKey);
            shifted.append(key);
        }
        return shifted;
    };
    if (!volumeKf.isEmpty())
        applyVolumeKf(*m_profile, cut.get(), shiftKeys(volumeKf, "db"),
                      in, out);
    if (!opacityKf.isEmpty())
        applyOpacityKf(*m_profile, cut.get(), shiftKeys(opacityKf, "alpha"),
                       in, out);
    if(!animatedEntry.value(ClipKey::editKeys).toMap().isEmpty()){
        auto shifted=animatedEntry;shifted[ClipKey::editKeys]=EditEngine::shiftedKeys(animatedEntry.value(ClipKey::editKeys).toMap(),in-oldIn,out-in+1);EditEngine::attach(*m_profile,*cut,shifted,in,out);reorderAttributeFilters(cut.get());
    }
    if(const auto effects=animatedEntry.value(ClipKey::videoEffects).toList();render::effectsAnimated(effects)){
        VideoEffects::apply(*m_profile,cut.get(),EditEngine::shiftedEffects(effects,in-oldIn),true);reorderAttributeFilters(cut.get());
    }
    const bool ok = lanePl->insert_at(newStart, *cut, 1) >= 0;
    tidyConnected();
    return ok;
}

// Transform a connected clip into a picture-in-picture: an "affine" filter
// on the lane cut (the qtblend FILTER blanks the clip — probed 7.40; the
// track transition can't differ per clip). x/y/w/h are 0..1 fractions of
// the frame; full frame removes the filter. The normalized rect rides the
// filter as omavid:rect — cut filters serialize, so XML keeps it, and
// restore() re-applies from the entries.
bool Timeline::setConnectedRect(int index, double x, double y, double w,
                                   double h) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    // Keep whatever rotation and anchor the clip already has.
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, clip);
    double ox = 0, oy = 0, ow = 1, oh = 1, rot = 0, ax = 0.5, ay = 0.5;
    if (info)
        connectedRect(info->cut, &ox, &oy, &ow, &oh, &rot, &ax, &ay);
    return setRectAt(track, clip, x, y, w, h, rot, ax, ay);
}

// A title in a transition is made again as the title drawn, with the
// transition held aside and put back across the same cut on the new title.
bool Timeline::updateTitleAt(int index, const QString &text,
                                const QVariantMap &style) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return withMixesHeld(track, clip, [&] { return updateTitlePlain(index, text, style); });
}

bool Timeline::updateTitlePlain(int index, const QString &text,
                                const QVariantMap &style) {
    int laneIdx = -1, entry = -1;
    if (!connectedLocate(index, &laneIdx, &entry) || text.trimmed().isEmpty() || trackLocked(laneIdx+1))
        return false;
    edit::ClipInfo *info = laneAt(laneIdx)->clip_info(entry);
    if (!info)
        return false;
    const char *old = info->producer ? info->producer->get("omavid:title") : nullptr;
    const int start = info->start;
    const int frames = info->frame_count;
    delete info;
    if (!old || !*old)
        return false; // only titles get their text edited
    // A restyle is a fresh producer: carry the clip's other attributes
    // (fades, opacity keys, PiP rect, label, link, markers, bypass) over.
    const int clip = clipIndexContaining(laneIdx + 1, start);
    const QVariantMap map = clip >= 0 ? entryMapOn(laneIdx + 1, clip) : QVariantMap();
    delete laneAt(laneIdx)->replace_with_blank(entry);
    QVariantMap newStyle=style;if(map.value(ClipKey::caption).toBool())newStyle["caption"]=true;
    // A caption keeps its speaker and its own-look mark unless told otherwise.
    if(!newStyle.contains("speaker")&&map.contains(ClipKey::speaker))newStyle["speaker"]=map.value(ClipKey::speaker);
    if(!newStyle.contains("ownStyle")&&map.value(ClipKey::ownStyle).toBool())newStyle["ownStyle"]=true;
    if (!connectTitleAt(text, start, frames, newStyle, laneIdx + 1))
        return false;
    const int placed = clipIndexAt(laneIdx + 1, start);
    if (placed >= 0 && !map.isEmpty())
        applyEntryAttributes(laneIdx + 1, placed, map);
    return true;
}

int Timeline::connectedCount() const {
    int clips = 0;
    for (int L = 0; L < laneTotal(); ++L) {
        if (!m_tracks[size_t(L)+1].audioEntries.isEmpty()) { clips+=m_tracks[size_t(L)+1].audioEntries.size(); continue; }
        for (int i = 0; i < laneAt(L)->count(); ++i)
            if (!laneAt(L)->is_blank(i) && !laneAt(L)->is_mix(i))
                ++clips;
    }
    return clips;
}

// Flat index — lane 1's clips in position order, then lane 2's, matching
// the order entries() lists them in — to (lane vector index, entry index).
bool Timeline::connectedLocate(int index, int *laneIdx, int *entryIdx) const {
    if (index < 0)
        return false;
    int clip = 0;
    for (int L = 0; L < laneTotal(); ++L) {
        const auto &logical=m_tracks[size_t(L)+1].audioEntries;
        if (!logical.isEmpty()) {
            if (index<clip+logical.size()) return false; // use the precise edit commands
            clip+=logical.size(); continue;
        }
        edit::Playlist *lanePl = laneAt(L);
        for (int i = 0; i < lanePl->count(); ++i) {
            if (lanePl->is_blank(i) || lanePl->is_mix(i))
                continue;
            if (clip == index) {
                if (m_tracks[size_t(L) + 1].locked)
                    return false; // locked: addressable to read, not to edit
                *laneIdx = L;
                *entryIdx = i;
                return true;
            }
            ++clip;
        }
    }
    return false;
}

// Merge adjacent blanks and drop trailing ones per lane, then drop empty
// top lanes — so lanes never pad the duration and the stack stays tight.
void Timeline::tidyConnected() {
    for (int L = 0; L < laneTotal(); ++L) {
        edit::Playlist *pl = laneAt(L);
        pl->consolidate_blanks(0);
        while (pl->count() > 0 && pl->is_blank(pl->count() - 1))
            pl->remove(pl->count() - 1);
    }
    // Tracks persist while empty (edit page) — no popping here; see
    // removeTrack().
}

// Pull every lane cut out first, then re-place — a shifting clip can never
// transiently overwrite a neighbour, whatever order the moves happen in.
// Cuts go back into the SAME lane they came from (flat order = lane by
// lane, matching entries()).
void Timeline::replaceLaneStarts(const QList<int> &wanted) {
    // A dissolve's mix entry is bound to its two clips — pulling them out
    // would orphan it. With any dissolve on any lane, shift by snapshot
    // instead: rewrite the lane starts and rebuild (restore places lanes
    // at absolute starts and re-applies their mixes).
    bool laneMixes = std::any_of(m_tracks.cbegin(),m_tracks.cend(),[](const Track &t) { return !t.audioEntries.isEmpty(); });
    for (int L = 0; L < laneTotal() && !laneMixes; ++L)
        for (int i = 0; i < laneAt(L)->count() && !laneMixes; ++i)
            laneMixes = laneAt(L)->is_mix(i);
    if (laneMixes) {
        const QVariantList before = entries();
        QVariantList result;
        int k = 0;
        for (const QVariant &v : before) {
            QVariantMap map = v.toMap();
            if (map.value(ClipKey::lane).toInt() >= 1) {
                const int want = k < wanted.size() ? wanted.at(k) : -2;
                ++k;
                if (want == -1)
                    continue; // dropped
                if (want >= 0) {
                    if (map.contains(ClipKey::audioTiming)) {
                        map=AudioTimeline::shifted(map,{qint64(want-map.value(ClipKey::start).toInt())*m_profile->frame_rate_den(),m_profile->frame_rate_num()},
                            m_profile->frame_rate_num(),m_profile->frame_rate_den());
                        if (map.isEmpty()) { m_rippleFailed = true; return; }
                    } else map.insert(ClipKey::start, want);
                }
            }
            result.append(map);
        }
        // Restore places lane clips in order and decorates the clip it just
        // placed: re-sort the moved lane entries by (lane, start), and slide a
        // clip that would land on its neighbour past it, as the live path's
        // insert_at does, rather than carve the clip it lands on.
        QList<int> places;
        QVariantList lanes;
        for (int i = 0; i < result.size(); ++i)
            if (result.at(i).toMap().value(ClipKey::lane).toInt() >= 1) {
                places << i;
                lanes << result.at(i);
            }
        std::stable_sort(lanes.begin(), lanes.end(), [](const QVariant &x, const QVariant &y) {
            const QVariantMap a = x.toMap(), b = y.toMap();
            const int la = a.value(ClipKey::lane).toInt(), lb = b.value(ClipKey::lane).toInt();
            return la != lb ? la < lb : a.value(ClipKey::start).toInt() < b.value(ClipKey::start).toInt();
        });
        int lane = -1, end = 0;
        for (QVariant &v : lanes) {
            QVariantMap map = v.toMap();
            const int at = map.value(ClipKey::lane).toInt(), start = map.value(ClipKey::start).toInt();
            if (at != lane) { lane = at; end = 0; }
            if (start < end) {
                if (map.contains(ClipKey::audioTiming)) {
                    map = AudioTimeline::shifted(map, {qint64(end - start) * m_profile->frame_rate_den(), m_profile->frame_rate_num()},
                                                 m_profile->frame_rate_num(), m_profile->frame_rate_den());
                    if (map.isEmpty()) { m_rippleFailed = true; return; }
                } else map.insert(ClipKey::start, end);
            }
            end = qMax(end, map.value(ClipKey::start).toInt() + map.value(ClipKey::frames).toInt());
            v = map;
        }
        for (int i = 0; i < places.size(); ++i)
            result[places.at(i)] = lanes.at(i);
        if (!rebuild(result, before))
            m_rippleFailed = true; // back to `before`: the storyline moved, the lanes did not
        return;
    }
    // Where each track's clips stand now, in the order `wanted` lists them.
    struct Standing { int start, frames, wanted; };
    std::vector<QList<Standing>> standing;
    standing.resize(size_t(laneTotal()));
    {
        int k = 0;
        for (const QVariant &v : entries()) {
            const QVariantMap map = v.toMap();
            const int lane = map.value(ClipKey::lane).toInt();
            if (lane < 1 || lane > laneTotal())
                continue;
            const int start = map.value(ClipKey::start).toInt();
            standing[size_t(lane) - 1].append({start, map.value(ClipKey::frames).toInt(), k < wanted.size() ? wanted.at(k) : start});
            ++k;
        }
    }
    // A ripple moves everything on a track from one clip onward by the same
    // amount. That is the space before that clip made longer or shorter: no
    // clip needs to come out. (Taking every clip of every track out and
    // putting it back made a ripple cost more with every clip downstream.)
    const auto shiftedAsOne = [this](int L, const QList<Standing> &clips) {
        int first = -1;
        for (int i = 0; i < clips.size(); ++i)
            if (clips.at(i).wanted != clips.at(i).start) { first = i; break; }
        if (first < 0)
            return true; // nothing on this track moves
        const int delta = clips.at(first).wanted - clips.at(first).start;
        for (int i = first; i < clips.size(); ++i)
            if (clips.at(i).wanted < 0 || clips.at(i).wanted - clips.at(i).start != delta)
                return false;
        edit::Playlist *pl = laneAt(L);
        if (m_tracks[size_t(L) + 1].locked)
            return false;
        int entry = -1, seen = 0;
        for (int i = 0; i < pl->count() && entry < 0; ++i) {
            if (pl->is_blank(i) || pl->is_mix(i))
                continue;
            if (seen++ == first)
                entry = i;
        }
        if (entry < 0)
            return false;
        if (delta > 0) {
            pl->insert_blank(entry, delta - 1);
            return true;
        }
        // Closer: only into space that is there.
        if (entry == 0 || !pl->is_blank(entry - 1))
            return false;
        const int space = pl->clip_length(entry - 1);
        if (space < -delta)
            return false;
        if (pl->remove(entry - 1) != 0)
            return false;
        if (space + delta > 0)
            pl->insert_blank(entry - 1, space + delta - 1);
        return true;
    };
    // The switch the other narrow paths have: 0 takes every clip out and back.
    const bool asOne = qgetenv("OMAEDIT_NARROW_CONFORM") != "0";
    int flat = 0; // this track's first place in `wanted`
    for (int L = 0; L < laneTotal(); ++L) {
        const QList<Standing> &clips = standing[size_t(L)];
        const int base = flat;
        flat += int(clips.size());
        edit::Playlist *lanePl = laneAt(L);
        int there = 0;
        for (int i = 0; i < lanePl->count(); ++i)
            there += !lanePl->is_blank(i);
        if (asOne && there == clips.size() && shiftedAsOne(L, clips))
            continue;
        // Anything else: every cut of the track out first, then back, so a
        // shifting clip can never land on a neighbour that has yet to move.
        std::vector<std::unique_ptr<edit::Producer>> cuts;
        for (int i = 0; i < lanePl->count(); ++i) {
            if (lanePl->is_blank(i))
                continue;
            cuts.emplace_back(lanePl->replace_with_blank(i));
        }
        for (size_t i = 0; i < cuts.size() && base + int(i) < wanted.size(); ++i) {
            const int to = wanted.at(base + int(i));
            if (to < 0 || !cuts[i] || !cuts[i]->is_valid())
                continue; // -1 = drop it (no live caller does today)
            // When two wanted spans collide within a lane, insert_at settles
            // the later clip as close as it can without overwriting — it
            // slides right past the occupant.
            lanePl->insert_at(to, *cuts[i], 1);
        }
    }
    tidyConnected();
}

void Timeline::rippleFrom(int sourceTrack, int position, int delta,
                             bool includeSource, bool primaryRides) {
    if (delta == 0)
        return;
    // Where each linked V1 picture sits before V1 takes its share: sound
    // linked to a picture moves only when that picture did, or a ripple V1
    // refused (it never destroys picture) would slide the sound out of sync.
    const auto primaryStarts = [this] {
        QHash<QString, int> starts;
        for (const QVariant &v : entries()) {
            const QVariantMap map = v.toMap();
            const QString link = map.value(ClipKey::link).toString();
            if (map.value(ClipKey::lane).toInt() < 1 && !link.isEmpty())
                starts.insert(link, map.value(ClipKey::start).toInt());
        }
        return starts;
    };
    const QHash<QString, int> pictureBefore = primaryStarts();
    if (primaryRides && sourceTrack != 0 && trackCount() > 0 && !m_tracks[0].locked
        && m_tracks[0].autoSelect)
        shiftPrimaryFrom(position, delta);
    if (connectedCount() == 0)
        return;
    const QHash<QString, int> pictureAfter = sourceTrack != 0 ? primaryStarts() : pictureBefore;
    QList<int> wanted;
    const QVariantList now = entries();
    for (const QVariant &v : now) {
        const QVariantMap map = v.toMap();
        const int track = map.value(ClipKey::lane).toInt();
        if (track < 1)
            continue;
        const int start = map.value(ClipKey::start).toInt();
        const Track &t = m_tracks[size_t(track)];
        bool rides = track == sourceTrack ? includeSource
                                          : (!t.locked && t.autoSelect);
        const QString link = map.value(ClipKey::link).toString();
        if (rides && sourceTrack != 0 && track != sourceTrack && pictureBefore.contains(link))
            rides = pictureAfter.value(link) != pictureBefore.value(link);
        wanted.append(rides && start >= position ? qMax(0, start + delta)
                                                 : start);
    }
    replaceLaneStarts(wanted);
}

// V1's share of another track's ripple. Pushing right opens empty space
// at `position`; pulling left closes empty space — and ONLY empty space:
// if the frames before `position` hold a clip, V1 stays put rather than
// losing picture (the never-destroy rule).
void Timeline::shiftPrimaryFrom(int position, int delta) {
    if (!primary() || clipCount() == 0 || position < 0
        || position >= primaryFrameCount())
        return;
    if (delta > 0) {
        if (!ensureBoundaryAt(position))
            return;
        const int entry = primary()->get_clip_index_at(position);
        if (primary()->is_mix(entry) || (entry > 0 && primary()->is_mix(entry - 1)))
            return; // never wedge space into a dissolve
        primary()->insert_blank(entry, delta - 1);
    } else {
        const int from = position + delta;
        if (from < 0)
            return;
        const int entry = primary()->get_clip_index_at(from);
        if (entry < 0 || !primary()->is_blank(entry))
            return;
        const int blankStart = primary()->clip_start(entry);
        const int blankEnd = blankStart + primary()->clip_length(entry);
        if (blankStart > from || blankEnd < position)
            return; // the space to close isn't all blank
        primary()->remove_region(from, -delta);
    }
    tidyPrimary();
}

int Timeline::storylineEndIn(const QVariantList &snapshot, int index) {
    int clip = 0;
    for (const QVariant &v : snapshot) {
        const QVariantMap map = v.toMap();
        if (map.value(ClipKey::lane).toInt() >= 1)
            continue;
        if (clip++ == index)
            return map.value(ClipKey::start).toInt()
                   + map.value(ClipKey::frames).toInt();
    }
    return 0;
}

QVariantList Timeline::parseSubtitles(const QString &text, double fps) {
    QVariantList cues;
    if (fps <= 0)
        return cues;
    QString body = text;
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = body.split(QLatin1Char('\n'));
    const bool webvtt = body.startsWith(QStringLiteral("WEBVTT")) || body.startsWith(QStringLiteral("\uFEFFWEBVTT"));
    int i = 0;
    while (i < lines.size()) {
        const QString line = lines.at(i).trimmed();
        const int arrow = line.indexOf(QStringLiteral("-->"));
        if (arrow < 0) { // an index, "WEBVTT", a NOTE, a blank: skip
            ++i;
            continue;
        }
        const int start = subtitleFrames(line.left(arrow), fps);
        // WebVTT may trail cue settings after the end stamp ("... line:0").
        const int end = subtitleFrames(line.mid(arrow + 3).trimmed()
                                           .section(QRegularExpression(QStringLiteral("\\s")), 0, 0), fps);
        ++i;
        QStringList textLines;
        while (i < lines.size() && !lines.at(i).trimmed().isEmpty()) {
            QString t = lines.at(i);
            t.remove(QRegularExpression(QStringLiteral("<[^>]*>"))); // VTT/HTML tags
            // A WebVTT file writes these three as entities.
            if (webvtt)
                t.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
                    .replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            textLines.append(t.trimmed());
            ++i;
        }
        if (start < 0 || end <= start || textLines.isEmpty())
            continue;
        QVariantMap cue;
        cue.insert(QStringLiteral("start"), start);
        cue.insert(QStringLiteral("frames"), end - start);
        cue.insert(QStringLiteral("text"), textLines.join(QLatin1Char('\n')));
        cues.append(cue);
    }
    std::sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toInt()
               < b.toMap().value(QStringLiteral("start")).toInt();
    });
    return cues;
}

QString Timeline::formatSrt(const QVariantList &cues, double fps) {
    QString out;
    if (fps <= 0)
        return out;
    int n = 0;
    for (const QVariant &v : cues) {
        const QVariantMap cue = v.toMap();
        const int start = cue.value(QStringLiteral("start")).toInt();
        const int frames = cue.value(QStringLiteral("frames")).toInt();
        QString text = cue.value(QStringLiteral("text")).toString().trimmed();
        // A blank line ends a SubRip block — a title with one inside
        // would split into a malformed second block.
        text.replace(QRegularExpression(QStringLiteral("\\n\\s*\\n+")), QStringLiteral("\n"));
        if (frames < 1 || text.isEmpty())
            continue;
        out += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
                   .arg(++n)
                   .arg(srtStamp(start, fps), srtStamp(start + frames, fps), text);
    }
    return out;
}

QString Timeline::formatVtt(const QVariantList &cues, double fps) {
    if (fps <= 0)
        return {};
    QString out = QStringLiteral("WEBVTT\n\n");
    const auto stamp = [fps](int frame) { return srtStamp(frame, fps).replace(QLatin1Char(','), QLatin1Char('.')); };
    for (const QVariant &v : cues) {
        const QVariantMap cue = v.toMap();
        const int start = cue.value(QStringLiteral("start")).toInt();
        const int frames = cue.value(QStringLiteral("frames")).toInt();
        QString text = cue.value(QStringLiteral("text")).toString().trimmed();
        // A blank line ends a cue, as in SubRip.
        text.replace(QRegularExpression(QStringLiteral("\\n\\s*\\n+")), QStringLiteral("\n"));
        if (frames < 1 || text.isEmpty())
            continue;
        text.replace(QStringLiteral("&"), QStringLiteral("&amp;")).replace(QStringLiteral("<"), QStringLiteral("&lt;"))
            .replace(QStringLiteral(">"), QStringLiteral("&gt;"));
        out += QStringLiteral("%1 --> %2\n%3\n\n").arg(stamp(start), stamp(start + frames), text);
    }
    return out;
}
