#include "timeline_p.h"

static double opacityOf(edit::Producer *cut);

namespace {

// The clip's crop as fractions trimmed off each edge (0 = untouched).
bool clipCrop(edit::Producer *cut, double *l, double *r, double *t, double *b,
              double *feather = nullptr, bool *invert = nullptr) {
    *l = 0.0;
    *r = 0.0;
    *t = 0.0;
    *b = 0.0;
    if (feather)
        *feather = 0.0;
    if (invert)
        *invert = false;
    if (!cut)
        return false;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->is_valid())
            continue;
        const char *marker = f->get("omavid:crop");
        if (!marker || !*marker)
            continue;
        const QStringList parts =
            QString::fromUtf8(marker).split(QLatin1Char(' '));
        // Four values before feather/invert existed; six since.
        if (parts.size() != 4 && parts.size() != 6)
            continue;
        *l = parts.at(0).toDouble();
        *r = parts.at(1).toDouble();
        *t = parts.at(2).toDouble();
        *b = parts.at(3).toDouble();
        if (feather)
            *feather = parts.size() >= 6 ? parts.at(4).toDouble() : 0.0;
        if (invert)
            *invert = parts.size() >= 6 && parts.at(5).toInt() != 0;
        return true;
    }
    return false;
}

// The clip's colour correction, parsed off its eq filter's marker.
void clipColor(edit::Producer *cut, double *b, double *c, double *s) {
    *b = 0.0;
    *c = 1.0;
    *s = 1.0;
    if (!cut)
        return;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (!f || !f->is_valid())
            continue;
        const char *marker = f->get("omavid:color");
        if (!marker || !*marker)
            continue;
        const QStringList parts =
            QString::fromUtf8(marker).split(QLatin1Char(' '));
        if (parts.size() != 3)
            continue;
        *b = parts.at(0).toDouble();
        *c = parts.at(1).toDouble();
        *s = parts.at(2).toDouble();
        return;
    }
}

// The clip's static volume adjustment in dB (0 = untouched).
double clipVolume(edit::Producer *cut) {
    if (!cut)
        return 0.0;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:volume"))
            return f->get_double("omavid:volume"); // "level" is an animation on a keyed clip
    }
    return 0.0;
}

// The clip's stereo pan, off its panner filter's marker (0 = centred).
double clipPanOf(edit::Producer *cut) {
    if (!cut)
        return 0.0;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:pan"))
            return f->get_double("omavid:pan");
    }
    return 0.0;
}

// One part of a clip's snapshot entry each, read from its cut. appendClipEntries
// calls them in this order; a later one may overrule an earlier (an adjustment
// clip renames itself last).
// Fades, keyframes, level, opacity, pan and EQ.
static void readLevels(edit::ClipInfo *info, ClipEntry &entry) {
    int fadeIn = 0, fadeOut = 0;
    fadeLengths(info->cut, info->frame_in, info->frame_out,
                &fadeIn, &fadeOut);
    entry.fadeIn = fadeIn;
    entry.fadeOut = fadeOut;
    for(int f=0;info->cut && f<info->cut->filter_count();++f){std::unique_ptr<edit::Filter> filter(info->cut->filter(f));
        if(filter && filter->get("omavid:audioCurve"))entry.audioCurve = QString::fromUtf8(filter->get("omavid:audioCurve"));}

    QVariantMap editKeys;
    for(int i=0;info->cut && i<info->cut->filter_count();++i){std::unique_ptr<edit::Filter> f(info->cut->filter(i));if(!f)continue;
        if(f->get("omavid:editKeys")){const auto keys=QJsonDocument::fromJson(f->get("omavid:editKeys")).toVariant().toMap();for(auto it=keys.begin();it!=keys.end();++it)editKeys[it.key()]=it.value();}
        if(f->get("omavid:effectOrder"))entry.effectOrder = QJsonDocument::fromJson(f->get("omavid:effectOrder")).toVariant().toStringList();
    }
    entry.editKeys = editKeys;
    entry.volume = clipVolume(info->cut);
    entry.volumeKf = volumeKfOf(info->cut);
    entry.opacity = opacityOf(info->cut);
    entry.opacityKf = opacityKfOf(info->cut);
    entry.pan = clipPanOf(info->cut);
    {
        QString eq;
        if (info->cut) {
            for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
                std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
                const char *marker =
                    f && f->is_valid() ? f->get("omavid:eq") : nullptr;
                if (marker && *marker) {
                    eq = QString::fromUtf8(marker);
                    break;
                }
            }
        }
        entry.eq = std::move(eq);
    }
}

// What the clip is marked as: gap, disabled, label, scaling, category, link, markers, takes, the original of a render.
static void readTags(edit::ClipInfo *info, ClipEntry &entry) {
    entry.gap = info->producer
                     && info->producer->get_int("omavid:gap") != 0;
    bool disabled = false;
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count() && !disabled; ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            disabled = f && f->is_valid() && f->get("omavid:disabled");
        }
    }
    entry.disabled = disabled;
    QString label;
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *marker =
                f && f->is_valid() ? f->get("omavid:label") : nullptr;
            if (marker && *marker) {
                label = QString::fromUtf8(marker);
                break;
            }
        }
    }
    entry.label = label;
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *scaling = f && f->is_valid() ? f->get("omavid:scaling") : nullptr;
            if (scaling && render::isScalingKey(QString::fromUtf8(scaling))) {
                entry.scaling = QString::fromUtf8(scaling);
                break;
            }
        }
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *category = f && f->is_valid() ? f->get("omavid:category") : nullptr;
            if (category) {
                entry.category = QString::fromUtf8(category);
                break;
            }
        }
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *link =
                f && f->is_valid() ? f->get("omavid:link") : nullptr;
            if (link && *link) {
                entry.link = QString::fromUtf8(link);
                break;
            }
        }
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *json =
                f && f->is_valid() ? f->get("omavid:markers") : nullptr;
            if (json && *json) {
                entry.markers = QJsonDocument::fromJson(json).array().toVariantList();
                break;
            }
        }
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *json =
                f && f->is_valid() ? f->get("omavid:original") : nullptr;
            if (json && *json) {
                entry.original = QJsonDocument::fromJson(json).object().toVariantMap();
                entry.rendered = true;
                break;
            }
        }
    }
}

// Bypassed attributes, multicam angle, flips, freeze, channel map, blend, lens and stabiliser.
static void readLooks(edit::ClipInfo *info, ClipEntry &entry) {
    if (info->cut) {
        QStringList bypass;
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            if (!f || !f->is_valid() || !f->get_int("disable"))
                continue;
            const QString key = bypassKeyOf(*f);
            if (!key.isEmpty() && !bypass.contains(key))
                bypass.append(key);
        }
        if (!bypass.isEmpty())
            entry.bypass = bypass;
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *mcam =
                f && f->is_valid() ? f->get("omavid:mcam") : nullptr;
            if (mcam && *mcam) {
                entry.mcam = QString::fromUtf8(mcam);
                entry.mcamAngle = f->get_int("omavid:mcamAngle");
                break;
            }
        }
        bool flipH = false, flipV = false;
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *flip = f && f->is_valid() ? f->get("omavid:flip") : nullptr;
            if (flip && *flip == 'h')
                flipH = true;
            else if (flip && *flip == 'v')
                flipV = true;
            const char *freeze = f && f->is_valid() ? f->get("omavid:freeze") : nullptr;
            if (freeze && *freeze)
                entry.freeze = f->get_int("omavid:freeze");
            const char *amap = f && f->is_valid() ? f->get("omavid:audioMap") : nullptr;
            if (amap && *amap)
                entry.audioMap = QString::fromUtf8(amap);
            if (f && f->is_valid() && f->get("omavid:normalize"))
                entry.normalize = true;
        }
        if (flipH)
            entry.flipH = true;
        if (flipV)
            entry.flipV = true;
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            if (!f || !f->is_valid())
                continue;
            if (f->get("omavid:blend"))
                entry.blend = f->get_int("omavid:blend");
            if (f->get("omavid:lens")) {
                entry.lensK1 = f->get_double("av.k1");
                entry.lensK2 = f->get_double("av.k2");
                entry.lensCx = f->get_double("av.cx");
                entry.lensCy = f->get_double("av.cy");
            }
            if (f->get("omavid:stab")) {
                QVariantMap stab;
                const QString results = QString::fromUtf8(f->get("results"));
                stab.insert(QStringLiteral("results"), results);
                stab.insert(QStringLiteral("shakiness"), f->get_int("shakiness"));
                stab.insert(QStringLiteral("smoothing"), f->get_int("smoothing"));
                stab.insert(QStringLiteral("missing"),
                            results.isEmpty() || !QFileInfo::exists(results));
                entry.stab = stab;
            }
        }
    }
}

// Video effects, and how its dissolve is styled and aligned.
static void readTransition(edit::ClipInfo *info, ClipEntry &entry) {
    entry.videoEffects = VideoEffects::read(info->cut);
    QString mixStyle, mixAlign;
    int mixExtA = 0, mixExtB = 0;
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            if (!f || !f->is_valid())
                continue;
            const char *style = f->get("omavid:mixStyle");
            if (style && *style)
                mixStyle = QString::fromUtf8(style);
            const char *align = f->get("omavid:mixAlign");
            if (align && *align) {
                mixAlign = QString::fromUtf8(align);
                mixExtA = f->get_int("omavid:mixExtA");
                mixExtB = f->get_int("omavid:mixExtB");
            }
        }
    }
    entry.mixStyle = std::move(mixStyle);
    entry.mixAlign = mixAlign;
    if (!mixAlign.isEmpty()) {
        entry.mixExtA = mixExtA;
        entry.mixExtB = mixExtB;
    }
}

// The simple colour controls and the Color page's grade.
static void readColour(edit::ClipInfo *info, ClipEntry &entry) {
    double colorB = 0, colorC = 1, colorS = 1;
    clipColor(info->cut, &colorB, &colorC, &colorS);
    entry.colorB = colorB;
    entry.colorC = colorC;
    entry.colorS = colorS;
    if (info->cut) { // the Color page's grade, its versions and the active one
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            if (!f || !f->is_valid() || !f->get("omavid:grade"))
                continue;
            const char *json = f->get("omavid:gradeJson");
            const QVariantMap g = GradeEngine::pinMaskModels(json && *json ? QJsonDocument::fromJson(json).object().toVariantMap() : QVariantMap());
            if (!g.isEmpty())
                entry.grade = g;
            const char *versions = f->get("omavid:gradeVersions");
            if (versions && *versions)
                entry.gradeVersions = GradeEngine::pinVersionMaskModels(QJsonDocument::fromJson(versions).array().toVariantList());
            entry.gradeVersion = f->get_int("omavid:gradeVersion");
            break;
        }
    }
}

// Speed, position and size, crop, frame blending; an adjustment clip.
static void readGeometry(edit::ClipInfo *info, ClipEntry &entry) {
    // Speed is signed: negative plays the material backwards.
    entry.speed = info->producer
                         && info->producer->get_double("omavid:speed") != 0.0
                     ? info->producer->get_double("omavid:speed")
                     : 1.0;
    double rx = 0, ry = 0, rw = 1, rh = 1, rot = 0, ax = 0.5, ay = 0.5;
    connectedRect(info->cut, &rx, &ry, &rw, &rh, &rot, &ax, &ay);
    entry.rectX = rx;
    entry.rectY = ry;
    entry.rectW = rw;
    entry.rectH = rh;
    entry.rotation = rot;
    entry.anchorX = ax;
    entry.anchorY = ay;
    double cl = 0, cr = 0, ct = 0, cb = 0, cf = 0;
    bool ci = false;
    clipCrop(info->cut, &cl, &cr, &ct, &cb, &cf, &ci);
    entry.cropL = cl;
    entry.cropR = cr;
    entry.cropT = ct;
    entry.cropB = cb;
    entry.cropFeather = cf;
    entry.cropInvert = ci;
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            const char *mode =
                f && f->is_valid() ? f->get("omavid:frameBlend") : nullptr;
            if (mode && *mode) {
                entry.frameBlend = QString::fromUtf8(mode).toInt();
                break;
            }
        }
    }
    if (info->cut) {
        for (int fi = 0; fi < info->cut->filter_count(); ++fi) {
            std::unique_ptr<edit::Filter> f(info->cut->filter(fi));
            if (f && f->is_valid() && f->get_int("omavid:adjust")) {
                entry.adjust = true;
                entry.generator = QStringLiteral("adjust");
                entry.name = QStringLiteral("Effect Layer");
                break;
            }
        }
    }
}

// Where the clip's material is: the path it was placed with, else what the
// producer says, with one of our solid colours named as "color:<value>".
static QString clipPathOf(edit::ClipInfo *info, const char *kept) {
    QString path = kept && *kept
        ? QString::fromUtf8(kept)
        : QString::fromUtf8(info->resource ? info->resource : "");
    if ((!kept || !*kept) && info->producer) {
        // A colour producer: one of our solids. (The
        // document loader may have prefixed its colour with the folder.)
        const QString service = QString::fromUtf8(info->producer->get("service"));
        if (service == QStringLiteral("color") || service == QStringLiteral("colour")) {
            const char *colour = info->producer->get("colour");
            const QString value = colour && *colour ? QString::fromUtf8(colour) : path.section(QLatin1Char('/'), -1);
            path = QStringLiteral("color:") + value;
        }
    }
    return path;
}

// How its source is read: sampling, origin, timing, metadata, retime; offline.
static void readSource(edit::ClipInfo *info, ClipEntry &entry) {
    if (info->producer) {
        if (info->producer->get("omavid:sampling"))
            entry.frameBlend = info->producer->get_int("omavid:sampling");
        if (info->producer->get("omavid:sourceOrigin"))
            entry.sourceOrigin = info->producer->get_double("omavid:sourceOrigin");
        if (const char *timing=info->producer->get("_omaedit.sourceTiming"))
            entry.sourceTiming = QJsonDocument::fromJson(timing).toVariant();
        if (info->producer->get_int("omavid:offline")) entry.offline = true;
        const char *sourceMeta = info->producer->get("omavid:sourceMeta");
        if (sourceMeta && *sourceMeta) entry.sourceMeta = QJsonDocument::fromJson(sourceMeta).toVariant();
        const char *retime = info->producer->get("omavid:retime");
        if (retime && *retime) entry.retime = QJsonDocument::fromJson(retime).toVariant();
    }
}

// What it is called, and what makes it a title, a gap or a generator.
static void readName(edit::ClipInfo *info, const QString &path, ClipEntry &entry) {
    const char *titleText =
        info->producer ? info->producer->get("omavid:title") : nullptr;
    if (titleText && *titleText) {
        entry.title = QString::fromUtf8(titleText);
        entry.name = QString::fromUtf8(titleText);
        const char *place = info->producer->get("omavid:titlePlace");
        const char *size = info->producer->get("omavid:titleSize");
        if(const char *typo=info->producer->get("omavid:titleTypography")){
            const auto values=QJsonDocument::fromJson(typo).toVariant().toMap();
            if(values.contains("tracking"))entry.titleTracking = values["tracking"];
            if(values.contains("lineSpacing"))entry.titleLineSpacing = values["lineSpacing"];
            if(values.contains("pixelSize"))entry.titlePixelSize = values["pixelSize"];
            if(values.contains("margin"))entry.titleMargin = values["margin"];
            if(values.contains("width"))entry.titleWidth = values["width"];
            QVariantMap look;
            for(const QString &key:EditEngine::titleLookKeys())
                if(values.contains(key))look.insert(key,values.value(key));
            if(!look.isEmpty())entry.titleLook = look;
        }
        if(const char *speaker=info->producer->get("omavid:speaker"); speaker && *speaker)
            entry.speaker = QString::fromUtf8(speaker);
        entry.ownStyle = info->producer->get_int("omavid:ownStyle") != 0;
        const char *font = info->producer->get("omavid:titleFont");
        const char *color = info->producer->get("omavid:titleColor");
        entry.titlePlace = place && *place ? QString::fromUtf8(place)
                                     : QStringLiteral("centre");
        entry.titleSize = size && *size ? QString::fromUtf8(size)
                                   : QStringLiteral("medium");
        entry.titleFont = font && *font ? QString::fromUtf8(font) : QString();
        entry.titleColor = color && *color ? QString::fromUtf8(color)
                                     : QString();
        const char *align = info->producer->get("omavid:titleAlign");
        const char *bg = info->producer->get("omavid:titleBg");
        entry.titleAlign = align && *align ? QString::fromUtf8(align) : QStringLiteral("centre");
        entry.titleWeight = info->producer->get("omavid:titleWeight") ? QStringLiteral("bold") : QString();
        entry.titleItalic = info->producer->get_int("omavid:titleItalic") != 0;
        entry.titleBg = bg && *bg ? QString::fromUtf8(bg) : QString();
        entry.titleOutline = !info->producer->get("omavid:titleOutline")
                         || info->producer->get_int("omavid:titleOutline") != 0;
        entry.caption = info->producer->get_int("omavid:caption") != 0;
    } else if (info->producer
               && info->producer->get_int("omavid:gap") != 0) {
        entry.name = QStringLiteral("Gap");
    } else if (!Timeline::generatorKindOf(path).isEmpty()) {
        const QString kind = Timeline::generatorKindOf(path);
        entry.generator = kind;
        entry.name = kind == QStringLiteral("bars") ? QStringLiteral("Colour Bars")
                     : kind == QStringLiteral("noise") ? QStringLiteral("Noise")
                     : kind == QStringLiteral("count") ? QStringLiteral("Counting Leader")
                                                        : QStringLiteral("Solid Colour");
        if (kind == QStringLiteral("solid"))
            entry.generatorColor = path.mid(6);
    } else if (path == Timeline::noFootagePath()) {
        entry.name = QStringLiteral("No footage");
    } else {
        entry.name = QFileInfo(path).fileName();
    }
}

// The clips a compound was made from, as saved JSON ("" when it is not one),
// and how many there were.
static QString compoundOf(edit::ClipInfo *info, const char *kept, int *count) {
    const char *compoundJson =
        info->producer ? info->producer->get("omavid:compound") : nullptr;
    QString compoundStr = compoundJson && *compoundJson
        ? QString::fromUtf8(compoundJson) : QString();
    int compoundCount =
        info->producer ? info->producer->get_int("omavid:compoundCount")
                       : 0;
    if (compoundStr.isEmpty() && (!kept || !*kept) && info->producer && info->resource
        && qstrcmp(info->resource, "<playlist>") == 0) {
        // Deserialized compound: playlist properties were dropped by the
        // xml consumer — the JSON survives on the first inner producer.
        edit::Playlist inner(*info->producer);
        if (inner.count() > 0) {
            if (edit::ClipInfo *ci = inner.clip_info(0)) {
                const char *j =
                    ci->producer ? ci->producer->get("omavid:compoundOf")
                                 : nullptr;
                if (j && *j) {
                    compoundStr = QString::fromUtf8(j);
                    compoundCount = inner.count();
                }
                delete ci;
            }
        }
    }
    *count = compoundCount;
    return compoundStr;
}

// One playlist's clips as QVariantMaps; blanks are skipped, but their span
// still shows in each clip's absolute start. A dissolve's mix entry is
// hidden too: it reports as mixOut on the clip before it (whose visual
// span and restored out grow by the overlap) and mixIn on the clip after
// it (whose restored in shrinks) — so restore() can rebuild the originals
// and re-apply the mix.
// With `only` >= 0 just the clip with that index on the lane is read (into an
// empty list): the others are walked for their place and their dissolves but
// none of their attributes, which is nearly all of the work.
void appendClipEntries(edit::Playlist &playlist, int lane, double fps, QList<ClipEntry> *list, int only = -1) {
    int laneIndex = 0;
    bool wantedIsLast = false;  // only >= 0: the wanted clip is the one just before this item
    int pendingMixIn = 0;
    int pendingBlank = 0; // blank frames since the previous clip
    // Read in one pass: asking the playlist for each clip in turn makes it
    // recount itself every time.
    auto infos = playlist.clip_infos();
    for (int i = 0; i < int(infos.size()); ++i) {
        if (playlist.is_blank(i)) {
            pendingBlank += infos[size_t(i)] ? infos[size_t(i)]->frame_count : playlist.clip_length(i);
            pendingMixIn = 0; // a dissolve never reaches across empty space
            continue;
        }
        edit::ClipInfo *info = infos[size_t(i)].release();
        if (!info)
            continue;
        const char *kept = info->producer ? info->producer->get("omavid:path") : nullptr;
        // A live dissolve region answers is_mix; a deserialized one loses
        // the mix link and comes back as a plain entry whose producer is an
        // anonymous "<tractor>" — restore()
        // re-applies it as a real mix either way.
        const bool isMixEntry =
            playlist.is_mix(i)
            || ((!kept || !*kept) && info->resource
                && qstrcmp(info->resource, "<tractor>") == 0);
        if (isMixEntry) {
            const int overlap = info->frame_count;
            if (!list->isEmpty() && list->last().lane == lane && (only < 0 || wantedIsLast)) {
                ClipEntry &prev = list->last();
                prev.mixOut = overlap;
                prev.frames += overlap;
                prev.out += overlap;
            }
            pendingMixIn = overlap;
            delete info;
            if (wantedIsLast) return;  // its dissolve out was the last thing to know
            continue;
        }
        if (only >= 0 && laneIndex != only) {
            delete info;
            if (wantedIsLast) return;  // past the wanted clip
            ++laneIndex;
            pendingBlank = 0;
            pendingMixIn = 0;
            continue;
        }
        const QString path = clipPathOf(info, kept);
        ClipEntry entry;
        readSource(info, entry);
        readName(info, path, entry);
        int compoundCount = 0;
        const QString compoundStr = compoundOf(info, kept, &compoundCount);
        if (!compoundStr.isEmpty()) {
            entry.compound = true;
            entry.compoundJson = compoundStr;
            entry.name = compoundCount > 0
                             ? QStringLiteral("Nested (%1)").arg(compoundCount)
                             : QStringLiteral("Nested");
        }
        entry.path = path;
        entry.timelineFps = fps;
        if(MediaPlayer::isStillPath(path))entry.still = true;
        entry.start = info->start;
        entry.frames = info->frame_count;
        entry.in = info->frame_in - pendingMixIn;
        entry.out = info->frame_out;
        entry.length = info->length;
        entry.lane = lane;
        entry.track = lane; // the edit-page name for it
        entry.index = laneIndex++;
        // Empty track space before this clip (A3). V1's restore rebuilds
        // blanks from it; lanes place by absolute start and ignore it.
        entry.gapBefore = pendingBlank;
        pendingBlank = 0;
        entry.mixIn = pendingMixIn;
        entry.mixOut = 0;
        entry.audioOnly = info->producer
                         && info->producer->get_int("omavid:audioOnly") != 0;
        bool videoOnly = info->producer
                         && info->producer->get_int("omavid:videoOnly") != 0;
        if (!compoundStr.isEmpty()) {
            const QVariantList subs = QJsonDocument::fromJson(compoundStr.toUtf8()).toVariant().toList();
            // A compound of detached pictures has no embedded sound. Derive
            // this from its saved contents so XML reload also avoids a fake AV row.
            videoOnly = !subs.isEmpty() && std::all_of(subs.cbegin(), subs.cend(), [](const QVariant &v) {
                const QVariantMap clip = v.toMap();
                return clip.value(ClipKey::videoOnly).toBool()
                    || clip.value(ClipKey::gap).toBool()
                    || clip.value(ClipKey::path).toString().startsWith(QStringLiteral("color:"));
            });
        }
        entry.videoOnly = videoOnly;
        readLevels(info, entry);
        readTags(info, entry);
        // Recovery materializes nests under content-addressed filenames.
        // Keep the sequence identity independent of that temporary path.
        if (ProjectFile::isProject(path)) {
            entry.nest = true;
            const QString id = info->producer ? QString::fromUtf8(info->producer->get("omavid:nestId")) : QString();
            entry.nestId = id.isEmpty() ? QFileInfo(path).completeBaseName() : id;
        }
        readLooks(info, entry);
        readTransition(info, entry);
        readColour(info, entry);
        readGeometry(info, entry);
        pendingMixIn = 0;
        delete info;
        list->append(entry);
        wantedIsLast = only >= 0;
    }
}
}

static bool pictureOfSilentSource(const QVariantMap &e);

static double opacityOf(edit::Producer *cut) {
    if (!cut)
        return 1.0;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:opacity"))
            return f->get_double("omavid:opacity");
    }
    return 1.0;
}

namespace {

// A source with no sound at all: a colour, or a file whose only streams are
// pictures (and data). Asked once per path.
bool silentSource(const QString &path) {
    if (path.startsWith(QStringLiteral("color:")))
        return true;
    static QMutex lock;
    static QHash<QString, bool> known;
    {
        QMutexLocker locked(&lock);
        const auto it = known.constFind(path);
        if (it != known.constEnd())
            return it.value();
    }
    const bool silent = QFileInfo(path).isFile() && !ProjectFile::isProject(path)
                        && !media::probeCached(path).hasAudio;
    QMutexLocker locked(&lock);
    if (known.size() > 4096)
        known.clear();
    known.insert(path, silent);
    return silent;
}
}

// A picture from a source with no sound: entries() marks it silent.
static bool pictureOfSilentSource(const QVariantMap &e) {
    return !(e.value(ClipKey::audioOnly).toBool() || e.value(ClipKey::videoOnly).toBool()
             || e.value(ClipKey::gap).toBool() || !e.value(ClipKey::title).toString().isEmpty()
             || e.contains(ClipKey::compoundJson)
             || !silentSource(e.value(ClipKey::path).toString()));
}

// A clip held as a picture whose source is a recording: entries() marks it
// pictureless, so nothing draws it or asks it for a filmstrip.
static bool pictureOfRecording(const QVariantMap &e) {
    return !(e.value(ClipKey::audioOnly).toBool() || e.value(ClipKey::gap).toBool()
             || !e.value(ClipKey::title).toString().isEmpty() || e.contains(ClipKey::compoundJson))
           && media::soundOnly(e.value(ClipKey::path).toString());
}

static std::atomic<quint64> clipListBuildCount{0};

// One clip's entry, as entries() gives it. While the kept list stands it is
// read from there. When it does not (a restore changes the timeline clip by
// clip and asks for each as it goes) only that clip is read: building the
// whole list for one clip made a restore, and so every edit that conforms
// linked sound, take time with the square of the timeline's length.
QVariantMap Timeline::entryMapOn(int track, int index) const {
    const auto fromList = [&] {
        for (const QVariant &v : entries()) {
            const QVariantMap map = v.toMap();
            if (map.value(ClipKey::track).toInt() == track
                && map.value(ClipKey::index).toInt() == index)
                return map;
        }
        return QVariantMap();
    };
    if (clipListCurrent())
        return fromList();
    const QVariantMap one = oneEntryOn(track, index);
    static const bool verify = qEnvironmentVariableIsSet("OMAEDIT_VERIFY_CLIP_LIST");
    if (verify && one != fromList())
        qFatal("one clip read alone differs from the list: track %d index %d", track, index);
    return one;
}

QVariantMap Timeline::oneEntryOn(int track, int index) const {
    if (!primary() || track < 0 || index < 0 || track > laneTotal())
        return {};
    QVariantMap entry;
    const QVariantList logical = track > 0 ? m_tracks[size_t(track)].audioEntries : QVariantList();
    if (!logical.isEmpty()) {
        if (index >= logical.size())
            return {};
        entry = logical[index].toMap();
        entry[ClipKey::lane] = track;
        entry[ClipKey::track] = track;
        entry[ClipKey::index] = index;
    } else {
        QList<ClipEntry> built;
        appendClipEntries(track == 0 ? *primary() : *laneAt(track - 1), track, fps(), &built, index);
        if (built.isEmpty())
            return {};
        entry = built.constFirst().toVariant();
    }
    if (pictureOfSilentSource(entry))
        entry.insert(ClipKey::silent, true);
    if (pictureOfRecording(entry))
        entry.insert(ClipKey::pictureless, true);
    return entry;
}

QVariantList Timeline::entries() const {
    QVariantList list = clipEntries();  // brings the cache up to date
    if (m_clipList.valid && m_clipList.hasEntries) return m_clipList.entries;
    // A picture from a source with no sound: nothing to show beneath it,
    // nothing to detach.
    for (QVariant &v : list) {
        QVariantMap e = v.toMap();
        const bool silent = pictureOfSilentSource(e), pictureless = pictureOfRecording(e);
        if (!silent && !pictureless)
            continue;
        if (silent) e.insert(ClipKey::silent, true);
        if (pictureless) e.insert(ClipKey::pictureless, true);
        v = e;
    }
    if (m_clipList.valid) { m_clipList.entries = list; m_clipList.hasEntries = true; }
    return list;
}

// Whether the clip list last built still describes the timeline.
bool Timeline::clipListCurrent() const {
    const ClipListCache &c = m_clipList;
    if (!c.valid || c.graph != edit::revision() || c.fps != fps() || c.tracks.size() != m_tracks.size())
        return false;
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        const Track &t = m_tracks[i];
        if (c.tracks[i].first != t.playlist.get() || c.tracks[i].second.size() != t.audioEntries.size()
            || c.tracks[i].second.constData() != t.audioEntries.constData())
            return false;
    }
    return true;
}
quint64 Timeline::clipListBuilds() { return clipListBuildCount.load(); }

QVariantList Timeline::clipEntries() const {
    // OMAEDIT_VERIFY_CLIP_LIST=1 builds the list every time as well and stops
    // if the kept one differs: the check that nothing changes the timeline
    // without the kept list noticing.
    static const bool verify = qEnvironmentVariableIsSet("OMAEDIT_VERIFY_CLIP_LIST");
    if (clipListCurrent()) {
        if (verify) {
            m_clipList.valid = false;
            const QVariantList kept = m_clipList.clips;
            const QVariantList built = clipEntries();
            if (built != kept) qFatal("the kept clip list is stale: %lld clips kept, %lld built", qlonglong(kept.size()), qlonglong(built.size()));
        }
        return m_clipList.clips;
    }
    m_clipList.valid = false;
    m_clipList.hasEntries = false;
    ++clipListBuildCount;
    QVariantList list;
    if (!primary())
        return list;
    // Typed while they are built; maps from here on, for QML, undo and the file.
    QList<ClipEntry> built;
    const auto flush = [&] { for (const ClipEntry &entry : std::as_const(built)) list.append(entry.toVariant()); built.clear(); };
    appendClipEntries(*primary(), 0, fps(), &built);
    flush();
    for (int L = 0; L < laneTotal(); ++L) {
        const auto &logical=m_tracks[size_t(L)+1].audioEntries;
        if (logical.isEmpty()) { appendClipEntries(*laneAt(L), L + 1, fps(), &built); flush(); }
        else for (int i=0;i<logical.size();++i) {
            auto entry=logical[i].toMap(); entry[ClipKey::lane]=L+1; entry[ClipKey::track]=L+1; entry[ClipKey::index]=i; list.append(entry);
        }
    }
    m_clipList.clips = list;
    m_clipList.fps = fps();
    m_clipList.tracks.clear();
    for (const Track &t : m_tracks) m_clipList.tracks.emplace_back(t.playlist.get(), t.audioEntries);
    // Building may itself touch the graph (a wrapper made, a blank kept);
    // nothing but this read has run, so the graph as it stands now is the
    // one the list describes.
    m_clipList.graph = edit::revision();
    // One thing in an entry is not in the graph: a stabilised clip says
    // whether its analysis file is still on disk. A list holding one is
    // built afresh every time.
    m_clipList.valid = std::none_of(list.cbegin(), list.cend(), [](const QVariant &v) {
        return v.toMap().contains(ClipKey::stab);
    });
    return list;
}
