// Helpers the Timeline sources share: reading and writing what a cut carries.
#include "timeline_p.h"


// The filter tags one bypass key covers ("level" is volume + pan; the
// keyframed twins ride with their base attribute).
QList<QByteArray> bypassTags(const QString &key) {
    if (key == QStringLiteral("level"))
        return {"omavid:volume", "omavid:volumeKf", "omavid:pan"};
    if (key == QStringLiteral("opacity"))
        return {"omavid:opacity", "omavid:opacityKf"};
    if (key == QStringLiteral("grade"))
        return {"omavid:gradeFx", "omavid:grade"};
    return {QByteArray("omavid:") + key.toUtf8()};
}

// The bypass key a tagged attribute filter belongs to ("" for none).
QString bypassKeyOf(edit::Filter &f) {
    for (const QString &key : Timeline::bypassableKeys())
        for (const QByteArray &tag : bypassTags(key))
            if (f.get(tag.constData()))
                return key;
    return QString();
}


// Drop `cut` onto a lane at `position` as an OVERWRITE: the span it covers
// is carved out first (a blank, a clip's middle, whatever is there), then
// the cut goes in. Plain insert_at only consumes the one blank under the
// position — a clip landing half on a blank pushed everything after it
// down the track by the difference, so every sideways move rippled.
bool laneOverwrite(edit::Playlist *pl, int position, edit::Producer &cut) {
    const int span = cut.get_playtime();
    if (position < pl->get_playtime())
        pl->remove_region(position, span); // closes the span (ripples left)
    if (position >= pl->get_playtime())
        return pl->insert_at(position, cut, 1) >= 0; // pads a blank, appends
    // A plain insert at the seam pushes what followed back to its frames;
    // insert_at would eat the blank there a second time.
    int entry = pl->get_clip_index_at(position);
    const int entryStart = pl->clip_start(entry);
    if (position > entryStart) {
        if (pl->split(entry, position - entryStart - 1) != 0)
            return false;
        ++entry;
    }
    return pl->insert(cut, entry) == 0;
}

// A file's clock and frame shape as a timeline profile.
void profileFromProbe(edit::Profile &profile, const media::Probe &probe) {
    profile.set_frame_rate(probe.fpsNum, probe.fpsDen);
    profile.set_width(probe.width);
    profile.set_height(probe.height);
    profile.set_sample_aspect(probe.sarNum, probe.sarDen);
    int n = probe.width * probe.sarNum, m = probe.height * probe.sarDen;
    const int divisor = std::max(1, std::gcd(n, m));
    profile.set_display_aspect(n / divisor, m / divisor);
    profile.set_progressive(probe.progressive ? 1 : 0);
    profile.set_colorspace(probe.colourspace > 0 ? probe.colourspace : 709);
}

// The picture pipeline order of a cut's attribute filters. Every setter
// detaches and re-attaches its own filter, so without this the order was
// the order of EDITING — crop after flip mirrored the band, and a reopen
// (which applies in a fixed order) showed a different picture. Setters
// call this after attaching; restore and paste go through the setters.
int attributeRank(edit::Filter &f) {
    static const char *const order[] = {
        "omavid:stab", "omavid:lens", "omavid:flip", "omavid:freeze", "omavid:color",
        "omavid:gradeFx", "omavid:grade", "omavid:effect", "omavid:crop", "omavid:rect", "omavid:opacity", "omavid:opacityKf",
        "omavid:blend", "omavid:fade", "omavid:volume", "omavid:volumeKf",
        "omavid:pan", "omavid:eq", "omavid:audioMap", "omavid:normalize"};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (f.get(order[i]))
            return int(i);
    return 100; // markers, labels, links and the like: order is moot
}

void reorderAttributeFilters(edit::Producer *cut) {
    if (!cut || cut->filter_count() < 2)
        return;
    QStringList order;
    for (int i=0;i<cut->filter_count();++i) { std::unique_ptr<edit::Filter> f(cut->filter(i));
        if(f && f->get("omavid:effectOrder")) order=QJsonDocument::fromJson(f->get("omavid:effectOrder")).toVariant().toStringList(); }
    std::vector<std::pair<int, std::unique_ptr<edit::Filter>>> filters;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid()) {
            const int custom = order.indexOf(f->get("omavid:effect") ? QStringLiteral("effects") : bypassKeyOf(*f));
            const int rank = custom >= 0 ? custom : order.size() + attributeRank(*f);
            filters.emplace_back(rank, std::move(f));
        }
    }
    bool sorted = true;
    for (size_t i = 1; i < filters.size() && sorted; ++i)
        sorted = filters[i - 1].first <= filters[i].first;
    if (sorted)
        return;
    std::stable_sort(filters.begin(), filters.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    // The wrappers hold a reference each, so a detached filter survives
    // until it is attached again.
    for (auto &pair : filters)
        cut->detach(*pair.second);
    for (auto &pair : filters)
        cut->attach(*pair.second);
}

// Effective fade lengths on a cut: only filters still anchored to the
// clip's current edges count — a stale anchor (left by a blade or an edge
// change) is inert and reported as no fade.
void fadeLengths(edit::Producer *cut, int in, int out, int *fadeIn, int *fadeOut) {
    *fadeIn = 0;
    *fadeOut = 0;
    if (!cut)
        return;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->is_valid())
            continue;
        const char *marker = f->get("omavid:fade");
        if (!marker)
            continue;
        if (qstrcmp(marker, "in") == 0 && f->get_in() == in)
            *fadeIn = qMax(*fadeIn, f->get_out() - f->get_in() + 1);
        else if (qstrcmp(marker, "out") == 0 && f->get_out() == out)
            *fadeOut = qMax(*fadeOut, f->get_out() - f->get_in() + 1);
    }
}

// A connected clip's PiP rect, parsed off its affine filter's marker.
// Returns false when the clip is full-frame (no filter).
bool connectedRect(edit::Producer *cut, double *x, double *y, double *w,
                   double *h, double *rotation, double *anchorX,
                   double *anchorY) {
    if (anchorX)
        *anchorX = 0.5;
    if (anchorY)
        *anchorY = 0.5;
    if (!cut)
        return false;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->is_valid())
            continue;
        const char *marker = f->get("omavid:rect");
        if (!marker || !*marker)
            continue;
        const QStringList parts =
            QString::fromUtf8(marker).split(QLatin1Char(' '));
        // Four values before rotation existed; five since. Old projects
        // read as unrotated.
        if (parts.size() < 4)
            continue;
        *x = parts.at(0).toDouble();
        *y = parts.at(1).toDouble();
        *w = parts.at(2).toDouble();
        *h = parts.at(3).toDouble();
        if (rotation)
            *rotation = parts.size() >= 5 ? parts.at(4).toDouble() : 0.0;
        // Seven since the anchor existed; older markers rotate about the centre.
        if (anchorX)
            *anchorX = parts.size() >= 7 ? parts.at(5).toDouble() : 0.5;
        if (anchorY)
            *anchorY = parts.size() >= 7 ? parts.at(6).toDouble() : 0.5;
        return true;
    }
    return false;
}

// A forward timewarp asks its source for the frame nearest t·rate, so the
// last frames of a slowed clip round to one PAST the source's end. Played
// through, the decoder just holds the last frame it had; a cold seek there
// (a single-frame render, a jump in the viewer) decoded a stray frame,
// often the file's first. Hold the last real frame on purpose from there.
// Reversed clips and sources without a video rate are left alone.
void holdRetimedTail(edit::Profile &profile, edit::Producer &producer, double rate) {
    const int length = producer.get_length();
    const int video = producer.get_int("video_index");
    const double sourceFps = producer.get_double(
        QStringLiteral("meta.media.%1.stream.frame_rate").arg(video).toUtf8().constData());
    if (rate <= 0.0 || video < 0 || length < 2 || sourceFps <= 0.0 || profile.fps() <= 0.0)
        return;
    // Half a source frame, counted in timewarp frames.
    const double margin = 0.5 * profile.fps() / (rate * sourceFps);
    const int last = int(std::ceil(length - margin)) - 1;
    if (last < 0 || last >= length - 1)
        return; // every frame lands inside the source
    edit::Filter hold(profile, "freeze");
    if (!hold.is_valid())
        return;
    hold.set("frame", last);
    hold.set("freeze_before", 0);
    hold.set("freeze_after", 1);
    hold.set("omavid:retimeTail", last);
    producer.attach(hold);
}

// A record on `target`: a service's name, its configuration, a marker.
void attachRecord(edit::Service &target, const char *service, const QVariantMap &config, const char *marker) {
    edit::Filter f(service);
    f.set("config", QJsonDocument::fromVariant(config).toJson(QJsonDocument::Compact).constData());
    f.set(marker, 1);
    target.attach(f);
}

// Shared by insertOn/overwriteOn: the bounded cut for a new track clip on
// track `track` (created when track == trackCount()), or null when the
// path is bad, the track is locked, or the kinds disagree.
std::unique_ptr<edit::Producer> boundedCut(edit::Producer &producer,
                                                 int in, int out) {
    if (producer.get("omavid:retime") && (in < -1 || (in >= 0 && (out < in || out >= producer.get_length()))))
        return nullptr;
    std::unique_ptr<edit::Producer> cut(
        producer.cut(in >= 0 ? in : 0, out >= 0 ? out : -1));
    if (!cut || !cut->is_valid())
        return nullptr;
    return cut;
}

// Keyframes either side of a blade `relative` frames into the clip: the
// left keeps what is before the cut (plus the value AT the cut, so its
// ramp ends where it did), the right takes the rest re-based to 0.
void splitKeys(const QVariantList &keys, const char *valueName, int relative,
                      QVariantList *left, QVariantList *right) {
    if (qstrcmp(valueName, "alpha") == 0) {
        *left = keys;
        *right = EditEngine::shiftedCurve(keys, relative);
        return;
    }
    const QString vk = QString::fromLatin1(valueName);
    auto valueAt = [&](int frame) {
        QVariantMap before, after;
        for (const QVariant &v : keys) {
            const QVariantMap k = v.toMap();
            const int f = k.value(QStringLiteral("frame")).toInt();
            if (f <= frame)
                before = k;
            else if (after.isEmpty())
                after = k;
        }
        if (before.isEmpty())
            return after.value(vk).toDouble();
        if (after.isEmpty())
            return before.value(vk).toDouble();
        const int f0 = before.value(QStringLiteral("frame")).toInt();
        const int f1 = after.value(QStringLiteral("frame")).toInt();
        const double v0 = before.value(vk).toDouble(), v1 = after.value(vk).toDouble();
        return f1 == f0 ? v0 : v0 + (v1 - v0) * double(frame - f0) / double(f1 - f0);
    };
    bool leftHasEnd = false, rightHasStart = false;
    for (const QVariant &v : keys) {
        QVariantMap k = v.toMap();
        const int f = k.value(QStringLiteral("frame")).toInt();
        if (f < relative) {
            left->append(k);
            leftHasEnd = leftHasEnd || f == relative - 1;
        } else {
            k.insert(QStringLiteral("frame"), f - relative);
            right->append(k);
            rightHasStart = rightHasStart || f == relative;
        }
    }
    if (!left->isEmpty() && !right->isEmpty()) {
        if (!leftHasEnd) {
            QVariantMap k{{QStringLiteral("frame"), relative - 1}, {vk, valueAt(relative - 1)}};
            left->append(k);
        }
        if (!rightHasStart) {
            QVariantMap k{{QStringLiteral("frame"), 0}, {vk, valueAt(relative)}};
            right->prepend(k);
        }
    }
}

// The shared volume-filter mechanics: one static "volume" filter on the
// given cut, marked omavid:volume. The filter's "level" IS in dB (verified
// 7.40 — see applyFades). 0 dB just removes the filter; -60 dB is Silence.
// Cut filters ride project XML; restore() re-applies from the entries'
// "volume" field like it does fades.
// Volume keyframes (guide p. 271): an ANIMATED volume filter anchored to
// the cut like a fade — anim keys are clip-relative frames, values in dB
// (the r30 fact). The keyframe list rides the filter as omavid:volumeKf
// JSON [[frame,db]...], so entries/restore/XML carry it; a static volume
// or a fresh keyframe set replaces whichever was there.
bool applyVolumeKf(edit::Profile &profile, edit::Producer *cut,
                          const QVariantList &kf, int cutIn, int cutOut) {
    // The static level rides the keyed filter too (omavid:volume), so the
    // Inspector still shows it, a new key seeds from the clip, and Clear
    // puts it back rather than 0 dB.
    double staticDb = 0.0;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid()
            && (f->get("omavid:volume") || f->get("omavid:volumeKf"))) {
            if (f->get("omavid:volume"))
                staticDb = f->get_double("omavid:volume");
            cut->detach(*f);
        }
    }
    if (kf.isEmpty()) // cleared: back to the static level
        return staticDb == 0.0 || applyVolumeDb(profile, cut, staticDb);
    QList<QPair<int, double>> keys;
    const int span = cutOut - cutIn;
    for (const QVariant &v : kf) {
        const QVariantMap m = v.toMap();
        keys.append({qBound(0, m.value(QStringLiteral("frame")).toInt(), span),
                     qBound(-60.0,
                            m.value(QStringLiteral("db")).toDouble(), 12.0)});
    }
    std::sort(keys.begin(), keys.end());
    QString anim;
    QVariantList stored;
    for (const auto &k : keys) {
        if (!anim.isEmpty())
            anim += QLatin1Char(';');
        anim += QStringLiteral("%1=%2").arg(QString::number(k.first),
                                            QString::number(k.second));
        // ⚠️ append(QVariantList) FLATTENS (the round-42 gotcha) — wrap.
        stored.append(QVariant(QVariantList{k.first, k.second}));
    }
    edit::Filter vol(profile, "volume");
    if (!vol.is_valid())
        return false;
    vol.set("level", anim.toUtf8().constData());
    vol.set_in_and_out(cutIn, cutOut);
    vol.set("omavid:volume", staticDb);
    vol.set("omavid:volumeKf",
            QJsonDocument::fromVariant(stored)
                .toJson(QJsonDocument::Compact).constData());
    cut->attach(vol);
    reorderAttributeFilters(cut);
    return true;
}

// The keyframe list off a cut's animated volume filter ({frame, db} maps).
QVariantList volumeKfOf(edit::Producer *cut) {
    QVariantList result;
    if (!cut)
        return result;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        const char *json =
            f && f->is_valid() ? f->get("omavid:volumeKf") : nullptr;
        if (!json || !*json)
            continue;
        const QVariantList pairs =
            QJsonDocument::fromJson(json).toVariant().toList();
        for (const QVariant &p : pairs) {
            const QVariantList pair = p.toList();
            if (pair.size() != 2)
                continue;
            QVariantMap key;
            key.insert(QStringLiteral("frame"), pair.at(0).toInt());
            key.insert(QStringLiteral("db"), pair.at(1).toDouble());
            result.append(key);
        }
        break;
    }
    return result;
}

// Clip opacity keeps its original control points outside trimmed bounds. The
// custom filter shares interpolation with the managed renderer. Native data
// accepts legacy [frame,alpha] pairs and an optional third interpolation field.
bool applyOpacityKf(edit::Profile &profile, edit::Producer *cut,
                           const QVariantList &kf, int cutIn, int cutOut) {
    QVariantList keys;
    if (!EditEngine::normalizeOpacityKeys(kf, &keys)) return false;
    edit::Filter dim(profile, "omaedit.opacity");
    if (!dim.is_valid()) return false;
    double staticAlpha = 1.0; // kept on the keyed filter, like volume
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid()
            && (f->get("omavid:opacity") || f->get("omavid:opacityKf"))) {
            if (f->get("omavid:opacity"))
                staticAlpha = f->get_double("omavid:opacity");
            cut->detach(*f);
        }
    }
    if (kf.isEmpty())
        return staticAlpha >= 1.0 || applyOpacityValue(profile, cut, staticAlpha);
    QVariantList stored;
    for (const auto &v : keys) {
        const auto k = v.toMap();
        QVariantList pair{k.value("frame"), k.value("alpha")};
        if (k.contains("interpolation")) pair << k.value("interpolation");
        stored.append(QVariant(pair));
    }
    dim.set_in_and_out(cutIn, cutOut);
    dim.set("omavid:opacity", staticAlpha);
    dim.set("omavid:opacityKf",
            QJsonDocument::fromVariant(stored)
                .toJson(QJsonDocument::Compact).constData());
    cut->attach(dim);
    reorderAttributeFilters(cut);
    return true;
}

QVariantList opacityKfOf(edit::Producer *cut) {
    QVariantList result;
    if (!cut)
        return result;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        const char *json =
            f && f->is_valid() ? f->get("omavid:opacityKf") : nullptr;
        if (!json || !*json)
            continue;
        const QVariantList pairs =
            QJsonDocument::fromJson(json).toVariant().toList();
        for (const QVariant &p : pairs) {
            const QVariantList pair = p.toList();
            if (pair.size() != 2 && pair.size() != 3)
                continue;
            QVariantMap key;
            key.insert(QStringLiteral("frame"), pair.at(0).toInt());
            key.insert(QStringLiteral("alpha"), pair.at(1).toDouble());
            if (pair.size() == 3) key.insert(QStringLiteral("interpolation"), pair.at(2).toString());
            result.append(key);
        }
        break;
    }
    return result;
}

bool applyOpacityValue(edit::Profile &profile, edit::Producer *cut,
                              double alpha) {
    alpha = qBound(0.0, alpha, 1.0);
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid()
            && (f->get("omavid:opacity") || f->get("omavid:opacityKf")))
            cut->detach(*f);
    }
    if (alpha < 1.0) {
        edit::Filter dim(profile, "brightness");
        if (!dim.is_valid())
            return false;
        dim.set("alpha", alpha);
        dim.set("omavid:opacity", alpha);
        cut->attach(dim);
        reorderAttributeFilters(cut);
    }
    return true;
}

bool applyVolumeDb(edit::Profile &profile, edit::Producer *cut,
                          double db) {
    db = qBound(-60.0, db, 12.0);
    // A static volume replaces keyframes too — an explicit set clears them.
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid()
            && (f->get("omavid:volume") || f->get("omavid:volumeKf")))
            cut->detach(*f);
    }
    if (db != 0.0) {
        edit::Filter vol(profile, "volume");
        if (!vol.is_valid())
            return false;
        vol.set("level", db);
        vol.set("omavid:volume", db);
        cut->attach(vol);
    }
    reorderAttributeFilters(cut);
    return true;
}

// Stereo pan via the panner: channel -1 = front balance, and the
// "start"
// property maps 0 = full left, 0.5 = centre, 1 = full right. Our pan is
// -1..+1 around centre; 0 removes the filter. Marked omavid:pan, so entries
// report it, restore() re-applies it and project XML keeps it.
bool applyPanValue(edit::Profile &profile, edit::Producer *cut,
                          double pan) {
    pan = qBound(-1.0, pan, 1.0);
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:pan"))
            cut->detach(*f);
    }
    if (pan != 0.0) {
        edit::Filter panner(profile, "panner");
        if (!panner.is_valid())
            return false;
        panner.set("channel", -1);
        panner.set("start", (pan + 1.0) / 2.0);
        panner.set("omavid:pan", pan);
        cut->attach(panner);
    }
    reorderAttributeFilters(cut);
    return true;
}

// EQ presets. Each preset is one or two av* filters on the
// cut, every one marked omavid:eq=<preset> so removal and entries() see
// them as a unit. Verified 7.40 (scratchpad eqprobe2): cut-attached
// avfilter.bass/treble honour av.g, bass moves a 100 Hz tone both ways
// and treble leaves it alone. Gains stay modest — a +12 shelf clips.
bool applyEqPreset(edit::Profile &profile, edit::Producer *cut,
                          const QString &preset) {
    // Reject an unknown name BEFORE stripping the current preset.
    static const QStringList known = {
        QStringLiteral("bass-boost"),   QStringLiteral("bass-reduce"),
        QStringLiteral("treble-boost"), QStringLiteral("treble-reduce"),
        QStringLiteral("voice"),        QStringLiteral("hum")};
    if (!preset.isEmpty() && !known.contains(preset))
        return false;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:eq"))
            cut->detach(*f);
    }
    if (preset.isEmpty())
        return true;

    auto attach = [&](const char *id,
                      std::initializer_list<std::pair<const char *, double>>
                          params) {
        edit::Filter f(profile, id);
        if (!f.is_valid())
            return false;
        for (const auto &p : params)
            f.set(p.first, p.second);
        f.set("omavid:eq", preset.toUtf8().constData());
        cut->attach(f);
        reorderAttributeFilters(cut);
        return true;
    };

    if (preset == QStringLiteral("bass-boost"))
        return attach("avfilter.bass", {{"av.g", 6.0}});
    if (preset == QStringLiteral("bass-reduce"))
        return attach("avfilter.bass", {{"av.g", -6.0}});
    if (preset == QStringLiteral("treble-boost"))
        return attach("avfilter.treble", {{"av.g", 6.0}});
    if (preset == QStringLiteral("treble-reduce"))
        return attach("avfilter.treble", {{"av.g", -6.0}});
    if (preset == QStringLiteral("voice")) // presence lift + mud cut
        return attach("avfilter.equalizer",
                      {{"av.f", 2500.0}, {"av.w", 1000.0}, {"av.g", 4.0}})
               && attach("avfilter.bass", {{"av.g", -2.0}});
    if (preset == QStringLiteral("hum")) // mains hum notch
        return attach("avfilter.equalizer",
                      {{"av.f", 50.0}, {"av.w", 20.0}, {"av.g", -24.0}});
    return false; // unknown preset name
}
bool sameSourceOrigin(const QVariantMap &a, const QVariantMap &b) {
    if (a.contains(ClipKey::sourceOrigin) != b.contains(ClipKey::sourceOrigin)) return false;
    return !a.contains(ClipKey::sourceOrigin)
           || std::abs(a.value(ClipKey::sourceOrigin).toDouble() - b.value(ClipKey::sourceOrigin).toDouble()) < 1e-9;
}
QVariantMap multicamDefinition(const QVariantMap &entry) {
    return QJsonDocument::fromJson(entry.value(ClipKey::mcam).toString().toUtf8()).toVariant().toMap();
}
bool multicamParentTiming(const QVariantMap &entry,const QVariantMap &camera,double fps,RetimeMap *timing) {
    const auto angles=camera.value("angles").toList();
    const int shown=entry.value(ClipKey::mcamAngle,-1).toInt();
    if(camera.value("version").toInt()!=2 || shown<0 || shown>=angles.size()
        || !std::isfinite(fps) || fps<=0)return false;
    auto angle=angles[shown].toMap();
    if(entry.contains(ClipKey::sourceMeta))angle=MulticamTiming::withMetadata(angle,entry.value(ClipKey::sourceMeta).toMap());
    const double native=MulticamTiming::nativeRate(angle),effective=MulticamTiming::effectiveRate(angle);
    const int count=MulticamTiming::nativeFrames(angle);
    if(!std::isfinite(native) || native<=0 || !std::isfinite(effective) || effective<=0 || count<1)return false;
    std::optional<double> origin;if(!SourceTiming::readOrigin(entry,origin))return false;
    const auto definition=entry.contains(ClipKey::retime)?entry.value(ClipKey::retime).toMap()
        :RetimeMap::conform(count,native,fps,entry.value(ClipKey::speed,1.).toDouble(),effective/native,origin);
    return timing->set(definition);
}
MulticamPoint multicamPoint(const QVariantMap &entry, const QVariantMap &camera,
                            int relativeFrame, double fps) {
    if(relativeFrame<0)return {};
    if(Timeline::angleWithoutFootage(entry)) {
        // A stretch its angle has no footage for counts the multicam clip's
        // own frames (and part of one, where its clock is not the timeline's).
        std::optional<double> origin;
        if(camera.value("version").toInt()!=2 || !std::isfinite(fps) || fps<=0 || !SourceTiming::readOrigin(entry,origin))return {};
        const double frame=entry.value(ClipKey::in).toLongLong()+relativeFrame+origin.value_or(0.)*fps;
        return {true,frame/fps,false,frame,0.};
    }
    RetimeMap timing;if(!multicamParentTiming(entry,camera,fps,&timing))return {};
    const auto angle=camera.value("angles").toList()[entry.value(ClipKey::mcamAngle).toInt()].toMap();
    const qint64 frame=entry.contains(ClipKey::freeze)?entry.value(ClipKey::freeze).toLongLong()
        :entry.value(ClipKey::in).toLongLong()+relativeFrame;
    if(frame<0 || frame>=timing.frames())return {};
    const auto point=timing.sample(int(frame));
    const double seconds=MulticamTiming::offsetSeconds(angle)+point.source/fps;
    if(!point.valid || !std::isfinite(seconds))return {};
    return {true,seconds,point.nextSource<point.source || point.holdBefore,point.source,MulticamTiming::offsetSeconds(angle)};
}
QVariantMap heldMulticamTiming(const QVariantMap &entry,const QVariantMap &camera,double fps,
                              int outputFrame,int frames) {
    RetimeMap parent;if(!multicamParentTiming(entry,camera,fps,&parent))return {};
    const auto point=parent.sample(outputFrame);if(!point.valid)return {};
    const bool backwards=point.nextSource<point.source || point.holdBefore;
    if((backwards && point.source<=1e-9) || (!backwards && point.source>=parent.sourceLength()-1e-9))return {};
    const QVariantMap segment{{"frames",frames},{"speed",0.},{"holdBefore",backwards}};
    RetimeMap hold;
    if(!hold.set({{"sourceIn",point.source},{"sourceLength",parent.sourceLength()},{"segments",QVariantList{segment}}}))return {};
    return hold.definition();
}
